#include "main_window.h"
#include "bot_engine.h"
#include "call_record.h"
#include "file_logger.h"
#include "local_audio.h"
#include "tool_registry.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QMouseEvent>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QSysInfo>
#include <stdexcept>

#include <pj/config.h> // PJ_VERSION_NUM_*：PJSIP 版本号

namespace qwenbot {
namespace {
constexpr int maxHistoryCalls = 128;
constexpr qsizetype maxTranscriptLines = 300;
constexpr qsizetype maxTranscriptCharacters = 60000;
constexpr qsizetype maxEntryCharacters = 8000;

// 关于页展示文案：开发者 / 联系方式等按实际情况修改
const QString aboutDeveloper = QStringLiteral("feiyonggao");
const QString aboutContact = QStringLiteral("QQ：736984328 · QQ群：482489124 · github.com/feiyonggao/voice-sip-robot");
const QString aboutCopyright = QStringLiteral("© 2026 feiyonggao · 基于 PJSIP (GPL)");

int integerSetting(QSettings &settings, const QString &key, int fallback,
                   int minimum, int maximum)
{
    bool ok = false;
    const int result = settings.value(key, fallback).toString().toInt(&ok);
    if (!ok || result < minimum || result > maximum) {
        throw std::runtime_error(
            QStringLiteral("配置项 %1 必须是 %2–%3 的整数")
                .arg(key).arg(minimum).arg(maximum).toStdString());
    }
    return result;
}

bool booleanSetting(QSettings &settings, const QString &key, bool fallback)
{
    const QString value = settings.value(key, fallback).toString().toLower();
    if (value == QStringLiteral("true") || value == QStringLiteral("1"))
        return true;
    if (value == QStringLiteral("false") || value == QStringLiteral("0"))
        return false;
    throw std::runtime_error(
        QStringLiteral("配置项 %1 必须是 true 或 false").arg(key).toStdString());
}

// 通话记录路径归一化：注册表中键存在但为空（QSettings 此时不会返回默认值）、
// 或值为旧版内置默认路径时，一律回落到当前代码默认 cdr/calls.jsonl
QString normalizeRecordPath(const QString &raw)
{
    const QString value = raw.trimmed();
    static const QStringList legacyDefaults{
        QStringLiteral("./call_records.jsonl"),
        QStringLiteral("call_records.jsonl")};
    if (value.isEmpty() || legacyDefaults.contains(value))
        return BotConfig().callRecordPath;
    return value;
}

// 把配置里的记录/日志相对路径解析为绝对路径（与写入侧一致：基于程序所在目录）
QString resolveDataPath(const QString &raw, const QString &fallback)
{
    QString path = raw.trimmed();
    if (path.isEmpty())
        path = fallback;
    if (QDir(path).isRelative())
        path = QCoreApplication::applicationDirPath() + QLatin1Char('/') + path;
    return QDir::cleanPath(path);
}

// 通话表中本地对话行的保留 ID（SIP 呼叫 ID 为非负整数，-1 不会冲突）
constexpr int localRowId = -1;

// 模型 token 用量 → 表格展示文本（汇总 + 悬停细分）
QPair<QString, QString> usageTexts(const ModelUsage &usage)
{
    if (!usage.valid)
        return {QStringLiteral("-"), {}};
    const qint64 total = usage.inputTokens + usage.outputTokens;
    const QString summary = total >= 1000
        ? QStringLiteral("%1.%2k").arg(total / 1000).arg((total / 100) % 10)
        : QString::number(total);
    const QString tip = QStringLiteral(
        "输入 %1（音频 %2 · 文本 %3 · 缓存 %4）\n输出 %5（音频 %6 · 文本 %7）\n合计 %8")
        .arg(usage.inputTokens).arg(usage.inputAudioTokens).arg(usage.inputTextTokens)
        .arg(usage.cachedTokens).arg(usage.outputTokens).arg(usage.outputAudioTokens)
        .arg(usage.outputTextTokens).arg(total);
    return {summary, tip};
}

QUrl modelEndpoint(const QString &text)
{
    if (text.trimmed().isEmpty())
        return {};
    QUrl url(text.trimmed(), QUrl::StrictMode);
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("model"));
    query.addQueryItem(QStringLiteral("model"),
                       QStringLiteral("qwen-audio-3.1-realtime-plus"));
    url.setQuery(query);
    return url;
}

QString durationText(qint64 milliseconds)
{
    const qint64 seconds = milliseconds / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// MCP 插件命令行解析：仅按中英文逗号分隔后逐项去空白（命令本身可含空格参数）
QStringList parseMcpCommands(const QString &text)
{
    QStringList commands;
    for (const auto &part : text.split(QRegularExpression(QStringLiteral("[,，]")), Qt::SkipEmptyParts)) {
        const QString command = part.trimmed();
        if (!command.isEmpty())
            commands.append(command);
    }
    return commands;
}

QString stateText(const QString &state)
{
    if (state == QStringLiteral("INCOMING")) return QStringLiteral("来电");
    if (state == QStringLiteral("EARLY")) return QStringLiteral("振铃");
    if (state == QStringLiteral("CONNECTING")) return QStringLiteral("连接中");
    if (state == QStringLiteral("CONFIRMED")) return QStringLiteral("通话中");
    if (state == QStringLiteral("DISCONNECTED")) return QStringLiteral("已结束");
    return state;
}

QString hangupByText(const QString &hangupBy)
{
    if (hangupBy == QStringLiteral("callee")) return QStringLiteral("被叫挂断");
    if (hangupBy == QStringLiteral("caller")) return QStringLiteral("主叫挂断");
    return {};
}

// 配置页位于 QScrollArea 内，滚轮经过 SpinBox/ComboBox 时会被控件吃掉变成调值，
// 极易误改配置。未聚焦时拦截滚轮并转交父控件（冒泡到滚动区域正常翻页），
// 点击聚焦后才允许用滚轮调节数值。
class WheelGuard final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Wheel) {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget && !widget->hasFocus()) {
                event->ignore();
                if (auto *parent = widget->parentWidget())
                    QCoreApplication::sendEvent(parent, event);
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

// 插件多选控件（标签式）：选中项显示为可单独删除的圆角标签；点击主体弹出
// 勾选列表，已选项右侧显示蓝色 ✓，右侧空状态 ▾ / 有标签时变为清空全部 ×。
// 不用 Q_OBJECT：交互通过事件过滤器与 lambda 完成。
class PluginComboBox : public QWidget {
public:
    explicit PluginComboBox(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(36);
        auto *outer = new QHBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        host_ = new QFrame(this);
        host_->setObjectName(QStringLiteral("pluginHost"));
        outer->addWidget(host_);

        row_ = new QHBoxLayout(host_);
        row_->setContentsMargins(5, 3, 5, 3);
        row_->setSpacing(4);
        placeholder_ = new QLabel(QStringLiteral("点击选择要启用的插件…"), host_);
        placeholder_->setStyleSheet(
            QStringLiteral("color:#9ca3af;border:none;background:transparent;"));
        placeholder_->setAttribute(Qt::WA_TransparentForMouseEvents);
        row_->addWidget(placeholder_);
        row_->addStretch(1);

        sideButton_ = new QToolButton(host_);
        sideButton_->setAutoRaise(true);
        sideButton_->setFixedSize(20, 24);
        sideButton_->setCursor(Qt::PointingHandCursor);
        sideButton_->setText(QStringLiteral("\xe2\x96\xbe")); // ▾
        row_->addWidget(sideButton_);
        connect(sideButton_, &QToolButton::clicked, this, [this] {
            // 空：打开下拉；有标签：清空全部
            if (checked_.isEmpty()) showPluginPopup();
            else { checked_.clear(); rebuildChips(); }
        });
        applyHostStyle(false);
    }
    QStringList checkedFiles() const { return checked_; }
    void setCheckedFiles(const QStringList &files)
    {
        const QStringList available = ToolRegistry::scanPluginFiles();
        checked_.clear();
        for (const auto &file : files)
            if (available.contains(file) && !checked_.contains(file))
                checked_.append(file);
        rebuildChips();
    }
protected:
    void mousePressEvent(QMouseEvent *) override { showPluginPopup(); }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            const QString file = watched->property("pluginFile").toString();
            if (!file.isEmpty()) {
                toggleFile(file);
                return true; // 拦截：不关闭弹出层，可连续多选
            }
        }
        if (watched == popup_ && event->type() == QEvent::Hide)
            applyHostStyle(false); // 弹出层关闭恢复边框
        return QWidget::eventFilter(watched, event);
    }
private:
    void toggleFile(const QString &file)
    {
        if (checked_.contains(file)) checked_.removeAll(file);
        else checked_.append(file);
        if (auto *label = checkLabels_.value(file, nullptr))
            label->setVisible(checked_.contains(file));
        rebuildChips();
    }
    void showPluginPopup()
    {
        rescan(); // 每次打开重新扫描，目录中新放入的插件即时可见
        if (popup_) { popup_->deleteLater(); popup_ = nullptr; }
        popup_ = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
        popup_->setObjectName(QStringLiteral("pluginPopup"));
        popup_->setStyleSheet(QStringLiteral(
            "#pluginPopup { background:#ffffff; border:1px solid #e5e7eb;"
            " border-radius:8px; }"));
        auto *layout = new QVBoxLayout(popup_);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(2);
        checkLabels_.clear();

        const QStringList files = ToolRegistry::scanPluginFiles();
        if (files.isEmpty()) {
            auto *empty = new QLabel(QStringLiteral("插件目录中没有可用插件"), popup_);
            empty->setStyleSheet(
                QStringLiteral("color:#9ca3af;padding:8px;border:none;"));
            layout->addWidget(empty);
        }
        for (const auto &file : files) {
            auto *item = new QWidget(popup_);
            item->setFixedHeight(32);
            item->setCursor(Qt::PointingHandCursor);
            item->setProperty("pluginFile", file);
            item->setStyleSheet(QStringLiteral(
                "QWidget { border-radius:4px; background:transparent; }"
                "QWidget:hover { background:#eef2ff; }"));
            item->installEventFilter(this);
            auto *itemRow = new QHBoxLayout(item);
            itemRow->setContentsMargins(8, 0, 8, 0);
            auto *name = new QLabel(file, item);
            name->setStyleSheet(QStringLiteral(
                "color:#374151;border:none;background:transparent;"));
            itemRow->addWidget(name);
            itemRow->addStretch(1);
            auto *check = new QLabel(QStringLiteral("\xe2\x9c\x93"), item); // ✓
            check->setStyleSheet(QStringLiteral(
                "color:#4f46e5;font-weight:bold;border:none;background:transparent;"));
            check->setVisible(checked_.contains(file));
            checkLabels_.insert(file, check);
            itemRow->addWidget(check);
            layout->addWidget(item);
        }
        popup_->setFixedWidth(width());
        const int heightHint = qMin(files.size(), 8) * 34 + 12;
        popup_->setFixedHeight(qMax(40, heightHint));
        popup_->move(mapToGlobal(QPoint(0, height() + 2)));
        popup_->installEventFilter(this);
        applyHostStyle(true); // 弹出时高亮边框
        popup_->show();
    }
    void rescan()
    {
        const QStringList files = ToolRegistry::scanPluginFiles();
        QStringList kept; // 已勾选但文件已被删除的条目自动失效
        for (const auto &file : checked_)
            if (files.contains(file)) kept.append(file);
        checked_ = kept;
        rebuildChips();
    }
    void rebuildChips()
    {
        // 清理旧标签
        for (auto *chip : chips_) { row_->removeWidget(chip); chip->deleteLater(); }
        chips_.clear();
        const int insertAt = row_->indexOf(placeholder_);
        for (const auto &file : checked_) {
            auto *chip = new QFrame(host_);
            chip->setObjectName(QStringLiteral("pluginChip"));
            chip->setStyleSheet(QStringLiteral(
                "#pluginChip { background:#f3f4f6; border:1px solid #e5e7eb;"
                " border-radius:4px; }"));
            auto *chipRow = new QHBoxLayout(chip);
            chipRow->setContentsMargins(6, 1, 3, 1);
            chipRow->setSpacing(4);
            // 展示名去掉扩展名
            auto *name = new QLabel(QFileInfo(file).completeBaseName(), chip);
            name->setStyleSheet(QStringLiteral(
                "color:#374151;border:none;background:transparent;"));
            chipRow->addWidget(name);
            auto *remove = new QToolButton(chip);
            remove->setAutoRaise(true);
            remove->setFixedSize(16, 16);
            remove->setCursor(Qt::PointingHandCursor);
            remove->setText(QStringLiteral("\xc3\x97")); // ×
            remove->setStyleSheet(QStringLiteral(
                "QToolButton { border:none;background:transparent;color:#9ca3af;"
                " font-weight:bold; }"
                "QToolButton:hover { color:#ef4444; }"));
            chipRow->addWidget(remove);
            connect(remove, &QToolButton::clicked, this, [this, file] {
                checked_.removeAll(file);
                rebuildChips();
            });
            // 标签本体点击穿透到主体（弹出下拉），仅 × 可交互
            chip->setAttribute(Qt::WA_TransparentForMouseEvents);
            name->setAttribute(Qt::WA_TransparentForMouseEvents);
            row_->insertWidget(insertAt, chip);
            chips_.append(chip);
        }
        placeholder_->setVisible(checked_.isEmpty());
        // 空 ▾（打开）/ 有标签 ×（清空全部）
        sideButton_->setText(checked_.isEmpty()
            ? QStringLiteral("\xe2\x96\xbe") : QStringLiteral("\xc3\x97"));
        sideButton_->setStyleSheet(checked_.isEmpty()
            ? QStringLiteral("QToolButton{border:none;background:transparent;"
                             "color:#6b7280;font-weight:bold;}"
                             "QToolButton:hover{color:#4f46e5;}")
            : QStringLiteral("QToolButton{border:none;background:transparent;"
                             "color:#6b7280;font-weight:bold;}"
                             "QToolButton:hover{color:#ef4444;}"));
        setToolTip(checked_.isEmpty() ? QString()
                    : QStringLiteral("已启用：%1").arg(checked_.join(QStringLiteral(", "))));
    }
    void applyHostStyle(bool active)
    {
        host_->setStyleSheet(active
            ? QStringLiteral("#pluginHost { background:#ffffff;"
                             " border:1px solid #4f46e5; border-radius:6px; }")
            : QStringLiteral("#pluginHost { background:#ffffff;"
                             " border:1px solid #d1d5db; border-radius:6px; }"));
    }
    QFrame *host_ = nullptr;
    QHBoxLayout *row_ = nullptr;
    QLabel *placeholder_ = nullptr;
    QToolButton *sideButton_ = nullptr;
    QFrame *popup_ = nullptr;
    QList<QFrame *> chips_;
    QHash<QString, QLabel *> checkLabels_;
    QStringList checked_;
};
} // namespace

