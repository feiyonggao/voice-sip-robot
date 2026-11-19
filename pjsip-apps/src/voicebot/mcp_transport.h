#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QProcess;

namespace qwenbot {

// MCP 传输层抽象：只负责把 JSON-RPC 消息送出去、把收到的消息抛上来，
// 以及报告通道可用/断开。initialize 握手、tools/list、调用超时与重连
// 退避统一由 McpClient 处理，stdio 与 HTTP 共用同一套上层逻辑。
class McpTransport : public QObject {
    Q_OBJECT
public:
    explicit McpTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~McpTransport() override = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool send(const QJsonObject &message) = 0;
    virtual QString protocolVersion() const = 0;
    // 握手完成后调用：HTTP 可借此挂服务端主动消息流（stdio 无需实现）
    virtual void startServerMessages() {}
    // Streamable HTTP 会话生命周期扩展点（stdio 默认无操作）
    virtual void markSessionEstablished() {}
    virtual void resetSession() {}
    virtual void setLegacyFallbackAllowed(bool) {}
    virtual bool isLegacy() const { return false; }

signals:
    void ready();                                     // 通道可用，可以发 initialize
    void messageReceived(const QJsonObject &message); // 收到一条完整 JSON-RPC 消息
    void failed(const QString &reason);               // 通道断开/启动失败（触发重连）
    void logMessage(const QString &text);
    void sessionExpired();                            // HTTP 会话失效（需重新 initialize）
};

// stdio：拉起子进程，换行分隔 JSON 帧，单行上限 1MB。
class McpStdioTransport : public McpTransport {
    Q_OBJECT
public:
    explicit McpStdioTransport(const QString &command, QObject *parent = nullptr);
    ~McpStdioTransport() override;

    void start() override;
    void stop() override;
    bool isRunning() const override;
    bool send(const QJsonObject &message) override;
    QString protocolVersion() const override { return QStringLiteral("2024-11-05"); }

private:
    void readStdout();
    void reportFailure(const QString &reason);

    QString command_;
    QProcess *process_ = nullptr;
    QByteArray readBuf_;
    bool dead_ = false;
};

// HTTP：优先 Streamable HTTP（单端点 POST，响应 JSON 或 SSE 流，Mcp-Session-Id
// 会话头，可选 GET 长连接收服务端主动消息）；POST 明确不支持时回退旧版
// HTTP+SSE（GET /sse 建流 + 服务端下发 endpoint 事件指定 /messages 发请求）。
class McpHttpTransport : public McpTransport {
    Q_OBJECT
public:
    McpHttpTransport(const QString &url, const QStringList &headers,
                     QObject *parent = nullptr);
    ~McpHttpTransport() override;

    void start() override;
    void stop() override;
    bool isRunning() const override { return running_; }
    bool send(const QJsonObject &message) override;
    QString protocolVersion() const override { return QStringLiteral("2025-03-26"); }
    void startServerMessages() override;
    void markSessionEstablished() override;
    void resetSession() override;
    void setLegacyFallbackAllowed(bool allowed) override { legacyFallbackAllowed_ = allowed; }
    bool isLegacy() const override { return legacy_; }

private:
    struct SseState {
        QByteArray buffer;
        QString event;
        QString data;
    };

    QUrl requestUrl() const;
    QUrl legacySseUrl() const;
    QUrl legacyMessagesUrlFallback() const;
    QList<QPair<QByteArray, QByteArray>> requestHeaders() const;
    void applyHeaders(QNetworkRequest &request, const QUrl &url, bool sseAccept) const;
    bool postMessage(const QJsonObject &message, const QUrl &url);
    void handlePostFinished(QNetworkReply *reply);
    void handleNotificationFinished(QNetworkReply *reply);
    void handleLegacyStreamFinished(QNetworkReply *reply);
    void feedSse(QNetworkReply *reply);
    void finishSse(QNetworkReply *reply);
    void dispatchSseEvent(QNetworkReply *reply, SseState &state);
    void handlePayload(const QString &payload);
    void captureSession(QNetworkReply *reply);
    void synthesizeError(const QByteArray &messageJson, const QString &reason);
    void cleanupReply(QNetworkReply *reply);
    void openNotificationStream();
    void openLegacyStream();
    void fallbackToLegacy(const QJsonObject &message);
    void flushLegacyQueue();
    void terminateSession();
    void reportFailure(const QString &reason);

    QString url_;
    QStringList headers_;
    QNetworkAccessManager *nam_ = nullptr;
    QHash<QNetworkReply *, SseState> sseStates_;
    QList<QJsonObject> legacyQueue_;
    QTimer notificationRetry_;
    QTimer legacyEndpointTimer_;
    QNetworkReply *notificationReply_ = nullptr;
    QNetworkReply *legacyStreamReply_ = nullptr;
    QString sessionId_;
    QUrl legacyMessagesUrl_;
    bool running_ = false;
    bool legacy_ = false;
    bool legacyAttempted_ = false;
    bool legacyFallbackAllowed_ = true; // 仅首次握手（尚未建立会话）允许回退旧版 SSE
    bool initialized_ = false;          // initialize 已完成（由 McpClient 标记）
    bool notificationStarted_ = false;
    bool notificationUnsupported_ = false;
    int notificationAttempts_ = 0;
    bool failed_ = false;
};

} // namespace qwenbot
