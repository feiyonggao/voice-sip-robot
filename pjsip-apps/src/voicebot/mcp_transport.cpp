#include "mcp_transport.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVariant>

#ifdef Q_OS_WIN
#ifndef CREATE_NO_WINDOW
#define CREATE_NO_WINDOW 0x08000000
#endif
#endif

namespace qwenbot {
namespace {

constexpr int kMaxLineBytes = 1024 * 1024;  // stdio 单行消息上限
constexpr int kMaxStderrLine = 500;         // 子进程 stderr 单行日志截断
constexpr qint64 kPostTimeoutMs = 12000;    // HTTP 单次请求上限
constexpr qint64 kReconnectMs = 5000;       // 通知流/旧版 SSE 断开后的重连退避
constexpr qint64 kLegacyEndpointMs = 5000;  // 旧版 SSE 等待 endpoint 事件的时长
constexpr int kMaxBodyLog = 300;            // HTTP 错误响应体日志截断

// 请求头值支持 ${ENV} 与 %ENV% 两种环境变量占位，避免把密钥写进配置文件
QString expandEnvironment(const QString &value)
{
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}")),
        QRegularExpression(QStringLiteral("%([A-Za-z_][A-Za-z0-9_]*)%"))};
    QString result = value;
    for (const auto &pattern : patterns) {
        QRegularExpressionMatchIterator it = pattern.globalMatch(result);
        QString replaced = result;
        // 从后往前替换，避免匹配位置随替换变化
        QList<QRegularExpressionMatch> matches;
        while (it.hasNext()) matches.append(it.next());
        for (int i = int(matches.size()) - 1; i >= 0; --i) {
            const auto match = matches.at(i);
            const QString name = match.captured(1);
            const QString env = qEnvironmentVariable(name.toLatin1().constData());
            replaced.replace(match.capturedStart(), match.capturedLength(), env);
        }
        result = replaced;
    }
    return result;
}

QString httpStatusText(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString reason =
        reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    if (status == 0) return reply->errorString();
    return QStringLiteral("HTTP %1%2").arg(status).arg(
        reason.isEmpty() ? QString() : QStringLiteral(" ") + reason);
}

// 判断 POST 失败是否意味着服务端不支持 Streamable HTTP（需要回退旧版 SSE）
bool needsLegacyFallback(int status)
{
    switch (status) {
    case 404: case 405: case 406: case 415: case 501:
        return true;
    default:
        return false;
    }
}

// 旧版回退只能发生在初次 initialize：其它请求的 4xx 不代表协议不兼容
bool isInitializeRequest(const QByteArray &payload)
{
    return QJsonDocument::fromJson(payload).object()
               .value(QStringLiteral("method")).toString()
        == QStringLiteral("initialize");
}

} // namespace

// ============================ stdio 传输 ============================

McpStdioTransport::McpStdioTransport(const QString &command, QObject *parent)
    : McpTransport(parent), command_(command)
{
}

McpStdioTransport::~McpStdioTransport()
{
    stop();
}

void McpStdioTransport::start()
{
    if (process_) return;
    dead_ = false;
    const QStringList parts = QProcess::splitCommand(command_);
    if (parts.isEmpty() || parts.constFirst().isEmpty()) {
        emit logMessage(QStringLiteral("MCP 插件启动命令为空，无法执行。"));
        emit failed(QStringLiteral("启动命令为空"));
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
    connect(process, &QProcess::readyReadStandardOutput, this, &McpStdioTransport::readStdout);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        for (const auto &line : process->readAllStandardError().split('\n')) {
            const QString text = QString::fromUtf8(line.trimmed());
            if (!text.isEmpty())
                emit logMessage(QStringLiteral("MCP 插件 stderr：%1").arg(text.left(kMaxStderrLine)));
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        QString reason = QStringLiteral("进程错误 %1").arg(int(error));
        if (error == QProcess::FailedToStart) {
            reason = QStringLiteral("无法启动（请检查命令与解释器是否可用）");
            // 首次启动失败不会经过在线状态，必须直接记日志否则无任何可见线索
            emit logMessage(QStringLiteral("MCP 插件 %1").arg(reason));
        }
        reportFailure(reason);
    });
    connect(process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        reportFailure(QStringLiteral("进程退出（code=%1，%2）")
                          .arg(exitCode)
                          .arg(status == QProcess::CrashExit ? QStringLiteral("异常终止")
                                                             : QStringLiteral("正常退出")));
    });
    process->start();
    emit logMessage(QStringLiteral("MCP 插件启动：%1").arg(command_));
    emit ready();
}