BotConfig loadConfig(QSettings &settings)
{
    if (settings.status() != QSettings::NoError)
        throw std::runtime_error("无法读取配置文件");
    BotConfig config;
    config.listenAddress = settings.value(QStringLiteral("sip/listenAddress"), config.listenAddress).toString();
    config.sipPort = static_cast<quint16>(integerSetting(settings, QStringLiteral("sip/port"), config.sipPort, 1, 65535));
    config.rtpPort = static_cast<quint16>(integerSetting(settings, QStringLiteral("sip/rtpPort"), config.rtpPort, 1, 65535));
    config.maxCalls = integerSetting(settings, QStringLiteral("sip/maxCalls"), config.maxCalls, 1, 180);
    config.allowedPeers = settings.value(QStringLiteral("sip/allowedPeers"), config.allowedPeers).toStringList();
    config.autoAnswer = booleanSetting(settings, QStringLiteral("sip/autoAnswer"), config.autoAnswer);
    config.sipIdentity = settings.value(QStringLiteral("sip/identity"), config.sipIdentity).toString();
    config.registrar = settings.value(QStringLiteral("sip/registrar"), config.registrar).toString();
    config.sipUser = settings.value(QStringLiteral("sip/user"), config.sipUser).toString();
    // SIP 密码：优先环境变量，其次配置文件（建议用环境变量避免明文落盘）
    config.sipPassword = qEnvironmentVariable("QWENBOT_SIP_PASSWORD");
    if (config.sipPassword.isEmpty())
        config.sipPassword = settings.value(QStringLiteral("sip/password")).toString();
    config.endpoint = modelEndpoint(settings.value(QStringLiteral("qwen/endpoint")).toString());
    // API Key：优先环境变量，其次配置文件（建议用环境变量避免明文落盘）
    config.apiKey = qEnvironmentVariable("DASHSCOPE_API_KEY");
    if (config.apiKey.isEmpty())
        config.apiKey = settings.value(QStringLiteral("qwen/apiKey")).toString().trimmed();
    config.voice = settings.value(QStringLiteral("qwen/voice"), config.voice).toString();
    config.instructions = settings.value(QStringLiteral("qwen/instructions"), config.instructions).toString();
    config.greeting = settings.value(QStringLiteral("qwen/greeting"), config.greeting).toString();
    config.turnMode = settings.value(QStringLiteral("qwen/turnMode"), config.turnMode).toString();
    config.maxHistoryTurns = integerSetting(settings, QStringLiteral("qwen/maxHistoryTurns"), config.maxHistoryTurns, 1, 50);
    config.uplinkChunkMs = integerSetting(settings, QStringLiteral("qwen/uplinkChunkMs"), config.uplinkChunkMs, 20, 100);
    config.silenceTimeoutSec = integerSetting(settings, QStringLiteral("interaction/silenceTimeoutSec"), config.silenceTimeoutSec, 1, 60);
    config.maxSilenceRetries = integerSetting(settings, QStringLiteral("interaction/maxSilenceRetries"), config.maxSilenceRetries, 1, 10);
    config.maxTurns = integerSetting(settings, QStringLiteral("interaction/maxTurns"), config.maxTurns, 1, 200);
    config.allowBargeIn = booleanSetting(settings, QStringLiteral("interaction/allowBargeIn"), config.allowBargeIn);
    config.silencePrompt = settings.value(QStringLiteral("interaction/silencePrompt"), config.silencePrompt).toString();
    config.farewellPrompt = settings.value(QStringLiteral("interaction/farewellPrompt"), config.farewellPrompt).toString();
    config.callRecordPath = normalizeRecordPath(
        settings.value(QStringLiteral("record/callRecordPath")).toString());
    config.callRecordWebhook = settings.value(QStringLiteral("record/callRecordWebhook"), config.callRecordWebhook).toString();
    config.callRecordMaxSize = settings.value(QStringLiteral("record/callRecordMaxSize"), config.callRecordMaxSize).toLongLong();
    config.callRecordMaxFiles = settings.value(QStringLiteral("record/callRecordMaxFiles"), config.callRecordMaxFiles).toInt();
    config.customVariables = settings.value(QStringLiteral("record/customVariables"), config.customVariables).toStringList();
    // 日志路径：空值回落到默认 log/voicebot.log（键存在但为空时 QSettings 不返回默认值）
    config.logPath = settings.value(QStringLiteral("log/path"), config.logPath).toString().trimmed();
    if (config.logPath.isEmpty())
        config.logPath = BotConfig().logPath;
    config.logMaxSize = settings.value(QStringLiteral("log/maxSize"), config.logMaxSize).toLongLong();
    config.logMaxFiles = settings.value(QStringLiteral("log/maxFiles"), config.logMaxFiles).toInt();
    if (settings.contains(QStringLiteral("tools/enabledPlugins"))) {
        // 新格式：只存启用的插件文件名，运行时解析解释器与打包目录中的绝对路径
        config.enabledPlugins = settings.value(QStringLiteral("tools/enabledPlugins")).toStringList();
        for (const auto &file : config.enabledPlugins) {
            if (const QString command = ToolRegistry::commandForPlugin(file); !command.isEmpty())
                config.mcpCommands.append(command);
        }
    } else {
        // 兼容旧配置 tools/mcpCommands（完整命令行），并尝试反推文件名供 GUI 勾选迁移
        config.mcpCommands = parseMcpCommands(settings.value(QStringLiteral("tools/mcpCommands")).toString());
        const QStringList available = ToolRegistry::scanPluginFiles();
        for (const auto &command : config.mcpCommands) {
            const QStringList parts = QProcess::splitCommand(command);
            if (parts.isEmpty()) continue;
            const QString file = QFileInfo(parts.constLast()).fileName();
            if (available.contains(file) && !config.enabledPlugins.contains(file))
                config.enabledPlugins.append(file);
        }
    }
    if (settings.status() != QSettings::NoError)
        throw std::runtime_error("配置文件格式或读取错误");
    return config;
}

void saveConfig(QSettings &settings, const BotConfig &config)
{
    settings.setValue(QStringLiteral("sip/listenAddress"), config.listenAddress);
    settings.setValue(QStringLiteral("sip/port"), config.sipPort);
    settings.setValue(QStringLiteral("sip/rtpPort"), config.rtpPort);
    settings.setValue(QStringLiteral("sip/maxCalls"), config.maxCalls);
    settings.setValue(QStringLiteral("sip/allowedPeers"), config.allowedPeers);
    settings.setValue(QStringLiteral("sip/autoAnswer"), config.autoAnswer);
    settings.setValue(QStringLiteral("sip/identity"), config.sipIdentity);
    settings.setValue(QStringLiteral("sip/registrar"), config.registrar);
    settings.setValue(QStringLiteral("sip/user"), config.sipUser);
    settings.setValue(QStringLiteral("qwen/endpoint"), config.endpoint.toString());
    settings.setValue(QStringLiteral("qwen/voice"), config.voice);
    settings.setValue(QStringLiteral("qwen/instructions"), config.instructions);
    settings.setValue(QStringLiteral("qwen/greeting"), config.greeting);
    settings.setValue(QStringLiteral("qwen/turnMode"), config.turnMode);
    settings.setValue(QStringLiteral("qwen/maxHistoryTurns"), config.maxHistoryTurns);
    settings.setValue(QStringLiteral("qwen/uplinkChunkMs"), config.uplinkChunkMs);
    settings.setValue(QStringLiteral("interaction/silenceTimeoutSec"), config.silenceTimeoutSec);
    settings.setValue(QStringLiteral("interaction/maxSilenceRetries"), config.maxSilenceRetries);
    settings.setValue(QStringLiteral("interaction/maxTurns"), config.maxTurns);
    settings.setValue(QStringLiteral("interaction/allowBargeIn"), config.allowBargeIn);
    settings.setValue(QStringLiteral("interaction/silencePrompt"), config.silencePrompt);
    settings.setValue(QStringLiteral("interaction/farewellPrompt"), config.farewellPrompt);
    settings.setValue(QStringLiteral("record/callRecordPath"), normalizeRecordPath(config.callRecordPath));
    settings.setValue(QStringLiteral("record/callRecordWebhook"), config.callRecordWebhook);
    settings.setValue(QStringLiteral("record/callRecordMaxSize"), config.callRecordMaxSize);
    settings.setValue(QStringLiteral("record/callRecordMaxFiles"), config.callRecordMaxFiles);
    settings.setValue(QStringLiteral("record/customVariables"), config.customVariables);
    settings.setValue(QStringLiteral("log/path"), config.logPath.trimmed().isEmpty()
                                                      ? BotConfig().logPath : config.logPath.trimmed());
    settings.setValue(QStringLiteral("log/maxSize"), config.logMaxSize);
    settings.setValue(QStringLiteral("log/maxFiles"), config.logMaxFiles);
    settings.setValue(QStringLiteral("tools/enabledPlugins"), config.enabledPlugins);
    settings.remove(QStringLiteral("tools/mcpCommands"));
    // 密钥不写入配置文件（loadConfig 会从配置文件读取，但保存时清除，避免 GUI 保存时意外落盘）
    settings.remove(QStringLiteral("sip/password"));
    settings.remove(QStringLiteral("qwen/apiKey"));
    settings.sync();
}

