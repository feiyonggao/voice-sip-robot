#pragma once

#include "bot_config.h"
#include "qwen_session.h"
#include <QObject>
#include <memory>

namespace qwenbot {

class ToolRegistry;

class BotEngine : public QObject {
    Q_OBJECT
public:
    explicit BotEngine(QObject *parent = nullptr);
    ~BotEngine() override;
    bool isRunning() const;
    bool start(const BotConfig &config);
    void stop();
    void answer(int callId);
    void hangup(int callId);
    // 注入工具注册表（主线程对象；每次通话打开时取工具快照并路由 MCP 工具调用）
    void setToolRegistry(ToolRegistry *registry);

signals:
    void runningChanged(bool running);
    void statusChanged(const QString &status);
    void callChanged(int callId, const QString &peer, const QString &state);
    void callEnded(int callId, int sipStatusCode, const QString &hangupBy, const QString &hangupReason);
    void callTurnsUpdated(int callId, int turns);
    // 模型 token 用量更新（response.done 后累计值，供界面实时展示）
    void callUsageUpdated(int callId, const qwenbot::ModelUsage &usage);
    void transcript(int callId, const QString &speaker, const QString &text);
    void logMessage(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qwenbot
