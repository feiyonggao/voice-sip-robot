#pragma once

#include "bot_config.h"
#include "qwen_session.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QFile>
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

class AecProcessor;
class QwenSession;

// 本地测试对话：麦克风 16k 采集 → QwenSession，模型 24k 输出 → 本地扬声器播放。
// 不经过 SIP/RTP，也不写通话记录。所有方法与信号都在所属线程事件循环上运行。
//
// 播放采用拉取（pull）模式：把自定义 QIODevice 交给 sink->start(pullDev)，
// 音频后端线程主动 readData() 取数据，队列空时静音补齐。Qt6 FFmpeg 后端的
// 推/拉模式均有停摆缺陷；WASAPI 后端（QT_MEDIA_BACKEND=windows）原生为
// 事件驱动拉取，与拉取模式匹配，实测稳定。停摆/打断统一销毁重建 sink 恢复。
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
    // 输入设备疑似回环/虚拟设备（会采集系统播放声），界面需要醒目提示
    void deviceWarning(const QString &text);

public slots:
    // 主线程把 MCP 工具结果送回（排队到音频工作线程）
    void completeToolCall(const QString &callId, const QString &outputJson);

private slots:
    void pumpUplink();    // 20ms：麦克风 → AEC → 上行
    void pumpDownlink();  // 20ms：预缓冲起播 + 停摆看门狗（播放由后端线程拉取）
    void monitorPlayback(); // 100ms：播放排空后驱动无声计时/挂机策略
    void checkFinished();

private:
    void playAudio(const QByteArray &pcm24k);
    void handleInterrupted();
    QAudioSink *createSink(const QAudioDevice &device, const QAudioFormat &format);
    void beginFinish(const QString &reason);
    bool modelSpeaking() const; // 队列有待播音频或设备缓冲仍在放音/尾音冷却
    bool playbackIdle() const;  // 播放已静止：FIFO 空且超过尾音时长无拉取
    qint64 queueBytes() const;  // FIFO 当前字节数（线程安全）
    // 音频后端线程调用：从 FIFO 取数据（可能不足 maxSize，调用方静音补齐）
    qint64 pullFromQueue(char *data, qint64 maxSize);

    std::unique_ptr<QwenSession> session_;
    QAudioSource *source_ = nullptr;
    QIODevice *srcDev_ = nullptr;
    QAudioSink *sink_ = nullptr;
    QIODevice *pullDev_ = nullptr; // 拉取模式播放设备（成员常驻，sink 重建不影响）
    bool sinkStarted_ = false;     // 拉取会话是否已 start
    int prefillBytes_ = 7200;      // 起播预缓冲（150ms @24k）：吸收首个网络抖动
    QAudioDevice outputDevice_;   // 重建 QAudioSink 需复用同一输出设备/格式
    QAudioFormat outputFormat_;
    // 必须以 this 为父对象：本对象在 GUI 线程构造后 moveToThread 到音频工作线程，
    // 无父对象的成员 QTimer 会留在 GUI 线程，在工作线程 start() 会被 Qt 静默拒绝
    // （"Timers cannot be started from another thread"），上行采集/看门狗将全部失效。
    QTimer uplinkTimer_{this};
    QTimer downlinkTimer_{this};  // 20ms 推模式写入
    QTimer monitorTimer_{this};   // 100ms 看门狗
    QTimer finishTimer_{this};
    QElapsedTimer finishDeadline_;
    QElapsedTimer playbackClock_;
    QByteArray uplinkBuf_;
    mutable QMutex queueMutex_;
    QByteArray pcmQueue_;   // 下行 FIFO：pumpDownlink 按设备可用空间写入
    int chunkBytes_ = 640;
    bool finishing_ = false;
    bool lastSpeakingLogged_ = false; // 半双工状态切换日志去重
    qint64 uplinkBytesDuringPlay_ = 0; // 本轮播报期间实际发送的上行字节数（全双工取证）
    bool allowBargeIn_ = true; // 与 SIP 语义一致：勾选=全双工（始终上行），未勾选=半双工（播放期间静音）
    // 客户端回声消除：仅全双工时启用。参考信号 = 写入扬声器的播放 PCM，
    // 参考消费按麦克风节拍推进，参考缓冲水位自动跟踪设备缓冲深度。
    std::unique_ptr<AecProcessor> aec_;
    QByteArray aecReferenceMirror_;
    mutable QMutex aecRefMutex_;
    QElapsedTimer aecStatsClock_;
    std::atomic<bool> sinkHadData_{false};
    std::atomic<qint64> lastPullMs_{-10000}; // 最近一次后端线程拉走音频的时刻（含静音补齐）
    qint64 lastInterruptMs_ = -10000; // 最近一次打断时刻：残余回声防护窗口计时基准
    // 诊断录音：上行原始（AEC 前）与实际上行（AEC 后）各录一份 WAV，用于定位识别差
    QFile debugNearFile_;
    QFile debugUplinkFile_;
    qint64 debugNearBytes_ = 0;
    qint64 debugUplinkBytes_ = 0;
    QString finishReason_;
    QString lastError_;
};

} // namespace qwenbot