MainWindow::MainWindow(BotEngine &engine, QSettings &settings,
                       const BotConfig &config, QWidget *parent)
    : QMainWindow(parent), engine_(engine), settings_(settings)
{
    setWindowTitle(QStringLiteral("Voice SIP Robot · 语音机器人"));
    resize(1240, 760);
    setMinimumSize(1040, 620);
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(14);
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(12);
    auto *avatar = new QLabel(QStringLiteral("V"), this);
    avatar->setObjectName(QStringLiteral("appAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    toolbar->addWidget(avatar);
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(1);
    auto *title = new QLabel(QStringLiteral("Voice SIP Robot"), this);
    title->setObjectName(QStringLiteral("appTitle"));
    auto *subtitle = new QLabel(QStringLiteral("Qwen-Audio 实时语音对话 · SIP智能AI电话智能体"), this);
    subtitle->setObjectName(QStringLiteral("appSubtitle"));
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    toolbar->addLayout(titleColumn);
    toolbar->addStretch();
    statusPill_ = new QFrame(this);
    statusPill_->setObjectName(QStringLiteral("statusPill"));
    statusPill_->setProperty("running", false);
    auto *pillLayout = new QHBoxLayout(statusPill_);
    pillLayout->setContentsMargins(10, 0, 12, 0);
    pillLayout->setSpacing(7);
    statusDot_ = new QLabel(statusPill_);
    statusDot_->setObjectName(QStringLiteral("statusDot"));
    statusDot_->setProperty("running", false);
    statusPillText_ = new QLabel(QStringLiteral("未启动"), statusPill_);
    statusPillText_->setObjectName(QStringLiteral("statusPillText"));
    pillLayout->addWidget(statusDot_);
    pillLayout->addWidget(statusPillText_);
    toolbar->addWidget(statusPill_);
    startStop_ = new QPushButton(QStringLiteral("启动机器人"), this);
    toolbar->addWidget(startStop_);
    localToggle_ = new QPushButton(QStringLiteral("开始本地对话"), this);
    localToggle_->setToolTip(QStringLiteral(
        "不走 SIP 通话，直接用本地麦克风/扬声器与模型对话，用于快速测试话术。\n"
        "与机器人服务互斥；建议佩戴耳机，以免扬声器回声误触发打断。"));
    toolbar->addWidget(localToggle_);
    layout->addLayout(toolbar);
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    layout->addWidget(tabs, 1);
    status_ = new QLabel(QStringLiteral("就绪"), this);
    status_->setObjectName(QStringLiteral("footerStatus"));
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    setCentralWidget(central);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // 配置页只纵向滚动；字段最小宽度已按默认窗口设计，不出现横向滚动条
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    configuration_ = new QWidget(scroll);
    auto *configurationLayout = new QVBoxLayout(configuration_);
    configurationLayout->setContentsMargins(4, 4, 4, 4);
    configurationLayout->setSpacing(14);
    auto *sipBox = new QGroupBox(QStringLiteral("SIP / RTP · UDP / IPv4 入站服务"), this);
    auto *sipGrid = new QGridLayout(sipBox);
    sipGrid->setVerticalSpacing(10);
    sipGrid->setHorizontalSpacing(12);
    sipGrid->setColumnStretch(1, 4);
    sipGrid->setColumnStretch(3, 3);
    sipGrid->setColumnStretch(5, 1);
    sipGrid->setColumnStretch(7, 1);
    auto line = [this](const QString &value) { return new QLineEdit(value, this); };
    auto port = [this](int value) {
        auto *field = new QSpinBox(this);
        field->setRange(1, 65535);
        field->setValue(value);
        return field;
    };
    listenAddress_ = line(config.listenAddress);
    sipPort_ = port(config.sipPort);
    rtpPort_ = port(config.rtpPort);
    maxCalls_ = new QSpinBox(this);
    maxCalls_->setRange(1, 180);
    maxCalls_->setValue(config.maxCalls);
    allowedPeers_ = new QPlainTextEdit(config.allowedPeers.join(QStringLiteral(", ")), this);
    allowedPeers_->setMaximumHeight(56);
    allowedPeers_->setPlaceholderText(QStringLiteral("IPv4 地址或网段（如 192.168.1.0/24），空格/逗号分隔，双击可放大编辑"));
    autoAnswer_ = new QCheckBox(QStringLiteral("自动接听允许的来电"), this);
    autoAnswer_->setChecked(config.autoAnswer);
    identity_ = line(config.sipIdentity);
    identity_->setMinimumWidth(235); // 完整显示 sip:user@host:port
    registrar_ = line(config.registrar);
    registrar_->setPlaceholderText(QStringLiteral("可选，如 sip:pbx.example.com"));
    registrar_->setMinimumWidth(190); // 完整显示 sip:host:port
    sipUser_ = line(config.sipUser);
    sipPassword_ = line(config.sipPassword);
    sipPassword_->setEchoMode(QLineEdit::Password);
    sipPassword_->setPlaceholderText(QStringLiteral("仅本次会话 / QWENBOT_SIP_PASSWORD"));
    auto *maxCallsLabel = new QLabel(QStringLiteral("最大并发"), this);
    const QString maxCallsTip = QStringLiteral(
        "单实例同时处理的最大通话数（1–180）。\n"
        "高并发（>32）请确认阿里 Qwen 实例规格是否支持相应并发连接数。\n"
        "RTP 端口段需预留 并发数×2 个端口。");
    maxCallsLabel->setToolTip(maxCallsTip);
    maxCalls_->setToolTip(maxCallsTip);
    sipGrid->addWidget(new QLabel(QStringLiteral("监听地址"), this), 0, 0);
    sipGrid->addWidget(listenAddress_, 0, 1);
    sipGrid->addWidget(new QLabel(QStringLiteral("SIP 端口"), this), 0, 2);
    sipGrid->addWidget(sipPort_, 0, 3);
    sipGrid->addWidget(new QLabel(QStringLiteral("RTP 起始端口"), this), 0, 4);
    sipGrid->addWidget(rtpPort_, 0, 5);
    sipGrid->addWidget(maxCallsLabel, 0, 6);
    sipGrid->addWidget(maxCalls_, 0, 7);
    sipGrid->addWidget(autoAnswer_, 0, 8, 1, 2);
    sipGrid->addWidget(new QLabel(QStringLiteral("SIP 身份 URI"), this), 1, 0);
    sipGrid->addWidget(identity_, 1, 1);
    sipGrid->addWidget(new QLabel(QStringLiteral("注册服务器"), this), 1, 2);
    sipGrid->addWidget(registrar_, 1, 3);
    sipGrid->addWidget(new QLabel(QStringLiteral("认证用户名"), this), 1, 4);
    sipGrid->addWidget(sipUser_, 1, 5);
    sipGrid->addWidget(new QLabel(QStringLiteral("认证密码"), this), 1, 6);
    sipGrid->addWidget(sipPassword_, 1, 7, 1, 3); // 延伸到行尾，与第一行总宽对齐
    sipGrid->addWidget(new QLabel(QStringLiteral("允许的对端 IP"), this), 2, 0, Qt::AlignTop);
    sipGrid->addWidget(allowedPeers_, 2, 1, 1, 9);
    configurationLayout->addWidget(sipBox);

    auto *qwenBox = new QGroupBox(QStringLiteral("Qwen 实时语音"), this);
    auto *qwenGrid = new QGridLayout(qwenBox);
    qwenGrid->setVerticalSpacing(10);
    qwenGrid->setHorizontalSpacing(10);
    qwenGrid->setColumnMinimumWidth(0, 96);
    // API Key 共 35 字符需要更宽的输入框；其余字段列不拉伸，输入框紧凑且各列对齐
    qwenGrid->setColumnStretch(1, 1);
    endpoint_ = line(config.endpoint.toString());
    endpoint_->setPlaceholderText(QStringLiteral("粘贴工作空间提供的完整 wss:// 端点（不含密钥）"));
    endpoint_->setToolTip(QStringLiteral("model 查询参数固定为 qwen-audio-3.1-realtime-plus；启动时自动加入。"));
    apiKey_ = line(config.apiKey);
    apiKey_->setEchoMode(QLineEdit::Password);
    apiKey_->setPlaceholderText(QStringLiteral("仅本次会话 / DASHSCOPE_API_KEY"));
    voice_ = line(config.voice);
    instructions_ = new QPlainTextEdit(config.instructions, this);
    instructions_->setMaximumHeight(130);
    instructions_->setPlaceholderText(QStringLiteral("双击可放大编辑"));
    greeting_ = new QPlainTextEdit(config.greeting, this);
    greeting_->setMaximumHeight(70);
    greeting_->setPlaceholderText(QStringLiteral("双击可放大编辑"));
    turnMode_ = new QComboBox(this);
    turnMode_->addItem(QStringLiteral("智能轮次（smart_turn）"), QStringLiteral("smart_turn"));
    turnMode_->addItem(QStringLiteral("服务端 VAD（server_vad）"), QStringLiteral("server_vad"));
    turnMode_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    turnMode_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    int turnIndex = turnMode_->findData(config.turnMode);
    if (turnIndex < 0) {
        turnMode_->addItem(config.turnMode, config.turnMode);
        turnIndex = turnMode_->count() - 1;
    }
    turnMode_->setCurrentIndex(turnIndex);
    uplinkChunkMs_ = new QSpinBox(this);
    uplinkChunkMs_->setRange(20, 100);
    uplinkChunkMs_->setSingleStep(20);
    uplinkChunkMs_->setValue(config.uplinkChunkMs);
    uplinkChunkMs_->setSuffix(QStringLiteral(" ms"));
    const QString chunkTip = QStringLiteral(
        "每次发送到模型的上行音频分片时长。\n20ms：延迟最低（默认）；100ms：官网建议值，上行消息量减少 80%。");
    uplinkChunkMs_->setToolTip(chunkTip);
    uplinkChunkMs_->setFixedWidth(72);
    maxHistoryTurns_ = new QSpinBox(this);
    maxHistoryTurns_->setRange(1, 50);
    maxHistoryTurns_->setValue(config.maxHistoryTurns);
    maxHistoryTurns_->setFixedWidth(72);
    maxHistoryTurns_->setToolTip(QStringLiteral(
        "阿里官方参数 session.max_history_turns：单次请求携带的对话历史轮次上限，\n"
        "范围 1–50，默认 20。调大可保留更长对话记忆，token 消耗更高；调小响应更快。"));
    qwenGrid->addWidget(new QLabel(QStringLiteral("完整 WSS 端点"), this), 0, 0);
    endpoint_->setMinimumWidth(560); // 独占剩余宽度，尽量完整显示 wss://...?model=... 长 URL
    qwenGrid->addWidget(endpoint_, 0, 1);
    qwenGrid->addWidget(new QLabel(QStringLiteral("轮次检测"), this), 0, 2);
    qwenGrid->addWidget(turnMode_, 0, 3);
    qwenGrid->addWidget(new QLabel(QStringLiteral("上下文轮次"), this), 0, 4);
    qwenGrid->addWidget(maxHistoryTurns_, 0, 5);
    qwenGrid->addWidget(new QLabel(QStringLiteral("API Key"), this), 1, 0);
    qwenGrid->addWidget(apiKey_, 1, 1);
    qwenGrid->addWidget(new QLabel(QStringLiteral("音色"), this), 1, 2);
    qwenGrid->addWidget(voice_, 1, 3);
    qwenGrid->addWidget(new QLabel(QStringLiteral("音频分片"), this), 1, 4);
    qwenGrid->addWidget(uplinkChunkMs_, 1, 5);
    qwenGrid->addWidget(new QLabel(QStringLiteral("系统指令"), this), 2, 0, Qt::AlignTop);
    qwenGrid->addWidget(instructions_, 2, 1, 1, 5);
    qwenGrid->addWidget(new QLabel(QStringLiteral("开场白指令"), this), 3, 0, Qt::AlignTop);
    qwenGrid->addWidget(greeting_, 3, 1, 1, 5);
    configurationLayout->addWidget(qwenBox);

    auto *strategyBox = new QGroupBox(QStringLiteral("交互策略"), this);
    auto *strategyGrid = new QGridLayout(strategyBox);
    strategyGrid->setVerticalSpacing(10);
    strategyGrid->setHorizontalSpacing(12);
    strategyGrid->setColumnMinimumWidth(0, 96);
    strategyGrid->setColumnMinimumWidth(2, 96);
    strategyGrid->setColumnMinimumWidth(4, 96);
    strategyGrid->setColumnStretch(1, 1);
    strategyGrid->setColumnStretch(3, 1);
    strategyGrid->setColumnStretch(5, 1);
    strategyGrid->setColumnStretch(7, 1);
    silenceTimeoutSec_ = new QSpinBox(this);
    silenceTimeoutSec_->setRange(1, 60);
    silenceTimeoutSec_->setValue(config.silenceTimeoutSec);
    silenceTimeoutSec_->setSuffix(QStringLiteral(" 秒"));
    maxSilenceRetries_ = new QSpinBox(this);
    maxSilenceRetries_->setRange(1, 10);
    maxSilenceRetries_->setValue(config.maxSilenceRetries);
    maxTurns_ = new QSpinBox(this);
    maxTurns_->setRange(1, 200);
    maxTurns_->setValue(config.maxTurns);
    allowBargeIn_ = new QCheckBox(QStringLiteral("允许用户打断（抢话）"), this);
    allowBargeIn_->setChecked(config.allowBargeIn);
    allowBargeIn_->setToolTip(QStringLiteral(
        "开启后，AI 播报期间用户说话可立即打断。\n"
        "SIP 通话：由模型服务端 VAD 检测抢话并自动取消播报；\n"
        "本地对话：播放期间也上送麦克风音频（全双工），不戴耳机时扬声器回声可能"
        "被误判为抢话，建议佩戴耳机。关闭则本地对话为半双工（播报结束后才能说话）。"));
    silencePrompt_ = new QLineEdit(config.silencePrompt, this);
    silencePrompt_->setPlaceholderText(QStringLiteral("每次无声超时后 AI 的提示语，留空则不提示"));
    farewellPrompt_ = new QLineEdit(config.farewellPrompt, this);
    farewellPrompt_->setPlaceholderText(QStringLiteral("挂机前 AI 的道别语，留空则直接挂断"));
    strategyGrid->addWidget(new QLabel(QStringLiteral("无声超时"), this), 0, 0);
    strategyGrid->addWidget(silenceTimeoutSec_, 0, 1);
    strategyGrid->addWidget(new QLabel(QStringLiteral("无声超时次数"), this), 0, 2);
    strategyGrid->addWidget(maxSilenceRetries_, 0, 3);
    strategyGrid->addWidget(new QLabel(QStringLiteral("最大对话轮次"), this), 0, 4);
    strategyGrid->addWidget(maxTurns_, 0, 5);
    strategyGrid->addWidget(allowBargeIn_, 0, 6, 1, 2);
    strategyGrid->addWidget(new QLabel(QStringLiteral("无声提示话术"), this), 1, 0);
    strategyGrid->addWidget(silencePrompt_, 1, 1, 1, 7);
    strategyGrid->addWidget(new QLabel(QStringLiteral("挂机话术"), this), 2, 0);
    strategyGrid->addWidget(farewellPrompt_, 2, 1, 1, 7);
    configurationLayout->addWidget(strategyBox);

    auto *recordBox = new QGroupBox(QStringLiteral("通话记录与日志"), this);
    auto *recordGrid = new QGridLayout(recordBox);
    recordGrid->setVerticalSpacing(10);
    recordGrid->setHorizontalSpacing(12);
    recordGrid->setColumnMinimumWidth(0, 96);
    recordGrid->setColumnMinimumWidth(2, 96);
    recordGrid->setColumnMinimumWidth(4, 96);
    recordGrid->setColumnStretch(1, 1);
    recordGrid->setColumnStretch(3, 1);
    recordGrid->setColumnStretch(5, 1);
    callRecordPath_ = new QLineEdit(config.callRecordPath, this);
    callRecordPath_->setPlaceholderText(QStringLiteral("本地文件路径，如 cdr/calls.jsonl"));
    callRecordPath_->setToolTip(QStringLiteral(
        "通话记录 JSONL 文件路径，每行一条记录（含对话转写 transcript 字段）。\n"
        "相对路径基于程序所在目录解析，目录不存在时自动创建。"));
    callRecordWebhook_ = new QLineEdit(config.callRecordWebhook, this);
    callRecordWebhook_->setPlaceholderText(QStringLiteral("可选，如 http://example.com/api/cdr"));
    callRecordWebhook_->setToolTip(QStringLiteral(
        "可选。通话结束后将完整记录（含对话转写）以 HTTP POST JSON 推送到该地址。"));
    callRecordMaxSize_ = new QSpinBox(this);
    callRecordMaxSize_->setRange(1, 1024);
    callRecordMaxSize_->setSuffix(QStringLiteral(" MB"));
    callRecordMaxSize_->setValue(int(config.callRecordMaxSize / (1024 * 1024)));
    callRecordMaxFiles_ = new QSpinBox(this);
    callRecordMaxFiles_->setRange(1, 100);
    callRecordMaxFiles_->setValue(config.callRecordMaxFiles);
    customVariables_ = new QLineEdit(config.customVariables.join(QStringLiteral(",")), this);
    customVariables_->setPlaceholderText(QStringLiteral("逗号分隔，如 X-Custom-*,X-Tenant-Id"));
    logPath_ = new QLineEdit(config.logPath, this);
    logPath_->setPlaceholderText(QStringLiteral("运行日志路径，如 log/voicebot.log，留空禁用"));
    logPath_->setToolTip(QStringLiteral(
        "运行日志文件路径，界面与无界面模式的所有运行日志统一写入该文件。\n"
        "相对路径基于程序所在目录解析，目录不存在时自动创建，留空则不写日志文件。"));
    logMaxSize_ = new QSpinBox(this);
    logMaxSize_->setRange(1, 1024);
    logMaxSize_->setSuffix(QStringLiteral(" MB"));
    logMaxSize_->setValue(int(config.logMaxSize / (1024 * 1024)));
    logMaxFiles_ = new QSpinBox(this);
    logMaxFiles_->setRange(1, 100);
    logMaxFiles_->setValue(config.logMaxFiles);
    recordGrid->addWidget(new QLabel(QStringLiteral("本地文件路径"), this), 0, 0);
    recordGrid->addWidget(callRecordPath_, 0, 1);
    recordGrid->addWidget(new QLabel(QStringLiteral("单文件大小上限"), this), 0, 2);
    recordGrid->addWidget(callRecordMaxSize_, 0, 3);
    recordGrid->addWidget(new QLabel(QStringLiteral("轮转保留份数"), this), 0, 4);
    recordGrid->addWidget(callRecordMaxFiles_, 0, 5);
    recordGrid->addWidget(new QLabel(QStringLiteral("回调接口 URL"), this), 1, 0);
    recordGrid->addWidget(callRecordWebhook_, 1, 1, 1, 3);
    recordGrid->addWidget(new QLabel(QStringLiteral("自定义变量头"), this), 1, 4);
    recordGrid->addWidget(customVariables_, 1, 5);
    recordGrid->addWidget(new QLabel(QStringLiteral("运行日志路径"), this), 2, 0);
    recordGrid->addWidget(logPath_, 2, 1);
    recordGrid->addWidget(new QLabel(QStringLiteral("日志大小上限"), this), 2, 2);
    recordGrid->addWidget(logMaxSize_, 2, 3);
    recordGrid->addWidget(new QLabel(QStringLiteral("日志份数"), this), 2, 4);
    recordGrid->addWidget(logMaxFiles_, 2, 5);
    configurationLayout->addWidget(recordBox);

    auto *mcpBox = new QGroupBox(QStringLiteral("工具插件（MCP）"), this);
    auto *mcpGrid = new QGridLayout(mcpBox);
    mcpGrid->setVerticalSpacing(10);
    mcpGrid->setHorizontalSpacing(12);
    mcpGrid->setColumnMinimumWidth(0, 96);
    mcpGrid->setColumnStretch(1, 1);
    mcpPlugins_ = new PluginComboBox(this);
    static_cast<PluginComboBox *>(mcpPlugins_)->setCheckedFiles(config.enabledPlugins);
    const QString mcpTip = QStringLiteral(
        "自动扫描随程序打包的 plugins 目录中的 MCP 插件（*.py / *.js），下拉勾选即启用，保存配置后立即生效（热插拔）。\n"
        "插件目录与可执行文件相对定位（<prefix>/plugins，与 bin 平级），也可用环境变量 VOICEBOT_PLUGINS_DIR 指定；\n"
        "Python 插件需本机安装 Python，JS 插件需 Node.js，插件自身依赖请自行安装；\n"
        "与内置工具重名的 MCP 工具会被忽略；\n"
        "进行中的通话使用开始时锁定的工具集，不受热插拔影响。");
    mcpPlugins_->setToolTip(mcpTip);
    auto *mcpLabel = new QLabel(QStringLiteral("启用插件"), this);
    mcpLabel->setToolTip(mcpTip);
    mcpGrid->addWidget(mcpLabel, 0, 0);
    mcpGrid->addWidget(mcpPlugins_, 0, 1);
    configurationLayout->addWidget(mcpBox);

    auto *save = new QPushButton(QStringLiteral("保存非敏感配置"), this);
    save->setObjectName(QStringLiteral("secondary"));
    configurationLayout->addWidget(save, 0, Qt::AlignRight);
    auto *notice = new QLabel(QStringLiteral("密码与 API Key 不写入配置。机器人不使用本机麦克风；仅接收允许的 SIP 来电。"), this);
    notice->setObjectName(QStringLiteral("hint"));
    notice->setWordWrap(true);
    configurationLayout->addWidget(notice);
    configurationLayout->addStretch();
    scroll->setWidget(configuration_);
    // 防止滚动配置页时滚轮误改 SpinBox/ComboBox 的值（聚焦后才允许滚轮调值）
    auto *wheelGuard = new WheelGuard(this);
    const auto spinBoxes = configuration_->findChildren<QSpinBox *>();
    for (auto *spin : spinBoxes)
        spin->installEventFilter(wheelGuard);
    const auto comboBoxes = configuration_->findChildren<QComboBox *>();
    for (auto *combo : comboBoxes)
        combo->installEventFilter(wheelGuard);
    // 系统指令 / 开场白 / 对端 IP 列表支持双击放大编辑
    instructions_->installEventFilter(this);
    greeting_->installEventFilter(this);
    allowedPeers_->installEventFilter(this);
    tabs->addTab(scroll, QStringLiteral("配置"));

    auto *callsPage = new QWidget(this);
    auto *callsLayout = new QVBoxLayout(callsPage);
    auto *callActions = new QHBoxLayout;
    answer_ = new QPushButton(QStringLiteral("接听选中来电"), this);
    answer_->setObjectName(QStringLiteral("secondary"));
    hangup_ = new QPushButton(QStringLiteral("挂断选中通话"), this);
    hangup_->setObjectName(QStringLiteral("danger"));
    viewRecords_ = new QPushButton(QStringLiteral("查看历史记录"), this);
    viewRecords_->setObjectName(QStringLiteral("secondary"));
    viewRecords_->setToolTip(QStringLiteral(
        "打开本地保存的通话记录文件（含轮转历史），可查看每通电话的\n"
        "详细信息与对话转写；文件与轮转份数由配置页决定。"));
    callActions->addWidget(answer_);
    callActions->addWidget(hangup_);
    callActions->addWidget(viewRecords_);
    callActions->addStretch();
    auto *callsHint = new QLabel(QStringLiteral("接通后计时 · 保留最近 128 条通话"), this);
    callsHint->setObjectName(QStringLiteral("hint"));
    callActions->addWidget(callsHint);
    callsLayout->addLayout(callActions);
    auto *splitter = new QSplitter(Qt::Vertical, this);
    callTable_ = new QTableWidget(0, 14, this);
    callTable_->setHorizontalHeaderLabels({
        QStringLiteral("ID"), QStringLiteral("主叫"), QStringLiteral("被叫"), QStringLiteral("呼叫状态"),
        QStringLiteral("通话时长"), QStringLiteral("对话轮次"), QStringLiteral("打断方式"), QStringLiteral("接听状态"),
        QStringLiteral("接听时间"), QStringLiteral("挂断时间"), QStringLiteral("挂断方"),
        QStringLiteral("SIP 状态码"), QStringLiteral("挂断原因"), QStringLiteral("Token 消耗")});
    callTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    callTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    callTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    callTable_->setShowGrid(false);
    callTable_->setAlternatingRowColors(true);
    callTable_->verticalHeader()->hide();
    callTable_->verticalHeader()->setDefaultSectionSize(34);
    callTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    callTable_->horizontalHeader()->setStretchLastSection(true);
    callTable_->setColumnWidth(0, 45);
    callTable_->setColumnWidth(1, 100); // 主叫 11 位号码完整显示
    callTable_->setColumnWidth(2, 95);
    callTable_->setColumnWidth(3, 70);
    callTable_->setColumnWidth(4, 80);
    callTable_->setColumnWidth(5, 70);
    callTable_->setColumnWidth(6, 70);
    callTable_->setColumnWidth(7, 70);
    callTable_->setColumnWidth(8, 90);
    callTable_->setColumnWidth(9, 90);
    callTable_->setColumnWidth(10, 80);
    callTable_->setColumnWidth(11, 80);
    callTable_->setColumnWidth(12, 140); // 挂断原因
    callTable_->setColumnWidth(13, 80);  // Token 消耗
    splitter->addWidget(callTable_);
    transcript_ = new QPlainTextEdit(this);
    transcript_->setReadOnly(true);
    transcript_->setPlaceholderText(QStringLiteral("选择通话查看识别文本与机器人回复；未产生内容时保持为空。"));
    transcript_->document()->setMaximumBlockCount(600);
    splitter->addWidget(transcript_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    callsLayout->addWidget(splitter);
    tabs->addTab(callsPage, QStringLiteral("通话与转写"));
    auto *logsPage = new QWidget(this);
    auto *logsLayout = new QVBoxLayout(logsPage);
    auto *logActions = new QHBoxLayout;
    viewLog_ = new QPushButton(QStringLiteral("查看日志文件"), this);
    viewLog_->setObjectName(QStringLiteral("secondary"));
    viewLog_->setToolTip(QStringLiteral(
        "打开运行日志文件（含轮转历史），加载文件末尾内容查看。\n"
        "文件与轮转份数由配置页决定，路径留空禁用文件日志。"));
    logActions->addWidget(viewLog_);
    logActions->addStretch();
    auto *logsHint = new QLabel(QStringLiteral("实时日志仅保留最近 600 行 · 完整历史点上方按钮"), this);
    logsHint->setObjectName(QStringLiteral("hint"));
    logActions->addWidget(logsHint);
    logsLayout->addLayout(logActions);
    logs_ = new QPlainTextEdit(this);
    logs_->setReadOnly(true);
    logs_->document()->setMaximumBlockCount(600);
    QFont monoFont(QStringLiteral("Cascadia Mono"));
    monoFont.setStyleHint(QFont::TypeWriter);
    monoFont.setPointSize(9);
    logs_->setFont(monoFont);
    logs_->setPlaceholderText(QStringLiteral("运行日志（实时，仅保留最近 600 行；完整日志请点「查看日志文件」）"));
    logsLayout->addWidget(logs_);
    tabs->addTab(logsPage, QStringLiteral("日志"));

    // ---------- 工具 ----------
    auto *toolsPage = new QWidget(this);
    auto *toolsLayout = new QVBoxLayout(toolsPage);
    toolsLayout->setContentsMargins(4, 8, 4, 8);
    toolsLayout->setSpacing(10);
    auto *toolsHint = new QLabel(QStringLiteral(
        "进行中的通话使用开始时锁定的工具集；MCP 插件热插拔，断线自动重连。"), toolsPage);
    toolsHint->setObjectName(QStringLiteral("hint"));
    toolsHint->setWordWrap(true);
    toolsLayout->addWidget(toolsHint);
    auto setupToolTable = [](QTableWidget *table) {
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setShowGrid(false);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(30);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(true);
    };
    auto *builtinBox = new QGroupBox(QStringLiteral("内置工具"), toolsPage);
    auto *builtinLayout = new QVBoxLayout(builtinBox);
    builtinLayout->setContentsMargins(6, 8, 6, 6);
    builtinTools_ = new QTableWidget(0, 2, builtinBox);
    builtinTools_->setHorizontalHeaderLabels({
        QStringLiteral("工具名"), QStringLiteral("说明")});
    builtinTools_->setColumnWidth(0, 160);
    setupToolTable(builtinTools_);
    builtinLayout->addWidget(builtinTools_);
    builtinBox->setMaximumHeight(170);
    toolsLayout->addWidget(builtinBox);

    auto *mcpBox2 = new QGroupBox(QStringLiteral("MCP 工具"), toolsPage);
    auto *mcpToolsLayout = new QVBoxLayout(mcpBox2);
    mcpToolsLayout->setContentsMargins(6, 8, 6, 6);
    mcpTools_ = new QTableWidget(0, 5, mcpBox2);
    mcpTools_->setHorizontalHeaderLabels({
        QStringLiteral("工具名"), QStringLiteral("来源插件"), QStringLiteral("状态"),
        QStringLiteral("依赖包"), QStringLiteral("说明")});
    mcpTools_->setColumnWidth(0, 150);
    mcpTools_->setColumnWidth(1, 160);
    mcpTools_->setColumnWidth(2, 100);
    mcpTools_->setColumnWidth(3, 140);
    setupToolTable(mcpTools_);
    mcpToolsLayout->addWidget(mcpTools_);
    mcpEmptyHint_ = new QLabel(QStringLiteral(
        "未启用 MCP 插件：在「配置」页「启用插件」下拉中勾选随程序打包的插件即可扩展工具"), mcpBox2);
    mcpEmptyHint_->setObjectName(QStringLiteral("hint"));
    mcpEmptyHint_->setWordWrap(true);
    mcpToolsLayout->addWidget(mcpEmptyHint_);
    toolsLayout->addWidget(mcpBox2, 1);
    tabs->addTab(toolsPage, QStringLiteral("工具"));

    // ---------- 关于 ----------
    auto *aboutPage = new QWidget(this);
    auto *aboutLayout = new QVBoxLayout(aboutPage);
    aboutLayout->setContentsMargins(4, 8, 4, 8);
    aboutLayout->setSpacing(14);
    auto aboutKey = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setObjectName(QStringLiteral("aboutKey"));
        return label;
    };
    auto aboutValue = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setObjectName(QStringLiteral("aboutValue"));
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };

    // 顶部横幅：应用标识 + 版本徽标
    auto *hero = new QFrame(aboutPage);
    hero->setObjectName(QStringLiteral("aboutHero"));
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(18, 14, 18, 14);
    heroLayout->setSpacing(14);
    auto *heroAvatar = new QLabel(hero);
    heroAvatar->setObjectName(QStringLiteral("aboutAvatar"));
    heroAvatar->setAlignment(Qt::AlignCenter);
    heroAvatar->setPixmap(QPixmap(QStringLiteral(":/icons/app.png"))
        .scaled(56, 56, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    heroLayout->addWidget(heroAvatar);
    auto *heroColumn = new QVBoxLayout;
    heroColumn->setSpacing(3);
    auto *heroNameRow = new QHBoxLayout;
    heroNameRow->setSpacing(10);
    auto *heroName = new QLabel(QStringLiteral("Voice SIP Robot"), hero);
    heroName->setObjectName(QStringLiteral("aboutName"));
    heroNameRow->addWidget(heroName);
    auto *heroBadge = new QFrame(hero);
    heroBadge->setObjectName(QStringLiteral("aboutBadge"));
    auto *badgeLayout = new QHBoxLayout(heroBadge);
    badgeLayout->setContentsMargins(10, 0, 10, 0);
    badgeLayout->setSpacing(0);
    auto *badgeText = new QLabel(QStringLiteral("v") + QStringLiteral(VOICEBOT_VERSION),
                                 heroBadge);
    badgeText->setObjectName(QStringLiteral("aboutBadgeText"));
    badgeLayout->addWidget(badgeText);
    heroNameRow->addWidget(heroBadge);
    heroNameRow->addStretch();
    heroColumn->addLayout(heroNameRow);
    auto *heroSubtitle = new QLabel(
        QStringLiteral("SIP 智能AI电话机器人 · 基于 PJSIP 与阿里 Qwen 实时语音模型"), hero);
    heroSubtitle->setObjectName(QStringLiteral("aboutSubtitle"));
    heroColumn->addWidget(heroSubtitle);
    heroLayout->addLayout(heroColumn);
    aboutLayout->addWidget(hero);

    auto *versionBox = new QGroupBox(QStringLiteral("版本信息"), aboutPage);
    auto *versionGrid = new QGridLayout(versionBox);
    versionGrid->setVerticalSpacing(9);
    versionGrid->setHorizontalSpacing(14);
    versionGrid->setColumnStretch(1, 1);
    versionGrid->setColumnStretch(3, 1);
    versionGrid->addWidget(aboutKey(QStringLiteral("软件版本")), 0, 0);
    versionGrid->addWidget(aboutValue(QStringLiteral(VOICEBOT_VERSION)), 0, 1);
    versionGrid->addWidget(aboutKey(QStringLiteral("构建时间")), 0, 2);
    versionGrid->addWidget(aboutValue(QStringLiteral(__DATE__ " " __TIME__)), 0, 3);
    versionGrid->addWidget(aboutKey(QStringLiteral("Qt 版本")), 1, 0);
    versionGrid->addWidget(aboutValue(QString::fromLatin1(qVersion())), 1, 1);
    versionGrid->addWidget(aboutKey(QStringLiteral("PJSIP 版本")), 1, 2);
    versionGrid->addWidget(aboutValue(QStringLiteral("%1.%2.%3")
        .arg(PJ_VERSION_NUM_MAJOR).arg(PJ_VERSION_NUM_MINOR).arg(PJ_VERSION_NUM_REV)), 1, 3);
    versionGrid->addWidget(aboutKey(QStringLiteral("音频链路")), 2, 0);
    versionGrid->addWidget(aboutValue(QStringLiteral(
        "WebSocket (wss) · 16k 上行 / 24k 下行 PCM 桥接")), 2, 1, 1, 3);
    versionGrid->addWidget(aboutKey(QStringLiteral("运行平台")), 3, 0);
    versionGrid->addWidget(aboutValue(QSysInfo::prettyProductName()), 3, 1, 1, 3);
    aboutLayout->addWidget(versionBox);

    auto *teamBox = new QGroupBox(QStringLiteral("关于我们"), aboutPage);
    auto *teamGrid = new QGridLayout(teamBox);
    teamGrid->setVerticalSpacing(9);
    teamGrid->setHorizontalSpacing(14);
    teamGrid->setColumnStretch(1, 1);
    auto *intro = new QLabel(QStringLiteral(
        "Voice SIP Robot 面向电话客服、智能外呼等场景，提供开箱即用的 AI 语音机器人：\n"
        "· PJSIP 承载 SIP/RTP 通话，白名单鉴权与自动接听\n"
        "· 阿里 Qwen 实时全双工模型驱动，支持抢话打断、无声超时与轮次上限策略\n"
        "· Function Calling 工具扩展：内置挂断工具 + MCP 插件体系（热插拔，支持天气/IP 查询等）\n"
        "· 通话记录本地 JSONL 持久化（含对话转写与 Token 统计），并支持 HTTP 回调对接业务系统"), this);
    intro->setObjectName(QStringLiteral("aboutBody"));
    intro->setWordWrap(true);
    teamGrid->addWidget(intro, 0, 0, 1, 2);
    teamGrid->addWidget(aboutKey(QStringLiteral("开发团队")), 1, 0);
    teamGrid->addWidget(aboutValue(aboutDeveloper), 1, 1);
    teamGrid->addWidget(aboutKey(QStringLiteral("联系方式")), 2, 0);
    teamGrid->addWidget(aboutValue(aboutContact), 2, 1);
    teamGrid->addWidget(aboutKey(QStringLiteral("版权声明")), 3, 0);
    teamGrid->addWidget(aboutValue(aboutCopyright), 3, 1);
    aboutLayout->addWidget(teamBox);
    aboutLayout->addStretch();
    tabs->addTab(aboutPage, QStringLiteral("关于"));

    connect(startStop_, &QPushButton::clicked, this, &MainWindow::toggleEngine);
    connect(save, &QPushButton::clicked, this, &MainWindow::saveSettings);
    connect(answer_, &QPushButton::clicked, this, [this] {
        const int id = selectedCall();
        if (id >= 0 && answer_->isEnabled()) engine_.answer(id);
    });
    connect(hangup_, &QPushButton::clicked, this, [this] {
        const int id = selectedCall();
        if (id >= 0 && hangup_->isEnabled()) engine_.hangup(id);
    });
    connect(viewRecords_, &QPushButton::clicked, this, &MainWindow::showRecordHistory);
    connect(viewLog_, &QPushButton::clicked, this, &MainWindow::showLogFileViewer);
    connect(callTable_, &QTableWidget::itemSelectionChanged, this, [this] {
        updateActions();
        showTranscript();
    });
    connect(&engine_, &BotEngine::runningChanged, this, &MainWindow::applyRunning);
    connect(&engine_, &BotEngine::statusChanged, this, [this](const QString &status) {
        lastStatus_ = safeText(status).left(maxEntryCharacters);
        status_->setText(lastStatus_);
        appendLog(status);
    });
    connect(&engine_, &BotEngine::callChanged, this, &MainWindow::updateCall);
    connect(&engine_, &BotEngine::callEnded, this, [this](int id, int sipCode, const QString &hangupBy, const QString &reason) {
        auto record = calls_.find(id);
        if (record == calls_.end()) return;
        record->hangupBy = hangupBy;
        const int row = record->idItem->row();
        if (record->hangupTime.isEmpty())
            record->hangupTime = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        callTable_->item(row, 9)->setText(record->hangupTime);
        callTable_->item(row, 10)->setText(hangupByText(hangupBy));
        callTable_->item(row, 11)->setText(sipCode > 0 ? QString::number(sipCode) : QString());
        callTable_->item(row, 12)->setText(reason.isEmpty() ? QString() : reason.left(80));
        endCall(id);
    });
    connect(&engine_, &BotEngine::callTurnsUpdated, this, [this](int id, int turns) {
        auto record = calls_.find(id);
        if (record == calls_.end()) return;
        record->turns = turns;
        callTable_->item(record->idItem->row(), 5)->setText(QString::number(turns));
    });
    // 模型 token 用量实时刷新（悬停显示输入/输出与音频/文本细分）
    connect(&engine_, &BotEngine::callUsageUpdated, this, [this](int id, const ModelUsage &usage) {
        auto record = calls_.find(id);
        if (record == calls_.end()) return;
        const auto [summary, tip] = usageTexts(usage);
        record->usageSummary = summary;
        record->usageTip = tip;
        auto *item = callTable_->item(record->idItem->row(), 13);
        item->setText(summary);
        if (!tip.isEmpty()) item->setToolTip(tip);
    });
    connect(&engine_, &BotEngine::transcript, this, &MainWindow::addTranscript);
    connect(&engine_, &BotEngine::logMessage, this, &MainWindow::appendLog);
    // ---------- 工具注册表：MCP 插件管理与工具页数据源 ----------
    tools_ = new ToolRegistry(this);
    engine_.setToolRegistry(tools_);
    tools_->applyMcpCommands(config.mcpCommands); // 差异比对，增量启动插件
    connect(tools_, &ToolRegistry::toolsChanged, this, &MainWindow::refreshTools);
    connect(tools_, &ToolRegistry::logMessage, this, &MainWindow::appendLog);
    // engine 也订阅 toolResult；这里处理属于本地对话的令牌
    connect(tools_, &ToolRegistry::toolResult,
            this, &MainWindow::localToolResultArrived);
    refreshTools();
    qRegisterMetaType<qwenbot::BotConfig>("qwenbot::BotConfig");
    // 本地对话记录写入器：与 SIP 通话共用同一文件/webhook 配置
    localRecorder_ = new CallRecorder(this);
    connect(localRecorder_, &CallRecorder::logMessage, this, &MainWindow::appendLog);
    local_ = new LocalAudioSession(); // 无 parent：将移动到专用音频线程
    audioThread_ = new QThread(this);
    audioThread_->setObjectName(QStringLiteral("voicebot-audio"));
    local_->moveToThread(audioThread_);
    connect(localToggle_, &QPushButton::clicked, this, &MainWindow::toggleLocal);
    connect(local_, &LocalAudioSession::activeChanged,
            this, &MainWindow::applyLocalActive);
    connect(local_, &LocalAudioSession::transcript,
            this, &MainWindow::appendLocalTranscript);
    connect(local_, &LocalAudioSession::failed, this, [this](const QString &message) {
        lastLocalFinishReason_ = QStringLiteral("failed: %1").arg(message);
        appendLog(QStringLiteral("本地对话失败：%1").arg(message));
        QMessageBox::critical(this, QStringLiteral("本地对话失败"), message);
    });
    connect(local_, &LocalAudioSession::logMessage,
            this, [this](const QString &text) { appendLog(text); });
    connect(local_, &LocalAudioSession::statusChanged, this, [this](const QString &text) {
        status_->setText(QStringLiteral("本地对话：%1").arg(text));
    });
    connect(local_, &LocalAudioSession::finished, this, [this](const QString &reason) {
        lastLocalFinishReason_ = reason;
        appendLog(QStringLiteral("本地对话结束：%1").arg(reason));
        status_->setText(QStringLiteral("本地对话已结束"));
    });
    connect(local_, &LocalAudioSession::turnCompleted, this, [this](int count) {
        localTurns_ = count;
        appendLog(QStringLiteral("本地对话已完成 %1 轮。").arg(count));
        // 同步刷新通话表本地行的轮次列
        if (auto rec = calls_.find(localRowId); rec != calls_.end() && !rec->ended) {
            rec->turns = count;
            callTable_->item(rec->idItem->row(), 5)->setText(QString::number(count));
        }
    });
    // 累计本地对话的模型 token 用量，结束时写进本地通话记录
    connect(local_, &LocalAudioSession::usageUpdated,
            this, &MainWindow::handleLocalUsageUpdated);
    connect(local_, &LocalAudioSession::toolCallRequested,
            this, &MainWindow::handleLocalToolCall);
    audioThread_->start();
    auto *clock = new QTimer(this);
    clock->setInterval(1000);
    connect(clock, &QTimer::timeout, this, &MainWindow::updateDurations);
    clock->start();

    const QIcon appIcon(QStringLiteral(":/icons/app.png"));
    setWindowIcon(appIcon);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(appIcon, this);
        tray_->setToolTip(QStringLiteral("Voice SIP Robot"));
        auto *menu = new QMenu(this);
        auto *showAction = menu->addAction(QStringLiteral("显示主窗口"));
        connect(showAction, &QAction::triggered, this, [this] {
            showNormal(); raise(); activateWindow();
        });
        trayToggle_ = menu->addAction(QStringLiteral("启动机器人"));
        connect(trayToggle_, &QAction::triggered, this, &MainWindow::toggleEngine);
        menu->addSeparator();
        auto *quit = menu->addAction(QStringLiteral("退出"));
        connect(quit, &QAction::triggered, this, &MainWindow::quitApplication);
        tray_->setContextMenu(menu);
        connect(tray_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                showNormal(); raise(); activateWindow();
            }
        });
        tray_->show();
    }
    auto *quit = new QAction(QStringLiteral("退出"), this);
    quit->setShortcut(QKeySequence::Quit);
    addAction(quit);
    connect(quit, &QAction::triggered, this, &MainWindow::quitApplication);
    applyRunning(engine_.isRunning());
}