void McpStdioTransport::stop()
{
    dead_ = true;
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

bool McpStdioTransport::isRunning() const
{
    return process_ && process_->state() != QProcess::NotRunning;
}

bool McpStdioTransport::send(const QJsonObject &message)
{
    if (!process_ || process_->state() == QProcess::NotRunning) return false;
    QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact);
    frame.append('\n');
    return process_->write(frame) == frame.size();
}

void McpStdioTransport::readStdout()
{
    if (!process_) return;
    readBuf_.append(process_->readAllStandardOutput());
    while (true) {
        const int newline = readBuf_.indexOf('\n');
        if (newline < 0) {
            if (readBuf_.size() > kMaxLineBytes) {
                // 违反 stdio 帧格式（单行超限），视为插件故障，重启进程
                emit logMessage(QStringLiteral("MCP 插件输出单行超限，重启进程。"));
                reportFailure(QStringLiteral("输出帧超限"));
            }
            return;
        }
        const QByteArray line = readBuf_.left(newline);
        readBuf_.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            emit logMessage(QStringLiteral("MCP 插件发来无法解析的行，已忽略。"));
            continue;
        }
        emit messageReceived(document.object());
    }
}

void McpStdioTransport::reportFailure(const QString &reason)
{
    if (dead_) return;
    dead_ = true;
    emit failed(reason);
}

// ============================ HTTP 传输 ============================

McpHttpTransport::McpHttpTransport(const QString &url, const QStringList &headers,
                                   QObject *parent)
    : McpTransport(parent), url_(url.trimmed()), headers_(headers),
      nam_(new QNetworkAccessManager(this))
{
    notificationRetry_.setSingleShot(true);
    connect(&notificationRetry_, &QTimer::timeout, this, &McpHttpTransport::openNotificationStream);
    legacyEndpointTimer_.setSingleShot(true);
    connect(&legacyEndpointTimer_, &QTimer::timeout, this, [this] {
        if (!running_ || legacyMessagesUrl_.isEmpty() == false) return;
        legacyMessagesUrl_ = legacyMessagesUrlFallback();
        emit logMessage(QStringLiteral("旧版 SSE 未下发 endpoint 事件，改用默认消息路径：%1")
                            .arg(legacyMessagesUrl_.toString()));
        flushLegacyQueue();
    });
}

McpHttpTransport::~McpHttpTransport()
{
    stop();
}

void McpHttpTransport::start()
{
    if (running_) return;
    const QUrl target(url_);
    if (!target.isValid() || target.isEmpty()
        || (target.scheme() != QStringLiteral("http")
            && target.scheme() != QStringLiteral("https"))) {
        emit logMessage(QStringLiteral("MCP HTTP 端点无效：%1").arg(url_));
        emit failed(QStringLiteral("HTTP 端点无效"));
        return;
    }
    running_ = true;
    failed_ = false;
    emit logMessage(QStringLiteral("连接 MCP HTTP 端点：%1").arg(url_));
    // 格式不对的请求头会被静默丢弃（例如只填了 "Bearer xxx" 缺少名称），
    // 直接导致 401。这里只统计数量，不打印内容，避免把密钥写进日志。
    int invalidHeaders = 0;
    for (const auto &header : headers_) {
        for (const auto &line : expandEnvironment(header).split('\n')) {
            const QString text = line.trimmed();
            if (text.isEmpty()) continue;
            const int colon = text.indexOf(QLatin1Char(':'));
            if (colon <= 0 || text.mid(colon + 1).trimmed().isEmpty())
                ++invalidHeaders;
        }
    }
    if (invalidHeaders > 0)
        emit logMessage(QStringLiteral(
            "MCP 请求头有 %1 项格式无效（应为「名称: 值」，环境变量未设置时值会为空），已忽略。")
                            .arg(invalidHeaders));
    emit ready();
}

