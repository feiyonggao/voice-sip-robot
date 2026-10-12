#pragma once

#include "bot_config.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <memory>

namespace qwenbot {

// 模型 token 用量（来自 response.done 的 usage 字段，跨多次响应累计）
struct ModelUsage {
    bool valid = false;            // 服务端是否上报过 usage
    qint64 inputTokens = 0;        // 输入总 token（含音频与文本）
    qint64 outputTokens = 0;       // 输出总 token
    qint64 inputAudioTokens = 0;   // 输入音频部分
    qint64 inputTextTokens = 0;    // 输入文本部分
    qint64 cachedTokens = 0;       // 输入命中缓存部分
    qint64 outputAudioTokens = 0;  // 输出音频部分
    qint64 outputTextTokens = 0;   // 输出文本部分

    void accumulate(const QJsonObject &usage); // 解析单次 usage 并累计
    QJsonObject toJson() const;                // 嵌入通话记录 JSON
};

// All methods and signals run on the owning Qt event-loop thread.
class QwenSession : public QObject {
    Q_OBJECT
public:
    explicit QwenSession(QObject *parent = nullptr);
    ~QwenSession() override;
    // tools：本次会话锁定的工具集（由调用方从 ToolRegistry 取快照，避免跨线程读注册表）；
    // 为空时兜底注册内置 hang_up。hang_up 之外的 function_call 经 toolCallRequested 上抛。
    void open(const BotConfig &config, const QJsonArray &tools = QJsonArray());
    void close();
    void appendAudio(const QByteArray &pcm16k);
    void greet();
    // outputEmpty：音频宿输出缓冲是否已排空；未排空时延迟启动无声计时
    void checkDrainAndStartSilence(bool outputEmpty);
    // 回传非内置工具的执行结果并请求模型继续作答；会话收尾后到达的结果被丢弃
    void completeToolCall(const QString &callId, const QString &outputJson);

signals:
    void ready();
    void audioReceived(const QByteArray &pcm24k);
    void interrupted();
    void transcript(const QString &speaker, const QString &text);
    void failed(const QString &message);
    void statusChanged(const QString &status);
    void endRequested(const QString &reason);
    void turnCompleted(int turnCount);
    // 每次响应完成后上报累计 token 用量（用于通话记录与界面展示）
    void usageUpdated(const qwenbot::ModelUsage &usage);
    void toolCallRequested(const QString &callId, const QString &name, const QString &arguments);
    void diagLog(const QString &text); // [debug-point] 服务端事件诊断

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qwenbot

// LocalAudioSession 在音频线程转发 usageUpdated，跨线程排队信号需要 metatype
Q_DECLARE_METATYPE(qwenbot::ModelUsage)
