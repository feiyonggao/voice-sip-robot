#pragma once

#include "mcp_plugin.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTimer>

namespace qwenbot {

class McpTransport;

// 单个 MCP 插件客户端：传输层由 McpPluginSpec 决定（stdio 子进程或 HTTP 服务），
// 本类只负责 initialize 握手、tools/list 发现、tools/call 转发与超时/重连。
// 断线自动重连（固定 5 秒退避），支持 notifications/tools/list_changed 热插拔。
class McpClient : public QObject {
    Q_OBJECT
public:
    struct ToolDesc {
        QString name;
        QString description;
        QJsonObject parameters; // inputSchema
    };

    explicit McpClient(const McpPluginSpec &spec, QObject *parent = nullptr);
    ~McpClient() override;

    void start();    // 启动/重启传输通道并握手
    void shutdown(); // 停止通道，不再重连
    bool isOnline() const { return online_; }
    QList<ToolDesc> tools() const { return tools_; }

    // 转发一次工具调用，返回请求 ID（<=0 表示当前不可调用）。
    // 结果（含超时错误）经 toolFinished 回传，每个请求 ID 只回传一次。
    int callTool(const QString &name, const QJsonObject &arguments);

signals:
    void onlineChanged(bool online, const QString &reason);
    void toolsUpdated();
    void toolFinished(int requestId, const QString &name, const QString &outputJson);
    void logMessage(const QString &text);

private:
    struct PendingCall {
        QString name;
        qint64 deadlineMs = 0;
    };

    bool send(const QJsonObject &event);
    void sendInitialize();
    void sendToolsList();
    void handleMessage(const QJsonObject &message);
    void handleResponse(int requestId, const QJsonObject &result, const QJsonObject &error);
    void handleTransportFailure(const QString &reason);
    void handleSessionExpired();
    void retryInitialize();
    void failPendingCalls(const QString &error);
    void cleanupTransport();
    void sweepTimeouts();
    QString outputFromResult(const QJsonObject &result) const;
    QString errorOutput(const QString &message) const;

    McpPluginSpec spec_;
    QString label_;
    McpTransport *transport_ = nullptr;
    QList<ToolDesc> tools_;
    QHash<int, PendingCall> calls_;
    QElapsedTimer clock_;
    QTimer deadlineTimer_;      // 500ms 扫描握手/调用超时
    QTimer reconnectTimer_;
    QTimer sessionRetryTimer_;  // 会话失效后的带退避重新 initialize
    qint64 lastInitializeMs_ = 0; // 本次 initialize 的发出时间（握手超时基准）
    int initRequestId_ = 0;
    int discoveryRequestId_ = 0;
    int nextRequestId_ = 1;
    int sessionRecoveryAttempts_ = 0;
    bool online_ = false;
    bool shuttingDown_ = false;
    bool sessionRecoveryActive_ = false;
    bool streamableSessionSeen_ = false; // 本客户端曾成功建立过 Streamable 会话
};

} // namespace qwenbot