MainWindow::~MainWindow()
{
    shutdownAudio();
}

BotConfig MainWindow::formConfig() const
{
    BotConfig config;
    config.listenAddress = listenAddress_->text().trimmed();
    config.sipPort = static_cast<quint16>(sipPort_->value());
    config.rtpPort = static_cast<quint16>(rtpPort_->value());
    config.maxCalls = maxCalls_->value();
    config.allowedPeers = allowedPeers_->toPlainText().split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
    config.autoAnswer = autoAnswer_->isChecked();
    config.sipIdentity = identity_->text().trimmed();
    config.registrar = registrar_->text().trimmed();
    config.sipUser = sipUser_->text().trimmed();
    config.sipPassword = sipPassword_->text();
    config.endpoint = modelEndpoint(endpoint_->text());
    config.apiKey = apiKey_->text().trimmed();
    config.voice = voice_->text().trimmed();
    config.instructions = instructions_->toPlainText();
    config.greeting = greeting_->toPlainText();
    config.turnMode = turnMode_->currentData().toString();
    config.maxHistoryTurns = maxHistoryTurns_->value();
    config.uplinkChunkMs = uplinkChunkMs_->value();
    config.silenceTimeoutSec = silenceTimeoutSec_->value();
    config.maxSilenceRetries = maxSilenceRetries_->value();
    config.maxTurns = maxTurns_->value();
    config.allowBargeIn = allowBargeIn_->isChecked();
    config.silencePrompt = silencePrompt_->text();
    config.farewellPrompt = farewellPrompt_->text();
    config.callRecordPath = callRecordPath_->text().trimmed();
    config.callRecordWebhook = callRecordWebhook_->text().trimmed();
    config.callRecordMaxSize = qint64(callRecordMaxSize_->value()) * 1024 * 1024;
    config.callRecordMaxFiles = callRecordMaxFiles_->value();
    config.logPath = logPath_->text().trimmed(); // 留空表示禁用文件日志
    config.logMaxSize = qint64(logMaxSize_->value()) * 1024 * 1024;
    config.logMaxFiles = logMaxFiles_->value();
    config.customVariables = customVariables_->text().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    // 勾选的插件文件名 → 解释器 + 打包目录绝对路径
    config.enabledPlugins = static_cast<PluginComboBox *>(mcpPlugins_)->checkedFiles();
    config.mcpCommands.clear();
    for (const auto &file : config.enabledPlugins)
        if (const QString command = ToolRegistry::commandForPlugin(file); !command.isEmpty())
            config.mcpCommands.append(command);
    return config;
}

