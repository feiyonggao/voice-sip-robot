#pragma once

#include "mcp_client.h"
#include "mcp_plugin.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <memory>
#include <vector>

namespace qwenbot {

// 内置 + MCP 双轨工具注册表：
// - 内置工具（hang_up）在进程内实现，注册后始终可用；
// - MCP 工具由插件提供（stdio 子进程或 HTTP 服务，可混用），支持热插拔与断线自动重连。
// BotEngine 在每次通话打开时取 toolsJson() 快照（同线程，无锁），
// 模型的工具调用经 callMcpTool() 转发，结果通过 toolResult 信号回传。
// 所有方法与信号都运行在所属线程（主/GUI 线程）。
class ToolRegistry : public QObject {
    Q_OBJECT
public:
    struct Tool {
        QString name;
        QString description;
        QString server;         // MCP 插件配置名（即 tools/plugin/<名称>，内置为空）
        QJsonObject parameters; // JSON Schema（parameters 字段）
        bool builtIn = false;
        bool online = true;     // MCP 插件断线时为 false（界面灰显）
    };

    // 因重名冲突未被注册的工具：保留可追溯状态，避免静默丢弃
    struct IgnoredTool {
        QString name;
        QString description; // tools/list 原始说明
        QString owner;       // 已占名的来源展示名（内置工具为“内置工具”）
    };

    // 单个 MCP 插件的运行状态（GUI 的「工具启用」分区与描述缓存用）
    struct PluginState {
        QString name;          // 配置中的唯一名称
        QString label;         // 展示名（脚本文件名或远端主机名）
        bool online = false;
        QString transport;     // 归一化后的传输方式：http / stdio
        QString lastError;     // 最近一次连接失败原因（在线时为空）
        QList<Tool> tools;     // 已注册的工具（离线时为空）
        QList<IgnoredTool> ignoredTools; // 因与其它来源同名而未注册的工具
    };

    explicit ToolRegistry(QObject *parent = nullptr);
    ~ToolRegistry() override;

    // 注册内置工具（进程内实现；同名 MCP 工具会被拒绝注册）
    void registerBuiltIn(const QString &name, const QString &description,
                         const QJsonObject &parameters = {});
    // 应用统一插件配置（stdio 与 HTTP 可混用）。
    // 与当前运行的差异比对：新增启动、移除停止、配置变化重启、未变化的保持运行（热插拔）。
    void applyMcpPlugins(const QList<McpPluginSpec> &plugins);
    // 兼容旧接口：每条一个 stdio 插件命令（如 python plugins/a.py）
    void applyMcpCommands(const QStringList &commands);
    // 当前可用工具 → session.update 的 tools JSON 数组（含内置 hang_up）
    QJsonArray toolsJson() const;
    // 展示用工具清单（内置在前，MCP 按配置顺序；含离线插件）
    QList<Tool> tools() const;
    // 每个已配置 MCP 插件的连接状态、已注册工具与被重名忽略的工具
    QList<PluginState> pluginStates() const;
    bool isBuiltIn(const QString &name) const;
    // 工具级启停：仅影响 toolsJson() 的下一次快照（新通话），
    // 不修改正在运行的通话已锁定的工具集，也不影响 callMcpTool 分发。
    void setDisabledTools(const QStringList &names);
    QStringList disabledTools() const;
    // 尚未完成首次工具发现的 MCP 插件数量（无头模式启动等待用）
    int pendingMcpCount() const;

    // 转发一次 MCP 工具调用，返回调用令牌；工具不存在/插件离线返回 -1。
    // 结果（含 10 秒超时错误）经 toolResult 信号回传，每个令牌只回传一次。
    int callMcpTool(const QString &name, const QJsonObject &arguments);

    // ---------- 插件目录：构建时打包在产物旁（<prefix>/plugins），按相对路径定位 ----------
    // 插件目录绝对路径；目录不存在时仍返回首选候选（供日志提示）。
    // 可用环境变量 VOICEBOT_PLUGINS_DIR 覆盖。
    static QString pluginsDir();
    // 扫描插件目录中的插件文件（*.py/*.js/*.mjs，按文件名排序）
    static QStringList scanPluginFiles();
    // 插件文件名 → 完整启动命令（自动选择 python/python3/node 解释器 + 绝对路径）；
    // 返回空串表示文件不存在或解释器未安装。
    static QString commandForPlugin(const QString &fileName);
    // 扫描插件源码中的第三方依赖包（解析 Python import / JS require，过滤标准库）
    static QStringList pluginDependencies(const QString &fileName);

signals:
    void toolsChanged(); // 工具集变化（上线/下线/热插拔）
    void toolResult(int token, const QString &name, const QString &outputJson);
    void logMessage(const QString &text);

private:
    struct McpServer {
        McpPluginSpec spec;
        QString key;                 // 差异比对键
        QString label;               // 展示名（取脚本文件名或远端主机名）
        McpClient *client = nullptr; // 生命周期由 ToolRegistry 管理
        bool online = false;
        QString lastError;           // 最近一次连接失败原因（在线时清空）
        QList<McpClient::ToolDesc> discovered; // 最近一次 tools/list 原始结果
        QList<Tool> tools;                     // 合并后实际注册的工具
        QList<IgnoredTool> ignored;            // 因重名未注册的工具
        QString sourceStamp;                   // stdio 插件文件指纹（mtime:size:sha256）
    };
    struct Pending {
        McpServer *server = nullptr;
        int requestId = 0;
        QString name;
        qint64 deadlineMs = 0; // 基于 registry 启动时钟
    };

    void createServer(const McpPluginSpec &spec);
    void shutdownServer(int index);
    void handleServerOnline(McpServer *server);
    void handleServerOffline(McpServer *server, const QString &reason);
    void handleServerTools(McpServer *server);
    // 按配置顺序重新合并所有插件的 tools/list 结果：先到者占名，
    // 后来者记为「重名冲突」；占名来源离线/被移除后同名工具自动接替。
    // 返回已注册工具集是否因此发生变化。
    bool rebuildToolSets();
    void handleClientResult(McpServer *server, int requestId, const QString &name,
                            const QString &outputJson);
    void failPendingForServer(McpServer *server, const QString &error);
    void sweepTimeouts();
    static QString labelFor(const QString &command);
    static QString labelForUrl(const QString &url);

    QList<Tool> builtins_;
    std::vector<std::unique_ptr<McpServer>> servers_; // move-only，QList 不支持
    QHash<int, Pending> pending_; // token → 调用
    QSet<QString> disabledTools_; // 工具级启停：仅从 toolsJson() 快照中剔除
    QSet<QString> conflictLogged_; // 已记录过日志的重名冲突，冲突解除后清除
    QElapsedTimer clock_;
    QTimer sweepTimer_; // 500ms 扫描超时令牌
    int nextToken_ = 1;
};

} // namespace qwenbot

