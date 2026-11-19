#include "mcp_client.h"
#include "mcp_transport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QUrl>

namespace qwenbot {
namespace {
constexpr qint64 kHandshakeTimeoutMs = 10000; // 握手+工具发现超时
constexpr qint64 kCallTimeoutMs = 10000;      // 单次工具调用超时
constexpr qint64 kReconnectMs = 5000;         // 断线重连退避
constexpr qint64 kSessionRetryBaseMs = 1000;  // 会话失效后重新初始化退避基数
constexpr int kMaxSessionRecoveryAttempts = 4; // 重新初始化最大尝试次数
constexpr int kMaxOutputChars = 4000;         // 回传给模型的工具输出上限

QString pluginLabel(const McpPluginSpec &spec)
{
    if (spec.isHttp()) {
        const QUrl url(spec.url);
        const QString host = url.host();
        const QString path = url.path();
        if (!host.isEmpty())
            return path.isEmpty() || path == QStringLiteral("/")
                ? host : host + path;
        return spec.url;
    }
    const QStringList parts = QProcess::splitCommand(spec.command);
    if (parts.isEmpty()) return spec.name.isEmpty() ? QStringLiteral("mcp") : spec.name;
    QString name = parts.constFirst();
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
} // namespace

McpClient::McpClient(const McpPluginSpec &spec, QObject *parent)
    : QObject(parent), spec_(spec), label_(pluginLabel(spec))
{
    clock_.start();
    deadlineTimer_.setInterval(500);
    connect(&deadlineTimer_, &QTimer::timeout, this, &McpClient::sweepTimeouts);
    deadlineTimer_.start();
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &McpClient::start);
    sessionRetryTimer_.setSingleShot(true);
    connect(&sessionRetryTimer_, &QTimer::timeout, this, &McpClient::retryInitialize);
}

McpClient::~McpClient()
{
    shuttingDown_ = true;
    reconnectTimer_.stop();
    deadlineTimer_.stop();
    failPendingCalls(QStringLiteral("MCP 插件已停止"));
    cleanupTransport();
}

void McpClient::start()
{
    if (shuttingDown_) return;
    sessionRetryTimer_.stop();
    sessionRecoveryActive_ = false;
    sessionRecoveryAttempts_ = 0;
    failPendingCalls(QStringLiteral("MCP 插件正在重启"));
    cleanupTransport();
    tools_.clear();
    online_ = false;

    if (spec_.isHttp()) {
        transport_ = new McpHttpTransport(spec_.url, spec_.headers, this);
        // 曾经成功建立过 Streamable 会话的端点，后续重连不允许再回退旧版 SSE
        if (streamableSessionSeen_)
            transport_->setLegacyFallbackAllowed(false);
    } else {
        transport_ = new McpStdioTransport(spec_.command, this);
    }
    connect(transport_, &McpTransport::ready, this, &McpClient::sendInitialize);
    connect(transport_, &McpTransport::messageReceived, this, &McpClient::handleMessage);
    connect(transport_, &McpTransport::failed, this, &McpClient::handleTransportFailure);
    connect(transport_, &McpTransport::logMessage, this, &McpClient::logMessage);
    connect(transport_, &McpTransport::sessionExpired, this, &McpClient::handleSessionExpired);
    lastInitializeMs_ = clock_.elapsed();
    transport_->start();
}

void McpClient::shutdown()
{
    shuttingDown_ = true;
    reconnectTimer_.stop();
    sessionRetryTimer_.stop();
    sessionRecoveryActive_ = false;
    sessionRecoveryAttempts_ = 0;
    failPendingCalls(QStringLiteral("MCP 插件已停止"));
    cleanupTransport();
    online_ = false;
}

bool McpClient::send(const QJsonObject &event)
{
    return transport_ && transport_->isRunning() && transport_->send(event);
}

void McpClient::sendInitialize()
{
    initRequestId_ = nextRequestId_++;
    lastInitializeMs_ = clock_.elapsed();
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), initRequestId_},
          {QStringLiteral("method"), QStringLiteral("initialize")},
          {QStringLiteral("params"), QJsonObject{
              {QStringLiteral("protocolVersion"),
               transport_ ? transport_->protocolVersion() : QStringLiteral("2024-11-05")},
              {QStringLiteral("capabilities"), QJsonObject{}},
              {QStringLiteral("clientInfo"), QJsonObject{
                  {QStringLiteral("name"), QStringLiteral("voicebot")},
                  {QStringLiteral("version"), QStringLiteral("1.0")}}}}}});
}

void McpClient::sendToolsList()
{
    discoveryRequestId_ = nextRequestId_++;
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), discoveryRequestId_},
          {QStringLiteral("method"), QStringLiteral("tools/list")},
          {QStringLiteral("params"), QJsonObject{}}});
}

