#include "mcp_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>

#ifdef Q_OS_WIN
#ifndef CREATE_NO_WINDOW
#define CREATE_NO_WINDOW 0x08000000
#endif
#endif

namespace qwenbot {
namespace {
constexpr qint64 kHandshakeTimeoutMs = 10000; // 握手+工具发现超时
constexpr qint64 kCallTimeoutMs = 10000;      // 单次工具调用超时
constexpr qint64 kReconnectMs = 5000;         // 断线重连退避
constexpr int kMaxLineBytes = 1024 * 1024;    // 单行消息上限
constexpr int kMaxOutputChars = 4000;         // 回传给模型的工具输出上限
constexpr int kMaxStderrLine = 500;           // 子进程 stderr 单行日志截断
} // namespace

McpClient::McpClient(const QString &label, const QString &command, QObject *parent)
    : QObject(parent), label_(label), command_(command)
{
    clock_.start();
    deadlineTimer_.setInterval(500);
    connect(&deadlineTimer_, &QTimer::timeout, this, &McpClient::sweepTimeouts);
    deadlineTimer_.start();
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &McpClient::start);
}

McpClient::~McpClient()
{
    shuttingDown_ = true;
    reconnectTimer_.stop();
    deadlineTimer_.stop();
    failPendingCalls(QStringLiteral("MCP 插件已停止"));
    cleanupProcess();
}

void McpClient::start()
{
    if (shuttingDown_) return;
    failPendingCalls(QStringLiteral("MCP 插件正在重启"));
    cleanupProcess();
    readBuf_.clear();
    tools_.clear();
    online_ = false;

    const QStringList parts = QProcess::splitCommand(command_);
    if (parts.isEmpty() || parts.constFirst().isEmpty()) {
        emit logMessage(QStringLiteral("MCP 插件 %1 的启动命令为空，无法执行。").arg(label_));
        return;
    }
    auto *process = new QProcess(this);
    process_ = process;
#ifdef Q_OS_WIN
    // GUI 程序下拉起控制台子进程会弹出黑色窗口，需隐藏
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process->setProgram(parts.constFirst());
    process->setArguments(parts.mid(1));
    connect(process, &QProcess::readyReadStandardOutput, this, &McpClient::readStdout);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        for (const auto &line : process->readAllStandardError().split('\n')) {
            const QString text = QString::fromUtf8(line.trimmed());
            if (!text.isEmpty())
                emit logMessage(QStringLiteral("MCP 插件 %1：%2")
                                    .arg(label_, text.left(kMaxStderrLine)));
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        QString reason = QStringLiteral("进程错误 %1").arg(int(error));
        if (error == QProcess::FailedToStart) {
            reason = QStringLiteral("无法启动（请检查命令与解释器是否可用）");
            // 首次启动失败不会经过 online 状态，必须直接记日志否则无任何可见线索
            emit logMessage(QStringLiteral("MCP 插件 %1 %2").arg(label_, reason));
        }
        handleProcessExit(reason);
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        handleProcessExit(QStringLiteral("进程退出（code=%1，%2）")
                              .arg(exitCode).arg(status == QProcess::CrashExit
                                                     ? QStringLiteral("异常终止")
                                                     : QStringLiteral("正常退出")));
    });
    startedMs_ = clock_.elapsed();
    process->start();
    emit logMessage(QStringLiteral("MCP 插件 %1 启动：%2").arg(label_, command_));
    sendInitialize();
}

void McpClient::shutdown()
{
    shuttingDown_ = true;
    reconnectTimer_.stop();
    failPendingCalls(QStringLiteral("MCP 插件已停止"));
    cleanupProcess();
    online_ = false;
}

bool McpClient::send(const QJsonObject &event)
{
    if (!process_ || process_->state() == QProcess::NotRunning)
        return false;
    QByteArray frame = QJsonDocument(event).toJson(QJsonDocument::Compact);
    frame.append('\n');
    return process_->write(frame) == frame.size();
}

void McpClient::sendInitialize()
{
    initRequestId_ = nextRequestId_++;
    send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
          {QStringLiteral("id"), initRequestId_},
          {QStringLiteral("method"), QStringLiteral("initialize")},
          {QStringLiteral("params"), QJsonObject{
              {QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")},
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
    if (!online_ || !process_ || process_->state() != QProcess::Running)
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

void McpClient::readStdout()
{
    if (!process_) return;
    readBuf_.append(process_->readAllStandardOutput());
    while (true) {
        const int newline = readBuf_.indexOf('\n');
        if (newline < 0) {
            if (readBuf_.size() > kMaxLineBytes) {
                // 违反 stdio 帧格式（单行超限），视为插件故障，重启进程
                emit logMessage(QStringLiteral("MCP 插件 %1 输出单行超限，重启进程。").arg(label_));
                handleProcessExit(QStringLiteral("输出帧超限"));
            }
            return;
        }
        const QByteArray line = readBuf_.left(newline);
        readBuf_.remove(0, newline + 1);
        if (!line.trimmed().isEmpty())
            handleLine(line);
    }
}

void McpClient::handleLine(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        emit logMessage(QStringLiteral("MCP 插件 %1 发来无法解析的行，已忽略。").arg(label_));
        return;
    }
    const QJsonObject message = document.object();
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
            handleProcessExit(QStringLiteral("initialize 失败"));
            return;
        }
        // 握手完成：发送 initialized 通知并发现工具
        send({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
              {QStringLiteral("method"), QStringLiteral("notifications/initialized")}});
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

void McpClient::handleProcessExit(const QString &reason)
{
    if (shuttingDown_) {
        cleanupProcess();
        return;
    }
    const bool wasOnline = online_;
    online_ = false;
    failPendingCalls(QStringLiteral("MCP 插件不可用（%1）").arg(reason));
    cleanupProcess();
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

void McpClient::cleanupProcess()
{
    if (!process_) return;
    auto *process = process_;
    process_ = nullptr;
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(1500);
    }
    process->deleteLater();
}

void McpClient::sweepTimeouts()
{
    const qint64 now = clock_.elapsed();
    // 握手超时：杀掉进程，走重连流程
    if (!online_ && process_ && process_->state() != QProcess::NotRunning &&
        now - startedMs_ > kHandshakeTimeoutMs) {
        emit logMessage(QStringLiteral("MCP 插件 %1 握手超时（10 秒）。").arg(label_));
        handleProcessExit(QStringLiteral("握手超时"));
        return;
    }
    // 工具调用超时：把错误回传给模型，不杀进程（可能只是单次执行慢）
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