void MainWindow::refreshTools()
{
    if (!tools_ || !builtinTools_ || !mcpTools_ || !mcpEmptyHint_)
        return;
    const auto list = tools_->tools();
    builtinTools_->setRowCount(0);
    mcpTools_->setRowCount(0);
    // 统一居中入口：QSS 对 QTableWidgetItem 的对齐不可靠，须在写入时设置
    auto centeredItem = [](const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        return item;
    };
    for (const auto &tool : list) {
        if (tool.builtIn) {
            const int row = builtinTools_->rowCount();
            builtinTools_->insertRow(row);
            auto *name = centeredItem(tool.name);
            auto *description = centeredItem(tool.description);
            description->setToolTip(tool.description);
            builtinTools_->setItem(row, 0, name);
            builtinTools_->setItem(row, 1, description);
        } else {
            const int row = mcpTools_->rowCount();
            mcpTools_->insertRow(row);
            auto *name = centeredItem(tool.name);
            auto *server = centeredItem(tool.server);
            auto *status = centeredItem(tool.online
                ? QStringLiteral("在线") : QStringLiteral("离线（重连中）"));
            // 依赖包来自插件源码扫描（来源字段即插件文件名）
            const QStringList deps = ToolRegistry::pluginDependencies(tool.server);
            auto *dependencies = centeredItem(deps.join(QStringLiteral(", ")));
            if (!deps.isEmpty())
                dependencies->setToolTip(
                    QStringLiteral("安装命令：pip install %1").arg(deps.join(QLatin1Char(' '))));
            auto *description = centeredItem(tool.description);
            description->setToolTip(tool.description);
            const QBrush offlineColor(QColor(QStringLiteral("#9ca3af")));
            if (!tool.online) {
                for (auto *item : {name, server, status, dependencies, description})
                    item->setForeground(offlineColor); // 插件断线灰显
            }
            mcpTools_->setItem(row, 0, name);
            mcpTools_->setItem(row, 1, server);
            mcpTools_->setItem(row, 2, status);
            mcpTools_->setItem(row, 3, dependencies);
            mcpTools_->setItem(row, 4, description);
        }
    }
    mcpEmptyHint_->setVisible(mcpTools_->rowCount() == 0);
}