int McpClient::callTool(const QString &name, const QJsonObject &arguments)
{
    if (!online_ || !transport_ || !transport_->isRunning())
        return -1;
    const int requestId = nextRequestId_++;
    calls_.insert(requestId, {name, clock_.elapsed() + kCallTimeoutMs});
    const bool sent = send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                            {QStringLiteral("id"), requestId},
                            {QStringLiteral("method"), QStringLiteral("tools/call")},
                            {QStringLiteral("params"), QJsonObject{
                                {QStringLiteral("name"), name},
                                {QStringLiteral("arguments"), arguments}}}});
    if (!sent) {
        calls_.remove(requestId);
        return -1;
    }
    return requestId;
}

void McpClient::handleMessage(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonValue idValue = message.value(QStringLiteral("id"));
    const bool hasId = idValue.isDouble();
    const int requestId = hasId ? int(idValue.toDouble()) : 0;

    if (!hasId && !method.isEmpty()) {
        // 服务端通知；目前只关心工具列表热插拔
        if (method == QStringLiteral("notifications/tools/list_changed") && online_)
            sendToolsList();
        return;
    }
    if (hasId && !method.isEmpty()) {
        // 服务端发起的请求（采样等），本客户端一律拒绝
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
              {QStringLiteral("id"), requestId},
              {QStringLiteral("error"), QJsonObject{
                  {QStringLiteral("code"), -32601},
                  {QStringLiteral("message"), QStringLiteral("Method not found")}}}});
        return;
    }
    if (hasId)
        handleResponse(requestId, message.value(QStringLiteral("result")).toObject(),
                       message.value(QStringLiteral("error")).toObject());
}

void McpClient::handleResponse(int requestId, const QJsonObject &result,
                               const QJsonObject &error)
{
    if (requestId == initRequestId_) {
        if (!error.isEmpty()) {
            emit logMessage(QStringLiteral("MCP 插件 %1 initialize 被拒绝：%2")
                                .arg(label_, QString::fromUtf8(
                                    QJsonDocument(error).toJson(QJsonDocument::Compact))));
            // 会话失效后的重新握手失败：按指数退避重试，不立即拆通道
            if (sessionRecoveryActive_) {
                ++sessionRecoveryAttempts_;
                if (sessionRecoveryAttempts_ <= kMaxSessionRecoveryAttempts) {
                    const qint64 delay = qMin<qint64>(
                        kSessionRetryBaseMs * (qint64(1) << (sessionRecoveryAttempts_ - 1)), 15000);
                    emit logMessage(QStringLiteral(
                        "MCP 插件 %1 重新初始化失败（第 %2 次），%3 ms 后重试。")
                                        .arg(label_)
                                        .arg(sessionRecoveryAttempts_)
                                        .arg(delay));
                    sessionRetryTimer_.start(int(delay));
                    return;
                }
                emit logMessage(QStringLiteral(
                    "MCP 插件 %1 多次重新初始化失败，进入正常重连退避。").arg(label_));
                sessionRecoveryActive_ = false;
                sessionRecoveryAttempts_ = 0;
            }
            // 把服务端/传输层的具体原因（如 Connection refused、HTTP 401）带到界面状态
            const QString detail = error.value(QStringLiteral("message")).toString();
            handleTransportFailure(detail.isEmpty()
                ? QStringLiteral("initialize 失败") : detail);
            return;
        }
        // 握手完成：发送 initialized 通知、标记会话已建立并发现工具
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
              {QStringLiteral("method"), QStringLiteral("notifications/initialized")}});
        if (spec_.isHttp() && transport_ && !transport_->isLegacy())
            streamableSessionSeen_ = true;
        if (transport_) {
            transport_->markSessionEstablished();
            transport_->startServerMessages();
        }
        sessionRetryTimer_.stop();
        sessionRecoveryActive_ = false;
        sessionRecoveryAttempts_ = 0;
        sendToolsList();
        return;
    }
    if (requestId == discoveryRequestId_) {
        if (!error.isEmpty()) {
            emit logMessage(QStringLiteral("MCP 插件 %1 tools/list 失败：%2")
                                .arg(label_, QString::fromUtf8(
                                    QJsonDocument(error).toJson(QJsonDocument::Compact))));
            return;
        }
        tools_.clear();
        const auto items = result.value(QStringLiteral("tools")).toArray();
        for (const auto &item : items) {
            const QJsonObject tool = item.toObject();
            ToolDesc desc;
            desc.name = tool.value(QStringLiteral("name")).toString();
            desc.description = tool.value(QStringLiteral("description")).toString();
            desc.parameters = tool.value(QStringLiteral("inputSchema")).toObject();
            if (!desc.name.isEmpty())
                tools_.append(desc);
        }
        if (!online_) {
            online_ = true;
            emit onlineChanged(true, {});
        }
        emit toolsUpdated();
        return;
    }
    const auto it = calls_.constFind(requestId);
    if (it != calls_.constEnd()) {
        const QString name = it->name;
        calls_.remove(requestId);
        if (!error.isEmpty()) {
            emit toolFinished(requestId, name, errorOutput(QStringLiteral(
                "工具返回错误：%1").arg(error.value(QStringLiteral("message")).toString())));
        } else {
            emit toolFinished(requestId, name, outputFromResult(result));
        }
    }
    // 未知 ID：超时后迟到的结果，忽略
}

