#include "local_audio.h"
#include "aec_processor.h"
#include "audio_device_util.h"
#include "qwen_session.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutexLocker>
#include <cstring>
#include <functional>

namespace qwenbot {
namespace {
// 设备缓冲 150ms：同时是起播预缓冲深度。拉取模式下缓冲深度决定打断时残留在
// 设备里的尾音长度；150ms 足以吸收后端抖动，且远小于 Speex MDF 尾长（600ms），
// AEC 参考对齐可收敛。
constexpr qint64 DownlinkBufferUsec = 150000;
constexpr int DownlinkPumpMs = 20;            // 起播检查/停摆看门狗节拍
constexpr qint64 FinishTimeoutMs = 8000;      // 排空等待兜底，防止状态误判卡死
constexpr int MaxQueueBytes = 4 * 1024 * 1024; // FIFO 上限约 83 秒音频，超限视为异常
constexpr qint64 PlaybackSettleMs = 300;       // 最后一次拉取后设备缓冲仍在放音的时长（150ms 缓冲 + 余量）
constexpr qint64 StallRestartMs = 700;         // 队列有数据但 700ms 无拉取 → 销毁重建扬声器
constexpr qint64 TailEchoGuardMs = 250;        // 打断后残余回声防护窗口：尾音/混响不上行

// 客户端 AEC 参数：内部统一 16 kHz / 20 ms 帧；尾长 600 ms 覆盖声学路径、
// 设备/后端抖动，以及参考时间轴与真实播放之间的对齐误差。
// 离线 harness 实测：100–500 ms 回声延迟下稳态 ERLE 约 24.6 dB，单核 CPU 0.6%。
constexpr int AecFrameSamples = 320;           // 20 ms @ 16 kHz
constexpr int AecTailMs = 600;
constexpr qint64 AecStatsIntervalMs = 2000;    // 诊断日志间隔，避免刷屏
constexpr int AecRefMirrorMaxBytes = 24000 * 2 * 2; // 参考暂存上限 2s@24k

QAudioFormat monoPcm(int sampleRate)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

// 诊断录音：写标准 44 字节 WAV 头（16kHz 单声道 int16），数据长度先占位，结束时回填
void writeWavHeader(QFile &file, qint64 dataBytes)
{
    auto putU32 = [&](quint32 v) {
        const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
        file.write(b, 4);
    };
    auto putU16 = [&](quint16 v) {
        const char b[2] = {char(v), char(v >> 8)};
        file.write(b, 2);
    };
    file.write("RIFF", 4); putU32(quint32(36 + dataBytes)); file.write("WAVE", 4);
    file.write("fmt ", 4); putU32(16); putU16(1); putU16(1);
    putU32(16000); putU32(32000); putU16(2); putU16(16);
    file.write("data", 4); putU32(quint32(dataBytes));
}

void finalizeWav(QFile &file, qint64 dataBytes)
{
    if (!file.isOpen())
        return;
    file.seek(0);
    writeWavHeader(file, dataBytes);
    file.close();
}

// 拉取模式播放设备：音频后端线程需要数据时主动调 readData() 来取。
// 本线程绝不调用 write()——推模式在 Qt6 FFmpeg/WASAPI 后端实测均不可靠
// （FFmpeg 假死不再消费；WASAPI 起播后 bytesFree 恒 0 写不进去）。
class PullDevice : public QIODevice {
public:
    explicit PullDevice(QObject *parent) : QIODevice(parent)
    {
        open(QIODevice::ReadOnly);
    }
    bool isSequential() const override { return true; }
    // 始终报告有数据：拉取模式依赖 bytesAvailable/readyRead 继续拉取，
    // 若队列空时返回 0，后端会进入 IdleState 且可能不再主动拉取。
    qint64 bytesAvailable() const override
    {
        return (onBytesAvailable ? onBytesAvailable() : 0) + (1 << 20) + QIODevice::bytesAvailable();
    }
    std::function<qint64(char *, qint64)> onRead;
    std::function<qint64()> onBytesAvailable;
    // 设备实际拿走的完整 PCM（含静音补齐）：作为 AEC 的 far-end 参考。
    std::function<void(const char *, qint64)> onDelivered;

protected:
    // 被音频后端线程调用：只做互斥出队，绝不阻塞/发信号。
    // 队列空/不足时用静音补齐并返回满额，保证后端永远有可播放数据，
    // 避免 underrun / IdleState。
    qint64 readData(char *data, qint64 maxSize) override
    {
        std::memset(data, 0, size_t(maxSize));
        const qint64 n = onRead ? onRead(data, maxSize) : 0;
        Q_UNUSED(n);
        if (onDelivered)
            onDelivered(data, maxSize);
        return maxSize;
    }
    qint64 writeData(const char *, qint64) override { return -1; }
};
} // namespace

LocalAudioSession::LocalAudioSession(QObject *parent) : QObject(parent)
{
    playbackClock_.start();
    auto *pull = new PullDevice(this);
    pullDev_ = pull;
    pull->onRead = [this](char *data, qint64 maxSize) {
        return pullFromQueue(data, maxSize);
    };
    pull->onBytesAvailable = [this] { return queueBytes(); };
    pull->onDelivered = [this](const char *data, qint64 size) {
        // 音频后端线程：只做互斥暂存，AEC 处理全部留在工作线程。
        QMutexLocker locker(&aecRefMutex_);
        aecReferenceMirror_.append(data, int(size));
        if (aecReferenceMirror_.size() > AecRefMirrorMaxBytes)
            aecReferenceMirror_.remove(0, aecReferenceMirror_.size() - AecRefMirrorMaxBytes);
    };
    uplinkTimer_.setTimerType(Qt::PreciseTimer);
    connect(&uplinkTimer_, &QTimer::timeout, this, &LocalAudioSession::pumpUplink);
    downlinkTimer_.setTimerType(Qt::PreciseTimer);
    downlinkTimer_.setInterval(DownlinkPumpMs);
    connect(&downlinkTimer_, &QTimer::timeout, this, &LocalAudioSession::pumpDownlink);
    monitorTimer_.setInterval(100);
    connect(&monitorTimer_, &QTimer::timeout, this, &LocalAudioSession::monitorPlayback);
    finishTimer_.setInterval(100);
    connect(&finishTimer_, &QTimer::timeout, this, &LocalAudioSession::checkFinished);
}

LocalAudioSession::~LocalAudioSession()
{
    stop();
}

bool LocalAudioSession::start(const BotConfig &config, const QJsonArray &tools)
{
    if (active())
        return true;
    lastError_.clear();
    // 与 SIP 完全一致的语义：勾选「允许用户打断」= 全双工（麦克风全程开启且持续上行，
    // 收到 speech_started 立即停播）；未勾选 = 半双工（AI 播放期间上行静音，播放结束立即恢复）。
    allowBargeIn_ = config.allowBargeIn;

    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (input.isNull() || output.isNull()) {
        lastError_ = QStringLiteral("未找到可用的默认音频输入/输出设备。");
        return false;
    }
    const QAudioFormat inFormat = monoPcm(16000);
    const QAudioFormat outFormat = monoPcm(24000);
    // 不用 isFormatSupported 否决：该判断在 Windows WASAPI 上偏保守，
    // 而 WASAPI 共享模式/Linux plughw 实际会由系统自动重采样。
    // 直接以目标格式启动，start 真正失败（设备独占占用等）才报错。

    source_ = new QAudioSource(input, inFormat, this);
    // 采集缓冲 120ms：缓冲深度只在“读得比产得慢”时才变成延迟；pumpUplink 每个
    // tick 都把缓冲里已有的数据排空，正常节拍下延迟仍是 ~20ms。
    // 缓冲太浅时定时器偶发抖动（Windows 调度延迟几十毫秒并不罕见）会撑爆缓冲
    // 丢帧 → 上行断续、识别差；120ms + 排空式读取两者兼得。
    source_->setBufferSize(inFormat.bytesForDuration(120000));
    connect(source_, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && source_ && source_->error() != QAudio::NoError)
            emit logMessage(QStringLiteral("麦克风异常停止（错误 %1）。").arg(int(source_->error())));
    });
    srcDev_ = source_->start();
    if (!srcDev_ || source_->error() != QAudio::NoError ||
        source_->state() == QAudio::StoppedState) {
        lastError_ = QStringLiteral("无法打开麦克风（%1），请检查设备是否被其他程序占用。")
                         .arg(input.description());
        stop();
        return false;
    }
    // 扬声器拉取模式：不在这里启动，队列攒够预缓冲后由 pumpDownlink 启动。
    outputDevice_ = output;
    outputFormat_ = outFormat;
    sink_ = createSink(outputDevice_, outputFormat_);
    prefillBytes_ = int(outFormat.bytesForDuration(DownlinkBufferUsec));
    const QString inputId = QString::fromUtf8(input.id()).left(160);
    const QString outputId = QString::fromUtf8(output.id()).left(160);
    emit logMessage(QStringLiteral(
        "[audio] 输入设备：%1（id：%2；首选 %3 Hz / %4 声道 / sampleFormat %5）；"
        "输出设备：%6（id：%7；首选 %8 Hz / %9 声道 / sampleFormat %10）；"
        "本地播放 24000 Hz / 1 声道 / int16（拉取模式，预缓冲后起播）。")
        .arg(input.description(), inputId.isEmpty() ? QStringLiteral("-") : inputId)
        .arg(input.preferredFormat().sampleRate())
        .arg(input.preferredFormat().channelCount())
        .arg(int(input.preferredFormat().sampleFormat()))
        .arg(output.description(), outputId.isEmpty() ? QStringLiteral("-") : outputId)
        .arg(output.preferredFormat().sampleRate())
        .arg(output.preferredFormat().channelCount())
        .arg(int(output.preferredFormat().sampleFormat())));
    if (isLikelyLoopbackInputName(input.description())) {
        const QString warning = QStringLiteral(
            "当前输入设备「%1」可能采集系统播放声音，会导致 AI 听到自己而自激，"
            "请改用真实麦克风。").arg(input.description());
        emit logMessage(QStringLiteral("[audio] 警告：") + warning);
        emit deviceWarning(warning);
    }
    aec_.reset();
    {
        QMutexLocker locker(&aecRefMutex_);
        aecReferenceMirror_.clear();
    }
    const bool headsetInput = isLikelyHeadsetInputName(input.description());
    const bool sameDevice = isSamePhysicalDevice(input.description(), output.description());
    const bool skipAec = headsetInput || sameDevice;
    if (allowBargeIn_) {
        if (skipAec) {
            // 耳机/同设备场景：扬声器→麦克风耦合路径物理上不存在或极短，
            // Speex AEC + preprocess 会把近端语音当"回声残余"压制削波
            // （诊断录音证实：AEC 后 RMS 降 24%，识别极差）。跳过客户端 AEC，
            // 上行直接走原始 near PCM——这是阿里官方对接模式的默认做法。
            aec_.reset();
            const QString reason = headsetInput
                ? QStringLiteral("检测到耳机输入设备「%1」").arg(input.description())
                : QStringLiteral("输入/输出为同一物理设备（「%1」≈「%2」）")
                      .arg(input.description(), output.description());
            emit logMessage(QStringLiteral(
                "[aec] 跳过客户端回声消除：%1 — 耳机场景扬声器→麦克风耦合路径极短，"
                "AEC 会压坏近端语音；上行直接发送原始 PCM。").arg(reason));
        } else {
            // 全双工 + 扬声器/麦克风分离设备（典型是笔记本内置喇叭+麦、
            // 桌面音箱+独立麦）：客户端 SpeexDSP 回声消除，参考信号 = 写入扬声器的播放 PCM。
            aec_ = std::make_unique<AecProcessor>();
            QString aecError;
            if (!aec_->init(AecFrameSamples, AecTailMs * 16, &aecError)) {
                aec_.reset();
                emit logMessage(QStringLiteral(
                    "[aec] 初始化失败（%1），本次全双工不做回声消除，回声可能触发自激。")
                    .arg(aecError));
            } else {
                emit logMessage(QStringLiteral(
                    "[aec] 已启用 SpeexDSP 回声消除：内部 16 kHz / 单声道 / %1 ms 帧 / 尾长 %2 ms"
                    "（%3 taps）；参考信号按设备写入量推进。")
                    .arg(AecFrameSamples * 1000 / 16000).arg(AecTailMs).arg(AecTailMs * 16));
            }
        }
        if (aec_)
            aecStatsClock_.start();
        emit logMessage(QStringLiteral(
            "[audio] 全双工（已勾选允许用户打断）：AI 播放期间麦克风保持开启、上行持续发送；"
            "收到 speech_started 立即停播（与 SIP 一致）。"));
    } else {
        emit logMessage(QStringLiteral(
            "[audio] 半双工（未勾选允许用户打断）：AI 播放期间上行静音，播放结束立即恢复监听。"));
    }
    session_ = std::make_unique<QwenSession>(this);
    connect(session_.get(), &QwenSession::ready, this, [this] {
        emit statusChanged(QStringLiteral("本地对话进行中，请说话"));
        session_->greet();
    });
    connect(session_.get(), &QwenSession::audioReceived,
            this, &LocalAudioSession::playAudio);
    connect(session_.get(), &QwenSession::diagLog,
            this, [this](const QString &t) { emit logMessage(t); });
    connect(session_.get(), &QwenSession::interrupted, this, [this] {
        emit logMessage(QStringLiteral("当前播报已中断。"));
        handleInterrupted();
    });
    connect(session_.get(), &QwenSession::transcript,
            this, &LocalAudioSession::transcript);
    connect(session_.get(), &QwenSession::failed, this, [this](const QString &message) {
        emit failed(message);
        stop(); // 会话失败即结束本地对话，UI 通过 activeChanged(false) 复位
    });
    connect(session_.get(), &QwenSession::statusChanged,
            this, &LocalAudioSession::statusChanged);
    connect(session_.get(), &QwenSession::turnCompleted,
            this, &LocalAudioSession::turnCompleted);
    connect(session_.get(), &QwenSession::usageUpdated,
            this, &LocalAudioSession::usageUpdated);
    connect(session_.get(), &QwenSession::endRequested,
            this, &LocalAudioSession::beginFinish);
    connect(session_.get(), &QwenSession::toolCallRequested,
            this, &LocalAudioSession::toolCallRequested);

    chunkBytes_ = inFormat.bytesForDuration(config.uplinkChunkMs * 1000);
    // 诊断录音：AEC 前后上行各录一份 WAV（每次对话覆盖），用于对比定位识别差
    const QString logDir = QCoreApplication::applicationDirPath() + QStringLiteral("/log");
    QDir().mkpath(logDir);
    debugNearBytes_ = 0;
    debugUplinkBytes_ = 0;
    debugNearFile_.setFileName(logDir + QStringLiteral("/debug_near.wav"));
    debugUplinkFile_.setFileName(logDir + QStringLiteral("/debug_uplink.wav"));
    if (debugNearFile_.open(QIODevice::WriteOnly)) {
        writeWavHeader(debugNearFile_, 0);
    }
    if (debugUplinkFile_.open(QIODevice::WriteOnly)) {
        writeWavHeader(debugUplinkFile_, 0);
    }
    if (debugNearFile_.isOpen())
        emit logMessage(QStringLiteral("[debug] 上行诊断录音：%1（AEC 前）/ %2（AEC 后）")
                            .arg(debugNearFile_.fileName(), debugUplinkFile_.fileName()));
    finishing_ = false;
    sinkHadData_.store(false);
    sinkStarted_ = false;
    lastPullMs_.store(-10000);
    lastSpeakingLogged_ = false;
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.clear();
    }
    // 与 SIP 走同一条判定路径：勾选时收到 speech_started 立即取消当前响应，无延迟确认。
    session_->open(config, tools);
    uplinkTimer_.start(config.uplinkChunkMs);
    downlinkTimer_.start();
    monitorTimer_.start();
    emit activeChanged(true);
    emit statusChanged(QStringLiteral("正在连接模型…"));
    return true;
}

