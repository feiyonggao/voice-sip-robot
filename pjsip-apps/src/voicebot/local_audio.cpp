#include "local_audio.h"
#include "qwen_session.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutexLocker>
#include <cstring>
#include <functional>

namespace qwenbot {
namespace {
constexpr qint64 DownlinkBufferUsec = 500000; // 设备缓冲 500ms：吸收线程/后端抖动
constexpr qint64 PrefillUsec = 400000;        // 开播前预缓冲 400ms，避免冷启动耗尽
constexpr qint64 FinishTimeoutMs = 8000;      // 排空等待兜底，防止状态误判卡死
constexpr int MaxQueueBytes = 4 * 1024 * 1024; // FIFO 上限约 83 秒音频，超限视为异常
constexpr qint64 StallRestartMs = 2000;        // 队列有数据但无拉取进展达 2s → 重启扬声器
constexpr qint64 PlaybackSettleMs = 600;       // 最后一次拉取后设备缓冲最长 500ms 仍在放音

QAudioFormat monoPcm(int sampleRate)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

// 拉取模式播放设备：音频后端线程需要数据时主动调 readData() 来取。
// 本线程绝不调用 write()——push 模式下 write 在设备缓冲吃不下时会阻塞等待
// 设备消费，若设备停摆则把整个工作线程事件循环冻死（定时器/上行全部停摆）。
class PullDevice : public QIODevice {
public:
    explicit PullDevice(QObject *parent) : QIODevice(parent)
    {
        open(QIODevice::ReadOnly);
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return (onBytesAvailable ? onBytesAvailable() : 0) + QIODevice::bytesAvailable();
    }
    std::function<qint64(char *, qint64)> onRead;
    std::function<qint64()> onBytesAvailable;

protected:
    // 可能被音频后端线程调用：只做互斥出队，绝不阻塞/发信号
    qint64 readData(char *data, qint64 maxSize) override
    {
        return onRead ? onRead(data, maxSize) : 0;
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
    uplinkTimer_.setTimerType(Qt::PreciseTimer);
    connect(&uplinkTimer_, &QTimer::timeout, this, &LocalAudioSession::pumpUplink);
    monitorTimer_.setInterval(100);
    connect(&monitorTimer_, &QTimer::timeout, this, &LocalAudioSession::monitorPlayback);
    finishTimer_.setInterval(100);
    connect(&finishTimer_, &QTimer::timeout, this, &LocalAudioSession::checkFinished);
    prefillTimer_.setSingleShot(true);
    prefillTimer_.setInterval(200);
    connect(&prefillTimer_, &QTimer::timeout, this, &LocalAudioSession::primeSink);
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
    allowBargeIn_ = config.allowBargeIn; // 抢话开关决定本地对话半双工/全双工

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
    source_->setBufferSize(inFormat.bytesForDuration(200000)); // 约 200ms，吸收定时抖动防丢帧
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
    // 扬声器延迟启动：FIFO 攒够约 400ms 再 start；之后由后端线程拉取播放。
    sink_ = new QAudioSink(output, outFormat, this);
    sink_->setBufferSize(outFormat.bytesForDuration(DownlinkBufferUsec));
    prefillBytes_ = int(outFormat.bytesForDuration(PrefillUsec));
    connect(sink_, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (state != QAudio::StoppedState || !sink_ || !sinkPrimed_)
            return;
        // 队列有数据却停机的下溢由 playAudio/monitorPlayback 重启处理，这里只记录硬错误
        if (sink_->error() != QAudio::NoError && sink_->error() != QAudio::UnderrunError)
            emit logMessage(QStringLiteral("扬声器异常停止（错误 %1）。").arg(int(sink_->error())));
    });

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
        emit logMessage(QStringLiteral("检测到抢话，已中断当前播报。"));
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
    finishing_ = false;
    sinkHadData_.store(false);
    lastPullMs_.store(-10000);
    lastSpeakingLogged_ = false;
    sinkPrimed_ = false;
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.clear();
    }
    session_->open(config, tools);
    uplinkTimer_.start(config.uplinkChunkMs);
    monitorTimer_.start();
    emit activeChanged(true);
    emit statusChanged(QStringLiteral("正在连接模型…"));
    return true;
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
    monitorTimer_.stop();
    finishTimer_.stop();
    prefillTimer_.stop();
    if (session_) {
        session_->close();
        session_.reset();
    }
    if (source_) {
        source_->stop();
        source_ = nullptr;
        srcDev_ = nullptr;
    }
    if (sink_) {
        sink_->stop();
        sink_ = nullptr;
    }
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.clear();
    }
    sinkPrimed_ = false;
    sinkHadData_.store(false);
    finishing_ = false;
    if (wasActive)
        emit activeChanged(false);
}

