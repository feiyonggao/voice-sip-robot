#include "tool_registry.h"
#include "mcp_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace qwenbot {
namespace {
constexpr qint64 kToolTimeoutMs = 10000; // 单次工具调用超时，超时把错误回给模型
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

void ToolRegistry::applyMcpCommands(const QStringList &commands)
{
    QStringList wanted;
    for (const auto &raw : commands) {
        const QString command = raw.trimmed();
        if (!command.isEmpty() && !wanted.contains(command))
            wanted.append(command);
    }
    bool changed = false;
    // 移除不再配置的插件（从后往前，避免索引失效）
    for (int i = int(servers_.size()) - 1; i >= 0; --i) {
        if (!wanted.contains(servers_[i]->command)) {
            emit logMessage(QStringLiteral("MCP 插件已停止：%1").arg(servers_[i]->label));
            shutdownServer(i);
            changed = true;
        }
    }
    // 启动新配置的插件
    for (const auto &command : wanted) {
        bool exists = false;
        for (const auto &server : servers_)
            exists = exists || server->command == command;
        if (!exists) {
            createServer(command);
            changed = true;
        }
    }
    if (changed)
        emit toolsChanged();
}

void ToolRegistry::createServer(const QString &command)
{
    auto server = std::make_unique<McpServer>();
    server->command = command;
    server->label = labelFor(command);
    auto *client = new McpClient(server->label, command, this);
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
    emit logMessage(QStringLiteral("MCP 插件 %1 已连接。").arg(server->label));
    emit toolsChanged();
}

void ToolRegistry::handleServerOffline(McpServer *server, const QString &reason)
{
    const bool wasOnline = server->online || !server->tools.isEmpty();
    server->online = false;
    server->tools.clear();
    failPendingForServer(server, QStringLiteral("MCP 插件不可用：%1").arg(reason));
    if (wasOnline) {
        emit logMessage(QStringLiteral("MCP 插件 %1 离线：%2").arg(server->label, reason));
        emit toolsChanged();
    }
}

void ToolRegistry::handleServerTools(McpServer *server)
{
    server->tools.clear();
    for (const auto &desc : server->client->tools()) {
        // 内置工具名与跨插件重名保护：先注册者优先
        if (isBuiltIn(desc.name)) {
            emit logMessage(QStringLiteral("MCP 插件 %1 的工具 %2 与内置工具重名，已忽略。")
                                .arg(server->label, desc.name));
            continue;
        }
        McpServer *owner = findServerForTool(desc.name);
        if (owner && owner != server) {
            emit logMessage(QStringLiteral("MCP 插件 %1 的工具 %2 与其他插件重名，已忽略。")
                                .arg(server->label, desc.name));
            continue;
        }
        server->tools.append({desc.name, desc.description, server->label,
                              desc.parameters, false, true});
    }
    emit logMessage(QStringLiteral("MCP 插件 %1 提供 %2 个工具：%3")
                        .arg(server->label).arg(server->tools.size())
                        .arg([this, server] {
                            QStringList names;
                            for (const auto &tool : server->tools) names.append(tool.name);
                            return names.join(QStringLiteral(", "));
                        }()));
    emit toolsChanged();
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
    for (const auto &tool : builtins_)
        array.append(encode(tool));
    for (const auto &server : servers_) {
        if (!server->online) continue; // 离线插件不进 tools，避免模型调用必失败
        for (const auto &tool : server->tools)
            array.append(encode(tool));
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

ToolRegistry::McpServer *ToolRegistry::findServerForTool(const QString &name)
{
    for (const auto &server : servers_)
        for (const auto &tool : server->tools)
            if (tool.name == name) return server.get();
    return nullptr;
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