QAudioSink *LocalAudioSession::createSink(const QAudioDevice &device,
                                          const QAudioFormat &format)
{
    auto *sink = new QAudioSink(device, format, this);
    sink->setBufferSize(format.bytesForDuration(DownlinkBufferUsec));
    connect(sink, &QAudioSink::stateChanged, this, [this, sink](QAudio::State state) {
        if (sink != sink_)
            return;
        static const char *names[] = {"Active", "Suspended", "Stopped", "Idle"};
        emit logMessage(QStringLiteral("[sink] 状态变化：%1（错误 %2）。")
                            .arg(state >= 0 && int(state) < 4 ? names[int(state)] : "?")
                            .arg(int(sink->error())));
        if (state == QAudio::StoppedState &&
            sink->error() != QAudio::NoError && sink->error() != QAudio::UnderrunError)
            emit logMessage(QStringLiteral("扬声器异常停止（错误 %1）。").arg(int(sink->error())));
    });
    return sink;
}

void LocalAudioSession::completeToolCall(const QString &callId,
                                         const QString &outputJson)
{
    // 本槽在音频工作线程执行（结果由主线程排队送回）
    if (!session_) return; // 对话已结束，迟到结果忽略
    session_->completeToolCall(callId, outputJson);
}

void LocalAudioSession::stop()
{
    const bool wasActive = static_cast<bool>(session_);
    uplinkTimer_.stop();
    downlinkTimer_.stop();
    monitorTimer_.stop();
    finishTimer_.stop();
    if (session_) {
        // 绝不在信号/定时器回调栈内析构会话：先断开槽连接并 close()，
        // 再用 deleteLater() 延后释放，避免回到已释放的 Impl 继续执行（UAF）。
        QObject::disconnect(session_.get(), nullptr, this, nullptr);
        session_->close();
        session_.release()->deleteLater();
    }
    if (source_) {
        source_->stop();
        source_ = nullptr;
        srcDev_ = nullptr;
    }
    if (sink_) {
        sink_->stop();
        sink_ = nullptr;
        sinkStarted_ = false;
    }
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.clear();
    }
    aec_.reset();
    {
        QMutexLocker locker(&aecRefMutex_);
        aecReferenceMirror_.clear();
    }
    sinkHadData_.store(false);
    finishing_ = false;
    lastSpeakingLogged_ = false;
    // 回填 WAV 头（数据长度），得到可直接播放的诊断录音
    finalizeWav(debugNearFile_, debugNearBytes_);
    finalizeWav(debugUplinkFile_, debugUplinkBytes_);
    if (wasActive)
        emit activeChanged(false);
}