// 本地对话的工具调用不经过 BotEngine，由这里路由到 ToolRegistry
void MainWindow::handleLocalToolCall(const QString &callId, const QString &name,
                                     const QString &arguments)
{
    auto errorJson = [](const QString &message) {
        return QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), message}}).toJson(QJsonDocument::Compact));
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(arguments.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        appendLog(QStringLiteral("本地对话：工具 %1 的参数不是有效 JSON 对象。").arg(name));
        replyLocalTool(callId, errorJson(QStringLiteral("工具参数不是有效的 JSON 对象")));
        return;
    }
    if (!tools_) {
        replyLocalTool(callId, errorJson(QStringLiteral("工具服务未启用")));
        return;
    }
    const int token = tools_->callMcpTool(name, document.object());
    if (token < 0) {
        appendLog(QStringLiteral("本地对话：工具 %1 不可用（未注册或插件离线）。").arg(name));
        replyLocalTool(callId, errorJson(
            QStringLiteral("该工具当前不可用，请向用户致歉并改用其他方式帮助")));
        return;
    }
    pendingLocalToolTokens_.insert(token, callId);
    appendLog(QStringLiteral("本地对话：调用工具 %1（插件执行中，10 秒超时）。").arg(name));
}

void MainWindow::localToolResultArrived(int token, const QString &name,
                                        const QString &outputJson)
{
    const auto it = pendingLocalToolTokens_.find(token);
    if (it == pendingLocalToolTokens_.end()) return; // 属于 engine 通话的令牌
    const QString callId = it.value();
    pendingLocalToolTokens_.erase(it);
    appendLog(QStringLiteral("本地对话：工具 %1 返回结果。").arg(name));
    replyLocalTool(callId, outputJson);
}

void MainWindow::replyLocalTool(const QString &callId, const QString &outputJson)
{
    // local_ 运行在音频工作线程：排队投递，避免跨线程直接操作会话
    QMetaObject::invokeMethod(local_, "completeToolCall", Qt::QueuedConnection,
                              Q_ARG(QString, callId), Q_ARG(QString, outputJson));
}

void MainWindow::toggleEngine()
{
    if (engine_.isRunning()) {
        engine_.stop();
        return;
    }
    lastStatus_.clear();
    if (!engine_.start(formConfig())) {
        QMessageBox::critical(this, QStringLiteral("启动失败"),
                              lastStatus_.isEmpty() ? QStringLiteral("机器人启动失败，请查看日志。") : lastStatus_);
    }
}

void MainWindow::applyRunning(bool running)
{
    configuration_->setEnabled(!running && !localActive_);
    const QString action = running ? QStringLiteral("停止机器人") : QStringLiteral("启动机器人");
    startStop_->setText(action);
    startStop_->setEnabled(!localActive_); // 本地对话与引擎互斥
    if (localToggle_) localToggle_->setEnabled(!running);
    setStatusPill(running, running ? QStringLiteral("运行中") : QStringLiteral("未启动"));
    if (trayToggle_) trayToggle_->setText(action);
    if (tray_) tray_->setToolTip(running ? QStringLiteral("Voice SIP Robot · 运行中") : QStringLiteral("Voice SIP Robot · 已停止"));
    updateActions();
}

void MainWindow::setStatusPill(bool running, const QString &text)
{
    if (!statusPill_) return;
    statusPill_->setProperty("running", running);
    statusDot_->setProperty("running", running);
    statusPillText_->setText(text);
    for (QWidget *w : std::initializer_list<QWidget *>{statusPill_, statusDot_, statusPillText_}) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
}

void MainWindow::toggleLocal()
{
    if (localActive_) {
        QMetaObject::invokeMethod(local_, &LocalAudioSession::stop, Qt::QueuedConnection);
        return;
    }
    if (engine_.isRunning()) {
        QMessageBox::information(this, QStringLiteral("无法开始"),
                                 QStringLiteral("机器人服务运行中，请先停止后再开始本地对话。"));
        return;
    }
    const BotConfig config = formConfig();
    if (const QString error = config.validate(); !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("配置无效"), error);
        return;
    }
    localRecorder_->init(config); // 与本次对话的配置保持一致（路径/webhook）
    status_->setText(QStringLiteral("本地对话正在开始：建议佩戴耳机，以免扬声器回声误触发打断。"));
    QMetaObject::invokeMethod(local_, "start", Qt::QueuedConnection,
                              Q_ARG(qwenbot::BotConfig, config),
                              Q_ARG(QJsonArray, tools_ ? tools_->toolsJson() : QJsonArray()));
}

void MainWindow::applyLocalActive(bool active)
{
    localActive_ = active;
    if (active) {
        // 新一轮本地对话：重置记录采集状态
        localStartTime_ = QDateTime::currentDateTime();
        localTurns_ = 0;
        localTranscriptLog_ = QJsonArray();
        lastLocalUsage_ = ModelUsage();
        lastLocalFinishReason_.clear();
        // 同步在通话表创建本地对话行（CONFIRMED 语义：已开始计时/已接听）
        updateCall(localRowId, QStringLiteral("sip:local@local"), QStringLiteral("CONFIRMED"));
        if (auto rec = calls_.find(localRowId); rec != calls_.end()) {
            rec->callerNumber = QStringLiteral("本地麦克风");
            rec->calleeNumber = QStringLiteral("AI 助手");
            const int row = rec->idItem->row();
            rec->idItem->setText(QStringLiteral("本地"));
            callTable_->item(row, 1)->setText(rec->callerNumber);
            callTable_->item(row, 2)->setText(rec->calleeNumber);
            callTable_->item(row, 7)->setText(QStringLiteral("已接听"));
            callTable_->selectRow(row);
        }
    } else {
        pendingLocalToolTokens_.clear(); // 对话结束，丢弃未完成的工具调用
        writeLocalCallRecord();
        // 收尾表格行：挂断方、挂断原因、最终时长
        if (auto rec = calls_.find(localRowId); rec != calls_.end() && !rec->ended) {
            rec->hangupBy = QStringLiteral("user");
            updateCall(localRowId, QStringLiteral("sip:local@local"), QStringLiteral("DISCONNECTED"));
            callTable_->item(rec->idItem->row(), 12)->setText(lastLocalFinishReason_.left(80));
        }
    }
    localToggle_->setText(active ? QStringLiteral("结束本地对话") : QStringLiteral("开始本地对话"));
    const bool engineRunning = engine_.isRunning();
    startStop_->setEnabled(!active);
    localToggle_->setEnabled(!engineRunning);
    configuration_->setEnabled(!active && !engineRunning);
    setStatusPill(active, active ? QStringLiteral("本地对话中") : QStringLiteral("未启动"));
    showTranscript();
}

