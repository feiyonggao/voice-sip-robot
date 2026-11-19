#include "tool_registry.h"
#include "mcp_client.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

namespace qwenbot {
namespace {
constexpr qint64 kToolTimeoutMs = 10000; // 单次工具调用超时，超时把错误回给模型

// stdio 插件源文件指纹：mtime + size + SHA-256。
// 用于识别「配置未变，但同名插件文件被覆盖上传」的情况（HTTP 无本地文件，返回空串）。
QString pluginFileStamp(const McpPluginSpec &spec)
{
    if (spec.isHttp()) return {};

    const QStringList parts = QProcess::splitCommand(spec.command);
    auto isScript = [](const QString &value) {
        const QString lower = value.toLower();
        return lower.endsWith(QStringLiteral(".py"))
            || lower.endsWith(QStringLiteral(".js"))
            || lower.endsWith(QStringLiteral(".mjs"));
    };
    QString path;
    for (int i = 1; i < parts.size(); ++i) {
        if (isScript(parts.at(i)) && QFileInfo(parts.at(i)).isFile()) {
            path = parts.at(i);
            break;
        }
    }
    if (path.isEmpty() && !spec.name.contains(QLatin1Char('/'))
        && !spec.name.contains(QLatin1Char('\\'))) {
        const QString candidate = QDir(ToolRegistry::pluginsDir()).filePath(spec.name);
        if (QFileInfo(candidate).isFile()) path = candidate;
    }
    if (path.isEmpty()) return {};

    const QFileInfo info(path);
    QString stamp = QStringLiteral("%1:%2")
                        .arg(info.lastModified().toMSecsSinceEpoch())
                        .arg(info.size());
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 4LL * 1024 * 1024) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (hash.addData(&file))
            stamp += QLatin1Char(':') + QString::fromLatin1(hash.result().toHex());
    }
    return stamp;
}
} // namespace

ToolRegistry::ToolRegistry(QObject *parent) : QObject(parent)
{
    clock_.start();
    sweepTimer_.setInterval(500);
    connect(&sweepTimer_, &QTimer::timeout, this, &ToolRegistry::sweepTimeouts);
    sweepTimer_.start();
    // 内置挂机工具：与 qwen_session 的兜底描述保持一致
    registerBuiltIn(QStringLiteral("hang_up"),
        QStringLiteral("挂断电话，结束本次通话。当用户明确道别、表示不再需要帮助，"
                       "或本次任务已完成时，先向用户简短道别，随后立即调用此工具。"));
}

ToolRegistry::~ToolRegistry()
{
    for (int i = int(servers_.size()) - 1; i >= 0; --i)
        shutdownServer(i);
}

void ToolRegistry::registerBuiltIn(const QString &name, const QString &description,
                                   const QJsonObject &parameters)
{
    for (const auto &tool : builtins_)
        if (tool.name == name) return; // 已注册，忽略
    builtins_.append({name, description, {}, parameters, true, true});
    emit toolsChanged();
}