bool LocalAudioSession::modelSpeaking() const
{
    // “正在播报”以实际拉取的音频为准：队列还有数据，或设备缓冲里仍有未播完的音频
    // （最后一次拉取之后，仍在 PlaybackSettleMs 的设备缓冲放音窗口内）。
    return queueBytes() > 0 || playbackClock_.elapsed() - lastPullMs_.load() < PlaybackSettleMs;
}

bool LocalAudioSession::playbackIdle() const
{
    return queueBytes() == 0 && playbackClock_.elapsed() - lastPullMs_.load() >= PlaybackSettleMs;
}

qint64 LocalAudioSession::queueBytes() const
{
    QMutexLocker locker(&queueMutex_);
    return pcmQueue_.size();
}

qint64 LocalAudioSession::pullFromQueue(char *data, qint64 maxSize)
{
    // 音频后端线程：只做互斥出队 + 打时间戳，绝不阻塞/发信号。
    qint64 n;
    {
        QMutexLocker locker(&queueMutex_);
        n = qMin(maxSize, qint64(pcmQueue_.size()));
        if (n > 0) {
            std::memcpy(data, pcmQueue_.constData(), size_t(n));
            pcmQueue_.remove(0, int(n));
        }
    }
    lastPullMs_.store(playbackClock_.elapsed());
    if (n > 0)
        sinkHadData_.store(true);
    return n;
}