void McpClient::handleSessionExpired()
{
    if (shuttingDown_ || !spec_.isHttp() || !transport_ || sessionRecoveryActive_)
        return;
    sessionRecoveryActive_ = true;
    sessionRecoveryAttempts_ = 0;
    const bool wasOnline = online_;
    online_ = false;
    tools_.clear();
    failPendingCalls(QStringLiteral("MCP 会话已失效，正在重新初始化"));
    emit logMessage(QStringLiteral("MCP 插件 %1 会话已失效，正在重新初始化。").arg(label_));
    if (wasOnline)
        emit onlineChanged(false, QStringLiteral("会话已失效，正在重新初始化"));
    // 清空本地会话（session id / 主动消息流），重新走 initialize
    transport_->resetSession();
    sessionRetryTimer_.start(0);
}

void McpClient::retryInitialize()
{
    if (shuttingDown_ || !sessionRecoveryActive_ || !transport_ || !transport_->isRunning())
        return;
    sendInitialize();
}

void McpClient::handleTransportFailure(const QString &reason)
{
    sessionRetryTimer_.stop();
    sessionRecoveryActive_ = false;
    sessionRecoveryAttempts_ = 0;
    if (shuttingDown_) {
        cleanupTransport();
        return;
    }
    const bool wasOnline = online_;
    online_ = false;
    failPendingCalls(QStringLiteral("MCP 插件不可用（%1）").arg(reason));
    cleanupTransport();
    if (wasOnline)
        emit logMessage(QStringLiteral("MCP 插件 %1 掉线：%2").arg(label_, reason));
    emit onlineChanged(false, reason);
    reconnectTimer_.start(int(kReconnectMs));
}

void McpClient::failPendingCalls(const QString &error)
{
    const QString output = errorOutput(error);
    for (auto it = calls_.constBegin(); it != calls_.constEnd(); ++it)
        emit toolFinished(it.key(), it->name, output);
    calls_.clear();
}

void McpClient::cleanupTransport()
{
    if (!transport_) return;
    auto *transport = transport_;
    transport_ = nullptr;
    transport->disconnect(this);
    transport->stop();
    transport->deleteLater();
}

void McpClient::sweepTimeouts()
{
    const qint64 now = clock_.elapsed();
    // 握手超时：终止当前通道，走重连流程（退避等待期间不计时）
    if (!online_ && transport_ && transport_->isRunning()
        && !sessionRetryTimer_.isActive()
        && now - lastInitializeMs_ > kHandshakeTimeoutMs) {
        emit logMessage(QStringLiteral("MCP 插件 %1 握手超时（10 秒）。").arg(label_));
        handleTransportFailure(QStringLiteral("握手超时"));
        return;
    }
    // 工具调用超时：把错误回传给模型，不重启通道（可能只是单次执行慢）
    for (auto it = calls_.begin(); it != calls_.end();) {
        if (it->deadlineMs > now) { ++it; continue; }
        const int requestId = it.key();
        const QString name = it->name;
        it = calls_.erase(it);
        emit toolFinished(requestId, name, errorOutput(QStringLiteral(
            "MCP 工具调用超时（10 秒），请告知用户暂时无法完成该操作。")));
    }
}

QString McpClient::outputFromResult(const QJsonObject &result) const
{
    const bool isError = result.value(QStringLiteral("isError")).toBool();
    QString text;
    const auto content = result.value(QStringLiteral("content")).toArray();
    for (const auto &item : content) {
        const QJsonObject entry = item.toObject();
        if (entry.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
            if (!text.isEmpty()) text += QLatin1Char('\n');
            text += entry.value(QStringLiteral("text")).toString();
        }
    }
    // 非文本内容（图片/资源）整体序列化回传，交由模型自行取舍
    if (text.isEmpty() && !content.isEmpty())
        text = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
    if (text.size() > kMaxOutputChars)
        text = text.left(kMaxOutputChars) + QStringLiteral("…（结果过长已截断）");
    QJsonObject output;
    if (isError) {
        output.insert(QStringLiteral("ok"), false);
        output.insert(QStringLiteral("error"),
                      text.isEmpty() ? QStringLiteral("工具执行出错") : text);
    } else {
        output.insert(QStringLiteral("ok"), true);
        output.insert(QStringLiteral("result"), text);
    }
    return QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact));
}

QString McpClient::errorOutput(const QString &message) const
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"), message.left(kMaxOutputChars)}})
        .toJson(QJsonDocument::Compact));
}

} // namespace qwenbot
