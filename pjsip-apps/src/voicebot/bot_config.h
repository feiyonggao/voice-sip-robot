#pragma once

#include "mcp_plugin.h"

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace qwenbot {

struct BotConfig {
    QString listenAddress = QStringLiteral("127.0.0.1");
    quint16 sipPort = 5060;
    quint16 rtpPort = 40000;
    int maxCalls = 4;
    QStringList allowedPeers = {QStringLiteral("127.0.0.1")};
    bool autoAnswer = true;
    QString sipIdentity = QStringLiteral("sip:robot@localhost");
    QString registrar;
    QString sipUser;
    QString sipPassword;
    // 实时语音：WorkspaceId + 地域 + 模型拼装成 endpoint；manualEndpoint 时直接使用 endpoint 原值
    QString realtimeWorkspaceId;
    QString realtimeRegion = QStringLiteral("cn-beijing");
    QString realtimeModel = QStringLiteral("qwen-audio-3.1-realtime-plus");
    bool realtimeManualEndpoint = false;
    QUrl endpoint;
    QString apiKey;
    QString voice = QStringLiteral("longanqian_v3.1");
    QString instructions = QStringLiteral(
        "你是电话语音助手，使用中文，回答简短自然，每次只问一个问题。"
        "当用户明确道别、表示不再需要帮助，或本次任务已完成时，"
        "先向用户简短道别，然后调用 hang_up 工具挂断电话。");
    QString greeting = QStringLiteral("请简短问候来电用户，并询问有什么可以帮助的。");
    QString turnMode = QStringLiteral("smart_turn");
    int silenceTimeoutSec = 6;
    int maxSilenceRetries = 3;
    int maxTurns = 40;
    bool allowBargeIn = true; // 勾选=全双工（麦克风常开、语音打断）；不勾=半双工（播放期间上行静音）
    QString silencePrompt = QStringLiteral(
        "对方长时间没有说话，请简短询问对方是否还在听。");
    QString farewellPrompt = QStringLiteral(
        "请用一句话简短道别，告知对方即将结束通话。");
    QString callRecordPath = QStringLiteral("cdr/calls.jsonl");
    QString callRecordWebhook;
    QStringList customVariables = {QStringLiteral("X-Custom-*")};
    qint64 callRecordMaxSize = 10 * 1024 * 1024; // 单个通话记录文件最大字节数，默认 10MB
    int callRecordMaxFiles = 5;                   // 轮转保留的旧文件数量，默认 5 份
    QString logPath = QStringLiteral("log/voicebot.log"); // 运行日志文件，空串禁用文件日志
    qint64 logMaxSize = 10 * 1024 * 1024;                 // 单个日志文件最大字节数，默认 10MB
    int logMaxFiles = 5;                                   // 日志轮转保留份数，默认 5 份
    int uplinkChunkMs = 20;                       // 上行音频分片时长（毫秒），默认 20ms（官网建议 100ms）
    int maxHistoryTurns = 20;                     // 模型上下文携带的历史问答轮次上限，1-50，默认 20
    QStringList mcpCommands;                      // 兼容旧配置：stdio 插件启动命令
    QStringList enabledPlugins;                   // 兼容旧配置：启用的本地插件文件名
    QList<McpPluginSpec> mcpPlugins;              // 统一插件配置（stdio 与 HTTP 可混用）
    QStringList disabledTools;                    // 工具级禁用名单（空 = 全部启用，兼容旧配置）

    // 内置实时语音模型（界面下拉与校验共用；新增模型只改这里）
    static QStringList realtimeModelChoices();
    // 内置地域（域名规则中的 {region} 段）与展示名
    static QStringList realtimeRegionChoices();
    static QString realtimeRegionName(const QString &region);
    static QString defaultRealtimeRegion();
    // 按 WorkspaceId + 地域 + 模型拼装最终 WSS 端点（失败返回空 URL）
    // 规则：wss://{WorkspaceId}.{region}.maas.aliyuncs.com/api-ws/v1/realtime?model={model}
    static QUrl buildRealtimeEndpoint(const QString &workspaceId, const QString &region,
                                      const QString &model, QString *error = nullptr);
    // 从完整端点拆出 WorkspaceId、地域与 model；返回是否识别出 Workspace 域名
    static bool parseRealtimeEndpoint(const QUrl &endpoint, QString *workspaceId,
                                      QString *region, QString *model);
    // 当前实际生效的模型：手动端点优先从 URL 的 model 参数解析，解析不出回落到配置模型
    QString effectiveRealtimeModel() const;
    // 实时语音配置的独立校验：缺模型、模型名非法、scheme 非 wss、拼装结果异常均返回可读原因
    QString realtimeError() const;

    QString validate() const;
    // 校验单个允许接入条目（IPv4 地址或 CIDR 网段，如 192.168.1.0/24），返回错误描述，空串表示合法
    static QString peerEntryError(const QString &entry);
};

} // namespace qwenbot

// 跨线程排队启动本地对话时需要按值传递配置
Q_DECLARE_METATYPE(qwenbot::BotConfig)
