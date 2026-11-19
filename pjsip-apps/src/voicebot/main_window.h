#pragma once

#include "bot_config.h"
#include "qwen_session.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QMainWindow>
#include <QStringList>

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSettings;
class QSpinBox;
class QSystemTrayIcon;
class QTableWidget;
class QTableWidgetItem;
class QThread;

namespace qwenbot {

class BotEngine;
class CallRecorder;
class LocalAudioSession;
class ToolRegistry;

// Invalid scalar settings throw std::runtime_error; secrets come only from env.
BotConfig loadConfig(QSettings &settings);
void saveConfig(QSettings &settings, const BotConfig &config);

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(BotEngine &engine, QSettings &settings,
               const BotConfig &config, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct CallRecord {
        QTableWidgetItem *idItem = nullptr;
        int engineId = -1; // 对应的 BotEngine 呼叫 ID（本地对话为 localRowId）
        QString state;
        bool ended = false;
        bool connected = false;
        QElapsedTimer connectedAt;
        qint64 finalDuration = 0;
        QStringList transcript;
        qsizetype transcriptSize = 0;
        // 新增字段
        QString callerNumber;
        QString calleeNumber;
        int turns = 0;
        bool answered = false;
        QString hangupBy;
        QString answerTime;
        QString hangupTime;
        QString usageSummary; // Token 消耗汇总（如 "1.2k"），空为未上报
        QString usageTip;     // Token 细分（悬停显示）
    };

    BotConfig formConfig() const;
    void toggleEngine();
    void setEngineBusy(bool busy, const QString &text);
    void applyRunning(bool running);
    void toggleLocal();
    void applyLocalActive(bool active);
    // 「交互策略」页的实时提示：全双工/半双工语义
    void updateInteractionHint();
    void appendLocalTranscript(const QString &speaker, const QString &text);
    void writeLocalCallRecord(); // 本地对话结束时写入通话记录（含对话转写）
    void setStatusPill(bool running, const QString &text);
    void saveSettings();
    void quitApplication();
    void shutdownAudio();
    void updateCall(int id, const QString &peer, const QString &state);
    // 追加/更新通话表行：rowKey 为表内唯一键，engineId 为 BotEngine 呼叫 ID
    int createCallRow(int engineId, const QString &displayId);
    void updateCallRow(int rowKey, const QString &peer, const QString &state);
    int rowKeyForEngine(int engineId) const;
    void endCall(int rowKey);
    void updateActions();
    void updateDurations();
    void showTranscript();
    void addTranscript(int rowKey, const QString &speaker, const QString &text);
    int selectedRowKey() const;
    void handleLocalUsageUpdated(const ModelUsage &usage); // 本地对话 token 用量累计
    void appendLog(const QString &text);
    // 查看本地持久化数据：通话记录 JSONL（含轮转历史与对话转写）、运行日志文件
    void showRecordHistory();
    void showLogFileViewer();
    QString safeText(const QString &text) const;
    int selectedCall() const;
    void refreshTools(); // 注册表变化时重建工具页两组表格
    // 「工具启用」表格：内置 + MCP 工具，每行只放启用开关
    void refreshToolEnableTable();
    void updateMcpConnectionStatus(); // MCP 连接状态与已发现工具数
    void setToolEnabled(const QString &name, bool enabled); // 即时影响下一次通话
    void testMcpConnections(); // 按当前配置测试连接 / 拉取工具列表（不落盘）
    void addRemoteMcpService(); // 新增 Streamable HTTP 远端服务（立即落盘并热插拔）
    void uploadStdioPlugin();   // 上传本地 stdio 插件文件并注册
    void removeSelectedMcpService(); // 删除所选工具来源配置
    bool persistMcpPlugins(const QString &message); // 写入 tools/plugin 并热插拔
    // 本地对话的 MCP 工具调用路由（不经过 BotEngine）
    void handleLocalToolCall(const QString &callId, const QString &name,
                             const QString &arguments);
    void localToolResultArrived(int token, const QString &name,
                                const QString &outputJson);
    void replyLocalTool(const QString &callId, const QString &outputJson);

    BotEngine &engine_;
    QSettings &settings_;
    ToolRegistry *tools_ = nullptr;
    QHash<int, CallRecord> calls_; // 键为表内唯一行键（跨引擎重启不复用）
    QHash<int, int> liveEngineRows_; // 当前引擎运行期内 呼叫 ID → 表行键
    int nextRowKey_ = 0;
    QWidget *configuration_ = nullptr;
    QLineEdit *listenAddress_ = nullptr;
    QSpinBox *sipPort_ = nullptr;
    QSpinBox *rtpPort_ = nullptr;
    QSpinBox *maxCalls_ = nullptr;
    QPlainTextEdit *allowedPeers_ = nullptr;
    QCheckBox *autoAnswer_ = nullptr;
    QLineEdit *identity_ = nullptr;
    QLineEdit *registrar_ = nullptr;
    QLineEdit *sipUser_ = nullptr;
    QLineEdit *sipPassword_ = nullptr;
    QLineEdit *workspaceId_ = nullptr;
    QComboBox *realtimeRegion_ = nullptr;
    QComboBox *realtimeModel_ = nullptr;
    QLineEdit *customModel_ = nullptr;
    QCheckBox *manualEndpoint_ = nullptr;
    QLineEdit *endpoint_ = nullptr;
    QLineEdit *apiKey_ = nullptr;
    QLineEdit *voice_ = nullptr;
    QPlainTextEdit *instructions_ = nullptr;
    QPlainTextEdit *greeting_ = nullptr;
    QComboBox *turnMode_ = nullptr;
    QSpinBox *uplinkChunkMs_ = nullptr;
    QSpinBox *maxHistoryTurns_ = nullptr;
    QSpinBox *silenceTimeoutSec_ = nullptr;
    QSpinBox *maxSilenceRetries_ = nullptr;
    QSpinBox *maxTurns_ = nullptr;
    QCheckBox *allowBargeIn_ = nullptr;
    QLabel *interactionHint_ = nullptr;        // 全双工/半双工的实时提示
    QLineEdit *silencePrompt_ = nullptr;
    QLineEdit *farewellPrompt_ = nullptr;
    QLineEdit *callRecordPath_ = nullptr;
    QLineEdit *callRecordWebhook_ = nullptr;
    QSpinBox *callRecordMaxSize_ = nullptr;
    QSpinBox *callRecordMaxFiles_ = nullptr;
    QLineEdit *logPath_ = nullptr;
    QSpinBox *logMaxSize_ = nullptr;
    QSpinBox *logMaxFiles_ = nullptr;
    QLineEdit *customVariables_ = nullptr;
    QList<McpPluginSpec> mcpPluginSpecs_; // 已配置的 MCP 服务/本地插件（stdio 或 http）
    QWidget *mcpConfigPanel_ = nullptr;   // 工具页「MCP 工具」操作区（通话中禁用）
    QPushButton *mcpRemoveButton_ = nullptr; // 删除所选工具来源
    QTableWidget *toolEnableTable_ = nullptr; // 「工具启用」：内置 + MCP 工具开关
    QLabel *mcpConnectionStatus_ = nullptr;   // MCP 连接状态 / 已发现工具数
    QStringList mcpDisabledTools_;            // 工具级禁用名单（空 = 全部启用）
    QTableWidget *callTable_ = nullptr;
    QTableWidget *builtinTools_ = nullptr;
    QTableWidget *mcpTools_ = nullptr;
    QLabel *mcpEmptyHint_ = nullptr;
    QPlainTextEdit *transcript_ = nullptr;
    QPlainTextEdit *logs_ = nullptr;
    QLabel *status_ = nullptr;
    QFrame *statusPill_ = nullptr;
    QLabel *statusDot_ = nullptr;
    QLabel *statusPillText_ = nullptr;
    QPushButton *startStop_ = nullptr;
    QPushButton *localToggle_ = nullptr;
    QPushButton *answer_ = nullptr;
    QPushButton *hangup_ = nullptr;
    QPushButton *viewRecords_ = nullptr;
    QPushButton *viewLog_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    QAction *trayToggle_ = nullptr;
    QThread *audioThread_ = nullptr;
    LocalAudioSession *local_ = nullptr;
    bool localActive_ = false;
    QHash<int, QString> pendingLocalToolTokens_; // token → 模型 callId（本地对话）
    int localRowKey_ = -1;   // 当前本地对话在通话表中的行键（内部仍用 -1 作为引擎 ID）
    int localCallSerial_ = 0; // 本地对话递增序号（跨启停保留）
    QString localCallLabel_;  // 展示/落盘用 ID：local-1、local-2…
    QStringList localTranscript_; // 本地麦克风对话的转写，不与通话记录混淆
    // 本地对话记录：与 SIP 通话记录写同一文件/webhook（caller 固定 local）
    CallRecorder *localRecorder_ = nullptr;
    QJsonArray localTranscriptLog_;
    QDateTime localStartTime_;
    int localTurns_ = 0;
    ModelUsage lastLocalUsage_; // 本地对话累计 token 用量
    QString lastLocalFinishReason_;
    QString lastStatus_;
    bool engineBusy_ = false; // 启动/停止执行中：按钮保持禁用并显示进度文案
    bool quitting_ = false;
};

} // namespace qwenbot
