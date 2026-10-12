#include "bot_config.h"
#include <QUrlQuery>

namespace qwenbot {

// 允许接入条目：IPv4 地址或 CIDR 网段（如 192.168.1.0/24），0.0.0.0 通配仍拒绝
QString BotConfig::peerEntryError(const QString &entry)
{
    const QStringList parts = entry.trimmed().split('/');
    if (parts.size() > 2)
        return QStringLiteral("“%1”格式无效，应为 IPv4 或 IPv4/掩码位（如 192.168.1.0/24）。").arg(entry);
    const QHostAddress address(parts.first().trimmed());
    if (address.isNull() || address.protocol() != QAbstractSocket::IPv4Protocol)
        return QStringLiteral("“%1”不是有效的 IPv4 地址。").arg(entry);
    if (address == QHostAddress::AnyIPv4)
        return QStringLiteral("不允许配置通配地址 0.0.0.0，请填写明确的 IPv4 地址或网段。");
    if (parts.size() == 2) {
        bool ok = false;
        const int prefix = parts.last().trimmed().toInt(&ok);
        if (!ok || prefix < 0 || prefix > 32)
            return QStringLiteral("“%1”的掩码位无效，应在 0–32 之间。").arg(entry);
    }
    return {};
}
QString BotConfig::validate() const
{
    const QHostAddress bind(listenAddress);
    if (bind.protocol() != QAbstractSocket::IPv4Protocol)
        return QStringLiteral("监听地址必须是 IPv4 地址（例如 0.0.0.0 或 127.0.0.1）。");
    if (!sipPort || rtpPort < 1024 || rtpPort % 2 || maxCalls < 1 || maxCalls > 180 ||
        unsigned(rtpPort) + unsigned(maxCalls) * 2 > 65535)
        return QStringLiteral("检查 SIP 端口、偶数 RTP 起始端口和并发数（1–180）。");
    if (sipPort >= rtpPort && sipPort < rtpPort + maxCalls * 2)
        return QStringLiteral("SIP 端口不能与 RTP/RTCP 端口范围重叠。");
    if (allowedPeers.isEmpty()) return QStringLiteral("至少配置一个允许接入的 FreeSWITCH IPv4 地址。");
    for (const auto &peer : allowedPeers)
        if (const QString error = peerEntryError(peer); !error.isEmpty()) return error;
    if (!sipIdentity.startsWith(QStringLiteral("sip:")) || sipIdentity.contains('\r') || sipIdentity.contains('\n'))
        return QStringLiteral("SIP 身份必须为 sip:user@host。");
    if (!registrar.isEmpty() && (!registrar.startsWith(QStringLiteral("sip:")) ||
        registrar.contains('\r') || registrar.contains('\n') || sipUser.isEmpty()))
        return QStringLiteral("注册服务器必须为 sip:host，且填写注册用户名。");
    if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("wss") || endpoint.host().isEmpty() ||
        !endpoint.userInfo().isEmpty() || endpoint.hasFragment() || endpoint.host().contains('{') ||
        QUrlQuery(endpoint).queryItemValue(QStringLiteral("model")) != QStringLiteral("qwen-audio-3.1-realtime-plus"))
        return QStringLiteral("填写有效 WSS 地址及实际业务空间，model 必须为 qwen-audio-3.1-realtime-plus。");
    if (apiKey.trimmed().isEmpty() || apiKey.contains('\r') || apiKey.contains('\n'))
        return QStringLiteral("填写有效 API Key，或设置 DASHSCOPE_API_KEY 环境变量。");
    if (voice.trimmed().isEmpty()) return QStringLiteral("音色不能为空。");
    if (turnMode != QStringLiteral("smart_turn") && turnMode != QStringLiteral("server_vad"))
        return QStringLiteral("轮次检测模式必须为 smart_turn 或 server_vad。");
    if (silenceTimeoutSec < 1 || silenceTimeoutSec > 60)
        return QStringLiteral("无声超时秒数必须在 1–60 之间。");
    if (maxSilenceRetries < 1 || maxSilenceRetries > 10)
        return QStringLiteral("最大无声超时次数必须在 1–10 之间。");
    if (maxTurns < 1 || maxTurns > 200)
        return QStringLiteral("最大对话轮次必须在 1–200 之间。");
    if (callRecordMaxSize < 1024)
        return QStringLiteral("通话记录文件最大大小不能小于 1KB。");
    if (callRecordMaxFiles < 1 || callRecordMaxFiles > 100)
        return QStringLiteral("通话记录轮转文件数必须在 1–100 之间。");
    if (!logPath.trimmed().isEmpty() && (logPath.contains('\r') || logPath.contains('\n')))
        return QStringLiteral("日志文件路径不能包含换行符。");
    if (logMaxSize < 1024)
        return QStringLiteral("日志文件最大大小不能小于 1KB。");
    if (logMaxFiles < 1 || logMaxFiles > 100)
        return QStringLiteral("日志轮转文件数必须在 1–100 之间。");
    if (uplinkChunkMs < 20 || uplinkChunkMs > 100 || uplinkChunkMs % 20 != 0)
        return QStringLiteral("上行音频分片必须在 20–100ms 之间且为 20 的整数倍。");
    if (maxHistoryTurns < 1 || maxHistoryTurns > 50)
        return QStringLiteral("上下文历史轮次必须在 1–50 之间。");
    return {};
}
} // namespace qwenbot
