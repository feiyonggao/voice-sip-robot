#include "bot_config.h"
#include <QRegularExpression>
#include <QUrlQuery>

namespace qwenbot {

QStringList BotConfig::realtimeModelChoices()
{
    return {QStringLiteral("qwen-audio-3.1-realtime-flash"),
            QStringLiteral("qwen-audio-3.1-realtime-plus")};
}

QStringList BotConfig::realtimeRegionChoices()
{
    return {QStringLiteral("cn-beijing"), QStringLiteral("ap-southeast-1")};
}

QString BotConfig::realtimeRegionName(const QString &region)
{
    if (region == QStringLiteral("cn-beijing")) return QStringLiteral("华北2（北京）");
    if (region == QStringLiteral("ap-southeast-1")) return QStringLiteral("新加坡");
    return region;
}

QString BotConfig::defaultRealtimeRegion()
{
    return QStringLiteral("cn-beijing");
}

QUrl BotConfig::buildRealtimeEndpoint(const QString &workspaceId, const QString &region,
                                      const QString &model, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QUrl();
    };
    const QString workspace = workspaceId.trimmed();
    if (workspace.isEmpty())
        return fail(QStringLiteral("请填写 WorkspaceId（阿里云百炼业务空间 ID）。"));
    static const QRegularExpression workspacePattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]{0,63}$"));
    if (!workspacePattern.match(workspace).hasMatch())
        return fail(QStringLiteral(
            "WorkspaceId“%1”非法：仅允许字母、数字和短横线，不能包含协议前缀或路径。")
                        .arg(workspace));
    const QString selectedRegion =
        region.trimmed().isEmpty() ? defaultRealtimeRegion() : region.trimmed();
    if (!realtimeRegionChoices().contains(selectedRegion))
        return fail(QStringLiteral("地域“%1”不受支持：请选择华北2（北京）或新加坡。")
                        .arg(selectedRegion));
    const QString selected = model.trimmed();
    if (selected.isEmpty()) return fail(QStringLiteral("请选择或填写实时语音模型名称。"));
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    if (!pattern.match(selected).hasMatch())
        return fail(QStringLiteral("模型名“%1”非法：仅允许字母、数字、点、下划线和短横线。").arg(selected));
    QUrl url;
    url.setScheme(QStringLiteral("wss"));
    // 官方地域专属域名：{WorkspaceId}.{region}.maas.aliyuncs.com
    url.setHost(workspace + QLatin1Char('.') + selectedRegion
                + QStringLiteral(".maas.aliyuncs.com"));
    url.setPath(QStringLiteral("/api-ws/v1/realtime"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("model"), selected);
    url.setQuery(query);
    return url;
}

bool BotConfig::parseRealtimeEndpoint(const QUrl &endpoint, QString *workspaceId,
                                      QString *region, QString *model)
{
    if (model) *model = QUrlQuery(endpoint).queryItemValue(QStringLiteral("model"));
    if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("wss") ||
        endpoint.path() != QStringLiteral("/api-ws/v1/realtime"))
        return false;
    static const QRegularExpression workspacePattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9-]{0,63}$"));
    const QString host = endpoint.host().toLower();
    for (const auto &candidate : realtimeRegionChoices()) {
        const QString suffix =
            QLatin1Char('.') + candidate + QStringLiteral(".maas.aliyuncs.com");
        if (!host.endsWith(suffix) || host.size() <= suffix.size()) continue;
        const QString prefix = host.left(host.size() - suffix.size());
        if (!workspacePattern.match(prefix).hasMatch()) return false;
        if (workspaceId) *workspaceId = prefix;
        if (region) *region = candidate;
        return true;
    }
    return false;
}

QString BotConfig::effectiveRealtimeModel() const
{
    if (realtimeManualEndpoint) {
        // 手动端点优先用 URL 里的 model 参数（不要求域名符合 Workspace 规则）
        const QString model = QUrlQuery(endpoint).queryItemValue(QStringLiteral("model")).trimmed();
        if (!model.isEmpty()) return model;
    }
    return realtimeModel.trimmed();
}

QString BotConfig::realtimeError() const
{
    if (realtimeManualEndpoint) {
        if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("wss") ||
            endpoint.host().isEmpty())
            return QStringLiteral("手动指定的完整端点无效：必须以 wss:// 开头并包含主机名。");
        if (endpoint.host().contains('{'))
            return QStringLiteral("手动指定的完整端点不能包含 {workspace} 占位符。");
        if (!endpoint.userInfo().isEmpty() || endpoint.hasFragment())
            return QStringLiteral("完整端点不能包含用户名、密码或 # 片段，请通过 API Key 认证。");
        return {};
    }
    QString buildError;
    const QUrl built = buildRealtimeEndpoint(realtimeWorkspaceId, realtimeRegion,
                                             realtimeModel, &buildError);
    if (!buildError.isEmpty()) return buildError;
    if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("wss") ||
        endpoint.host().isEmpty())
        return QStringLiteral("实时语音地址拼装结果无效：请检查 WorkspaceId、地域与模型。");
    if (endpoint != built)
        return QStringLiteral("实时语音地址拼装结果与 WorkspaceId/地域/模型不一致，请重新保存。");
    return {};
}

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
    if (const QString error = realtimeError(); !error.isEmpty()) return error;
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