void McpHttpTransport::stop()
{
    // 规范：会话终止使用 HTTP DELETE。用独立网络管理器发送，避免关闭流程阻塞，
    // 也不受本对象生命周期影响；失败只记日志。
    terminateSession();
    sessionId_.clear();
    initialized_ = false;
    running_ = false;
    failed_ = true;
    notificationRetry_.stop();
    legacyEndpointTimer_.stop();
    const auto replies = sseStates_.keys();
    for (auto *reply : replies) {
        reply->disconnect(this);
        if (reply->isRunning()) reply->abort();
        reply->deleteLater();
    }
    sseStates_.clear();
    notificationReply_ = nullptr;
    legacyStreamReply_ = nullptr;
    legacyQueue_.clear();
    if (nam_) nam_->deleteLater();
    nam_ = nullptr;
}

QUrl McpHttpTransport::requestUrl() const
{
    return QUrl(url_);
}

QUrl McpHttpTransport::legacySseUrl() const
{
    QUrl sse = requestUrl();
    const QString path = sse.path();
    if (!path.endsWith(QStringLiteral("/sse"))) {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        const QString base = slash >= 0 ? path.left(slash) : QString();
        sse.setPath(base + QStringLiteral("/sse"));
        sse.setQuery(QString());
    }
    return sse;
}

QUrl McpHttpTransport::legacyMessagesUrlFallback() const
{
    QUrl messages = legacySseUrl();
    QString path = messages.path();
    if (path.endsWith(QStringLiteral("/sse")))
        path.chop(4); // 去掉 "/sse"
    messages.setPath(path + QStringLiteral("/messages"));
    messages.setQuery(QString());
    return messages;
}

QList<QPair<QByteArray, QByteArray>> McpHttpTransport::requestHeaders() const
{
    QList<QPair<QByteArray, QByteArray>> result;
    for (const auto &header : headers_) {
        for (const auto &line : expandEnvironment(header).split('\n')) {
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon <= 0) continue;
            const QString name = line.left(colon).trimmed();
            const QString value = line.mid(colon + 1).trimmed();
            if (name.isEmpty() || value.isEmpty()) continue;
            // 由传输层统一控制的头部不允许覆盖
            const QString lower = name.toLower();
            if (lower == QStringLiteral("content-type")
                || lower == QStringLiteral("accept")
                || lower == QStringLiteral("content-length")
                || lower == QStringLiteral("host")
                || lower == QStringLiteral("mcp-session-id"))
                continue;
            result.append({name.toUtf8(), value.toUtf8()});
        }
    }
    return result;
}

void McpHttpTransport::applyHeaders(QNetworkRequest &request, const QUrl &, bool sseAccept) const
{
    request.setRawHeader("Accept", sseAccept
        ? "application/json, text/event-stream" : "application/json");
    if (!sessionId_.isEmpty())
        request.setRawHeader("Mcp-Session-Id", sessionId_.toUtf8());
    for (const auto &header : requestHeaders())
        request.setRawHeader(header.first, header.second);
}

bool McpHttpTransport::send(const QJsonObject &message)
{
    if (!running_) return false;
    if (legacy_) {
        if (legacyMessagesUrl_.isEmpty()) {
            // 旧版 SSE 尚未收到 endpoint 事件：先缓存，建流后补发
            legacyQueue_.append(message);
            if (!legacyStreamReply_) openLegacyStream();
            return true;
        }
        return postMessage(message, legacyMessagesUrl_);
    }
    return postMessage(message, requestUrl());
}

void McpHttpTransport::startServerMessages()
{
    if (!running_ || legacy_ || notificationStarted_) return;
    notificationStarted_ = true;
    openNotificationStream();
}

void McpHttpTransport::markSessionEstablished()
{
    initialized_ = true;
    // 会话一旦建立，后续任何失败都不能再退回旧版 SSE
    legacyFallbackAllowed_ = false;
}

void McpHttpTransport::resetSession()
{
    sessionId_.clear();
    initialized_ = false;
    if (notificationReply_) {
        QNetworkReply *reply = notificationReply_;
        notificationReply_ = nullptr;
        reply->disconnect(this);
        if (reply->isRunning()) reply->abort();
        sseStates_.remove(reply);
        reply->deleteLater();
    }
    // 重新初始化后需要重新挂主动消息流（使用新会话头）
    notificationRetry_.stop();
    notificationStarted_ = false;
    notificationUnsupported_ = false;
    notificationAttempts_ = 0;
}

