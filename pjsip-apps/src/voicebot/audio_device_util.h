#pragma once

#include <QString>

namespace qwenbot {

// 输入设备名是否疑似回环/虚拟设备（会采集系统播放声，导致 AI 听到自己而自激）。
// 只做识别，不自动切换设备：由启动日志与界面警告暴露给用户。
inline bool isLikelyLoopbackInputName(const QString &name)
{
    const QString lowered = name.toLower();
    static const char *const patterns[] = {
        "立体声混音", "stereo mix", "what u hear", "loopback", "virtual",
        "虚拟", "vb-audio", "voicemeeter", "cable", "远程音频", "remote audio",
    };
    for (const char *pattern : patterns) {
        if (lowered.contains(QString::fromUtf8(pattern)))
            return true;
    }
    return false;
}

// 半双工判定：仅当未勾选「允许用户打断」且 AI 正在播放时，才暂停上行。
// 勾选（全双工）时即使 AI 正在播放也持续发送上行音频；播放结束后立即恢复。
inline bool shouldMuteUplink(bool allowBargeIn, bool aiSpeaking)
{
    return !allowBargeIn && aiSpeaking;
}

// 输入设备是否疑似耳机/USB  headset（输入输出同体，扬声器→麦克风耦合路径
// 极短或物理不存在，客户端 AEC 不仅无用还会把近端语音当"回声残余"压掉）。
// 关键词覆盖常见品牌（华为/小米/苹果）、通用类型词与 USB 接口标识。
inline bool isLikelyHeadsetInputName(const QString &name)
{
    const QString lowered = name.toLower();
    static const char *const patterns[] = {
        "耳机", "headset", "headphone", "earphone",
        "usb-c", "usb c", "usb headset", "usb headphone",
        "huawei usb", "huawei usb", "小米耳机", "xiaomi",
        "airpods", "airpod", "beats", "sony wh", "sennheiser",
    };
    for (const char *pattern : patterns) {
        if (lowered.contains(QString::fromUtf8(pattern)))
            return true;
    }
    return false;
}

// 输入输出描述名是否高度相似——同一物理设备的耳机/USB 声卡场景下，
// Qt 给出的 input/output description 往往前后只差"(麦克风)"/(扬声器)"后缀。
inline bool isSamePhysicalDevice(const QString &inputDesc, const QString &outputDesc)
{
    const QString inClean = inputDesc.toLower().remove("（麦克风）").remove("(麦克风)")
                               .remove("microphone").remove(" mic");
    const QString outClean = outputDesc.toLower().remove("（扬声器）").remove("(扬声器)")
                                .remove("speaker").remove("output");
    if (inClean.isEmpty() || outClean.isEmpty())
        return false;
    // 清理后互相包含对方 → 同一物理设备
    return inClean.contains(outClean) || outClean.contains(inClean);
}

} // namespace qwenbot
