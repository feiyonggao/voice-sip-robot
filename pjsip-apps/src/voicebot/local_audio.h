#pragma once

#include "bot_config.h"
#include "qwen_session.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <memory>

class QAudioSink;
class QAudioSource;
class QIODevice;

namespace qwenbot {

class QwenSession;

// 本地测试对话：麦克风 16k 采集 → QwenSession，模型 24k 输出 → 本地扬声器播放。
// 不经过 SIP/RTP，也不写通话记录。所有方法与信号都在所属线程事件循环上运行。
class LocalAudioSession : public QObject {
    Q_OBJECT
public:
    explicit LocalAudioSession(QObject *parent = nullptr);
    ~LocalAudioSession() override;

    void stop();                         // 幂等，可重复调用
    bool active() const { return static_cast<bool>(session_); }
    QString lastError() const { return lastError_; }

public slots:
    // 供工作线程排队调用；失败通过 failed 信号通知，成功通过 activeChanged(true)。
    // tools：本次对话锁定的工具集快照（主线程取自 ToolRegistry，跨线程按值传递）。
    bool start(const BotConfig &config, const QJsonArray &tools = QJsonArray());

signals:
    void activeChanged(bool active);
    void transcript(const QString &speaker, const QString &text);
    void failed(const QString &message);
    void statusChanged(const QString &status);
    void logMessage(const QString &text); // 设备/会话诊断日志
    // 模型请求结束（达轮次/无声上限）且挂机话术播放排空后发出
    void finished(const QString &reason);
    void turnCompleted(int count);
    // 累计模型 token 用量（QwenSession usageUpdated 转发，用于本地对话记录）
    void usageUpdated(const qwenbot::ModelUsage &usage);
    // 模型请求调用 MCP 工具：由主线程路由到 ToolRegistry，结果回调 completeToolCall
    void toolCallRequested(const QString &callId, const QString &name,
                           const QString &arguments);

public slots:
    // 主线程把 MCP 工具结果送回（排队到音频工作线程）
    void completeToolCall(const QString &callId, const QString &outputJson);

private slots:
    void pumpUplink();
    void monitorPlayback(); // 看门狗：队列有数据但后端停止拉取时重启扬声器
    void checkFinished();
    void primeSink();

private:
    void playAudio(const QByteArray &pcm24k);
    void handleInterrupted();
    void beginFinish(const QString &reason);
    void hardRestartSink(); // stop()+start() 恢复停摆的扬声器
    bool modelSpeaking() const; // 队列有待播音频或设备仍在播放/尾音冷却
    bool playbackIdle() const;  // 播放已静止：FIFO 空且超过尾音时长无拉取
    qint64 queueBytes() const;  // FIFO 当前字节数（线程安全）
    qint64 pullFromQueue(char *data, qint64 maxSize); // 音频后端线程调用

    std::unique_ptr<QwenSession> session_;
    QAudioSource *source_ = nullptr;
    QIODevice *srcDev_ = nullptr;
    QAudioSink *sink_ = nullptr;
    QIODevice *pullDev_ = nullptr; // 只读拉取设备：后端线程经 readData() 取数据
    // 必须以 this 为父对象：本对象在 GUI 线程构造后 moveToThread 到音频工作线程，
    // 无父对象的成员 QTimer 会留在 GUI 线程，在工作线程 start() 会被 Qt 静默拒绝
    // （"Timers cannot be started from another thread"），上行采集/看门狗将全部失效。
    QTimer uplinkTimer_{this};
    QTimer monitorTimer_{this};   // 100ms 看门狗
    QTimer finishTimer_{this};
    QTimer prefillTimer_{this};   // 首包到达后限时启动扬声器，保证短响应也能开播
    QElapsedTimer finishDeadline_;
    QElapsedTimer playbackClock_;
    QByteArray uplinkBuf_;
    mutable QMutex queueMutex_;
    QByteArray pcmQueue_;   // 下行 FIFO：后端线程拉取播放，本线程只入队
    int chunkBytes_ = 640;
    int prefillBytes_ = 19200; // 约 400ms@24kHz16bit
    bool finishing_ = false;
    bool sinkPrimed_ = false;  // 扬声器是否已启动
    bool lastSpeakingLogged_ = false; // 半双工状态切换日志去重
    bool allowBargeIn_ = true; // 允许抢话：播放期间也上送麦克风（全双工），否则半双工静默
    std::atomic<bool> sinkHadData_{false};
    std::atomic<qint64> lastPullMs_{-10000}; // 后端最近一次拉取音频的时刻
    qint64 lastRestartMs_ = -10000;  // 最近一次重启扬声器的时刻，防止重启风暴
    QString finishReason_;
    QString lastError_;
};

} // namespace qwenbot