void ToolRegistry::applyMcpPlugins(const QList<McpPluginSpec> &plugins)
{
    QList<McpPluginSpec> wanted;
    for (auto spec : plugins) {
        spec.name = spec.name.trimmed();
        spec.transport = spec.isHttp() ? QStringLiteral("http") : QStringLiteral("stdio");
        if (spec.isHttp()) {
            spec.url = spec.url.trimmed();
            if (spec.name.isEmpty()) spec.name = labelForUrl(spec.url);
            if (spec.url.isEmpty()) {
                emit logMessage(QStringLiteral("MCP 插件 %1 缺少 HTTP 端点 URL，已跳过。")
                                    .arg(spec.name));
                continue;
            }
        } else {
            spec.command = spec.command.trimmed();
            if (spec.command.isEmpty())
                spec.command = commandForPlugin(spec.name);
            if (spec.command.isEmpty()) {
                emit logMessage(QStringLiteral(
                    "MCP 插件 %1 无法解析启动命令（文件不存在或解释器未安装），已跳过。")
                                    .arg(spec.name.isEmpty() ? QStringLiteral("mcp") : spec.name));
                continue;
            }
            if (spec.name.isEmpty()) spec.name = labelFor(spec.command);
        }
        const QString key = spec.key();
        bool duplicate = false;
        for (const auto &existing : wanted) {
            if (existing.key() == key) { duplicate = true; break; }
        }
        if (duplicate) {
            emit logMessage(QStringLiteral("MCP 插件 %1 重复配置，已忽略。").arg(spec.name));
            continue;
        }
        wanted.append(spec);
    }

    // stdio 插件的源文件指纹（HTTP 为空），用于识别同名文件被覆盖上传
    QHash<QString, QString> wantedStamps;
    for (const auto &spec : wanted)
        wantedStamps.insert(spec.key(), pluginFileStamp(spec));

    bool changed = false;
    // 移除不再配置的插件（从后往前，避免索引失效）
    for (int i = int(servers_.size()) - 1; i >= 0; --i) {
        bool keep = false;
        for (const auto &spec : wanted)
            keep = keep || servers_[i]->key == spec.key();
        if (!keep) {
            emit logMessage(QStringLiteral("MCP 插件已停止：%1").arg(servers_[i]->label));
            shutdownServer(i);
            changed = true;
        }
    }
    // 启动新配置的插件；同 key 的保持运行，但 stdio 插件文件被覆盖时需整进程重启
    for (const auto &spec : wanted) {
        int existing = -1;
        for (int i = 0; i < int(servers_.size()); ++i) {
            if (servers_[i]->key == spec.key()) { existing = i; break; }
        }
        if (existing < 0) {
            createServer(spec);
            changed = true;
            continue;
        }
        if (spec.isHttp()) continue; // HTTP 无本地文件，不需要文件变更重启
        const QString stamp = wantedStamps.value(spec.key());
        if (!stamp.isEmpty() && servers_[existing]->sourceStamp != stamp) {
            emit logMessage(QStringLiteral("插件文件已更新，正在重启子进程：%1")
                                .arg(servers_[existing]->label));
            shutdownServer(existing);
            createServer(spec);
            changed = true;
        }
    }
    // 来源被移除后，此前被其占名压住的同名工具应立即接替
    if (rebuildToolSets())
        changed = true;
    if (changed)
        emit toolsChanged();
}

void ToolRegistry::applyMcpCommands(const QStringList &commands)
{
    QList<McpPluginSpec> plugins;
    for (const auto &raw : commands) {
        const QString command = raw.trimmed();
        if (command.isEmpty()) continue;
        McpPluginSpec spec;
        spec.transport = QStringLiteral("stdio");
        spec.command = command;
        spec.name = labelFor(command);
        plugins.append(spec);
    }
    applyMcpPlugins(plugins);
}

void ToolRegistry::createServer(const McpPluginSpec &spec)
{
    auto server = std::make_unique<McpServer>();
    server->spec = spec;
    server->key = spec.key();
    server->label = spec.isHttp() ? labelForUrl(spec.url) : labelFor(spec.command);
    server->sourceStamp = pluginFileStamp(spec);
    auto *client = new McpClient(spec, this);
    server->client = client;
    connect(client, &McpClient::onlineChanged, this,
            [this, raw = server.get()](bool online, const QString &reason) {
        if (online) handleServerOnline(raw);
        else handleServerOffline(raw, reason);
    });
    connect(client, &McpClient::toolsUpdated, this,
            [this, raw = server.get()] { handleServerTools(raw); });
    connect(client, &McpClient::toolFinished, this,
            [this, raw = server.get()](int requestId, const QString &name, const QString &outputJson) {
        handleClientResult(raw, requestId, name, outputJson);
    });
    connect(client, &McpClient::logMessage, this, &ToolRegistry::logMessage);
    servers_.push_back(std::move(server));
    client->start();
}