void LocalAudioSession::pumpDownlink()
{
    if (!session_)
        return;
    const qint64 nowMs = playbackClock_.elapsed();
    const qint64 queued = queueBytes();
    if (!sinkStarted_) {
        // sink 可能因停摆重建/自然播完/打断而被销毁（sink_=nullptr），
        // 或打断重建后仍然存在（handleInterrupted 创建了新 sink 但未 start）。
        // 两种情况统一检查并按需创建，避免 return 跳过起播逻辑。
        if (!sink_) {
            sink_ = createSink(outputDevice_, outputFormat_);
            sink_->setBufferSize(int(outputFormat_.bytesForDuration(DownlinkBufferUsec)));
        }
        // 预缓冲起播：队列攒够 150ms（或收尾阶段有残余数据）再启动拉取会话，
        // 避免起播即欠载。之后由后端线程自行拉取，静音补齐兜底不会欠载。
        if (queued < prefillBytes_ && !(finishing_ && queued > 0))
            return;
        sink_->start(pullDev_);
        sinkStarted_ = true;
        lastPullMs_.store(nowMs); // 停摆判定宽限基准
        emit logMessage(QStringLiteral("[sink] 起播（拉取模式，队列 %1 字节，设备缓冲 %2 字节）。")
                            .arg(queued).arg(sink_->bufferSize()));
        if (sink_->state() == QAudio::StoppedState && sink_->error() != QAudio::NoError) {
            lastError_ = QStringLiteral("扬声器启动失败，请检查设备是否被其他程序占用。");
            emit failed(lastError_);
            stop();
        }
        return;
    }
    if (!sink_)
        return;
    // 停摆看门狗：队列有数据但后端超过 StallRestartMs 没来拉取 = 播放假死。
    // 销毁 sink 回到初始状态，让下次从头走干净的 prefill 起播路径（有完整日志、
    // sinkStarted_=false、WASAPI 冷启动无残留）。
    // 自然播完的 sink（StoppedState + sinkStarted_=true）也走同一销毁路径——
    // 不能留着已 stop 的 sink 让下一轮 audio 到来时被停摆看门狗误判重建。
    const qint64 pullIdleMs = nowMs - lastPullMs_.load();
    const bool normalStopIdle = queued == 0 &&
        sink_->state() == QAudio::StoppedState && sink_->error() == QAudio::NoError;
    if ((queued > 0 && pullIdleMs > StallRestartMs) || normalStopIdle) {
        if (normalStopIdle) {
            // 正常播完后 sink 处于 StoppedState + 队列为空 → 销毁休息，等下一轮 audio
            // 来时从零创建 + prefill 起播。避免已 stop 的 sink 让停摆看门狗误杀，
            // 也避免 WASAPI 内部缓冲残留导致下一轮开头杂音。
            sink_->deleteLater();
            sink_ = nullptr;
            sinkStarted_ = false;
            lastPullMs_.store(-10000);
            return;
        }
        emit logMessage(QStringLiteral(
            "[sink] 播放停摆（%1 ms 无拉取，状态 %2），销毁重建扬声器（队列 %3 字节）。")
            .arg(pullIdleMs).arg(int(sink_->state())).arg(queued));
        sink_->stop();
        sink_->deleteLater();
        sink_ = nullptr;
        sinkStarted_ = false;
        lastPullMs_.store(-10000);
        // 销毁后 sink_ 为空，下一个 tick 会从 if (!sinkStarted_) 分支重新创建 sink
        // 并 prefill 起播，路径干净且有完整起播日志。
    }
}