void McpHttpTransport::terminateSession()
{
    if (legacy_ || !initialized_ || sessionId_.isEmpty()) return;

    const QString targetUrl = url_;
    const QString session = sessionId_;
    const auto headers = requestHeaders();

    // 独立管理器：不阻塞 stop()，也不依赖本对象是否已被销毁
    auto *nam = new QNetworkAccessManager();
    QNetworkRequest request{QUrl(targetUrl)};
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Mcp-Session-Id", session.toUtf8());
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);

    QNetworkReply *reply = nam->deleteResource(request);
    if (!reply) {
        delete nam;
        qWarning().noquote() << QStringLiteral("MCP HTTP 会话终止请求发送失败：%1").arg(targetUrl);
        return;
    }
    emit logMessage(QStringLiteral("已向 MCP 端点发送会话终止请求（DELETE）。"));
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, nam, session] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status >= 400)
            qWarning().noquote() << QStringLiteral("MCP HTTP 会话终止失败（%1）：%2")
                                        .arg(session, httpStatusText(reply));
        reply->deleteLater();
        nam->deleteLater();
    });
}

bool McpHttpTransport::postMessage(const QJsonObject &message, const QUrl &url)
{
    if (!running_ || !nam_) return false;
    QNetworkRequest request(url);
    applyHeaders(request, url, true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = nam_->post(request, payload);
    if (!reply) return false;
    reply->setProperty("mcpPayload", payload);
    sseStates_.insert(reply, SseState{});
    QTimer::singleShot(kPostTimeoutMs, reply, [reply] {
        if (reply->isRunning()) reply->abort(); // 客户端 10 秒超时兜底，这里只回收悬挂连接
    });
    // SSE 响应在 readyRead 就会派发给客户端，finished 里再保存会话头已经太晚：
    // 客户端收到 initialize 结果后立刻发出的 initialized/tools/list/GET 会缺少会话头。
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        captureSession(reply);
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        captureSession(reply);
        const QString contentType =
            reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (contentType.contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive))
            feedSse(reply);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handlePostFinished(reply);
    });
    return true;
}

void McpHttpTransport::handlePostFinished(QNetworkReply *reply)
{
    const QByteArray payload = reply->property("mcpPayload").toByteArray();
    const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    const bool isSse =
        contentType.contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive);
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // 先保存会话头：SSE 响应会在下面同步派发给客户端，
    // 客户端可能立即发出 notifications/initialized、tools/list 与主动消息流 GET，
    // 这些请求都必须带上 Mcp-Session-Id（否则服务端返回 400 Missing session ID）。
    captureSession(reply);
    QByteArray body;
    if (isSse) {
        feedSse(reply);
        finishSse(reply);
    } else {
        body = reply->readAll();
    }

    if (!running_) {
        cleanupReply(reply);
        return;
    }
    // 会话已建立后收到 404：按规范视为会话失效，清空会话并重新 initialize。
    // 绝不能把整条通道回退到旧版 SSE（否则后续请求会在未握手的通道上发送）。
    if (!legacy_ && initialized_ && status == 404) {
        emit logMessage(QStringLiteral("MCP HTTP 会话已失效（404），准备重新初始化。"));
        synthesizeError(payload, QStringLiteral("MCP 会话已失效，正在重新初始化"));
        cleanupReply(reply);
        emit sessionExpired();
        return;
    }
    // 仅当尚未建立会话、且失败的是初次 initialize 时，才允许回退旧版 HTTP+SSE
    if (!legacy_ && !legacyAttempted_ && !initialized_ && legacyFallbackAllowed_
        && needsLegacyFallback(status) && isInitializeRequest(payload)) {
        emit logMessage(QStringLiteral("端点不支持 Streamable HTTP（%1），回退旧版 HTTP+SSE。")
                            .arg(httpStatusText(reply)));
        fallbackToLegacy(QJsonDocument::fromJson(payload).object());
        cleanupReply(reply);
        return;
    }

    const QNetworkReply::NetworkError error = reply->error();
    // 先按 HTTP 状态码处理：4xx/5xx 视为单次请求失败，把错误回给模型，不判整条链路死亡
    if (status >= 400) {
        const QString snippet = QString::fromUtf8(body).trimmed().left(kMaxBodyLog);
        emit logMessage(QStringLiteral("MCP HTTP 请求返回 %1%2")
                            .arg(httpStatusText(reply))
                            .arg(snippet.isEmpty() ? QString()
                                                   : QStringLiteral("：") + snippet));
        synthesizeError(payload, httpStatusText(reply));
        cleanupReply(reply);
        return;
    }
    if (error != QNetworkReply::NoError) {
        // 服务端保持 POST 流而由本地超时中止：已完成响应，不算失败
        if (!(error == QNetworkReply::OperationCanceledError && status >= 200 && status < 300)) {
            emit logMessage(QStringLiteral("MCP HTTP 请求失败：%1").arg(httpStatusText(reply)));
            synthesizeError(payload, QStringLiteral("MCP HTTP 请求失败：%1").arg(reply->errorString()));
            reportFailure(QStringLiteral("HTTP 请求失败：%1").arg(reply->errorString()));
        }
        cleanupReply(reply);
        return;
    }
    if (!isSse && !body.trimmed().isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(body);
        if (document.isObject() || document.isArray())
            handlePayload(QString::fromUtf8(body));
        else
            emit logMessage(QStringLiteral("MCP HTTP 响应不是 JSON，已忽略。"));
    }
    cleanupReply(reply);
}