void MainWindow::writeLocalCallRecord()
{
    // 仅在对话真正开始过的情况下写记录；写后置空时间戳防止重复写入
    if (!localStartTime_.isValid() || !localRecorder_)
        return;
    const BotConfig config = formConfig(); // 对话期间配置面板禁用，表单仍是启动时的值
    const QDateTime now = QDateTime::currentDateTime();
    qwenbot::CallRecord record; // 显式限定：MainWindow 内有同名嵌套 struct CallRecord（通话列表条目）
    record.callId = QStringLiteral("local-%1").arg(localStartTime_.toString(QStringLiteral("yyyyMMddHHmmsszzz")));
    record.caller = QStringLiteral("local");
    record.callee = QStringLiteral("local");
    record.voice = config.voice;
    record.startTime = localStartTime_;
    record.answerTime = localStartTime_;
    record.hangupTime = now;
    record.answered = true;
    record.allowBargeIn = config.allowBargeIn;
    record.turns = localTurns_;
    record.hangupBy = QStringLiteral("user");
    record.hangupReason = lastLocalFinishReason_;
    record.durationMs = localStartTime_.msecsTo(now);
    record.callDurationMs = record.durationMs;
    record.transcript = localTranscriptLog_;
    record.usage = lastLocalUsage_.toJson();
    localRecorder_->write(record);
    localStartTime_ = QDateTime();
    localTranscriptLog_ = QJsonArray();
    lastLocalUsage_ = ModelUsage();
}

void MainWindow::handleLocalUsageUpdated(const ModelUsage &usage)
{
    lastLocalUsage_ = usage;
    // 同步刷新通话表本地行的 Token 消耗列（悬停显示细分）
    if (auto rec = calls_.find(localRowId); rec != calls_.end() && !rec->ended) {
        const auto [summary, tip] = usageTexts(usage);
        rec->usageSummary = summary;
        rec->usageTip = tip;
        auto *item = callTable_->item(rec->idItem->row(), 13);
        item->setText(summary);
        if (!tip.isEmpty()) item->setToolTip(tip);
    }
}

void MainWindow::appendLocalTranscript(const QString &speaker, const QString &text)
{
    if (text.isEmpty()) return;
    const QString stamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    // 同步收集对话记录（与 SIP 通话记录 transcript 字段格式一致）
    if (localActive_) {
        QJsonObject entry;
        entry[QStringLiteral("time")] = stamp;
        entry[QStringLiteral("speaker")] = speaker;
        entry[QStringLiteral("text")] = text;
        localTranscriptLog_.append(entry);
    }
    const QString display = speaker == QStringLiteral("caller") ? QStringLiteral("用户") : speaker;
    const QString entry = QStringLiteral("[%1] %2：%3")
        .arg(QDateTime::fromString(stamp, Qt::ISODateWithMs).toString(QStringLiteral("HH:mm:ss.zzz")),
             safeText(display).left(80), safeText(text).left(maxEntryCharacters));
    localTranscript_.append(entry);
    while (localTranscript_.size() > maxTranscriptLines)
        localTranscript_.removeFirst();
    // 同步追加到通话表本地行的转写缓存（选中本地行时下方转写区可回看）
    addTranscript(localRowId, display, text);
    if (localActive_) {
        transcript_->setPlainText(localTranscript_.join(QLatin1Char('\n')));
        transcript_->moveCursor(QTextCursor::End);
    }
}

void MainWindow::shutdownAudio()
{
    if (!audioThread_ || !local_)
        return;
    if (audioThread_->isRunning()) {
        // 同步停止设备/会话，避免退出时仍在播放或采集
        QMetaObject::invokeMethod(local_, &LocalAudioSession::stop,
                                  Qt::BlockingQueuedConnection);
        audioThread_->quit();
        audioThread_->wait(3000);
    }
    delete local_;
    local_ = nullptr;
    localActive_ = false;
}

void MainWindow::saveSettings()
{
    const BotConfig config = formConfig();
    // 对端 IP 列表必须全部是 IPv4 地址或 CIDR 网段，否则禁止提交
    for (const auto &peer : config.allowedPeers)
        if (const QString error = BotConfig::peerEntryError(peer); !error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("不能保存"), error);
            return;
        }
    // Endpoints must not carry authentication that would then be persisted.
    const QUrlQuery query(config.endpoint);
    if (!config.endpoint.userInfo().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("不能保存"), QStringLiteral("WSS 端点不能包含用户名或密码，请通过 API Key 字段认证。"));
        return;
    }
    for (const auto &item : query.queryItems()) {
        const QString key = item.first.toLower();
        if (key.contains(QStringLiteral("key")) || key.contains(QStringLiteral("token")) ||
            key.contains(QStringLiteral("secret")) || key.contains(QStringLiteral("password")) ||
            key.contains(QStringLiteral("authorization"))) {
            QMessageBox::warning(this, QStringLiteral("不能保存"), QStringLiteral("WSS 端点不能含密钥或令牌查询参数，请通过 API Key 字段认证。"));
            return;
        }
    }
    saveConfig(settings_, config);
    // MCP 插件命令热插拔：差异比对，仅启动新增、停止移除
    if (tools_)
        tools_->applyMcpCommands(config.mcpCommands);
    if (settings_.status() != QSettings::NoError)
        QMessageBox::critical(this, QStringLiteral("保存失败"), QStringLiteral("无法写入配置文件，请检查文件权限。"));
    else
        status_->setText(QStringLiteral("已保存非敏感配置；密码与 API Key 仅保留在本次会话。"));
}

void MainWindow::quitApplication()
{
    quitting_ = true;
    shutdownAudio();
    engine_.stop();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!quitting_ && tray_ && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        event->ignore();
        return;
    }
    shutdownAudio();
    engine_.stop();
    event->accept();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonDblClick &&
        (watched == instructions_ || watched == greeting_ || watched == allowedPeers_)) {
        auto *editor = qobject_cast<QPlainTextEdit *>(watched);
        if (!editor) return QMainWindow::eventFilter(watched, event);
        QString title;
        if (watched == instructions_) title = QStringLiteral("系统指令");
        else if (watched == greeting_) title = QStringLiteral("开场白指令");
        else title = QStringLiteral("允许的对端 IP（每行一个）");
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("放大编辑 · ") + title);
        dialog.resize(640, 440);
        auto *dlgLayout = new QVBoxLayout(&dialog);
        // 对端 IP 列表：紧凑视图逗号分隔省空间，放大视图保持每行一个
        QString initial = editor->toPlainText();
        if (watched == allowedPeers_)
            initial = initial.split(QRegularExpression(QStringLiteral("[\\s,;]+")),
                                    Qt::SkipEmptyParts).join(QLatin1Char('\n'));
        auto *edit = new QPlainTextEdit(initial, &dialog);
        edit->setPlaceholderText(editor->placeholderText());
        dlgLayout->addWidget(edit);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        dlgLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) {
            if (watched == allowedPeers_)
                editor->setPlainText(edit->toPlainText()
                                         .split(QRegularExpression(QStringLiteral("[\\s,;]+")),
                                                Qt::SkipEmptyParts)
                                         .join(QStringLiteral(", ")));
            else
                editor->setPlainText(edit->toPlainText());
        }
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateCall(int id, const QString &peer, const QString &state)
{
    auto existing = calls_.find(id);
    if (existing != calls_.end() && existing->ended && state == QStringLiteral("DISCONNECTED"))
        return;
    if (existing == calls_.end()) {
        if (calls_.size() >= maxHistoryCalls) {
            for (int row = 0; row < callTable_->rowCount(); ++row) {
                const int oldId = callTable_->item(row, 0)->data(Qt::UserRole).toInt();
                const auto oldRecord = calls_.constFind(oldId);
                if (oldRecord != calls_.cend() && oldRecord->ended) {
                    calls_.remove(oldId);
                    callTable_->removeRow(row);
                    break;
                }
            }
        }
        const int row = callTable_->rowCount();
        callTable_->insertRow(row);
        CallRecord record;
        record.idItem = new QTableWidgetItem(QString::number(id));
        record.idItem->setData(Qt::UserRole, id);
        callTable_->setItem(row, 0, record.idItem);
        for (int column = 1; column < 14; ++column)
            callTable_->setItem(row, column, new QTableWidgetItem);
        callTable_->item(row, 6)->setText(allowBargeIn_->isChecked()
            ? QStringLiteral("打断") : QStringLiteral("不打断"));
        existing = calls_.insert(id, record);
    } else if (existing->ended) {
        auto *idItem = existing->idItem;
        *existing = CallRecord{};
        existing->idItem = idItem;
    }
    CallRecord &record = existing.value();
    record.state = state;
    const int row = record.idItem->row();
    // 解析主叫/被叫（peer 格式为 sip:xxx@host）
    if (record.callerNumber.isEmpty()) {
        static const QRegularExpression re(QStringLiteral("sip:([^@;>]+)"));
        const auto match = re.match(peer);
        record.callerNumber = match.hasMatch() ? match.captured(1) : peer;
    }
    if (record.calleeNumber.isEmpty()) {
        // 被叫为本机号码，从 SIP Identity 提取
        static const QRegularExpression re(QStringLiteral("sip:([^@;>]+)"));
        const auto match = re.match(identity_->text());
        record.calleeNumber = match.hasMatch() ? match.captured(1) : identity_->text();
    }
    callTable_->item(row, 1)->setText(safeText(record.callerNumber).left(80));
    callTable_->item(row, 2)->setText(safeText(record.calleeNumber).left(80));
    callTable_->item(row, 3)->setText(stateText(state));
    if (state == QStringLiteral("CONFIRMED") && !record.connected) {
        record.connected = true;
        record.answered = true;
        record.connectedAt.start();
        record.answerTime = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        callTable_->item(row, 8)->setText(record.answerTime);
    }
    if (state == QStringLiteral("DISCONNECTED")) {
        record.ended = true;
        record.finalDuration = record.connected ? record.connectedAt.elapsed() : 0;
        if (record.hangupTime.isEmpty())
            record.hangupTime = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        callTable_->item(row, 3)->setText(QStringLiteral("已结束"));
        callTable_->item(row, 4)->setText(
            record.connected ? durationText(record.finalDuration) : QStringLiteral("未接通"));
        callTable_->item(row, 5)->setText(QString::number(record.turns));
        callTable_->item(row, 7)->setText(record.answered ? QStringLiteral("已接听") : QStringLiteral("未接听"));
        callTable_->item(row, 9)->setText(record.hangupTime);
        callTable_->item(row, 10)->setText(hangupByText(record.hangupBy));
    }
    if (callTable_->currentRow() < 0) callTable_->selectRow(row);
    updateActions();
    updateDurations();
    showTranscript();
}

void MainWindow::endCall(int id)
{
    auto record = calls_.find(id);
    if (record == calls_.end() || record->ended) return;
    record->ended = true;
    record->finalDuration = record->connected ? record->connectedAt.elapsed() : 0;
    const int row = record->idItem->row();
    callTable_->item(row, 3)->setText(QStringLiteral("已结束"));
    callTable_->item(row, 4)->setText(
        record->connected ? durationText(record->finalDuration) : QStringLiteral("未接通"));
    updateActions();
}

