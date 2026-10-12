#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTimer>

class QProcess;

namespace qwenbot {

// 单个 MCP stdio 插件客户端：拉起子进程，完成 initialize 握手与 tools/list
// 工具发现，转发 tools/call 调用。断线自动重连（固定 5 秒退避），
// 支持 notifications/tools/list_changed 热插拔。
// 每条 JSON-RPC 消息占一行（MCP stdio 帧格式），单行上限 1MB。
class McpClient : public QObject {
    Q_OBJECT
public:
    struct ToolDesc {
        QString name;
        QString description;
        QJsonObject parameters; // inputSchema
    };

    explicit McpClient(const QString &label, const QString &command,
                       QObject *parent = nullptr);
    ~McpClient() override;

    void start();    // 启动/重启子进程并握手
    void shutdown(); // 停止子进程，不再重连
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
    void readStdout();
    void handleLine(const QByteArray &line);
    void handleResponse(int requestId, const QJsonObject &result, const QJsonObject &error);
    void handleProcessExit(const QString &reason);
    void failPendingCalls(const QString &error);
    void cleanupProcess();
    void sweepTimeouts();
    QString outputFromResult(const QJsonObject &result) const;
    QString errorOutput(const QString &message) const;

    QString label_;
    QString command_;
    QProcess *process_ = nullptr;
    QByteArray readBuf_;
    QList<ToolDesc> tools_;
    QHash<int, PendingCall> calls_;
    QElapsedTimer clock_;
    QTimer deadlineTimer_; // 500ms 扫描握手/调用超时
    QTimer reconnectTimer_;
    qint64 startedMs_ = 0;      // 当前子进程启动时刻
    int initRequestId_ = 0;
    int discoveryRequestId_ = 0;
    int nextRequestId_ = 1;
    bool online_ = false;
    bool shuttingDown_ = false;
};

} // namespace qwenbot
