#include "aec_processor.h"

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <speex/speex_resampler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace qwenbot {
namespace {

constexpr int NearRate = 16000;        // 上行/内部统一采样率
constexpr int FarRate = 24000;         // 模型下发采样率
constexpr int ResampleQuality = 4;     // Speex 重采样质量（4 = 默认）
// 参考缓冲上限：正常稳态约等于 QAudioSink 设备缓冲（500ms），
// 这里留到 1s，超出说明时钟有漂移，丢弃最旧数据防止无限增长。
constexpr int MaxReferenceMs = 1000;

double rmsOf(qint64 count, double sumSquares)
{
    if (count <= 0)
        return 0.0;
    return std::sqrt(sumSquares / double(count));
}

} // namespace

struct AecProcessor::Impl {
    SpeexEchoState *echo = nullptr;
    SpeexPreprocessState *pre = nullptr;
    SpeexResamplerState *resampler = nullptr;
    int frameSamples = 320;
    int tailSamples = 8000;
    QByteArray reference16k; // 已重采样的 far-end，按 near 采集节拍消费
    QByteArray nearPending;  // 不足一帧的近端残留
    std::vector<spx_int16_t> refFrame;
    std::vector<spx_int16_t> residualFrame;
    qint64 statFrames = 0;
    qint64 statSamples = 0;
    double nearSumSquares = 0.0;
    double residualSumSquares = 0.0;
    qint64 refProvided = 0;     // 窗口内真实参考字节
    qint64 refSilencePadded = 0; // 窗口内静音补齐字节（参考断流诊断用）
};

AecProcessor::AecProcessor() = default;

AecProcessor::~AecProcessor()
{
    if (!impl_)
        return;
    if (impl_->pre)
        speex_preprocess_state_destroy(impl_->pre);
    if (impl_->echo)
        speex_echo_state_destroy(impl_->echo);
    if (impl_->resampler)
        speex_resampler_destroy(impl_->resampler);
}

bool AecProcessor::init(int frameSamples, int tailSamples, QString *error)
{
    if (frameSamples <= 0 || tailSamples < frameSamples) {
        if (error)
            *error = QStringLiteral("非法 AEC 参数：帧长 %1、尾长 %2")
                         .arg(frameSamples).arg(tailSamples);
        return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->frameSamples = frameSamples;
    impl_->tailSamples = tailSamples;
    impl_->refFrame.assign(size_t(frameSamples), 0);
    impl_->residualFrame.assign(size_t(frameSamples), 0);

    impl_->echo = speex_echo_state_init(frameSamples, tailSamples);
    if (!impl_->echo) {
        if (error)
            *error = QStringLiteral("speex_echo_state_init 失败");
        impl_.reset();
        return false;
    }
    int rate = NearRate;
    speex_echo_ctl(impl_->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    // 残余回声抑制（preprocess 挂上 echo state）；关闭 AGC/降噪，
    // 避免改变上行音量/音色影响模型识别，只保留线性 AEC 之外的残余抑制。
    impl_->pre = speex_preprocess_state_init(frameSamples, NearRate);
    if (impl_->pre) {
        spx_int32_t disabled = 0;
        speex_preprocess_ctl(impl_->pre, SPEEX_PREPROCESS_SET_AGC, &disabled);
        speex_preprocess_ctl(impl_->pre, SPEEX_PREPROCESS_SET_DENOISE, &disabled);
        speex_preprocess_ctl(impl_->pre, SPEEX_PREPROCESS_SET_DEREVERB, &disabled);
        speex_preprocess_ctl(impl_->pre, SPEEX_PREPROCESS_SET_ECHO_STATE, impl_->echo);
    }

    int resamplerError = 0;
    impl_->resampler = speex_resampler_init(1, FarRate, NearRate, ResampleQuality,
                                            &resamplerError);
    if (!impl_->resampler) {
        if (error)
            *error = QStringLiteral("speex_resampler_init 失败（错误 %1）")
                         .arg(resamplerError);
        impl_.reset();
        return false;
    }
    return true;
}

void AecProcessor::reset()
{
    if (!impl_)
        return;
    if (impl_->echo)
        speex_echo_state_reset(impl_->echo);
    if (impl_->resampler)
        speex_resampler_reset_mem(impl_->resampler);
    impl_->reference16k.clear();
    impl_->nearPending.clear();
    impl_->statFrames = 0;
    impl_->statSamples = 0;
    impl_->nearSumSquares = 0.0;
    impl_->residualSumSquares = 0.0;
    impl_->refProvided = 0;
    impl_->refSilencePadded = 0;
}

void AecProcessor::resetTimeline()
{
    if (!impl_)
        return;
    // 播放被取消（打断清队/设备重建）后，缓冲里“已拉取但未真正播出”的音频
    // 不能继续作为参考，必须清空；但自适应滤波器（echo state）与重采样器状态
    // 一律保留——声学路径不因打断改变，保留收敛结果才能打破
    // “打断→滤波器清零→回声泄漏→又被打断”的死循环。
    impl_->reference16k.clear();
}

void AecProcessor::pushReference(const QByteArray &pcm24k)
{
    if (!impl_ || !impl_->resampler || pcm24k.isEmpty())
        return;
    const int inSamples = pcm24k.size() / 2;
    if (inSamples <= 0)
        return;
    std::vector<spx_int16_t> input(size_t(inSamples), 0);
    std::memcpy(input.data(), pcm24k.constData(), size_t(inSamples) * 2);

    // 24k → 16k 输出上限约为输入的 2/3，留 8 个采样点余量
    std::vector<spx_int16_t> output(size_t(inSamples) * 2 / 3 + 8, 0);
    spx_uint32_t inLen = spx_uint32_t(inSamples);
    spx_uint32_t outLen = spx_uint32_t(output.size());
    const int err = speex_resampler_process_int(impl_->resampler, 0, input.data(),
                                                &inLen, output.data(), &outLen);
    if (err != RESAMPLER_ERR_SUCCESS || outLen == 0)
        return;
    impl_->reference16k.append(reinterpret_cast<const char *>(output.data()),
                               int(outLen) * 2);

    const int maxBytes = NearRate * 2 * MaxReferenceMs / 1000;
    if (impl_->reference16k.size() > maxBytes)
        impl_->reference16k.remove(0, impl_->reference16k.size() - maxBytes);
}

QByteArray AecProcessor::process(const QByteArray &near16k)
{
    QByteArray result;
    if (!impl_ || !impl_->echo)
        return near16k;
    impl_->nearPending.append(near16k);

    const int frameBytes = impl_->frameSamples * 2;
    const int frames = impl_->nearPending.size() / frameBytes;
    if (frames <= 0)
        return result;
    result.reserve(frames * frameBytes);

    for (int f = 0; f < frames; ++f) {
        const spx_int16_t *near =
            reinterpret_cast<const spx_int16_t *>(impl_->nearPending.constData() +
                                                  f * frameBytes);
        // 参考按采集节拍消费：正常播放时缓冲里约有一整个设备缓冲（500ms）的余量，
        // 因此当前消费到的样本≈此刻正在播放的声音；数据不足时补静音。
        spx_int16_t *ref = impl_->refFrame.data();
        const int available = int(std::min<qsizetype>(frameBytes, impl_->reference16k.size()));
        if (available > 0) {
            std::memcpy(ref, impl_->reference16k.constData(), size_t(available));
            impl_->reference16k.remove(0, available);
        }
        if (available < frameBytes)
            std::memset(ref + available / 2, 0, size_t(frameBytes - available));
        impl_->refProvided += available;
        impl_->refSilencePadded += frameBytes - available;

        speex_echo_cancellation(impl_->echo, near, ref, impl_->residualFrame.data());
        if (impl_->pre)
            speex_preprocess_run(impl_->pre, impl_->residualFrame.data());

        result.append(reinterpret_cast<const char *>(impl_->residualFrame.data()),
                      frameBytes);

        double nearSquares = 0.0;
        double residualSquares = 0.0;
        for (int i = 0; i < impl_->frameSamples; ++i) {
            nearSquares += double(near[i]) * double(near[i]);
            residualSquares += double(impl_->residualFrame[size_t(i)]) *
                               double(impl_->residualFrame[size_t(i)]);
        }
        impl_->nearSumSquares += nearSquares;
        impl_->residualSumSquares += residualSquares;
        impl_->statSamples += impl_->frameSamples;
        ++impl_->statFrames;
    }
    impl_->nearPending.remove(0, frames * frameBytes);
    return result;
}

AecProcessor::Stats AecProcessor::takeStats()
{
    Stats stats;
    if (!impl_)
        return stats;
    stats.frames = impl_->statFrames;
    stats.samples = impl_->statSamples;
    if (impl_->statSamples > 0) {
        stats.nearRms = rmsOf(impl_->statSamples, impl_->nearSumSquares);
        stats.residualRms = rmsOf(impl_->statSamples, impl_->residualSumSquares);
        const double nearPower = impl_->nearSumSquares / double(impl_->statSamples);
        const double residualPower =
            std::max(impl_->residualSumSquares / double(impl_->statSamples), 1e-9);
        stats.erleDb = 10.0 * std::log10(std::max(nearPower, 1e-9) / residualPower);
    }
    if (impl_->refProvided + impl_->refSilencePadded > 0) {
        stats.refFillPercent = int(impl_->refProvided * 100 /
                                   (impl_->refProvided + impl_->refSilencePadded));
    }
    impl_->statFrames = 0;
    impl_->statSamples = 0;
    impl_->nearSumSquares = 0.0;
    impl_->residualSumSquares = 0.0;
    impl_->refProvided = 0;
    impl_->refSilencePadded = 0;
    return stats;
}

int AecProcessor::frameSamples() const
{
    return impl_ ? impl_->frameSamples : 0;
}

} // namespace qwenbot