void McpHttpTransport::cleanupReply(QNetworkReply *reply)
{
    if (!reply) return;
    sseStates_.remove(reply);
    reply->deleteLater();
    // 主消息流断开只在网络层错误时判定链路故障，正常 200 结束不受影响
}

void McpHttpTransport::handleNotificationFinished(QNetworkReply *reply)
{
    notificationReply_ = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool running = running_;
    cleanupReply(reply);
    if (!running) return;
    if (status == 404 || status == 405 || status == 406 || status == 415 || status == 501) {
        if (!notificationUnsupported_)
            emit logMessage(QStringLiteral(
                "服务端不支持 GET 主动消息流（%1），工具列表热插拔仅依赖 POST 响应流。")
                .arg(QString::number(status)));
        notificationUnsupported_ = true;
        return;
    }
    ++notificationAttempts_;
    if (notificationAttempts_ <= 3 || notificationAttempts_ % 12 == 0)
        emit logMessage(QStringLiteral("MCP HTTP 主动消息流断开，5 秒后重连（第 %1 次）。")
                            .arg(notificationAttempts_));
    notificationRetry_.start(int(kReconnectMs));
}

void McpHttpTransport::handleLegacyStreamFinished(QNetworkReply *reply)
{
    const QString status = httpStatusText(reply);
    const bool running = running_;
    legacyStreamReply_ = nullptr;
    cleanupReply(reply);
    if (!running) return;
    reportFailure(QStringLiteral("旧版 SSE 流断开（%1）").arg(status));
}

void McpHttpTransport::feedSse(QNetworkReply *reply)
{
    auto it = sseStates_.find(reply);
    if (it == sseStates_.end()) return;
    SseState &state = it.value();
    state.buffer.append(reply->readAll());
    while (true) {
        const int newline = state.buffer.indexOf('\n');
        if (newline < 0) break;
        QByteArray line = state.buffer.left(newline);
        state.buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) {
            dispatchSseEvent(reply, state);
            continue;
        }
        if (line.startsWith(':')) continue; // 注释行
        const int colon = line.indexOf(':');
        const QString field =
            QString::fromUtf8(colon >= 0 ? line.left(colon) : line).trimmed();
        QByteArray value = colon >= 0 ? line.mid(colon + 1) : QByteArray();
        if (value.startsWith(' ')) value.remove(0, 1);
        if (field == QStringLiteral("event"))
            state.event = QString::fromUtf8(value).trimmed();
        else if (field == QStringLiteral("data")) {
            if (!state.data.isEmpty()) state.data += QLatin1Char('\n');
            state.data += QString::fromUtf8(value);
        }
        // id/retry 字段对 MCP 无意义，忽略
    }
}

void McpHttpTransport::finishSse(QNetworkReply *reply)
{
    auto it = sseStates_.find(reply);
    if (it == sseStates_.end()) return;
    SseState &state = it.value();
    // 流结束但没有空行分隔：把残余数据当作一个事件处理
    if (!state.buffer.isEmpty()) {
        const QString text = QString::fromUtf8(state.buffer).trimmed();
        state.buffer.clear();
        if (!state.data.isEmpty()) state.data += QLatin1Char('\n') + text;
        else if (text.startsWith(QStringLiteral("data:")))
            state.data = text.mid(5).trimmed();
    }
    dispatchSseEvent(reply, state);
}