void ToolRegistry::shutdownServer(int index)
{
    if (index < 0 || index >= int(servers_.size())) return;
    auto server = std::move(servers_[index]);
    servers_.erase(servers_.begin() + index);
    failPendingForServer(server.get(), QStringLiteral("MCP 插件已停止"));
    if (server->client) {
        server->client->disconnect(this);
        server->client->shutdown();
        server->client->deleteLater();
        server->client = nullptr;
    }
}

void ToolRegistry::handleServerOnline(McpServer *server)
{
    server->online = true;
    server->lastError.clear();
    emit logMessage(QStringLiteral("MCP 插件 %1 已连接。").arg(server->label));
    emit toolsChanged();
}

void ToolRegistry::handleServerOffline(McpServer *server, const QString &reason)
{
    const bool wasOnline = server->online || !server->tools.isEmpty()
        || !server->ignored.isEmpty();
    server->online = false;
    server->lastError = reason;
    server->discovered.clear();
    server->tools.clear();
    server->ignored.clear();
    failPendingForServer(server, QStringLiteral("MCP 插件不可用：%1").arg(reason));
    // 该来源占用的名字被释放，其它来源的同名工具应自动接替
    const bool reassigned = rebuildToolSets();
    if (wasOnline || reassigned) {
        emit logMessage(QStringLiteral("MCP 插件 %1 离线：%2").arg(server->label, reason));
        emit toolsChanged();
    }
}

void ToolRegistry::handleServerTools(McpServer *server)
{
    server->discovered = server->client->tools();
    rebuildToolSets();
    QStringList names;
    for (const auto &tool : server->tools) names.append(tool.name);
    QString ignoredText;
    if (!server->ignored.isEmpty()) {
        QStringList ignoredNames;
        for (const auto &item : server->ignored)
            ignoredNames.append(QStringLiteral("%1（与 %2 同名）").arg(item.name, item.owner));
        ignoredText = QStringLiteral("；%1 个工具因重名未注册：%2")
                          .arg(server->ignored.size())
                          .arg(ignoredNames.join(QStringLiteral(", ")));
    }
    emit logMessage(QStringLiteral("MCP 插件 %1 提供 %2 个工具：%3%4")
                        .arg(server->label).arg(server->tools.size())
                        .arg(names.join(QStringLiteral(", ")), ignoredText));
    emit toolsChanged();
}

bool ToolRegistry::rebuildToolSets()
{
    bool changed = false;
    QHash<QString, QString> claimed; // 工具名 → 占名来源展示名
    for (const auto &tool : builtins_)
        claimed.insert(tool.name, QStringLiteral("内置工具"));
    QSet<QString> currentConflicts;
    for (auto &server : servers_) {
        QList<Tool> effective;
        QList<IgnoredTool> ignored;
        if (server->online) {
            for (const auto &desc : server->discovered) {
                const QString owner = claimed.value(desc.name);
                if (!owner.isEmpty()) {
                    ignored.append({desc.name, desc.description, owner});
                    continue;
                }
                // server 字段用配置名而非展示名：GUI 的传输方式/来源查找都以
                // tools/plugin/<名称> 为键，远端展示名（主机名）会导致查不到而误显示 stdio
                claimed.insert(desc.name, server->spec.name);
                effective.append({desc.name, desc.description, server->spec.name,
                                  desc.parameters, false, true});
            }
        }
        // 工具集是否变化以名称序列为准（说明文字变化不影响模型快照的可用性判断）
        auto sameNames = [](const QList<Tool> &lhs, const QList<Tool> &rhs) {
            if (lhs.size() != rhs.size()) return false;
            for (int i = 0; i < lhs.size(); ++i)
                if (lhs.at(i).name != rhs.at(i).name) return false;
            return true;
        };
        if (!sameNames(server->tools, effective))
            changed = true;
        const QList<IgnoredTool> previous = server->ignored;
        server->tools = effective;
        server->ignored = ignored;
        // 冲突日志只记一次；冲突解除（占名来源离线/被移除）时补一条启用记录
        for (const auto &item : ignored) {
            const QString key = server->key + QLatin1Char('\x1f') + item.name
                                + QLatin1Char('\x1f') + item.owner;
            currentConflicts.insert(key);
            if (!conflictLogged_.contains(key)) {
                emit logMessage(QStringLiteral(
                    "MCP 插件 %1 的工具 %2 与 %3 的同名工具冲突，已忽略；"
                    "移除或停用 %3 后会自动启用该工具。")
                                    .arg(server->label, item.name, item.owner));
            }
        }
        if (server->online) {
            for (const auto &old : previous) {
                bool stillIgnored = false;
                for (const auto &item : ignored)
                    stillIgnored = stillIgnored || item.name == old.name;
                bool nowActive = false;
                for (const auto &tool : effective)
                    nowActive = nowActive || tool.name == old.name;
                if (!stillIgnored && nowActive) {
                    emit logMessage(QStringLiteral(
                        "MCP 插件 %1 的工具 %2 重名冲突已解除，已启用。")
                                        .arg(server->label, old.name));
                }
            }
        }
    }
    conflictLogged_ = currentConflicts;
    return changed;
}

