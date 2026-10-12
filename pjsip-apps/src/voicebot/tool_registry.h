#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTimer>
#include <memory>
#include <vector>

namespace qwenbot {

class McpClient;

// 内置 + MCP 双轨工具注册表：
// - 内置工具（hang_up）在进程内实现，注册后始终可用；
// - MCP 工具由 stdio 子进程插件提供，支持热插拔与断线自动重连。
// BotEngine 在每次通话打开时取 toolsJson() 快照（同线程，无锁），
// 模型的工具调用经 callMcpTool() 转发，结果通过 toolResult 信号回传。
// 所有方法与信号都运行在所属线程（主/GUI 线程）。
class ToolRegistry : public QObject {
    Q_OBJECT
public:
    struct Tool {
        QString name;
        QString description;
        QString server;         // MCP 插件名（内置为空）
        QJsonObject parameters; // JSON Schema（parameters 字段）
        bool builtIn = false;
        bool online = true;     // MCP 插件断线时为 false（界面灰显）
    };

    explicit ToolRegistry(QObject *parent = nullptr);
    ~ToolRegistry() override;

    // 注册内置工具（进程内实现；同名 MCP 工具会被拒绝注册）
    void registerBuiltIn(const QString &name, const QString &description,
                         const QJsonObject &parameters = {});
    // 应用 MCP 插件命令行列表（每条一个 stdio 插件命令，如 python plugins/a.py）。
    // 与当前运行的差异比对：新增启动、移除停止、未变化的保持运行（热插拔）。
    void applyMcpCommands(const QStringList &commands);
    // 当前可用工具 → session.update 的 tools JSON 数组（含内置 hang_up）
    QJsonArray toolsJson() const;
    // 展示用工具清单（内置在前，MCP 按配置顺序）
    QList<Tool> tools() const;
    bool isBuiltIn(const QString &name) const;
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
        QString command; // 完整启动命令行
        QString label;   // 展示名（取脚本/可执行文件名）
        McpClient *client = nullptr; // 生命周期由 ToolRegistry 管理
        bool online = false;
        QList<Tool> tools;
    };
    struct Pending {
        McpServer *server = nullptr;
        int requestId = 0;
        QString name;
        qint64 deadlineMs = 0; // 基于 registry 启动时钟
    };

    void createServer(const QString &command);
    void shutdownServer(int index);
    void handleServerOnline(McpServer *server);
    void handleServerOffline(McpServer *server, const QString &reason);
    void handleServerTools(McpServer *server);
    void handleClientResult(McpServer *server, int requestId, const QString &name,
                            const QString &outputJson);
    void failPendingForServer(McpServer *server, const QString &error);
    void sweepTimeouts();
    McpServer *findServerForTool(const QString &name);
    static QString labelFor(const QString &command);

    QList<Tool> builtins_;
    std::vector<std::unique_ptr<McpServer>> servers_; // move-only，QList 不支持
    QHash<int, Pending> pending_; // token → 调用
    QElapsedTimer clock_;
    QTimer sweepTimer_; // 500ms 扫描超时令牌
    int nextToken_ = 1;
};

} // namespace qwenbot