void McpHttpTransport::dispatchSseEvent(QNetworkReply *reply, SseState &state)
{
    const QString event = state.event;
    const QString data = state.data;
    state.event.clear();
    state.data.clear();
    if (data.trimmed().isEmpty()) return;
    if (event == QStringLiteral("endpoint")) {
        const QUrl endpoint(data);
        QUrl resolved = endpoint;
        if (endpoint.isRelative())
            resolved = legacySseUrl().resolved(endpoint);
        if (!resolved.isValid() || resolved.isEmpty()) {
            emit logMessage(QStringLiteral("旧版 SSE endpoint 事件地址无效：%1").arg(data));
            return;
        }
        legacyMessagesUrl_ = resolved;
        legacyEndpointTimer_.stop();
        emit logMessage(QStringLiteral("旧版 SSE 消息端点：%1").arg(resolved.toString()));
        flushLegacyQueue();
        return;
    }
    if (event.isEmpty() || event == QStringLiteral("message"))
        handlePayload(data);
    Q_UNUSED(reply);
}

void McpHttpTransport::handlePayload(const QString &payload)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        emit logMessage(QStringLiteral("MCP HTTP 收到无法解析的消息帧，已忽略。"));
        return;
    }
    if (document.isObject()) {
        emit messageReceived(document.object());
    } else if (document.isArray()) {
        for (const auto &item : document.array())
            if (item.isObject()) emit messageReceived(item.toObject());
    }
}

void McpHttpTransport::captureSession(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // 4xx 响应（如 Missing session ID）也可能带一个新建但未初始化的会话，
    // 直接采纳会覆盖可用会话，导致后续请求全部 400。
    if (status >= 400) return;
    const QByteArray session = reply->rawHeader("Mcp-Session-Id");
    if (session.isEmpty()) return;
    const QString value = QString::fromUtf8(session).trimmed();
    if (value.isEmpty() || value == sessionId_) return;
    sessionId_ = value;
    emit logMessage(QStringLiteral("MCP HTTP 会话：%1").arg(sessionId_));
}

void McpHttpTransport::synthesizeError(const QByteArray &messageJson, const QString &reason)
{
    const QJsonObject request = QJsonDocument::fromJson(messageJson).object();
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (!id.isDouble()) return; // 通知没有 id，无对应响应
    QJsonObject response{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), -32000},
            {QStringLiteral("message"), reason}}}};
    emit messageReceived(response);
}

void McpHttpTransport::openNotificationStream()
{
    if (!running_ || legacy_ || notificationUnsupported_ || !nam_) return;
    if (notificationReply_) return;
    const QUrl target = requestUrl();
    QNetworkRequest request(target);
    applyHeaders(request, target, true);
    QNetworkReply *reply = nam_->get(request);
    if (!reply) return;
    notificationReply_ = reply;
    sseStates_.insert(reply, SseState{});
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { feedSse(reply); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { handleNotificationFinished(reply); });
}

void McpHttpTransport::openLegacyStream()
{
    if (!running_ || !nam_) return;
    if (legacyStreamReply_) return;
    const QUrl sse = legacySseUrl();
    QNetworkRequest request(sse);
    applyHeaders(request, sse, true);
    QNetworkReply *reply = nam_->get(request);
    if (!reply) return;
    legacyStreamReply_ = reply;
    sseStates_.insert(reply, SseState{});
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { feedSse(reply); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply] { handleLegacyStreamFinished(reply); });
    // 部分旧版服务端不会下发 endpoint 事件，超时后按约定路径兜底
    legacyEndpointTimer_.start(int(kLegacyEndpointMs));
    emit logMessage(QStringLiteral("旧版 SSE 建流：%1").arg(sse.toString()));
}

void McpHttpTransport::fallbackToLegacy(const QJsonObject &message)
{
    if (legacy_ || !running_) return;
    legacy_ = true;
    legacyAttempted_ = true;
    sessionId_.clear();
    if (!message.isEmpty()) legacyQueue_.append(message);
    openLegacyStream();
}

void McpHttpTransport::flushLegacyQueue()
{
    if (!running_ || legacyMessagesUrl_.isEmpty()) return;
    const auto queue = legacyQueue_;
    legacyQueue_.clear();
    for (const auto &message : queue)
        postMessage(message, legacyMessagesUrl_);
}

void McpHttpTransport::reportFailure(const QString &reason)
{
    if (failed_) return;
    failed_ = true;
    running_ = false;
    emit failed(reason);
}

} // namespace qwenbot
