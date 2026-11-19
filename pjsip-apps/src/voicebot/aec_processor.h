#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

namespace qwenbot {

// 本地对话客户端回声消除（SpeexDSP MDF）。
//
// 数据流：
//   far-end（参考）：QAudioSink 实际拉走的 24k/mono/int16 PCM → 24k→16k 重采样
//                    → 参考缓冲（按采集节拍消费，约等于真正播放时刻）
//   near-end（近端）：QAudioSource 读到的 16k/mono/int16
//   → speex_echo_cancellation(near, far) → 残差 PCM（再交给 appendAudio）
//
// 统一内部采样率 16 kHz、帧长 20 ms（320 采样点），AEC 尾长按需设置。
// 所有方法都在音频工作线程上调用；pushReference 可由音频后端线程调用，
// 调用方需自行串行化（LocalAudioSession 用独立互斥量暂存后在本线程推送）。
class AecProcessor {
public:
    // 自上次 takeStats() 以来的统计（用于日志诊断）
    struct Stats {
        qint64 frames = 0;        // 参与统计的 20ms 帧数
        qint64 samples = 0;       // 参与统计的采样点数
        double erleDb = 0.0;      // 回声回损：10log10(近端功率/残差功率)
        double nearRms = 0.0;     // 消除前上行 RMS（int16 单位）
        double residualRms = 0.0; // 消除后上行 RMS（int16 单位）
        int refFillPercent = -1;  // 参考缓冲填充率：应消费的参考帧里真实数据占比；
                                  // -1 表示本窗口无帧。持续偏低说明参考未送达（管道断裂），
                                  // ERLE 接近 0 且此值正常则说明参考对齐/收敛问题。
    };

    AecProcessor();
    ~AecProcessor();
    AecProcessor(const AecProcessor &) = delete;
    AecProcessor &operator=(const AecProcessor &) = delete;

    // frameSamples / tailSamples 均以 16 kHz 采样点计。
    // 失败时返回 false 并在 error 中给出原因（调用方应降级而不是崩溃）。
    bool init(int frameSamples, int tailSamples, QString *error);
    void reset();

    // 只清参考时间轴，保留自适应滤波器/重采样器状态。
    // 用于打断/设备重建：被丢弃未播出的音频不能再当参考（须清掉），
    // 但扬声器→麦克风的声学路径并未改变，保留已收敛的滤波器，
    // 打断风暴下 AEC 仍能持续收敛、下一轮播放立即生效。
    // 清空后 process() 会以静音参考推进（与“此刻无在播音频”的现实一致）。
    void resetTimeline();

    // 参考信号：QAudioSink 从 PullDevice 实际拉走的 24k/mono/int16 PCM（含静音补齐）。
    void pushReference(const QByteArray &pcm24k);

    // 近端 16k/mono/int16。返回去回声后的 PCM；不足一整帧的部分会保留到下次调用。
    QByteArray process(const QByteArray &near16k);

    // 读取并清零统计窗口
    Stats takeStats();

    int frameSamples() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qwenbot