void LocalAudioSession::pumpUplink()
{
    if (!session_ || finishing_ || !srcDev_)
        return;
    // 先把写入设备的播放 PCM 交给 AEC（不做任何门控，保持参考时间轴连续）。
    if (aec_) {
        QByteArray reference;
        {
            QMutexLocker locker(&aecRefMutex_);
            reference.swap(aecReferenceMirror_);
        }
        aec_->pushReference(reference);
    }
    const bool speaking = modelSpeaking();
    if (speaking != lastSpeakingLogged_) {
        lastSpeakingLogged_ = speaking;
        if (speaking) {
            uplinkBytesDuringPlay_ = 0;
            emit logMessage(allowBargeIn_
                ? QStringLiteral("[audio] AI 播放中（全双工）：麦克风保持开启、上行持续发送")
                : QStringLiteral("[audio] AI 播放中（半双工）：麦克风上行静音"));
        } else {
            emit logMessage(allowBargeIn_
                ? QStringLiteral("[audio] AI 播放结束：麦克风继续上行（本轮播放期间已发送 %1 字节）。")
                      .arg(uplinkBytesDuringPlay_)
                : QStringLiteral("[audio] AI 播放结束：麦克风恢复上行。"));
        }
    }
    // 半双工：AI 播放期间丢弃上行（读走丢弃，避免设备缓冲堆积）；播放结束立即恢复。
    // 全双工：麦克风全程开启、上行不停。
    if (shouldMuteUplink(allowBargeIn_, speaking)) {
        // 排空丢弃：定时器抖动迟到的 tick 要把积压一起清掉，否则小缓冲丢帧
        srcDev_->skip(qMin<qint64>(chunkBytes_ * 8, srcDev_->bytesAvailable()));
        return;
    }

    // 残余回声防护：以下两种时刻没有任何在播音频可作参考（参考流为静音），
    // AEC 必然消不掉，照常上行会被服务端转写成“用户说话”→ 触发 speech_started
    // 自打断死循环（日志证据：打断几乎都发生在尾音时段）：
    // 1) 打断后 250ms：设备已停，但刚放出的声音尾音/房间混响仍在麦克风里；
    // 2) 队列已空但设备尾音尚未排空（settle 窗口）：播报自然结束后，设备缓冲里
    //    最后一段音频还在放，此刻采到的全是回声。
    // 窗口内照常跑 AEC（保持近端/参考时间轴连续），结果丢弃不上行。
    const qint64 nowMs = playbackClock_.elapsed();
    const bool tailEchoGuard = aec_ &&
        (nowMs - lastInterruptMs_ < TailEchoGuardMs ||
         (queueBytes() == 0 && nowMs - lastPullMs_.load() < PlaybackSettleMs));

    if (uplinkBuf_.size() < chunkBytes_)
        uplinkBuf_.resize(chunkBytes_);
    // 排空式读取：每个 tick 把设备缓冲里已有的数据读完（上限 160ms 防单次过大）。
    // 正常节拍下每 tick 就 20ms，延迟不变；定时器偶发迟到几十毫秒时，积压数据
    // 在下一个 tick 一次发出——不丢帧，代价只是那一帧稍晚到达。
    for (int drainedChunks = 0; drainedChunks < 8; ++drainedChunks) {
        const qint64 read = srcDev_->read(uplinkBuf_.data(), chunkBytes_);
        if (read <= 0)
            break;
        const QByteArray near(uplinkBuf_.constData(), int(read));
        if (debugNearFile_.isOpen()) {
            debugNearFile_.write(near);
            debugNearBytes_ += near.size();
        }
        if (tailEchoGuard) {
            aec_->process(near);
        } else {
            const QByteArray uplink = aec_ ? aec_->process(near) : near;
            if (!uplink.isEmpty()) {
                if (debugUplinkFile_.isOpen()) {
                    debugUplinkFile_.write(uplink);
                    debugUplinkBytes_ += uplink.size();
                }
                if (speaking)
                    uplinkBytesDuringPlay_ += uplink.size();
                session_->appendAudio(uplink);
            }
        }
        if (read < chunkBytes_)
            break; // 缓冲已排空
    }
    // ERLE 诊断：约每 2 秒一行，不刷屏。ERLE 为负说明该窗口是用户近端说话占主导，
    // AEC 本就不应消除近端语音，属正常现象。
    if (aec_ && aecStatsClock_.elapsed() >= AecStatsIntervalMs) {
        aecStatsClock_.restart();
        const AecProcessor::Stats stats = aec_->takeStats();
        if (stats.frames > 0) {
            emit logMessage(QStringLiteral(
                "[aec] 近端 RMS %1 / 残差 RMS %2 / ERLE %3 dB（%4 帧）/ 参考填充率 %5%%")
                .arg(stats.nearRms, 0, 'f', 0)
                .arg(stats.residualRms, 0, 'f', 0)
                .arg(stats.erleDb, 0, 'f', 1)
                .arg(stats.frames)
                .arg(stats.refFillPercent));
        }
    }
}

