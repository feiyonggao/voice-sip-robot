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
    void applyRunning(bool running);
    void toggleLocal();
    void applyLocalActive(bool active);
    void appendLocalTranscript(const QString &speaker, const QString &text);
    void writeLocalCallRecord(); // 本地对话结束时写入通话记录（含对话转写）
    void setStatusPill(bool running, const QString &text);
    void saveSettings();
    void quitApplication();
    void shutdownAudio();
    void updateCall(int id, const QString &peer, const QString &state);
    void endCall(int id);
    void updateActions();
    void updateDurations();
    void showTranscript();
    void addTranscript(int id, const QString &speaker, const QString &text);
    void handleLocalUsageUpdated(const ModelUsage &usage); // 本地对话 token 用量累计
    void appendLog(const QString &text);
    // 查看本地持久化数据：通话记录 JSONL（含轮转历史与对话转写）、运行日志文件
    void showRecordHistory();
    void showLogFileViewer();
    QString safeText(const QString &text) const;
    int selectedCall() const;
    void refreshTools(); // 注册表变化时重建工具页两组表格
    // 本地对话的 MCP 工具调用路由（不经过 BotEngine）
    void handleLocalToolCall(const QString &callId, const QString &name,
                             const QString &arguments);
    void localToolResultArrived(int token, const QString &name,
                                const QString &outputJson);
    void replyLocalTool(const QString &callId, const QString &outputJson);

    BotEngine &engine_;
    QSettings &settings_;
    ToolRegistry *tools_ = nullptr;
    QHash<int, CallRecord> calls_;
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
    QWidget *mcpPlugins_ = nullptr; // 启用插件标签式多选（实际类型 PluginComboBox）
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
    QStringList localTranscript_; // 本地麦克风对话的转写，不与通话记录混淆
    // 本地对话记录：与 SIP 通话记录写同一文件/webhook（caller 固定 local）
    CallRecorder *localRecorder_ = nullptr;
    QJsonArray localTranscriptLog_;
    QDateTime localStartTime_;
    int localTurns_ = 0;
    ModelUsage lastLocalUsage_; // 本地对话累计 token 用量
    QString lastLocalFinishReason_;
    QString lastStatus_;
    bool quitting_ = false;
};

} // namespace qwenbot