void ToolRegistry::handleClientResult(McpServer *server, int requestId,
                                      const QString &name, const QString &outputJson)
{
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->server != server || it->requestId != requestId) continue;
        const int token = it.key();
        pending_.erase(it);
        emit toolResult(token, name, outputJson);
        return;
    }
    // 未匹配：多半是超时后迟到的结果，忽略
}

void ToolRegistry::failPendingForServer(McpServer *server, const QString &error)
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->server != server) { ++it; continue; }
        const int token = it.key();
        const QString name = it->name;
        it = pending_.erase(it);
        emit toolResult(token, name,
            QString::fromUtf8(QJsonDocument(QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), error}}).toJson(QJsonDocument::Compact)));
    }
}

void ToolRegistry::sweepTimeouts()
{
    const qint64 now = clock_.elapsed();
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->deadlineMs > now) { ++it; continue; }
        const int token = it.key();
        const QString name = it->name;
        it = pending_.erase(it);
        emit toolResult(token, name, QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral(
                "MCP 工具调用超时（10 秒），请告知用户暂时无法完成该操作。")}})
            .toJson(QJsonDocument::Compact)));
    }
}

QJsonArray ToolRegistry::toolsJson() const
{
    auto encode = [](const Tool &tool) {
        QJsonObject parameters = tool.parameters;
        if (parameters.isEmpty())
            parameters = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                     {QStringLiteral("properties"), QJsonObject{}},
                                     {QStringLiteral("required"), QJsonArray{}}};
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("function")},
            {QStringLiteral("function"), QJsonObject{
                {QStringLiteral("name"), tool.name},
                {QStringLiteral("description"), tool.description},
                {QStringLiteral("parameters"), parameters}}}};
    };
    QJsonArray array;
    for (const auto &tool : builtins_) {
        if (disabledTools_.contains(tool.name)) continue; // 工具级禁用：不进新通话的工具集
        array.append(encode(tool));
    }
    for (const auto &server : servers_) {
        if (!server->online) continue; // 离线插件不进 tools，避免模型调用必失败
        for (const auto &tool : server->tools) {
            if (disabledTools_.contains(tool.name)) continue;
            array.append(encode(tool));
        }
    }
    return array;
}

QList<ToolRegistry::Tool> ToolRegistry::tools() const
{
    QList<Tool> result = builtins_;
    for (const auto &server : servers_) {
        for (Tool tool : server->tools) {
            tool.online = server->online;
            result.append(tool);
        }
    }
    return result;
}