void LocalAudioSession::playAudio(const QByteArray &pcm24k)
{
    if (!sink_ || pcm24k.isEmpty())
        return;
    if (queueBytes() + pcm24k.size() > MaxQueueBytes) {
        lastError_ = QStringLiteral("播放队列积压超过 83 秒，音频消费异常，结束对话。");
        emit failed(lastError_);
        stop();
        return;
    }
    QMutexLocker locker(&queueMutex_);
    pcmQueue_.append(pcm24k);
    // 起播与持续写入均由 pumpDownlink（20ms 节拍）完成
}

void LocalAudioSession::monitorPlayback()
{
    if (!session_)
        return;
    // 本地模式没有 PcmBridge 驱动无声计时，由这里在输出排空后启动
    // （否则无声超时/挂机策略在本地对话中完全不生效）。
    session_->checkDrainAndStartSilence(playbackIdle());
}

void LocalAudioSession::handleInterrupted()
{
    // 抢话打断：丢弃尚未播放的 TTS（含 FIFO 与设备内部缓冲）。
    if (!sink_)
        return;
    if (!modelSpeaking()) {
        // 服务端在本地已无任何在播/待播音频时下发 speech_started：
        // 无残留可清，直接忽略，避免无谓的扬声器重启与 AEC 时间轴抖动。
        emit logMessage(QStringLiteral("[barge] 收到打断但本地无待播音频，无需清理。"));
        return;
    }
    const qint64 queuedBytes = queueBytes();
    const qint64 deviceBuffered =
        qMax<qint64>(0, sink_->bufferSize() - sink_->bytesFree());
    const qint64 bytesPerMs =
        qMax<qint64>(1, sink_->format().bytesForDuration(1000));
    emit logMessage(QStringLiteral(
        "[barge] 打断时残留：队列 %1 字节（%2 ms）+ 设备缓冲 %3 字节（%4 ms）。")
        .arg(queuedBytes).arg(queuedBytes / bytesPerMs)
        .arg(deviceBuffered).arg(deviceBuffered / bytesPerMs));

    lastInterruptMs_ = playbackClock_.elapsed(); // 残余回声防护窗口起点
    // 拉取模式下 stop() 无法丢弃已进入 WASAPI 设备缓冲的音频（会继续播完，
    // 表现为“打断后还听到上一句末尾”）——销毁重建 sink 彻底丢弃设备缓冲。
    if (sinkStarted_) {
        // 拉取模式 stop() 无法丢弃 WASAPI 设备缓冲的音频（会继续播完），
        // 销毁 sink 彻底丢弃。下次 audio 来时 pumpDownlink 从零创建 + prefill 起播。
        sink_->stop();
        sink_->deleteLater();
        sink_ = nullptr;
        sinkStarted_ = false;
    }
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.clear();
    }
    // 被打断丢弃的音频（FIFO + 设备缓冲）没有真正播出，参考时间轴必须一起清零，
    // 否则 AEC 会拿“没真正播放过”的音频当参考；但自适应滤波器状态保留——
    // 扬声器→麦克风的声学路径不因打断改变，清滤波器会让 AEC 在打断风暴下
    // 永远无法收敛（自打断死循环的根因）。
    if (aec_)
        aec_->resetTimeline();
    {
        QMutexLocker locker(&aecRefMutex_);
        aecReferenceMirror_.clear();
    }
    lastPullMs_.store(-10000); // 立刻视为“未在播报”，半双工上行立即恢复
    lastSpeakingLogged_ = false;
    sinkHadData_.store(false);
    emit logMessage(QStringLiteral("[barge] 播放已停止（队列与设备缓冲已清空），等待下一轮音频。"));
}

void LocalAudioSession::beginFinish(const QString &reason)
{
    if (finishing_)
        return;
    finishing_ = true;
    finishReason_ = reason;
    // 停止上行发送：保护挂机话术完整播放，期间不再发送用户音频
    uplinkTimer_.stop();
    finishDeadline_.start();
    finishTimer_.start();
}

void LocalAudioSession::checkFinished()
{
    if (!finishing_)
        return;
    // 已排空：设备接收过数据且播放已静止（FIFO 空 + 超过设备缓冲尾音时长无写入）。
    const bool drained = !sink_ || (sinkHadData_.load() && playbackIdle());
    if (drained || finishDeadline_.elapsed() > FinishTimeoutMs) {
        const QString reason = finishReason_;
        stop();
        emit finished(reason);
    }
}

} // namespace qwenbot