bool LocalAudioSession::modelSpeaking() const
{
    // 只看拉取进展，不看 sink 状态：pull 模式下队列空了设备可能一直挂在 Active，
    // 靠状态判断会永久判为"播放中"，麦克风再也无法放行。
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
    QMutexLocker locker(&queueMutex_);
    const qint64 n = qMin(maxSize, qint64(pcmQueue_.size()));
    if (n <= 0)
        return 0; // 无数据返回 0（-1 表示流结束，不能用）
    std::memcpy(data, pcmQueue_.constData(), size_t(n));
    pcmQueue_.remove(0, int(n));
    sinkHadData_.store(true);
    lastPullMs_.store(playbackClock_.elapsed());
    return n;
}

void LocalAudioSession::pumpUplink()
{
    if (!session_ || finishing_ || !srcDev_)
        return;
    // 半双工保护（仅在未开启抢话时）：AI 播放期间不发送麦克风音频。
    // 避免不戴耳机时扬声器声音被采回触发服务端 VAD，导致播报被取消/回声自激。
    // 开启抢话（allowBargeIn）时全双工上送，由服务端 VAD 检测用户说话并自动取消播报。
    const bool speaking = modelSpeaking();
    if (speaking != lastSpeakingLogged_) {
        lastSpeakingLogged_ = speaking;
        if (!allowBargeIn_) {
            emit logMessage(speaking ? QStringLiteral("AI 播放中，麦克风静默（防回声，可在配置中开启抢话）。")
                                     : QStringLiteral("AI 播放结束，麦克风已放行。"));
        }
    }
    if (speaking && !allowBargeIn_) {
        srcDev_->skip(chunkBytes_); // 读走丢弃，避免设备缓冲堆积
        return;
    }
    if (uplinkBuf_.size() < chunkBytes_)
        uplinkBuf_.resize(chunkBytes_);
    const qint64 read = srcDev_->read(uplinkBuf_.data(), chunkBytes_);
    if (read > 0)
        session_->appendAudio(QByteArray::fromRawData(uplinkBuf_.constData(), int(read)));
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
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.append(pcm24k);
    }
    if (!sinkPrimed_) {
        if (!prefillTimer_.isActive())
            prefillTimer_.start();
        if (queueBytes() >= prefillBytes_)
            primeSink();
    } else if (sink_->state() == QAudio::StoppedState) {
        hardRestartSink(); // 回合间隙下溢停机后新数据到达，立即续播
    }
}

void LocalAudioSession::primeSink()
{
    if (sinkPrimed_ || !sink_ || queueBytes() == 0)
        return;
    emit logMessage(QStringLiteral("[prime] 启动扬声器，预缓冲 %1 字节。").arg(queueBytes()));
    sink_->start(pullDev_);
    sinkPrimed_ = true;
    if (sink_->state() == QAudio::StoppedState && sink_->error() != QAudio::NoError &&
        sink_->error() != QAudio::UnderrunError) {
        lastError_ = QStringLiteral("扬声器启动失败，请检查设备是否被其他程序占用。");
        emit failed(lastError_);
        stop();
    }
}

void LocalAudioSession::monitorPlayback()
{
    if (!session_)
        return;
    // 本地模式没有 PcmBridge 驱动无声计时，由这里在输出排空后启动
    // （否则无声超时/挂机策略在本地对话中完全不生效）。
    session_->checkDrainAndStartSilence(playbackIdle());
    if (!sinkPrimed_ || queueBytes() == 0)
        return;
    const qint64 now = playbackClock_.elapsed();
    if (now - lastPullMs_.load() > StallRestartMs && now - lastRestartMs_ > StallRestartMs) {
        emit logMessage(QStringLiteral("播放 %1 ms 无拉取进展（队列 %2 字节），重启扬声器。")
                            .arg(StallRestartMs).arg(queueBytes()));
        hardRestartSink();
    }
}

void LocalAudioSession::hardRestartSink()
{
    if (!sink_ || !pullDev_)
        return;
    if (playbackClock_.elapsed() - lastRestartMs_ < 200)
        return; // 防止重启风暴
    lastRestartMs_ = playbackClock_.elapsed();
    emit logMessage(QStringLiteral("[sink] 重启扬声器（队列 %1 字节）。").arg(queueBytes()));
    sink_->stop();
    sink_->start(pullDev_);
}

void LocalAudioSession::handleInterrupted()
{
    // 抢话打断：丢弃尚未播放的 TTS（含 FIFO）并回到预缓冲状态，
    // 等下一轮数据重新攒缓冲开播。
    if (!sink_)
        return;
    sink_->stop();
    sinkPrimed_ = false;
    {
        QMutexLocker locker(&queueMutex_);
        pcmQueue_.clear();
    }
    sinkHadData_.store(false);
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
    // 已排空：设备接收过数据且播放已静止（FIFO 空 + 超过设备缓冲尾音时长无拉取）。
    // 不看 sink 状态：pull 模式下队列空了设备可能一直挂在 Active。
    const bool drained = !sink_ || (sinkHadData_.load() && playbackIdle());
    if (drained || finishDeadline_.elapsed() > FinishTimeoutMs) {
        const QString reason = finishReason_;
        stop();
        emit finished(reason);
    }
}

} // namespace qwenbot