QList<ToolRegistry::PluginState> ToolRegistry::pluginStates() const
{
    QList<PluginState> result;
    for (const auto &server : servers_) {
        result.append({server->spec.name, server->label, server->online,
                       server->spec.isHttp() ? QStringLiteral("http")
                                             : QStringLiteral("stdio"),
                       server->lastError, server->tools, server->ignored});
    }
    return result;
}

void ToolRegistry::setDisabledTools(const QStringList &names)
{
    QSet<QString> wanted;
    for (const auto &raw : names) {
        const QString name = raw.trimmed();
        if (!name.isEmpty()) wanted.insert(name);
    }
    // 不发射 toolsChanged：禁用只影响下一次通话的 toolsJson() 快照，
    // 也不需要在 GUI 中重建表格（避免在开关自身信号里销毁控件）。
    disabledTools_ = wanted;
}

QStringList ToolRegistry::disabledTools() const
{
    QStringList names = QStringList(disabledTools_.begin(), disabledTools_.end());
    names.sort();
    return names;
}

bool ToolRegistry::isBuiltIn(const QString &name) const
{
    for (const auto &tool : builtins_)
        if (tool.name == name) return true;
    return false;
}

int ToolRegistry::pendingMcpCount() const
{
    int count = 0;
    for (const auto &server : servers_)
        count += server->online ? 0 : 1;
    return count;
}

int ToolRegistry::callMcpTool(const QString &name, const QJsonObject &arguments)
{
    for (const auto &server : servers_) {
        if (!server->online) continue;
        for (const auto &tool : server->tools) {
            if (tool.name != name) continue;
            const int requestId = server->client->callTool(name, arguments);
            if (requestId <= 0) return -1;
            const int token = nextToken_++;
            pending_.insert(token, {server.get(), requestId, name,
                                    clock_.elapsed() + kToolTimeoutMs});
            return token;
        }
    }
    return -1;
}

QString ToolRegistry::pluginsDir()
{
    const QByteArray env = qgetenv("VOICEBOT_PLUGINS_DIR");
    if (!env.isEmpty())
        return QDir::cleanPath(QString::fromLocal8Bit(env));
    // 安装布局：<prefix>/bin/voicebot + <prefix>/plugins；也兼容 bin/plugins
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {appDir + QStringLiteral("/../plugins"),
                                    appDir + QStringLiteral("/plugins")};
    for (const auto &candidate : candidates)
        if (QDir(candidate).exists())
            return QDir::cleanPath(candidate);
    return QDir::cleanPath(candidates.constFirst()); // 不存在时返回首选，供日志提示
}

QStringList ToolRegistry::scanPluginFiles()
{
    QDir dir(pluginsDir());
    if (!dir.exists()) return {};
    QStringList files = dir.entryList(
        {QStringLiteral("*.py"), QStringLiteral("*.js"), QStringLiteral("*.mjs")},
        QDir::Files, QDir::Name);
    files.removeDuplicates();
    return files;
}