int MainWindow::selectedCall() const
{
    const int row = callTable_->currentRow();
    const auto *item = row >= 0 ? callTable_->item(row, 0) : nullptr;
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

void MainWindow::updateActions()
{
    const auto record = calls_.constFind(selectedCall());
    const bool active = engine_.isRunning() && record != calls_.cend() && !record->ended;
    answer_->setEnabled(active && (record->state == QStringLiteral("INCOMING") || record->state == QStringLiteral("EARLY")));
    hangup_->setEnabled(active);
}

void MainWindow::updateDurations()
{
    for (auto record = calls_.cbegin(); record != calls_.cend(); ++record) {
        if (record->ended) continue;
        const qint64 duration = record->connected ? record->connectedAt.elapsed() : 0;
        callTable_->item(record->idItem->row(), 4)->setText(
            record->connected ? durationText(duration) : QStringLiteral("未接通"));
    }
}

void MainWindow::showTranscript()
{
    if (localActive_) {
        transcript_->setPlainText(localTranscript_.join(QLatin1Char('\n')));
        return;
    }
    const auto record = calls_.constFind(selectedCall());
    transcript_->setPlainText(record == calls_.cend() ? QString() : record->transcript.join(QLatin1Char('\n')));
}

void MainWindow::addTranscript(int id, const QString &speaker, const QString &text)
{
    auto record = calls_.find(id);
    if (record == calls_.end() || record->ended || text.isEmpty()) return;
    const QString entry = QStringLiteral("[%1] %2：%3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             safeText(speaker).left(80), safeText(text).left(maxEntryCharacters));
    record->transcript.append(entry);
    record->transcriptSize += entry.size();
    while (record->transcript.size() > maxTranscriptLines || record->transcriptSize > maxTranscriptCharacters)
        record->transcriptSize -= record->transcript.takeFirst().size();
    if (selectedCall() == id) {
        showTranscript();
        transcript_->moveCursor(QTextCursor::End);
    }
}

QString MainWindow::safeText(const QString &text) const
{
    QString result = text;
    const QString key = apiKey_->text().trimmed();
    if (!key.isEmpty()) result.replace(key, QStringLiteral("[已隐藏]"));
    if (!sipPassword_->text().isEmpty()) result.replace(sipPassword_->text(), QStringLiteral("[已隐藏]"));
    return result;
}

void MainWindow::appendLog(const QString &text)
{
    // 统一落盘运行日志（logger 由 main 创建；此处只写文件，界面截断不影响文件全文）
    if (auto *logger = FileLogger::instance())
        logger->write(text);
    logs_->appendPlainText(QStringLiteral("[%1] %2")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
             safeText(text).left(maxEntryCharacters)));
}

// 查看本地通话记录文件（含轮转历史 .N），表格列出各条记录，选中显示详情与对话转写
void MainWindow::showRecordHistory()
{
    const QString path = resolveDataPath(
        normalizeRecordPath(callRecordPath_->text()), BotConfig().callRecordPath);
    // 从旧到新收集轮转文件与当前文件（.maxFiles → ... → .1 → 当前）
    const int maxFiles = qBound(1, callRecordMaxFiles_->value(), 100);
    QStringList files;
    for (int i = maxFiles; i >= 1; --i) {
        const QString candidate = QStringLiteral("%1.%2").arg(path).arg(i);
        if (QFileInfo::exists(candidate))
            files << candidate;
    }
    if (QFileInfo::exists(path))
        files << path;
    if (files.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("历史通话记录"),
            QStringLiteral("尚未生成通话记录文件：\n%1\n\n完成一次通话（或本地对话）后重试。").arg(path));
        return;
    }
    QList<QJsonObject> records;
    for (const QString &file : files) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        while (!f.atEnd()) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readLine());
            if (doc.isObject())
                records.append(doc.object());
        }
    }
    constexpr int maxShownRecords = 1000; // 防大文件卡顿：仅展示最近 1000 条
    if (records.size() > maxShownRecords)
        records.remove(0, records.size() - maxShownRecords);
    if (records.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("历史通话记录"),
            QStringLiteral("记录文件存在但没有可解析的记录：\n%1").arg(files.first()));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("历史通话记录 — %1").arg(path));
    dlg.resize(900, 620);
    auto *layout = new QVBoxLayout(&dlg);
    QString hintText = QStringLiteral("共 %1 条 · 选中记录查看详情与对话转写").arg(records.size());
    if (records.size() >= maxShownRecords)
        hintText.prepend(QStringLiteral("仅显示最近 %1 条，").arg(maxShownRecords));
    auto *hint = new QLabel(hintText, &dlg);
    hint->setObjectName(QStringLiteral("hint"));
    layout->addWidget(hint);
    auto *splitter = new QSplitter(Qt::Vertical, &dlg);
    auto *table = new QTableWidget(records.size(), 7, &dlg);
    table->setHorizontalHeaderLabels({
        QStringLiteral("开始时间"), QStringLiteral("主叫"), QStringLiteral("被叫"),
        QStringLiteral("通话时长"), QStringLiteral("轮次"), QStringLiteral("挂断方"), QStringLiteral("挂断原因")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(30);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 150);
    table->setColumnWidth(1, 110);
    table->setColumnWidth(2, 110);
    table->setColumnWidth(3, 80);
    table->setColumnWidth(4, 55);
    table->setColumnWidth(5, 80);
    auto centered = [](const QString &text) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(Qt::AlignCenter);
        return item;
    };
    for (int row = 0; row < records.size(); ++row) {
        const QJsonObject &obj = records.at(row);
        QString timeText = obj.value(QStringLiteral("answer_time")).toString();
        if (timeText.isEmpty())
            timeText = obj.value(QStringLiteral("start_time")).toString();
        const QDateTime start = QDateTime::fromString(timeText, Qt::ISODateWithMs);
        const qint64 durationMs = static_cast<qint64>(
            obj.value(QStringLiteral("call_duration_ms")).toDouble());
        QString duration = QStringLiteral("-");
        if (durationMs > 0)
            duration = QStringLiteral("%1:%2:%3")
                .arg(durationMs / 3600000, 2, 10, QLatin1Char('0'))
                .arg(durationMs / 60000 % 60, 2, 10, QLatin1Char('0'))
                .arg(durationMs / 1000 % 60, 2, 10, QLatin1Char('0'));
        table->setItem(row, 0, centered(start.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        table->setItem(row, 1, centered(obj.value(QStringLiteral("caller")).toString()));
        table->setItem(row, 2, centered(obj.value(QStringLiteral("callee")).toString()));
        table->setItem(row, 3, centered(duration));
        table->setItem(row, 4, centered(QString::number(obj.value(QStringLiteral("turns")).toInt())));
        table->setItem(row, 5, centered(obj.value(QStringLiteral("hangup_by")).toString()));
        table->setItem(row, 6, new QTableWidgetItem(obj.value(QStringLiteral("hangup_reason")).toString()));
    }
    splitter->addWidget(table);
    auto *detail = new QTextBrowser(&dlg);
    detail->setOpenExternalLinks(false);
    splitter->addWidget(detail);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);
    auto connectDetail = [&records, table, detail]() {
        const auto selected = table->selectionModel()->selectedRows();
        if (selected.isEmpty())
            return;
        const QJsonObject &obj = records.at(selected.first().row());
        QString html = QStringLiteral(
            "<style>td{padding:2px 14px 2px 0;white-space:nowrap} th{text-align:left;padding-right:14px}</style>"
            "<table>"
            "<tr><th>开始时间</th><td>%1</td><th>应答时间</th><td>%2</td></tr>"
            "<tr><th>挂断时间</th><td>%3</td><th>通话时长</th><td>%4 s（振铃 %5 s）</td></tr>"
            "<tr><th>主叫 / 被叫</th><td>%6 / %7</td><th>SIP 状态码</th><td>%8</td></tr>"
            "<tr><th>接听状态</th><td>%9</td><th>挂断方</th><td>%10</td></tr>"
            "<tr><th>挂断原因</th><td colspan=\"3\">%11</td></tr>"
            "<tr><th>轮次 / 打断方式</th><td>%12 / %13</td><th>音色 / 模型</th><td>%14 / %15</td></tr>")
            .arg(obj.value(QStringLiteral("start_time")).toString(),
                 obj.value(QStringLiteral("answer_time")).toString(),
                 obj.value(QStringLiteral("hangup_time")).toString(),
                 QString::number(obj.value(QStringLiteral("call_duration_sec")).toDouble(), 'f', 1),
                 QString::number(obj.value(QStringLiteral("duration_sec")).toDouble(), 'f', 1),
                 obj.value(QStringLiteral("caller")).toString(),
                 obj.value(QStringLiteral("callee")).toString(),
                 QString::number(obj.value(QStringLiteral("sip_status_code")).toInt()),
                 obj.value(QStringLiteral("answered")).toBool() ? QStringLiteral("已接听") : QStringLiteral("未接听"),
                 obj.value(QStringLiteral("hangup_by")).toString())
            .arg(obj.value(QStringLiteral("hangup_reason")).toString(),
                 QString::number(obj.value(QStringLiteral("turns")).toInt()),
                 obj.value(QStringLiteral("barge_in_mode")).toString() == QStringLiteral("interrupt")
                     ? QStringLiteral("允许打断") : QStringLiteral("不打断"),
                 obj.value(QStringLiteral("voice")).toString(),
                 obj.value(QStringLiteral("model_id")).toString());
        if (obj.contains(QStringLiteral("custom_variables"))) {
            const QJsonObject vars = obj.value(QStringLiteral("custom_variables")).toObject();
            QStringList items;
            for (auto it = vars.begin(); it != vars.end(); ++it)
                items << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
            html += QStringLiteral("<tr><th>自定义变量</th><td colspan=\"3\">%1</td></tr>").arg(items.join(QStringLiteral("，")));
        }
        if (obj.contains(QStringLiteral("usage"))) {
            const QJsonObject u = obj.value(QStringLiteral("usage")).toObject();
            if (!u.isEmpty()) {
                html += QStringLiteral(
                    "<tr><th>Token 消耗</th><td colspan=\"3\">输入 %1（音频 %2 · 文本 %3 · 缓存 %4）"
                    "　输出 %5（音频 %6 · 文本 %7）　合计 %8</td></tr>")
                    .arg(static_cast<qint64>(u.value(QStringLiteral("input_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("input_audio_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("input_text_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("cached_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("output_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("output_audio_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("output_text_tokens")).toDouble()))
                    .arg(static_cast<qint64>(u.value(QStringLiteral("total_tokens")).toDouble()));
            }
        }
        html += QStringLiteral("</table>");
        const QJsonArray transcript = obj.value(QStringLiteral("transcript")).toArray();
        if (!transcript.isEmpty()) {
            html += QStringLiteral("<hr><b>对话转写（%1 条）</b>").arg(transcript.size());
            for (const auto &value : transcript) {
                const QJsonObject entry = value.toObject();
                const QString speaker = entry.value(QStringLiteral("speaker")).toString();
                const QDateTime time = QDateTime::fromString(
                    entry.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs);
                html += QStringLiteral("<div><span style=\"color:%1\">[%2] %3</span>　%4</div>")
                    .arg(speaker == QStringLiteral("AI") ? QStringLiteral("#4f46e5") : QStringLiteral("#0f766e"),
                         time.toString(QStringLiteral("HH:mm:ss")),
                         speaker == QStringLiteral("AI") ? QStringLiteral("机器人")
                             : speaker == QStringLiteral("caller") ? QStringLiteral("用户") : speaker,
                         entry.value(QStringLiteral("text")).toString().toHtmlEscaped());
            }
        }
        detail->setHtml(html);
        detail->verticalScrollBar()->setValue(0);
    };
    connect(table, &QTableWidget::itemSelectionChanged, &dlg, connectDetail);
    auto *buttons = new QDialogButtonBox(&dlg);
    auto *openFolder = buttons->addButton(QStringLiteral("打开所在文件夹"), QDialogButtonBox::ActionRole);
    auto *closeBtn = buttons->addButton(QDialogButtonBox::Close);
    connect(openFolder, &QPushButton::clicked, &dlg, [path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    layout->addWidget(buttons);
    table->selectRow(records.size() - 1); // 默认选中最新一条
    dlg.exec();
}

// 查看运行日志文件（含轮转历史 .N），下拉切换文件，加载末尾内容
void MainWindow::showLogFileViewer()
{
    if (logPath_->text().trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("运行日志"),
            QStringLiteral("文件日志未启用（配置页「运行日志路径」为空）。"));
        return;
    }
    const QString path = resolveDataPath(logPath_->text(), BotConfig().logPath);
    const int maxFiles = qBound(1, logMaxFiles_->value(), 100);
    QStringList files; // 从新到旧：当前文件 → .1 → ...
    if (QFileInfo::exists(path))
        files << path;
    for (int i = 1; i <= maxFiles; ++i) {
        const QString candidate = QStringLiteral("%1.%2").arg(path).arg(i);
        if (QFileInfo::exists(candidate))
            files << candidate;
    }
    if (files.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("运行日志"),
            QStringLiteral("尚未生成日志文件：\n%1\n\n启动程序后重试。").arg(path));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("运行日志 — %1").arg(path));
    dlg.resize(940, 640);
    auto *layout = new QVBoxLayout(&dlg);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(new QLabel(QStringLiteral("日志文件"), &dlg));
    auto *selector = new QComboBox(&dlg);
    for (const QString &file : files) {
        const QString name = QFileInfo(file).fileName();
        selector->addItem(file == path ? name + QStringLiteral("（当前）") : name, file);
    }
    fileRow->addWidget(selector, 1);
    auto *tailHint = new QLabel(QStringLiteral("加载文件末尾 512KB"), &dlg);
    tailHint->setObjectName(QStringLiteral("hint"));
    fileRow->addWidget(tailHint);
    layout->addLayout(fileRow);
    auto *view = new QPlainTextEdit(&dlg);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = view->font();
    mono.setStyleHint(QFont::TypeWriter);
    mono.setFamily(QStringLiteral("Cascadia Mono"));
    mono.setPointSize(9);
    view->setFont(mono);
    layout->addWidget(view, 1);
    auto loadFile = [view](const QString &file) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            view->setPlainText(QStringLiteral("无法打开文件：%1").arg(file));
            return;
        }
        constexpr qint64 tailBytes = 512 * 1024;
        if (f.size() > tailBytes) {
            f.seek(f.size() - tailBytes);
            f.readLine(); // 丢弃被截断的半行
        }
        view->setPlainText(QString::fromUtf8(f.readAll()));
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum()); // 定位到最新
    };
    connect(selector, &QComboBox::currentIndexChanged, &dlg,
        [selector, loadFile](int index) { loadFile(selector->itemData(index).toString()); });
    auto *buttons = new QDialogButtonBox(&dlg);
    auto *openFolder = buttons->addButton(QStringLiteral("打开所在文件夹"), QDialogButtonBox::ActionRole);
    auto *closeBtn = buttons->addButton(QDialogButtonBox::Close);
    connect(openFolder, &QPushButton::clicked, &dlg, [path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    layout->addWidget(buttons);
    loadFile(files.first());
    dlg.exec();
}

} // namespace qwenbot