QString ToolRegistry::commandForPlugin(const QString &fileName)
{
    // 只接受文件名，防止配置被构造为路径穿越
    if (fileName.isEmpty() || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\')))
        return {};
    const QString path = QDir(pluginsDir()).filePath(fileName);
    if (!QFileInfo::exists(path)) return {};

    auto pickInterpreter = [](const QStringList &names) -> QString {
        for (const auto &name : names)
            if (!QStandardPaths::findExecutable(name).isEmpty()) return name;
        return {};
    };
    const QString lower = fileName.toLower();
    QString interpreter;
    if (lower.endsWith(QStringLiteral(".py"))) {
#ifdef Q_OS_WIN
        // Windows 上 python 最常见（python3 可能是商店别名），Unix 上 python3 优先
        interpreter = pickInterpreter({QStringLiteral("python"), QStringLiteral("python3"),
                                       QStringLiteral("py")});
#else
        interpreter = pickInterpreter({QStringLiteral("python3"), QStringLiteral("python")});
#endif
    } else if (lower.endsWith(QStringLiteral(".js"))
               || lower.endsWith(QStringLiteral(".mjs"))) {
        interpreter = pickInterpreter({QStringLiteral("node")});
    }
    if (interpreter.isEmpty()) return {};
    // 路径加引号：经 QProcess::splitCommand 解析时可容忍目录中的空格
    return QStringLiteral("%1 \"%2\"")
        .arg(interpreter, QDir::toNativeSeparators(path));
}

QStringList ToolRegistry::pluginDependencies(const QString &fileName)
{
    if (fileName.isEmpty() || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\')))
        return {};
    QFile file(QDir(pluginsDir()).filePath(fileName));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString source = QString::fromUtf8(file.readAll());

    QStringList raw;
    const bool python = fileName.toLower().endsWith(QStringLiteral(".py"));
    if (python) {
        // import x.y / from x.y import ...（取顶层包名 x）
        QRegularExpression re(QStringLiteral(
            "(?m)^\\s*(?:import|from)\\s+([A-Za-z_][A-Za-z0-9_]*)"));
        auto it = re.globalMatch(source);
        while (it.hasNext()) raw.append(it.next().captured(1));
    } else {
        // Node: require('x') / from 'x'，排除相对路径与 node 内置模块
        QRegularExpression re(QStringLiteral(
            "(?:require\\(\\s*|from\\s*)['\"]([A-Za-z@][^'\"]*)['\"]"));
        auto it = re.globalMatch(source);
        while (it.hasNext()) raw.append(it.next().captured(1));
    }

    // 标准库/内置模块不展示
    static const QSet<QString> builtins = {
        // Python 标准库（插件中常见的）
        QStringLiteral("json"), QStringLiteral("os"), QStringLiteral("sys"),
        QStringLiteral("re"), QStringLiteral("typing"), QStringLiteral("collections"),
        QStringLiteral("datetime"), QStringLiteral("asyncio"), QStringLiteral("enum"),
        QStringLiteral("abc"), QStringLiteral("dataclasses"), QStringLiteral("time"),
        QStringLiteral("math"), QStringLiteral("functools"), QStringLiteral("itertools"),
        QStringLiteral("pathlib"), QStringLiteral("__future__"), QStringLiteral("typing_extensions"),
        // Node 内置模块
        QStringLiteral("fs"), QStringLiteral("path"), QStringLiteral("http"),
        QStringLiteral("https"), QStringLiteral("url"), QStringLiteral("util"),
        QStringLiteral("stream"), QStringLiteral("events"), QStringLiteral("child_process"),
        QStringLiteral("os")};
    QStringList result;
    for (const auto &name : raw)
        if (!builtins.contains(name) && !result.contains(name))
            result.append(name);
    result.sort();
    return result;
}

QString ToolRegistry::labelForUrl(const QString &url)
{
    const QUrl parsed(url);
    const QString authority = parsed.authority();
    if (authority.isEmpty())
        return url.isEmpty() ? QStringLiteral("mcp-http") : url;
    const QString path = parsed.path();
    return path.isEmpty() || path == QStringLiteral("/")
        ? authority : authority + path;
}

QString ToolRegistry::labelFor(const QString &command)
{
    const QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) return QStringLiteral("mcp");
    QString name = parts.constFirst();
    // 命令是解释器时取脚本文件名展示
    static const QStringList interpreters = {
        QStringLiteral("python"), QStringLiteral("python3"), QStringLiteral("py"),
        QStringLiteral("node"), QStringLiteral("deno"), QStringLiteral("bun")};
    if (parts.size() > 1 && interpreters.contains(name.toLower()))
        name = parts.at(1);
    const int slash = qMax(name.lastIndexOf(QLatin1Char('/')),
                           name.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0) name = name.mid(slash + 1);
    return name.isEmpty() ? QStringLiteral("mcp") : name;
}

} // namespace qwenbot
