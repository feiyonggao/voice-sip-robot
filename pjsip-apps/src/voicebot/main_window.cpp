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
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileInfo>
#include <QJsonArray>
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
#include <QSaveFile>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QUrl>
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
#include <QWheelEvent>
#include <QSysInfo>
#include <functional>
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

// 工具描述缓存：插件未连接时用上次 tools/list 的结果填充「工具启用」表，
// 避免出现空白行。值用 Base64 JSON，规避 QSettings INI 对逗号/换行的拆分。
QString toolCacheGroup(const QString &pluginName)
{
    QString key = pluginName;
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return QStringLiteral("tools/cache/") + key + QLatin1Char('/');
}

QList<QPair<QString, QString>> loadCachedTools(QSettings &settings, const QString &pluginName)
{
    QList<QPair<QString, QString>> result;
    if (pluginName.isEmpty()) return result;
    const QString raw = settings.value(
        toolCacheGroup(pluginName) + QStringLiteral("tools")).toString();
    if (raw.isEmpty()) return result;
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray::fromBase64(raw.toLatin1()));
    if (!document.isArray()) return result;
    for (const auto &value : document.array()) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) continue;
        result.append({name, object.value(QStringLiteral("description")).toString()});
    }
    return result;
}

void saveCachedTools(QSettings &settings, const QString &pluginName,
                     const QList<ToolRegistry::Tool> &tools)
{
    if (pluginName.isEmpty() || tools.isEmpty()) return;
    QJsonArray array;
    for (const auto &tool : tools)
        array.append(QJsonObject{{QStringLiteral("name"), tool.name},
                                 {QStringLiteral("description"), tool.description}});
    settings.setValue(toolCacheGroup(pluginName) + QStringLiteral("tools"),
                      QString::fromLatin1(
                          QJsonDocument(array).toJson(QJsonDocument::Compact).toBase64()));
}

// 说明列：MCP 插件可能不提供 description，用可读占位代替空白行
QString toolDescriptionText(const QString &description)
{
    const QString text = description.trimmed();
    return text.isEmpty() ? QStringLiteral("（插件未提供工具说明）") : text;
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

// 配置页位于 QScrollArea 内，滚轮扫过 SpinBox 时容易误改数值。
// QAbstractSpinBox 默认使用 Qt::WheelFocus，悬停滚轮即可把焦点转到控件上并调值；
// 这里统一派生：仅点击/键盘聚焦后滚轮才调值，未聚焦时忽略事件，
// 让滚轮继续冒泡给页面级 QScrollArea 正常滚动。
class WheelSafeSpinBox final : public QSpinBox {
public:
    explicit WheelSafeSpinBox(QWidget *parent = nullptr) : QSpinBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus); // 排除 WheelFocus，滚轮不再自动获焦
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        if (!hasFocus()) {
            event->ignore();
            return;
        }
        QSpinBox::wheelEvent(event);
    }
};

// 配置页位于 QScrollArea 内，滚轮经过 ComboBox 会被当成切换选项，极易误改配置。
// 这里统一派生：滚轮事件一律忽略（不消费、不获焦），冒泡给页面级 QScrollArea 滚动；
// 只有点击弹出列表后选择、或键盘方向键/键入才会改变选中项。
// 说明：本应用的配置下拉均为不可编辑（setEditable 未启用），文本输入不受影响；
// 若将来有可编辑下拉，滚轮同样不会改动文本，仍可用键盘正常输入。
class WheelSafeComboBox final : public QComboBox {
public:
    explicit WheelSafeComboBox(QWidget *parent = nullptr) : QComboBox(parent)
    {
        setFocusPolicy(Qt::StrongFocus); // 排除 WheelFocus，滚轮不再自动获焦
    }

protected:
    void wheelEvent(QWheelEvent *event) override
    {
        // 无论是否已聚焦都不改值：忽略后由父级 QScrollArea 接收滚动
        event->ignore();
    }
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
    // 实时语音：新配置 qwen/workspaceId + qwen/region + qwen/model；
    // 兼容上一版 qwen/baseUrl + qwen/model，以及更早只有 qwen/endpoint 的完整端点。
    // 域名规则 wss://{WorkspaceId}.{region}.maas.aliyuncs.com/api-ws/v1/realtime
    const QString storedWorkspace = settings.value(QStringLiteral("qwen/workspaceId")).toString().trimmed();
    const QString storedRegion = settings.value(QStringLiteral("qwen/region")).toString().trimmed();
    const QString storedModel = settings.value(QStringLiteral("qwen/model")).toString().trimmed();
    const bool storedManual = booleanSetting(settings, QStringLiteral("qwen/manualEndpoint"), false);
    const QString legacyEndpoint = settings.value(QStringLiteral("qwen/endpoint")).toString().trimmed();
    const QString legacyBase = settings.value(QStringLiteral("qwen/baseUrl")).toString().trimmed();
    config.realtimeWorkspaceId = storedWorkspace;
    config.realtimeRegion = storedRegion.isEmpty() ? BotConfig::defaultRealtimeRegion() : storedRegion;
    config.realtimeModel = storedModel.isEmpty()
        ? BotConfig::realtimeModelChoices().last() : storedModel;
    config.realtimeManualEndpoint = storedManual && !legacyEndpoint.isEmpty();
    if (config.realtimeManualEndpoint) {
        config.endpoint = QUrl(legacyEndpoint, QUrl::StrictMode);
    } else if (!storedWorkspace.isEmpty()) {
        config.endpoint = BotConfig::buildRealtimeEndpoint(
            config.realtimeWorkspaceId, config.realtimeRegion, config.realtimeModel);
    } else if (!legacyEndpoint.isEmpty()) {
        // 旧完整端点：能识别 Workspace 域名就迁移成 WorkspaceId + 地域 + 模型，
        // 识别不了则转手动模式并保留原值，避免静默丢弃用户配置
        const QUrl parsed(legacyEndpoint, QUrl::StrictMode);
        QString workspace;
        QString region;
        QString model;
        if (parsed.isValid() &&
            BotConfig::parseRealtimeEndpoint(parsed, &workspace, &region, &model)) {
            config.realtimeWorkspaceId = workspace;
            config.realtimeRegion = region;
            if (!model.isEmpty()) config.realtimeModel = model;
            config.endpoint = BotConfig::buildRealtimeEndpoint(
                config.realtimeWorkspaceId, config.realtimeRegion, config.realtimeModel);
        } else {
            config.realtimeManualEndpoint = true;
            config.endpoint = parsed;
        }
    } else if (!legacyBase.isEmpty()) {
        // 上一版「服务地址 + 模型」：能解析成 Workspace 就迁移，否则保留为手动端点
        const QUrl base(legacyBase, QUrl::StrictMode);
        QString workspace;
        QString region;
        if (base.isValid() &&
            BotConfig::parseRealtimeEndpoint(base, &workspace, &region, nullptr)) {
            config.realtimeWorkspaceId = workspace;
            config.realtimeRegion = region;
            config.endpoint = BotConfig::buildRealtimeEndpoint(
                config.realtimeWorkspaceId, config.realtimeRegion, config.realtimeModel);
        } else {
            QUrl preserved = base;
            QUrlQuery query(preserved);
            query.addQueryItem(QStringLiteral("model"), config.realtimeModel);
            preserved.setQuery(query);
            config.realtimeManualEndpoint = true;
            config.endpoint = preserved;
        }
    } else {
        config.endpoint = BotConfig::buildRealtimeEndpoint(
            config.realtimeWorkspaceId, config.realtimeRegion, config.realtimeModel);
    }
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
    // 插件配置：新格式 tools/plugins（名称列表）+ 每插件 tools/plugin/<名称>/ 分组，
    // 可选 transport=stdio/http、url、headers；否则依次兼容旧 tools/enabledPlugins
    //（纯文件名，视为 stdio）与 tools/mcpCommands（完整命令）
    // tools/plugins 为空时继续兼容旧格式，避免手工只填 enabledPlugins 的配置被忽略
    const QStringList configuredPlugins =
        settings.value(QStringLiteral("tools/plugins")).toStringList();
    config.enabledPlugins = settings.value(QStringLiteral("tools/enabledPlugins")).toStringList();
    if (!configuredPlugins.isEmpty()) {
        config.mcpPlugins = loadMcpPlugins(settings);
    } else if (!config.enabledPlugins.isEmpty()) {
        for (const auto &file : config.enabledPlugins) {
            McpPluginSpec spec;
            spec.name = file;
            spec.transport = QStringLiteral("stdio");
            spec.command = ToolRegistry::commandForPlugin(file);
            config.mcpPlugins.append(spec);
        }
    } else {
        config.mcpCommands = parseMcpCommands(settings.value(QStringLiteral("tools/mcpCommands")).toString());
        for (const auto &command : config.mcpCommands) {
            const QStringList parts = QProcess::splitCommand(command);
            if (parts.isEmpty()) continue;
            McpPluginSpec spec;
            spec.transport = QStringLiteral("stdio");
            spec.command = command;
            spec.name = QFileInfo(parts.constLast()).fileName();
            if (spec.name.isEmpty()) spec.name = parts.constFirst();
            config.mcpPlugins.append(spec);
        }
    }
    // 派生旧字段，供只认识旧格式的版本/外部工具继续工作
    config.enabledPlugins.clear();
    config.mcpCommands.clear();
    const QStringList availablePlugins = ToolRegistry::scanPluginFiles();
    for (const auto &spec : config.mcpPlugins) {
        if (spec.isHttp()) continue;
        if (!spec.command.isEmpty()) config.mcpCommands.append(spec.command);
        if (availablePlugins.contains(spec.name)) config.enabledPlugins.append(spec.name);
    }
    if (settings.status() != QSettings::NoError)
        throw std::runtime_error("配置文件格式或读取错误");
    // 工具级禁用名单：键不存在即全部启用，与旧配置天然兼容
    config.disabledTools.clear();
    for (const auto &raw : settings.value(QStringLiteral("tools/disabledTools")).toStringList()) {
        const QString name = raw.trimmed();
        if (!name.isEmpty() && !config.disabledTools.contains(name))
            config.disabledTools.append(name);
    }
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
    settings.setValue(QStringLiteral("qwen/workspaceId"), config.realtimeWorkspaceId);
    settings.setValue(QStringLiteral("qwen/region"), config.realtimeRegion);
    settings.setValue(QStringLiteral("qwen/model"), config.realtimeModel);
    settings.setValue(QStringLiteral("qwen/manualEndpoint"), config.realtimeManualEndpoint);
    // 保留完整端点键，兼容旧版本与本机无界面模式
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
    saveMcpPlugins(settings, config.mcpPlugins);
    // 旧字段仅保留 stdio 本地插件，保证降级运行仍按旧语义启动
    QStringList legacyFiles;
    const QStringList availablePlugins = ToolRegistry::scanPluginFiles();
    for (const auto &spec : config.mcpPlugins) {
        if (spec.isHttp()) continue;
        if (availablePlugins.contains(spec.name)) legacyFiles.append(spec.name);
    }
    if (config.mcpPlugins.isEmpty()) legacyFiles = config.enabledPlugins;
    settings.setValue(QStringLiteral("tools/enabledPlugins"), legacyFiles);
    settings.remove(QStringLiteral("tools/mcpCommands"));
    // 工具级禁用：空名单直接移除键，保持旧版可读（缺键 = 全部启用）
    if (config.disabledTools.isEmpty())
        settings.remove(QStringLiteral("tools/disabledTools"));
    else
        settings.setValue(QStringLiteral("tools/disabledTools"), config.disabledTools);
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
    // 默认宽度取 1300：Qwen 卡片最小内容宽约 1210px（四列最小宽 + 标签列），
    // 1240 窗口客户区仅 1224px 会出现横向滚动条；1300 客户区 1284px，
    // 扣除边距与纵向滚动条后可视宽约 1234px，仍保留约 24px 余量
    resize(1300, 760);
    setMinimumSize(1040, 620);
    mcpPluginSpecs_ = config.mcpPlugins; // 工具页的唯一数据源（新增/上传/删除都改这份列表）
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
    // 配置页纵向按需滚动；横向同样按需：Qwen 卡片最小内容宽约 1210px，
    // 默认窗口下不出现横向滚动条，窄窗口下由页面级 QScrollArea 按需提供，避免卡片被裁
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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
        auto *field = new WheelSafeSpinBox(this);
        field->setRange(1, 65535);
        field->setValue(value);
        return field;
    };
    listenAddress_ = line(config.listenAddress);
    sipPort_ = port(config.sipPort);
    rtpPort_ = port(config.rtpPort);
    maxCalls_ = new WheelSafeSpinBox(this);
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
    // 三行字段共用一套视觉网格：标签列按本组最长标签的实测像素固定，
    // 标签右对齐、控件左对齐；行内与行间距统一 10px，行尾用弹性空白吸收剩余宽度。
    auto *qwenLayout = new QVBoxLayout(qwenBox);
    qwenLayout->setSpacing(10);
    QFont qwenMeasureFont = qwenBox->font();
    qwenMeasureFont.setPixelSize(13); // 与全局样式表字号一致，量出的宽度才可靠
    const QFontMetrics qwenMetrics(qwenMeasureFont);
    int qwenLabelWidth = 0;
    const QStringList qwenLabelTexts{
        QStringLiteral("WorkspaceId"), QStringLiteral("地域"), QStringLiteral("模型"),
        QStringLiteral("上下文轮次"), QStringLiteral("完整端点"), QStringLiteral("API Key"),
        QStringLiteral("音色"), QStringLiteral("音频分片"), QStringLiteral("轮次检测"),
        QStringLiteral("系统指令"), QStringLiteral("开场白指令"), QStringLiteral("自定义模型")};
    for (const auto &text : qwenLabelTexts)
        qwenLabelWidth = qMax(qwenLabelWidth, qwenMetrics.horizontalAdvance(text));
    qwenLabelWidth += 4; // 左对齐后留少量右侧余量，避免最长标签贴边被裁
    auto qwenLabel = [this, qwenLabelWidth](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setFixedWidth(qwenLabelWidth);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return label;
    };
    // 输入框最小宽 = 候选文本（当前值 / 占位文本 / 典型取值）中最宽的 advance
    // + 左内边距 11 + 右内边距 6 + 边框 2 + 呼吸位；控件不固定宽度，
    // 由列网格 stretch 随窗口伸展，变窄时收缩到该最小宽（配合页面级横向滚动）。
    auto qwenLineWidth = [](const QStringList &basisTexts, int spare) {
        QFont measureFont = QApplication::font();
        measureFont.setPixelSize(13);
        const QFontMetrics metrics(measureFont);
        int advance = 0;
        for (const auto &text : basisTexts)
            advance = qMax(advance, metrics.horizontalAdvance(text));
        return advance + 11 + 6 + 2 + spare;
    };
    // 下拉框最小宽 = 最长选项文本 advance + 左内边距 8 + 右内边距 4 + 下拉箭头 22
    // + 边框 2 + 6px 余量；QSS 的 padding/drop-down 不参与 QComboBox::sizeHint，
    // 仅靠 AdjustToContents 会把文字挤到分隔线上，故按实测值给最小宽度。
    auto qwenComboWidth = [](QComboBox *combo) {
        QFont measureFont = QApplication::font();
        measureFont.setPixelSize(13);
        const QFontMetrics metrics(measureFont);
        int textWidth = 0;
        for (int index = 0; index < combo->count(); ++index)
            textWidth = qMax(textWidth, metrics.horizontalAdvance(combo->itemText(index)));
        return textWidth + 8 + 4 + 22 + 2 + 6;
    };
    // SpinBox 最小宽 = 最大数值文本 + 左内边距 8 + 右内边距 4 + 上下按钮 20
    // + 边框 2 + 6px 呼吸位
    auto qwenSpinWidth = [](const QString &maxText) {
        QFont measureFont = QApplication::font();
        measureFont.setPixelSize(13);
        const QFontMetrics metrics(measureFont);
        return metrics.horizontalAdvance(maxText) + 8 + 4 + 20 + 2 + 6;
    };
    workspaceId_ = line(config.realtimeWorkspaceId);
    workspaceId_->setPlaceholderText(QStringLiteral("llm-xxxxxxxx"));
    workspaceId_->setToolTip(QStringLiteral(
        "阿里云百炼业务空间 ID（WorkspaceId）。\n"
        "程序按官方规则拼装：wss://{WorkspaceId}.{地域}.maas.aliyuncs.com/api-ws/v1/realtime\n"
        "只填 ID 本身，不要带协议前缀或路径。"));
    // 按当前已配置 ID 与占位文本量算；随后再与 API Key 同列对齐（见下方统一设宽）
    realtimeRegion_ = new WheelSafeComboBox(this);
    for (const auto &region : BotConfig::realtimeRegionChoices())
        realtimeRegion_->addItem(BotConfig::realtimeRegionName(region), region);
    realtimeRegion_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    realtimeRegion_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    realtimeRegion_->setToolTip(QStringLiteral(
        "选择业务空间所在地域：华北2（北京）cn-beijing / 新加坡 ap-southeast-1。\n"
        "地域决定域名中的 {region} 段，需与 WorkspaceId 所在地域一致。"));
    const int regionIndex = realtimeRegion_->findData(config.realtimeRegion);
    if (regionIndex >= 0) realtimeRegion_->setCurrentIndex(regionIndex);
    realtimeModel_ = new WheelSafeComboBox(this);
    for (const auto &name : BotConfig::realtimeModelChoices())
        realtimeModel_->addItem(name, name);
    realtimeModel_->addItem(QStringLiteral("自定义…"), QStringLiteral("__custom__"));
    realtimeModel_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    realtimeModel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    realtimeModel_->setToolTip(QStringLiteral(
        "内置 qwen-audio-3.1-realtime-flash / plus 两个模型；选择「自定义」可手填模型名。"));
    customModel_ = line(QString());
    customModel_->setPlaceholderText(QStringLiteral("输入模型名称"));
    customModel_->setToolTip(QStringLiteral("自定义模型名仅允许字母、数字、点、下划线和短横线。"));
    customModel_->setMinimumWidth(qwenLineWidth({QStringLiteral("my-custom-model")}, 8));
    // 自定义模型输入单独成行（仅选「自定义…」时显示），不占首行宽度
    auto *customRow = new QWidget(this);
    const QString modelName = config.realtimeModel.trimmed();
    int modelIndex = realtimeModel_->findData(modelName);
    if (modelIndex < 0) {
        modelIndex = realtimeModel_->count() - 1; // 自定义
        customModel_->setText(modelName);
    }
    realtimeModel_->setCurrentIndex(modelIndex);
    // 最长选项为 28 字符模型名，按实测给最小宽度，避免 QSS 箭头吃掉文字
    realtimeModel_->setMinimumWidth(qwenComboWidth(realtimeModel_));
    auto updateCustomModel = [this, customRow] {
        const bool custom = realtimeModel_->currentData().toString() == QStringLiteral("__custom__");
        customModel_->setVisible(custom);
        customRow->setVisible(custom);
    };
    connect(realtimeModel_, &QComboBox::currentIndexChanged, this, updateCustomModel);
    updateCustomModel();
    manualEndpoint_ = new QCheckBox(QStringLiteral("手动指定完整端点"), this);
    manualEndpoint_->setChecked(config.realtimeManualEndpoint);
    manualEndpoint_->setToolTip(QStringLiteral(
        "高级兜底：勾选后忽略「WorkspaceId + 地域 + 模型」，直接使用完整端点；\n"
        "仅适用于特殊实例或调试。"));
    endpoint_ = line(config.endpoint.toString());
    endpoint_->setPlaceholderText(QStringLiteral("wss://.../realtime?model=..."));
    endpoint_->setToolTip(QStringLiteral(
        "非手动模式显示自动拼装结果（只读）；勾选「手动指定完整端点」后可编辑，必须以 wss:// 开头。"));
    // 非手动模式下随 WorkspaceId / 地域 / 模型的改动实时刷新拼装预览
    auto updatePreview = [this] {
        if (manualEndpoint_->isChecked()) return;
        const QString model = realtimeModel_->currentData().toString() == QStringLiteral("__custom__")
            ? customModel_->text().trimmed() : realtimeModel_->currentText();
        endpoint_->setText(BotConfig::buildRealtimeEndpoint(
            workspaceId_->text().trimmed(), realtimeRegion_->currentData().toString(), model).toString());
    };
    auto updateEndpointMode = [this, updatePreview] {
        const bool manual = manualEndpoint_->isChecked();
        workspaceId_->setEnabled(!manual);
        realtimeRegion_->setEnabled(!manual);
        realtimeModel_->setEnabled(!manual);
        customModel_->setEnabled(!manual);
        endpoint_->setEnabled(manual);
        if (!manual) updatePreview();
    };
    connect(manualEndpoint_, &QCheckBox::toggled, this, updateEndpointMode);
    connect(workspaceId_, &QLineEdit::textChanged, this, [updatePreview] { updatePreview(); });
    connect(realtimeRegion_, &QComboBox::currentIndexChanged, this, [updatePreview] { updatePreview(); });
    connect(realtimeModel_, &QComboBox::currentIndexChanged, this, [updatePreview] { updatePreview(); });
    connect(customModel_, &QLineEdit::textChanged, this, [updatePreview] { updatePreview(); });
    updateEndpointMode();
    // 端点框最小宽：按占位文本、当前端点与常见自动拼装端点（96 字符）量算；
    // 实际宽度随窗口伸展，复选框紧跟其后（间距由网格 10px 保证）
    endpoint_->setMinimumWidth(qwenLineWidth(
        {endpoint_->placeholderText(), endpoint_->text(),
         QStringLiteral("wss://llm-xxxxxxxxxxxxxxxx.cn-beijing.maas.aliyuncs.com/"
                        "api-ws/v1/realtime?model=qwen-audio-3.1-realtime-plus")}, 10));
    apiKey_ = line(config.apiKey);
    apiKey_->setEchoMode(QLineEdit::Password);
    // 占位提示只保留能完整显示的部分，完整说明放到字段下方（可换行），避免被裁剪
    apiKey_->setPlaceholderText(QStringLiteral("仅本次会话"));
    // DashScope Key 实际形态为 sk- + 32 位十六进制（35 字符），按该样本与
    // 当前密钥、占位文本中最宽者量算 + 24px 内边距边框 + 8px 余量，确保不滚动不截断
    apiKey_->setMinimumWidth(qwenLineWidth(
        {QStringLiteral("sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"),
         config.apiKey, apiKey_->placeholderText()}, 8));
    // 第 1、3 行同列等宽：列最小宽取两者较大值，控件填满列，两行 x 始终对齐
    workspaceId_->setMinimumWidth(qwenLineWidth(
        {config.realtimeWorkspaceId, workspaceId_->placeholderText()}, 8));
    voice_ = line(config.voice);
    // 按当前音色与最长常见音色名（longxiaochun_v3.1，17 字符）实测给最小宽
    voice_->setMinimumWidth(qwenLineWidth(
        {config.voice, QStringLiteral("longxiaochun_v3.1")}, 8));
    // 地域最小宽按最长选项文本 + 箭头区重算，保证「华北2（北京）」不被分隔线截断；
    // 与音色同列，运行时由列网格拉伸到同一宽度
    realtimeRegion_->setMinimumWidth(qwenComboWidth(realtimeRegion_));
    instructions_ = new QPlainTextEdit(config.instructions, this);
    instructions_->setMaximumHeight(130);
    instructions_->setPlaceholderText(QStringLiteral("双击可放大编辑"));
    greeting_ = new QPlainTextEdit(config.greeting, this);
    greeting_->setMaximumHeight(70);
    greeting_->setPlaceholderText(QStringLiteral("双击可放大编辑"));
    turnMode_ = new WheelSafeComboBox(this);
    turnMode_->addItem(QStringLiteral("智能轮次（smart_turn）"), QStringLiteral("smart_turn"));
    turnMode_->addItem(QStringLiteral("服务端 VAD（server_vad）"), QStringLiteral("server_vad"));
    turnMode_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    turnMode_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    int turnIndex = turnMode_->findData(config.turnMode);
    if (turnIndex < 0) {
        turnMode_->addItem(config.turnMode, config.turnMode);
        turnIndex = turnMode_->count() - 1;
    }
    turnMode_->setCurrentIndex(turnIndex);
    // 最长选项「服务端 VAD（server_vad）」advance 162，按实测给最小宽，避免文字被箭头遮挡
    turnMode_->setMinimumWidth(qwenComboWidth(turnMode_));
    uplinkChunkMs_ = new WheelSafeSpinBox(this);
    uplinkChunkMs_->setRange(20, 100);
    uplinkChunkMs_->setSingleStep(20);
    uplinkChunkMs_->setValue(config.uplinkChunkMs);
    uplinkChunkMs_->setSuffix(QStringLiteral(" ms"));
    const QString chunkTip = QStringLiteral(
        "每次发送到模型的上行音频分片时长。\n20ms：延迟最低（默认）；100ms：官网建议值，上行消息量减少 80%。");
    uplinkChunkMs_->setToolTip(chunkTip);
    // 按可能出现的最大文本收紧：音频分片 max=100 + " ms"，上下文轮次 max=50
    uplinkChunkMs_->setMinimumWidth(qwenSpinWidth(QStringLiteral("100 ms")));
    maxHistoryTurns_ = new WheelSafeSpinBox(this);
    maxHistoryTurns_->setRange(1, 50);
    maxHistoryTurns_->setValue(config.maxHistoryTurns);
    maxHistoryTurns_->setMinimumWidth(qwenSpinWidth(QStringLiteral("50")));
    maxHistoryTurns_->setToolTip(QStringLiteral(
        "阿里官方参数 session.max_history_turns：单次请求携带的对话历史轮次上限，\n"
        "范围 1–50，默认 20。调大可保留更长对话记忆，token 消耗更高；调小响应更快。"));
    // 第 1、3 行共用同一套 4 列：列最小宽取两行同列字段的较大实测内容宽，
    // 两个网格设置相同的 columnMinimumWidth 与 stretch，保证列起点 x 严格一致；
    // 字段填满列并按 3:2:2:1 随窗口伸展，变窄时收缩回最小宽（超出部分由页面横向滚动承接）。
    const int qwenColumnWidth[4] = {
        qMax(workspaceId_->minimumWidth(), apiKey_->minimumWidth()),
        qMax(realtimeRegion_->minimumWidth(), voice_->minimumWidth()),
        qMax(realtimeModel_->minimumWidth(), turnMode_->minimumWidth()),
        qMax(maxHistoryTurns_->minimumWidth(), uplinkChunkMs_->minimumWidth())};
    const int qwenColumnStretch[4] = {3, 2, 2, 1}; // 按各列内容量比例分配新增宽度
    // 第 1 行：WorkspaceId / 地域 / 模型 / 上下文轮次
    auto *qwenRow1 = new QWidget(this);
    auto *row1Grid = new QGridLayout(qwenRow1);
    row1Grid->setContentsMargins(0, 0, 0, 0);
    row1Grid->setHorizontalSpacing(10);
    row1Grid->setVerticalSpacing(0);
    for (int column = 0; column < 4; ++column) {
        row1Grid->setColumnMinimumWidth(column * 2 + 1, qwenColumnWidth[column]);
        row1Grid->setColumnStretch(column * 2 + 1, qwenColumnStretch[column]);
    }
    // 仅指定垂直对齐：水平方向由 Expanding 填满列宽，窗口变宽时字段随之变宽
    row1Grid->addWidget(qwenLabel(QStringLiteral("WorkspaceId")), 0, 0);
    row1Grid->addWidget(workspaceId_, 0, 1, Qt::AlignVCenter);
    row1Grid->addWidget(qwenLabel(QStringLiteral("地域")), 0, 2);
    row1Grid->addWidget(realtimeRegion_, 0, 3, Qt::AlignVCenter);
    row1Grid->addWidget(qwenLabel(QStringLiteral("模型")), 0, 4);
    row1Grid->addWidget(realtimeModel_, 0, 5, Qt::AlignVCenter);
    row1Grid->addWidget(qwenLabel(QStringLiteral("上下文轮次")), 0, 6);
    row1Grid->addWidget(maxHistoryTurns_, 0, 7, Qt::AlignVCenter);
    qwenLayout->addWidget(qwenRow1);

    // 自定义模型：仅选「自定义…」时单独出现一行，避免挤占第 1 行宽度
    auto *customGrid = new QGridLayout(customRow);
    customGrid->setContentsMargins(0, 0, 0, 0);
    customGrid->setHorizontalSpacing(10);
    customGrid->setVerticalSpacing(0);
    customGrid->setColumnStretch(2, 1);
    customGrid->addWidget(qwenLabel(QStringLiteral("自定义模型")), 0, 0);
    customGrid->addWidget(customModel_, 0, 1);
    qwenLayout->addWidget(customRow);

    // 第 2 行：标签在左，端点输入框随窗口伸展，复选框紧随其后（网格间距 10px），
    // 不再右对齐贴行尾；窗口变宽时端点框变宽、复选框随之右移，间距保持 10px。
    auto *endpointRow = new QWidget(this);
    auto *endpointGrid = new QGridLayout(endpointRow);
    endpointGrid->setContentsMargins(0, 0, 0, 0);
    endpointGrid->setHorizontalSpacing(10);
    endpointGrid->setVerticalSpacing(0);
    endpointGrid->setColumnStretch(1, 1);
    endpointGrid->addWidget(qwenLabel(QStringLiteral("完整端点")), 0, 0);
    endpointGrid->addWidget(endpoint_, 0, 1);
    endpointGrid->addWidget(manualEndpoint_, 0, 2, Qt::AlignLeft | Qt::AlignVCenter);
    qwenLayout->addWidget(endpointRow);

    // 第 3 行：API Key / 音色 / 轮次检测 / 音频分片（与第 1 行共用列宽，x 严格对齐）
    auto *qwenRow3 = new QWidget(this);
    auto *row3Grid = new QGridLayout(qwenRow3);
    row3Grid->setContentsMargins(0, 0, 0, 0);
    row3Grid->setHorizontalSpacing(10);
    row3Grid->setVerticalSpacing(4);
    for (int column = 0; column < 4; ++column) {
        row3Grid->setColumnMinimumWidth(column * 2 + 1, qwenColumnWidth[column]);
        row3Grid->setColumnStretch(column * 2 + 1, qwenColumnStretch[column]);
    }
    row3Grid->addWidget(qwenLabel(QStringLiteral("API Key")), 0, 0);
    row3Grid->addWidget(apiKey_, 0, 1, Qt::AlignVCenter);
    row3Grid->addWidget(qwenLabel(QStringLiteral("音色")), 0, 2);
    row3Grid->addWidget(voice_, 0, 3, Qt::AlignVCenter);
    // 与上一版实测顺序（音频分片在左、轮次检测在右）对调：轮次检测移到第 3 列
    row3Grid->addWidget(qwenLabel(QStringLiteral("轮次检测")), 0, 4);
    row3Grid->addWidget(turnMode_, 0, 5, Qt::AlignVCenter);
    row3Grid->addWidget(qwenLabel(QStringLiteral("音频分片")), 0, 6);
    row3Grid->addWidget(uplinkChunkMs_, 0, 7, Qt::AlignVCenter);
    // 完整说明放在字段下方并允许换行：窗口变窄时换行显示，不出现省略号截断
    auto *apiKeyHint = new QLabel(
        QStringLiteral("仅本次会话保存；若已设置 DASHSCOPE_API_KEY 环境变量则优先生效。"), this);
    apiKeyHint->setObjectName(QStringLiteral("hint"));
    apiKeyHint->setWordWrap(true);
    row3Grid->addWidget(apiKeyHint, 1, 1, 1, 7);
    qwenLayout->addWidget(qwenRow3);

    // 系统指令 / 开场白指令（标签顶部对齐，编辑区撑满该行）
    auto *instructionsRow = new QWidget(this);
    auto *instructionsGrid = new QGridLayout(instructionsRow);
    instructionsGrid->setContentsMargins(0, 0, 0, 0);
    instructionsGrid->setHorizontalSpacing(10);
    instructionsGrid->setVerticalSpacing(0);
    instructionsGrid->setColumnStretch(1, 1);
    auto *instructionsLabel = qwenLabel(QStringLiteral("系统指令"));
    instructionsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    instructionsGrid->addWidget(instructionsLabel, 0, 0, Qt::AlignTop);
    instructionsGrid->addWidget(instructions_, 0, 1);
    qwenLayout->addWidget(instructionsRow);

    auto *greetingRow = new QWidget(this);
    auto *greetingGrid = new QGridLayout(greetingRow);
    greetingGrid->setContentsMargins(0, 0, 0, 0);
    greetingGrid->setHorizontalSpacing(10);
    greetingGrid->setVerticalSpacing(0);
    greetingGrid->setColumnStretch(1, 1);
    auto *greetingLabel = qwenLabel(QStringLiteral("开场白指令"));
    greetingLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    greetingGrid->addWidget(greetingLabel, 0, 0, Qt::AlignTop);
    greetingGrid->addWidget(greeting_, 0, 1);
    qwenLayout->addWidget(greetingRow);

    // 同组输入框/下拉/数值框统一高度（35px，与既有样式一致）
    for (auto *field : {workspaceId_, customModel_, endpoint_, apiKey_, voice_})
        field->setFixedHeight(35);
    for (auto *combo : {realtimeRegion_, realtimeModel_, turnMode_})
        combo->setFixedHeight(35);
    for (auto *spin : {uplinkChunkMs_, maxHistoryTurns_})
        spin->setFixedHeight(35);
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
    silenceTimeoutSec_ = new WheelSafeSpinBox(this);
    silenceTimeoutSec_->setRange(1, 60);
    silenceTimeoutSec_->setValue(config.silenceTimeoutSec);
    silenceTimeoutSec_->setSuffix(QStringLiteral(" 秒"));
    maxSilenceRetries_ = new WheelSafeSpinBox(this);
    maxSilenceRetries_->setRange(1, 10);
    maxSilenceRetries_->setValue(config.maxSilenceRetries);
    maxTurns_ = new WheelSafeSpinBox(this);
    maxTurns_->setRange(1, 200);
    maxTurns_->setValue(config.maxTurns);
    allowBargeIn_ = new QCheckBox(QStringLiteral("允许用户打断（抢话）"), this);
    allowBargeIn_->setChecked(config.allowBargeIn);
    allowBargeIn_->setToolTip(QStringLiteral(
        "勾选：全双工。AI 播报期间麦克风保持开启、上行不停，用户一说话就立即停止播报。\n"
        "不勾：半双工。AI 播报期间麦克风上行静音，播报结束立即恢复监听。\n"
        "本地对话与 SIP 通话行为一致。外放时若出现 AI 自激，请改用耳机或检查输入设备是否为回环/虚拟设备。"));
    interactionHint_ = new QLabel(this);
    interactionHint_->setWordWrap(true);
    interactionHint_->setTextFormat(Qt::PlainText);
    connect(allowBargeIn_, &QCheckBox::toggled, this, [this] { updateInteractionHint(); });
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
    strategyGrid->addWidget(interactionHint_, 3, 0, 1, 8);
    updateInteractionHint();
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
    callRecordMaxSize_ = new WheelSafeSpinBox(this);
    callRecordMaxSize_->setRange(1, 1024);
    callRecordMaxSize_->setSuffix(QStringLiteral(" MB"));
    callRecordMaxSize_->setValue(int(config.callRecordMaxSize / (1024 * 1024)));
    callRecordMaxFiles_ = new WheelSafeSpinBox(this);
    callRecordMaxFiles_->setRange(1, 100);
    callRecordMaxFiles_->setValue(config.callRecordMaxFiles);
    customVariables_ = new QLineEdit(config.customVariables.join(QStringLiteral(",")), this);
    customVariables_->setPlaceholderText(QStringLiteral("逗号分隔，如 X-Custom-*,X-Tenant-Id"));
    logPath_ = new QLineEdit(config.logPath, this);
    logPath_->setPlaceholderText(QStringLiteral("运行日志路径，如 log/voicebot.log，留空禁用"));
    logPath_->setToolTip(QStringLiteral(
        "运行日志文件路径，界面与无界面模式的所有运行日志统一写入该文件。\n"
        "相对路径基于程序所在目录解析，目录不存在时自动创建，留空则不写日志文件。"));
    logMaxSize_ = new WheelSafeSpinBox(this);
    logMaxSize_->setRange(1, 1024);
    logMaxSize_->setSuffix(QStringLiteral(" MB"));
    logMaxSize_->setValue(int(config.logMaxSize / (1024 * 1024)));
    logMaxFiles_ = new WheelSafeSpinBox(this);
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

    auto *save = new QPushButton(QStringLiteral("保存非敏感配置"), this);
    save->setObjectName(QStringLiteral("secondary"));
    configurationLayout->addWidget(save, 0, Qt::AlignRight);
    auto *notice = new QLabel(QStringLiteral("密码与 API Key 不写入配置。机器人不使用本机麦克风；仅接收允许的 SIP 来电。"), this);
    notice->setObjectName(QStringLiteral("hint"));
    notice->setWordWrap(true);
    configurationLayout->addWidget(notice);
    configurationLayout->addStretch();
    scroll->setWidget(configuration_);
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
    callTable_->setColumnWidth(0, 85); // 需完整显示 local-1 / local-123
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

    // ---------- 工具（唯一入口）：一屏三分区（工具启用 / 内置工具 / MCP 工具） ----------
    auto *toolsPage = new QWidget(this);
    auto *toolsLayout = new QVBoxLayout(toolsPage);
    toolsLayout->setContentsMargins(4, 8, 4, 8);
    toolsLayout->setSpacing(8);
    auto *toolsHint = new QLabel(QStringLiteral(
        "进行中的通话使用开始时锁定的工具集；MCP 插件热插拔，断线自动重连。"), toolsPage);
    toolsHint->setObjectName(QStringLiteral("hint"));
    toolsHint->setWordWrap(true);
    toolsLayout->addWidget(toolsHint);
    // 三张表统一列宽/行高/只读风格，第一列宽度一致，视觉上对齐
    auto setupToolTable = [](QTableWidget *table) {
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setShowGrid(false);
        table->setAlternatingRowColors(true);
        table->setWordWrap(false);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(30);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(true);
    };
    // 左右分栏：左窄栏上下叠「工具启用」「内置工具」，右宽栏放「MCP 工具」管理区。
    // 默认宽度约 1:1.3，拖动可调；两栏都设了适度的最小宽度，窗口收窄时只压缩空白，
    // 不会把表格压扁到看不清，也不至于在常见分辨率下出现横向滚动条。
    auto *toolsSplitter = new QSplitter(Qt::Horizontal, toolsPage);
    toolsSplitter->setChildrenCollapsible(false);
    toolsSplitter->setHandleWidth(8);
    auto *toolsLeft = new QWidget(toolsSplitter);
    toolsLeft->setMinimumWidth(340);
    auto *leftLayout = new QVBoxLayout(toolsLeft);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    // ---- 分区 1「工具启用」：唯一的开关位置（内置 + 所有 MCP 工具） ----
    auto *enableBox = new QGroupBox(QStringLiteral("工具启用"), toolsLeft);
    auto *enableLayout = new QVBoxLayout(enableBox);
    enableLayout->setContentsMargins(6, 8, 6, 6);
    enableLayout->setSpacing(6);
    auto *toolEnableHint = new QLabel(QStringLiteral(
        "勾选后加入下一次通话的工具集；取消勾选立即从新通话中移除。进行中的通话使用开始时的快照，不受影响。"),
        enableBox);
    toolEnableHint->setObjectName(QStringLiteral("hint"));
    toolEnableHint->setWordWrap(true);
    toolEnableHint->setToolTip(QStringLiteral(
        "开关只影响下一次通话的工具清单（tools/disabledTools），不影响已锁定的进行中通话。"));
    enableLayout->addWidget(toolEnableHint);
    toolEnableTable_ = new QTableWidget(0, 4, enableBox);
    toolEnableTable_->setHorizontalHeaderLabels({
        QStringLiteral("工具"), QStringLiteral("来源"),
        QStringLiteral("说明"), QStringLiteral("启用")});
    toolEnableTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    toolEnableTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    toolEnableTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    toolEnableTable_->setShowGrid(false);
    toolEnableTable_->setAlternatingRowColors(true);
    toolEnableTable_->setWordWrap(false);
    toolEnableTable_->verticalHeader()->hide();
    toolEnableTable_->verticalHeader()->setDefaultSectionSize(30);
    auto *toolEnableHeader = toolEnableTable_->horizontalHeader();
    toolEnableHeader->setStretchLastSection(false);
    toolEnableHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    toolEnableHeader->setSectionResizeMode(1, QHeaderView::Interactive);
    toolEnableHeader->setSectionResizeMode(2, QHeaderView::Stretch);
    toolEnableHeader->setSectionResizeMode(3, QHeaderView::Fixed);
    // 窄栏：工具/来源压到刚好可读，说明列吃掉剩余宽度（超长省略，tooltip 里看全文）
    toolEnableHeader->setMinimumSectionSize(40);
    toolEnableTable_->setColumnWidth(0, 120);
    toolEnableTable_->setColumnWidth(1, 88);
    toolEnableTable_->setColumnWidth(3, 48);
    toolEnableTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    enableLayout->addWidget(toolEnableTable_, 1);
    enableBox->setMinimumHeight(190);

    // ---- 分区 2「内置工具」：只读说明（开关统一在上方「工具启用」） ----
    auto *builtinBox = new QGroupBox(QStringLiteral("内置工具"), toolsLeft);
    auto *builtinLayout = new QVBoxLayout(builtinBox);
    builtinLayout->setContentsMargins(6, 8, 6, 6);
    builtinLayout->setSpacing(6);
    auto *builtinHint = new QLabel(QStringLiteral(
        "只读说明：内置工具随程序实现，无需安装或连接；启用开关统一在上方「工具启用」中。"), builtinBox);
    builtinHint->setObjectName(QStringLiteral("hint"));
    builtinHint->setWordWrap(true);
    builtinLayout->addWidget(builtinHint);
    builtinTools_ = new QTableWidget(0, 2, builtinBox);
    builtinTools_->setHorizontalHeaderLabels({
        QStringLiteral("名称"), QStringLiteral("用途")});
    builtinTools_->setColumnWidth(0, 120);
    setupToolTable(builtinTools_);
    builtinLayout->addWidget(builtinTools_, 1);
    builtinBox->setMinimumHeight(130);
    builtinBox->setMaximumHeight(170);

    // ---- 分区 3「MCP 工具」：外部工具管理（新增 / 上传 / 删除 + 连接状态） ----
    auto *mcpBox = new QGroupBox(QStringLiteral("MCP 工具"), toolsSplitter);
    mcpBox->setMinimumWidth(540);
    auto *mcpLayout = new QVBoxLayout(mcpBox);
    mcpLayout->setContentsMargins(6, 8, 6, 6);
    mcpLayout->setSpacing(6);
    auto *mcpHint = new QLabel(QStringLiteral(
        "新增远端 HTTP 服务或上传 stdio 插件后立即写入配置并热插拔；删除只移除配置，不会删除插件目录中的文件。"),
        mcpBox);
    mcpHint->setObjectName(QStringLiteral("hint"));
    mcpHint->setWordWrap(true);
    mcpLayout->addWidget(mcpHint);
    mcpBox->setMinimumHeight(230);

    mcpConfigPanel_ = new QWidget(mcpBox);
    auto *mcpPanelLayout = new QVBoxLayout(mcpConfigPanel_);
    mcpPanelLayout->setContentsMargins(0, 0, 0, 0);
    mcpPanelLayout->setSpacing(6);

    auto *mcpAddRow = new QHBoxLayout;
    mcpAddRow->setContentsMargins(0, 0, 0, 0);
    mcpAddRow->setSpacing(8);
    auto *addRemoteButton = new QPushButton(
        QStringLiteral("+ 新增远端服务（Streamable HTTP）"), mcpConfigPanel_);
    addRemoteButton->setObjectName(QStringLiteral("secondary"));
    addRemoteButton->setToolTip(QStringLiteral(
        "添加一个远端 MCP 服务：填写展示名、端点 URL 与可选请求头。\n"
        "确认后按 tools/plugin/<名称> 分组写入配置并立即热插拔（优先 Streamable HTTP，失败回退旧版 HTTP+SSE）。"));
    connect(addRemoteButton, &QPushButton::clicked, this, &MainWindow::addRemoteMcpService);
    auto *uploadButton = new QPushButton(QStringLiteral("上传 stdio 插件"), mcpConfigPanel_);
    uploadButton->setObjectName(QStringLiteral("secondary"));
    uploadButton->setToolTip(QStringLiteral(
        "选择 .py / .js / .mjs 插件文件并复制到插件目录（<prefix>/plugins，可用 VOICEBOT_PLUGINS_DIR 覆盖）。\n"
        "重名时会询问覆盖或自动改名，不会静默覆盖；成功后按 stdio 方式注册并加入插件列表。"));
    connect(uploadButton, &QPushButton::clicked, this, &MainWindow::uploadStdioPlugin);
    mcpRemoveButton_ = new QPushButton(QStringLiteral("删除所选来源"), mcpConfigPanel_);
    mcpRemoveButton_->setObjectName(QStringLiteral("danger"));
    mcpRemoveButton_->setToolTip(QStringLiteral(
        "删除下方表格中所选工具对应的 MCP 服务或本地插件配置（只影响下一次通话的工具集）。"));
    mcpRemoveButton_->setEnabled(false);
    connect(mcpRemoveButton_, &QPushButton::clicked, this, &MainWindow::removeSelectedMcpService);
    mcpAddRow->addWidget(addRemoteButton);
    mcpAddRow->addWidget(uploadButton);
    mcpAddRow->addWidget(mcpRemoveButton_);
    mcpAddRow->addStretch(1);
    mcpPanelLayout->addLayout(mcpAddRow);

    auto *mcpActionRow = new QHBoxLayout;
    mcpActionRow->setContentsMargins(0, 0, 0, 0);
    mcpActionRow->setSpacing(8);
    mcpConnectionStatus_ = new QLabel(mcpConfigPanel_);
    mcpConnectionStatus_->setObjectName(QStringLiteral("hint"));
    mcpConnectionStatus_->setWordWrap(true);
    mcpActionRow->addWidget(mcpConnectionStatus_, 1);
    auto *testButton = new QPushButton(QStringLiteral("测试连接 / 拉取工具"), mcpConfigPanel_);
    testButton->setObjectName(QStringLiteral("secondary"));
    testButton->setToolTip(QStringLiteral(
        "按当前配置立即启动或更新连接，并拉取 tools/list 工具列表（不写入配置文件）。"));
    connect(testButton, &QPushButton::clicked, this, &MainWindow::testMcpConnections);
    mcpActionRow->addWidget(testButton);
    auto *toolSave = new QPushButton(QStringLiteral("保存非敏感配置"), mcpConfigPanel_);
    toolSave->setObjectName(QStringLiteral("secondary"));
    toolSave->setToolTip(QStringLiteral(
        "与配置页的「保存非敏感配置」按钮作用相同：保存所有页面的非敏感配置"
        "（密码与 API Key 不落盘），保存后立即热插拔。\n"
        "MCP 来源的新增/删除与工具开关已即时保存，此按钮用于统一保存其余配置项。"));
    connect(toolSave, &QPushButton::clicked, this, &MainWindow::saveSettings);
    mcpActionRow->addWidget(toolSave);
    mcpPanelLayout->addLayout(mcpActionRow);
    mcpLayout->addWidget(mcpConfigPanel_);

    mcpTools_ = new QTableWidget(0, 6, mcpBox);
    mcpTools_->setHorizontalHeaderLabels({
        QStringLiteral("工具名"), QStringLiteral("来源插件"), QStringLiteral("传输方式"),
        QStringLiteral("状态"), QStringLiteral("依赖包"), QStringLiteral("说明")});
    setupToolTable(mcpTools_);
    auto *mcpHeader = mcpTools_->horizontalHeader();
    mcpHeader->setStretchLastSection(false);
    for (int i = 0; i < 5; ++i)
        mcpHeader->setSectionResizeMode(i, QHeaderView::Interactive);
    // 说明列吃剩余宽度；其余列按内容给最小可读宽度，超长省略靠 tooltip 看全文
    mcpHeader->setSectionResizeMode(5, QHeaderView::Stretch);
    mcpHeader->setMinimumSectionSize(40);
    mcpTools_->setColumnWidth(0, 130);
    mcpTools_->setColumnWidth(1, 100);
    mcpTools_->setColumnWidth(2, 56);
    mcpTools_->setColumnWidth(3, 110);
    mcpTools_->setColumnWidth(4, 88);
    connect(mcpTools_, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!mcpRemoveButton_) return;
        const int row = mcpTools_->currentRow();
        const bool deletable = row >= 0 && mcpTools_->item(row, 1)
            && !mcpTools_->item(row, 1)->data(Qt::UserRole).toString().isEmpty();
        mcpRemoveButton_->setEnabled(deletable);
    });
    mcpLayout->addWidget(mcpTools_, 1);
    mcpEmptyHint_ = new QLabel(QStringLiteral(
        "未配置 MCP 工具：点击「新增远端服务」或「上传 stdio 插件」，连接成功后工具会出现在这里与「工具启用」中。"),
        mcpBox);
    mcpEmptyHint_->setObjectName(QStringLiteral("hint"));
    mcpEmptyHint_->setWordWrap(true);
    mcpLayout->addWidget(mcpEmptyHint_);

    leftLayout->addWidget(enableBox, 1);
    leftLayout->addWidget(builtinBox, 0);
    toolsSplitter->addWidget(toolsLeft);
    toolsSplitter->addWidget(mcpBox);
    // 初始约 1:1.3，拉伸时按同一权重分配（QSplitter 只接受整数因子）
    toolsSplitter->setStretchFactor(0, 10);
    toolsSplitter->setStretchFactor(1, 13);
    toolsSplitter->setSizes({480, 624});
    toolsLayout->addWidget(toolsSplitter, 1);
    // 工具页在日志页之后才构建，用 insertTab 固定到「配置 | 通话与转写 | 工具 | 日志」的
    // 第 3 位；默认仍选中索引 0 的「配置」，不影响任何页面父子关系与信号连接
    tabs->insertTab(2, toolsPage, QStringLiteral("工具"));

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
        const int rowKey = rowKeyForEngine(id);
        auto record = calls_.find(rowKey);
        if (record == calls_.end()) return;
        record->hangupBy = hangupBy;
        const int row = record->idItem->row();
        if (record->hangupTime.isEmpty())
            record->hangupTime = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        callTable_->item(row, 9)->setText(record->hangupTime);
        callTable_->item(row, 10)->setText(hangupByText(hangupBy));
        callTable_->item(row, 11)->setText(sipCode > 0 ? QString::number(sipCode) : QString());
        callTable_->item(row, 12)->setText(reason.isEmpty() ? QString() : reason.left(80));
        endCall(rowKey);
    });
    connect(&engine_, &BotEngine::callTurnsUpdated, this, [this](int id, int turns) {
        const int rowKey = rowKeyForEngine(id);
        auto record = calls_.find(rowKey);
        if (record == calls_.end()) return;
        record->turns = turns;
        callTable_->item(record->idItem->row(), 5)->setText(QString::number(turns));
    });
    // 模型 token 用量实时刷新（悬停显示输入/输出与音频/文本细分）
    connect(&engine_, &BotEngine::callUsageUpdated, this, [this](int id, const ModelUsage &usage) {
        const int rowKey = rowKeyForEngine(id);
        auto record = calls_.find(rowKey);
        if (record == calls_.end()) return;
        const auto [summary, tip] = usageTexts(usage);
        record->usageSummary = summary;
        record->usageTip = tip;
        auto *item = callTable_->item(record->idItem->row(), 13);
        item->setText(summary);
        if (!tip.isEmpty()) item->setToolTip(tip);
    });
    connect(&engine_, &BotEngine::transcript, this,
            [this](int id, const QString &speaker, const QString &text) {
        const int rowKey = rowKeyForEngine(id);
        if (rowKey >= 0) addTranscript(rowKey, speaker, text);
    });
    connect(&engine_, &BotEngine::logMessage, this, &MainWindow::appendLog);
    // ---------- 工具注册表：MCP 插件管理与工具页数据源 ----------
    tools_ = new ToolRegistry(this);
    engine_.setToolRegistry(tools_);
    // 工具级启停名单先于插件启动应用，避免首帧工具表把禁用工具当成启用
    mcpDisabledTools_ = config.disabledTools;
    tools_->setDisabledTools(config.disabledTools);
    tools_->applyMcpPlugins(config.mcpPlugins); // 差异比对，增量启动插件（stdio/http 混用）
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
        if (auto rec = calls_.find(localRowKey_); rec != calls_.end() && !rec->ended) {
            rec->turns = count;
            callTable_->item(rec->idItem->row(), 5)->setText(QString::number(count));
        }
    });
    // 累计本地对话的模型 token 用量，结束时写进本地通话记录
    connect(local_, &LocalAudioSession::usageUpdated,
            this, &MainWindow::handleLocalUsageUpdated);
    connect(local_, &LocalAudioSession::toolCallRequested,
            this, &MainWindow::handleLocalToolCall);
    connect(local_, &LocalAudioSession::deviceWarning, this, [this](const QString &text) {
        status_->setText(QStringLiteral("本地对话：%1").arg(text));
        QMessageBox::warning(this, QStringLiteral("输入设备警告"), text);
    });
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
    config.realtimeManualEndpoint = manualEndpoint_->isChecked();
    config.realtimeWorkspaceId = workspaceId_->text().trimmed();
    config.realtimeRegion = realtimeRegion_->currentData().toString();
    config.realtimeModel = realtimeModel_->currentData().toString() == QStringLiteral("__custom__")
        ? customModel_->text().trimmed() : realtimeModel_->currentText();
    config.endpoint = config.realtimeManualEndpoint
        ? QUrl(endpoint_->text().trimmed(), QUrl::StrictMode)
        : BotConfig::buildRealtimeEndpoint(config.realtimeWorkspaceId, config.realtimeRegion,
                                           config.realtimeModel);
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
    // 每个插件的传输配置（stdio/http、URL、请求头），本地插件与远端服务混排
    config.mcpPlugins.clear();
    config.enabledPlugins.clear();
    config.mcpCommands.clear();
    config.mcpPlugins = mcpPluginSpecs_;
    const QStringList available = ToolRegistry::scanPluginFiles();
    for (const auto &spec : config.mcpPlugins) {
        if (spec.isHttp()) continue;
        if (!spec.command.isEmpty()) config.mcpCommands.append(spec.command);
        if (available.contains(spec.name)) config.enabledPlugins.append(spec.name);
    }
    // 工具级启停：来自「工具启用」表格，与插件配置分开存储
    config.disabledTools = mcpDisabledTools_;
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
    // 插件状态：MCP 工具表需要展示来源名、传输方式与连接状态
    const auto states = tools_->pluginStates();
    auto stateFor = [&states](const QString &name) -> const ToolRegistry::PluginState * {
        for (const auto &state : states)
            if (state.name == name) return &state;
        return nullptr;
    };
    // 传输方式以运行时归一化结果为准（PluginState::transport），其次查配置；
    // 查不到时显示「—」，不再默认回落到 stdio，避免把 HTTP 来源误标成本地插件
    auto transportOf = [&states, this](const QString &name) {
        for (const auto &state : states)
            if (state.name.compare(name, Qt::CaseInsensitive) == 0)
                return state.transport;
        for (const auto &spec : mcpPluginSpecs_)
            if (spec.name.compare(name, Qt::CaseInsensitive) == 0)
                return spec.isHttp() ? QStringLiteral("http") : QStringLiteral("stdio");
        return QStringLiteral("—");
    };
    auto transportText = [](const QString &transport) {
        if (transport == QLatin1String("http")) return QStringLiteral("HTTP");
        if (transport == QLatin1String("stdio")) return QStringLiteral("stdio");
        return QStringLiteral("—");
    };
    auto specFor = [this](const QString &name) -> const McpPluginSpec * {
        for (const auto &spec : mcpPluginSpecs_)
            if (spec.name.compare(name, Qt::CaseInsensitive) == 0) return &spec;
        return nullptr;
    };
    QStringList coveredSources;
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
            const auto *state = stateFor(tool.server);
            const auto *spec = specFor(tool.server);
            // 来源插件列显示配置里的展示名（本地插件即文件名，远端即用户填的名称）
            const QString label = spec ? spec->name : (state ? state->label : tool.server);
            const QString transport = transportOf(tool.server);
            const bool online = state ? state->online : tool.online;
            // 依赖包只对 stdio 本地插件扫描源码；HTTP 服务无此概念
            const QStringList deps = transport == QLatin1String("stdio")
                ? ToolRegistry::pluginDependencies(tool.server) : QStringList{};
            if (!coveredSources.contains(tool.server))
                coveredSources.append(tool.server);
            const int row = mcpTools_->rowCount();
            mcpTools_->insertRow(row);
            auto *name = centeredItem(tool.name);
            auto *server = centeredItem(label);
            server->setData(Qt::UserRole, tool.server);
            if (spec && spec->isHttp() && !spec->url.isEmpty())
                server->setToolTip(spec->url);
            auto *transportItem = centeredItem(transportText(transport));
            auto *status = centeredItem(online
                ? QStringLiteral("在线")
                : (state && !state->lastError.isEmpty()
                    ? QStringLiteral("未连接")
                    : QStringLiteral("离线（重连中）")));
            if (!online && state && !state->lastError.isEmpty())
                status->setToolTip(QStringLiteral("连接失败：%1；后台自动重试中。")
                                       .arg(state->lastError));
            auto *dependencies = centeredItem(deps.isEmpty()
                ? QStringLiteral("—") : deps.join(QStringLiteral(", ")));
            if (!deps.isEmpty())
                dependencies->setToolTip(
                    QStringLiteral("安装命令：pip install %1").arg(deps.join(QLatin1Char(' '))));
            else if (transport == QLatin1String("http"))
                dependencies->setToolTip(QStringLiteral("HTTP 服务由远端提供，无需本地依赖包"));
            auto *description = centeredItem(toolDescriptionText(tool.description));
            description->setToolTip(toolDescriptionText(tool.description));
            if (!online) {
                const QBrush offlineColor(QColor(QStringLiteral("#9ca3af")));
                for (auto *item : {name, server, transportItem, status, dependencies, description})
                    item->setForeground(offlineColor); // 插件断线灰显
            }
            mcpTools_->setItem(row, 0, name);
            mcpTools_->setItem(row, 1, server);
            mcpTools_->setItem(row, 2, transportItem);
            mcpTools_->setItem(row, 3, status);
            mcpTools_->setItem(row, 4, dependencies);
            mcpTools_->setItem(row, 5, description);
        }
    }
    // 已配置但未返回可用工具的服务/插件：插入占位行，仍可查看状态并删除来源
    // 重名被忽略的工具必须可见：状态列给出冲突来源，说明列给出处理办法，
    // 选中该行后用「删除所选来源」即可让同一份工具在另一个来源生效。
    for (const auto &state : states) {
        const QString transport = transportOf(state.name);
        const auto *spec = specFor(state.name);
        const QString label = spec ? spec->name : state.label;
        const QStringList deps = transport == QLatin1String("stdio")
            ? ToolRegistry::pluginDependencies(state.name) : QStringList{};
        const QString depsText = deps.isEmpty() ? QStringLiteral("—")
                                                : deps.join(QStringLiteral(", "));
        for (const auto &ignoredTool : state.ignoredTools) {
            const int row = mcpTools_->rowCount();
            mcpTools_->insertRow(row);
            const QString reason = QStringLiteral(
                "与「%1」提供的同名工具 %2 冲突，未注册到本次工具集；"
                "删除「%1」来源后会自动启用此工具。")
                .arg(ignoredTool.owner, ignoredTool.name);
            auto *name = centeredItem(ignoredTool.name);
            auto *server = centeredItem(label);
            server->setData(Qt::UserRole, state.name);
            if (spec && spec->isHttp() && !spec->url.isEmpty())
                server->setToolTip(spec->url);
            auto *transportItem = centeredItem(transportText(transport));
            auto *status = centeredItem(
                QStringLiteral("已忽略（与 %1 重名）").arg(ignoredTool.owner));
            auto *dependencies = centeredItem(depsText);
            if (!deps.isEmpty())
                dependencies->setToolTip(
                    QStringLiteral("安装命令：pip install %1").arg(deps.join(QLatin1Char(' '))));
            auto *description = centeredItem(reason);
            description->setToolTip(ignoredTool.description.isEmpty()
                ? reason : reason + QStringLiteral("\n\n原工具说明：") + ignoredTool.description);
            const QBrush conflictColor(QColor(QStringLiteral("#b45309")));
            name->setForeground(conflictColor);
            status->setForeground(conflictColor);
            status->setToolTip(reason);
            mcpTools_->setItem(row, 0, name);
            mcpTools_->setItem(row, 1, server);
            mcpTools_->setItem(row, 2, transportItem);
            mcpTools_->setItem(row, 3, status);
            mcpTools_->setItem(row, 4, dependencies);
            mcpTools_->setItem(row, 5, description);
        }
        if (coveredSources.contains(state.name)) continue;
        if (!state.ignoredTools.isEmpty()) continue; // 工具全部因重名被忽略：已有冲突行，不再追加占位
        // 没有任何工具时按真实原因区分：未连接（含失败原因） vs 服务端确实无工具
        QString placeholder;
        QString statusText;
        QString detail;
        if (!state.online) {
            placeholder = QStringLiteral("（未连接）");
            statusText = QStringLiteral("未连接（自动重试）");
            detail = state.lastError.isEmpty()
                ? QStringLiteral("尚未连接；连接成功后自动拉取工具列表。")
                : QStringLiteral("连接失败：%1；后台自动重试中。").arg(state.lastError);
        } else {
            placeholder = QStringLiteral("（服务端无工具）");
            statusText = QStringLiteral("在线（0 个工具）");
            detail = QStringLiteral("已连接，但 tools/list 返回 0 个工具（服务端确实未提供工具）。");
        }
        const int row = mcpTools_->rowCount();
        mcpTools_->insertRow(row);
        auto *name = centeredItem(placeholder);
        auto *server = centeredItem(label);
        server->setData(Qt::UserRole, state.name);
        if (spec && spec->isHttp() && !spec->url.isEmpty())
            server->setToolTip(spec->url);
        auto *transportItem = centeredItem(transportText(transport));
        auto *status = centeredItem(statusText);
        status->setToolTip(detail);
        auto *dependencies = centeredItem(depsText);
        if (!deps.isEmpty())
            dependencies->setToolTip(
                QStringLiteral("安装命令：pip install %1").arg(deps.join(QLatin1Char(' '))));
        auto *description = centeredItem(detail);
        description->setToolTip(detail);
        if (!state.online) {
            const QBrush offlineColor(QColor(QStringLiteral("#9ca3af")));
            for (auto *item : {name, server, transportItem, status, dependencies, description})
                item->setForeground(offlineColor);
        }
        mcpTools_->setItem(row, 0, name);
        mcpTools_->setItem(row, 1, server);
        mcpTools_->setItem(row, 2, transportItem);
        mcpTools_->setItem(row, 3, status);
        mcpTools_->setItem(row, 4, dependencies);
        mcpTools_->setItem(row, 5, description);
    }
    mcpEmptyHint_->setVisible(mcpTools_->rowCount() == 0);
    // 缓存在线插件最近一次 tools/list 结果：离线时用于「工具启用」占位
    for (const auto &state : tools_->pluginStates())
        if (state.online && !state.tools.isEmpty())
            saveCachedTools(settings_, state.name, state.tools);
    refreshToolEnableTable();
    updateMcpConnectionStatus();
}

// 「工具启用」表格：内置 hang_up + 已发现 MCP 工具，每行只放启用开关。
// 离线插件优先用上次 tools/list 缓存，无缓存时插入可读占位行，不留空白。
void MainWindow::refreshToolEnableTable()
{
    if (!toolEnableTable_ || !tools_) return;
    QSignalBlocker blocker(toolEnableTable_);
    toolEnableTable_->setRowCount(0);

    auto addRow = [this](const QString &name, const QString &source,
                         const QString &description, bool enabled, bool toggleable) {
        const int row = toolEnableTable_->rowCount();
        toolEnableTable_->insertRow(row);
        const QString tip = QStringLiteral("%1\n来源：%2").arg(description, source);
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setTextAlignment(Qt::AlignCenter);
        nameItem->setToolTip(tip);
        auto *sourceItem = new QTableWidgetItem(source);
        sourceItem->setTextAlignment(Qt::AlignCenter);
        sourceItem->setToolTip(QStringLiteral("工具来源：%1").arg(source));
        auto *descItem = new QTableWidgetItem(description);
        descItem->setTextAlignment(Qt::AlignCenter);
        descItem->setToolTip(tip);
        if (!toggleable) {
            const QBrush dim(QColor(QStringLiteral("#9ca3af")));
            nameItem->setForeground(dim);
            sourceItem->setForeground(dim);
            descItem->setForeground(dim);
        }
        toolEnableTable_->setItem(row, 0, nameItem);
        toolEnableTable_->setItem(row, 1, sourceItem);
        toolEnableTable_->setItem(row, 2, descItem);
        // 开关放在居中的容器里，避免 QTableWidget 直接拉伸复选框
        auto *cell = new QWidget(toolEnableTable_);
        auto *cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setAlignment(Qt::AlignCenter);
        auto *check = new QCheckBox(cell);
        check->setChecked(toggleable && enabled);
        check->setEnabled(toggleable);
        check->setToolTip(toggleable
            ? QStringLiteral("%1\n勾选启用；取消勾选后从下一次通话的工具集中移除。").arg(name)
            : (description.isEmpty()
                ? QStringLiteral("插件尚未提供可用的工具列表，暂时无法启用。")
                : description));
        connect(check, &QCheckBox::toggled, this, [this, name](bool checked) {
            setToolEnabled(name, checked);
        });
        cellLayout->addWidget(check);
        toolEnableTable_->setCellWidget(row, 3, cell);
    };

    // 内置工具：固定中文说明
    for (const auto &tool : tools_->tools()) {
        if (!tool.builtIn) continue;
        addRow(tool.name, QStringLiteral("内置"),
               toolDescriptionText(tool.description),
               !mcpDisabledTools_.contains(tool.name), true);
    }
    // MCP 工具：按插件归属分组；离线时使用缓存或占位行
    // 来源列与「MCP 工具」区一致：优先显示配置中的展示名
    auto sourceLabel = [this](const ToolRegistry::PluginState &state) {
        for (const auto &spec : mcpPluginSpecs_)
            if (spec.name.compare(state.name, Qt::CaseInsensitive) == 0)
                return spec.name;
        return state.label;
    };
    for (const auto &state : tools_->pluginStates()) {
        const QString source = sourceLabel(state);
        if (state.online) {
            for (const auto &tool : state.tools)
                addRow(tool.name, source, toolDescriptionText(tool.description),
                       !mcpDisabledTools_.contains(tool.name), true);
            // 重名被忽略的工具也出现在启用表（只读），让用户能定位冲突来源并处理
            for (const auto &ignoredTool : state.ignoredTools)
                addRow(ignoredTool.name, source,
                       QStringLiteral("与「%1」提供的同名工具冲突，已忽略；删除「%1」来源后会自动启用。")
                           .arg(ignoredTool.owner),
                       false, false);
            if (state.tools.isEmpty() && state.ignoredTools.isEmpty())
                addRow(QStringLiteral("（服务端无工具）"), source,
                       QStringLiteral("已连接，但 tools/list 返回 0 个工具（服务端确实未提供工具）。"),
                       false, false);
            continue;
        }
        const auto cached = loadCachedTools(settings_, state.name);
        if (cached.isEmpty()) {
            addRow(QStringLiteral("（未连接）"), source,
                   state.lastError.isEmpty()
                       ? QStringLiteral("尚未连接；连接成功后自动拉取工具列表。")
                       : QStringLiteral("连接失败：%1；后台自动重试中。").arg(state.lastError),
                   false, false);
            continue;
        }
        for (const auto &entry : cached)
            addRow(entry.first, source, toolDescriptionText(entry.second),
                   !mcpDisabledTools_.contains(entry.first), true);
    }
}

void MainWindow::setToolEnabled(const QString &name, bool enabled)
{
    const QString toolName = name.trimmed();
    if (toolName.isEmpty()) return;
    if (enabled) {
        mcpDisabledTools_.removeAll(toolName);
    } else if (!mcpDisabledTools_.contains(toolName)) {
        mcpDisabledTools_.append(toolName);
    }
    // 只更新下一次通话的工具集快照；进行中的通话不受影响
    if (tools_) tools_->setDisabledTools(mcpDisabledTools_);
    // 工具启用是独立小配置：开关变化即时落盘，避免用户切换标签后忘记保存
    if (mcpDisabledTools_.isEmpty())
        settings_.remove(QStringLiteral("tools/disabledTools"));
    else
        settings_.setValue(QStringLiteral("tools/disabledTools"), mcpDisabledTools_);
    if (status_)
        status_->setText(QStringLiteral("工具 %1 已%2；已写入配置，下一次通话生效（进行中的通话不受影响）。")
                             .arg(toolName, enabled ? QStringLiteral("启用")
                                                    : QStringLiteral("禁用")));
}

void MainWindow::updateMcpConnectionStatus()
{
    if (!mcpConnectionStatus_ || !tools_) return;
    const auto states = tools_->pluginStates();
    if (states.isEmpty()) {
        mcpConnectionStatus_->setText(
            QStringLiteral("未配置 MCP 服务：点击「新增远端服务」或「上传 stdio 插件」。"));
        mcpConnectionStatus_->setToolTip(QString());
        return;
    }
    int online = 0;
    int toolCount = 0;
    int conflictCount = 0;
    QStringList detail;
    for (const auto &state : states) {
        if (state.online) {
            ++online;
            toolCount += int(state.tools.size());
            conflictCount += int(state.ignoredTools.size());
        }
        QString count = state.online
            ? QStringLiteral("%1 个工具").arg(state.tools.size())
            : (state.lastError.isEmpty()
                ? QStringLiteral("未连接（自动重试中）")
                : QStringLiteral("未连接：%1").arg(state.lastError));
        if (state.online && !state.ignoredTools.isEmpty())
            count += QStringLiteral("（%1 个重名忽略）").arg(state.ignoredTools.size());
        detail.append(QStringLiteral("%1 %2：%3")
                          .arg(state.online ? QStringLiteral("●") : QStringLiteral("○"),
                               state.label, count));
    }
    QString text = QStringLiteral("MCP 连接 %1/%2 · 已发现工具 %3 个")
                       .arg(online).arg(states.size()).arg(toolCount);
    if (conflictCount > 0)
        text += QStringLiteral(" · 重名忽略 %1 个").arg(conflictCount);
    mcpConnectionStatus_->setText(text);
    mcpConnectionStatus_->setToolTip(detail.join(QLatin1Char('\n')));
}

// 按当前配置立即连接并拉取工具列表（不写配置）；结果经 toolsChanged 回到界面
void MainWindow::testMcpConnections()
{
    if (!tools_) return;
    tools_->applyMcpPlugins(mcpPluginSpecs_);
    updateMcpConnectionStatus();
    if (status_)
        status_->setText(QStringLiteral("已按当前配置测试连接（未写入配置文件）。"));
}

// 工具配置的唯一写入口：立即写入 tools/plugins 与 tools/plugin/<名称>，并热插拔
bool MainWindow::persistMcpPlugins(const QString &message)
{
    saveMcpPlugins(settings_, mcpPluginSpecs_);
    settings_.sync();
    if (settings_.status() != QSettings::NoError)
        return false;
    if (tools_) {
        tools_->applyMcpPlugins(mcpPluginSpecs_);
        tools_->setDisabledTools(mcpDisabledTools_); // 新增插件时保持已有工具级开关
    }
    refreshTools();
    if (status_ && !message.isEmpty())
        status_->setText(message);
    return true;
}

// 新增 Streamable HTTP 远端服务：校验展示名/URL 后立即落盘并热插拔
void MainWindow::addRemoteMcpService()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新增远端 MCP 服务（Streamable HTTP）"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // 输入框统一撑满可用宽度；窄窗口下也不把标签折到输入框上方
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QStringLiteral("例如 weather_remote"));
    nameEdit->setToolTip(QStringLiteral(
        "写入 tools/plugin/<名称> 分组键；不能与已有服务或插件目录中的文件名重复。"));
    auto *urlEdit = new QLineEdit(&dialog);
    urlEdit->setPlaceholderText(QStringLiteral("https://example.com/mcp"));
    urlEdit->setToolTip(QStringLiteral(
        "MCP 端点：优先 Streamable HTTP（单端点 POST/SSE），失败时自动回退旧版 HTTP+SSE。"));
    auto *headersEdit = new QLineEdit(&dialog);
    headersEdit->setPlaceholderText(
        QStringLiteral("可选，多个用 ; 分隔，如 Authorization: Bearer ${MCP_TOKEN}"));
    headersEdit->setToolTip(QStringLiteral("请求头支持环境变量占位，如 ${MCP_TOKEN}。"));
    form->addRow(QStringLiteral("展示名"), nameEdit);
    form->addRow(QStringLiteral("URL"), urlEdit);
    form->addRow(QStringLiteral("请求头"), headersEdit);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    // 宽度只按「请求头提示文案 + 标签列 + 内边距」量算，不再硬编码：
    // 先以默认 sizeHint 完成一次布局，量出输入框之外的固定宽度（标签列/间距/窗口边距），
    // 再加上提示文本的实际像素宽度与 QLineEdit 文本区内边距，只留 12px 余量
    layout->setSizeConstraint(QLayout::SetDefaultConstraint);
    dialog.adjustSize();
    layout->activate();
    const int frame = headersEdit->style()->pixelMetric(
        QStyle::PM_DefaultFrameWidth, nullptr, headersEdit);
    const int inputInset = 2 * (frame + 2); // QLineEdit 文本区左右内边距（样式边框 + 默认 2px）
    const QFontMetrics metrics(headersEdit->font());
    const QString hint = headersEdit->placeholderText();
    const int chrome = dialog.width() - headersEdit->width();
    int targetWidth = chrome + inputInset + metrics.horizontalAdvance(hint) + 12;
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    dialog.setMinimumWidth(targetWidth);
    dialog.resize(targetWidth, dialog.sizeHint().height());
    layout->activate();
    // 复核：提示文本在实际文本区内必须能完整显示；字体或样式有差异时按差值补齐
    const int available = headersEdit->width() - inputInset;
    if (metrics.elidedText(hint, Qt::ElideRight, available) != hint) {
        targetWidth += metrics.horizontalAdvance(hint) - available;
        dialog.setMinimumWidth(targetWidth);
        dialog.resize(targetWidth, dialog.sizeHint().height());
        layout->activate();
    }
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // 请求头按中英文逗号/分号分隔成多项，每项必须是「名称: 值」
    auto headerParts = [&headersEdit]() {
        QStringList parts;
        for (const auto &part : headersEdit->text().trimmed().split(
                 QRegularExpression(QStringLiteral("[,;，；]")), Qt::SkipEmptyParts)) {
            if (!part.trimmed().isEmpty())
                parts.append(part.trimmed());
        }
        return parts;
    };

    while (true) {
        if (dialog.exec() != QDialog::Accepted)
            return;
        const QString name = nameEdit->text().trimmed();
        const QString urlText = urlEdit->text().trimmed();
        QString warning;
        if (name.isEmpty()) {
            warning = QStringLiteral("展示名不能为空。");
        } else if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
            warning = QStringLiteral("展示名不能包含 / 或 \\。");
        } else {
            bool duplicated = false;
            for (const auto &spec : mcpPluginSpecs_) {
                if (spec.name.compare(name, Qt::CaseInsensitive) == 0) {
                    duplicated = true;
                    break;
                }
            }
            if (duplicated)
                warning = QStringLiteral("已存在同名服务或插件，请换一个展示名。");
            else if (ToolRegistry::scanPluginFiles().contains(name))
                warning = QStringLiteral("该名称与插件目录中的本地插件文件重名，请换一个展示名。");
        }
        if (warning.isEmpty()) {
            QUrl url(urlText);
            const QString scheme = url.scheme().toLower();
            if (!url.isValid() || url.host().isEmpty()
                || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
                warning = QStringLiteral("URL 必须是完整的 http:// 或 https:// 端点。");
            }
        }
        if (warning.isEmpty()) {
            // 只填 "Bearer xxx" 会被传输层当作无冒号行丢弃并导致 401，必须先拦住
            for (const auto &part : headerParts()) {
                const int colon = part.indexOf(QLatin1Char(':'));
                if (colon <= 0 || part.left(colon).trimmed().isEmpty()
                    || part.mid(colon + 1).trimmed().isEmpty()) {
                    warning = QStringLiteral(
                        "请求头每项必须是「名称: 值」，多项用 ; 分隔，例如 "
                        "Authorization: Bearer <token>。");
                    break;
                }
            }
        }
        if (warning.isEmpty())
            break;
        QMessageBox::warning(this, QStringLiteral("新增远端服务"), warning);
    }

    McpPluginSpec spec;
    spec.name = nameEdit->text().trimmed();
    spec.transport = QStringLiteral("http");
    spec.url = urlEdit->text().trimmed();
    spec.headers = headerParts();
    mcpPluginSpecs_.append(spec);
    if (!persistMcpPlugins(QStringLiteral("已新增 MCP 服务「%1」，正在按 HTTP 方式连接…").arg(spec.name))) {
        mcpPluginSpecs_.removeLast();
        QMessageBox::warning(this, QStringLiteral("新增远端服务"),
                             QStringLiteral("写入配置失败，未添加该服务。"));
    }
}

// 上传 stdio 插件：复制到插件目录后按 stdio 注册；重名必须显式确认，失败不留配置
void MainWindow::uploadStdioPlugin()
{
    const QString sourcePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("上传 stdio MCP 插件"), QString(),
        QStringLiteral("MCP 插件 (*.py *.js *.mjs)"));
    if (sourcePath.isEmpty())
        return;
    const QFileInfo source(sourcePath);
    const QString suffix = source.suffix().toLower();
    if (suffix != QStringLiteral("py") && suffix != QStringLiteral("js")
        && suffix != QStringLiteral("mjs")) {
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("仅支持 .py / .js / .mjs 插件文件。"));
        return;
    }
    constexpr qint64 maxPluginBytes = 2 * 1024 * 1024;
    if (source.size() <= 0 || source.size() > maxPluginBytes) {
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("插件文件为空或超过 2 MB 上限：%1")
                                 .arg(QDir::toNativeSeparators(sourcePath)));
        return;
    }
    const QString pluginsDir = ToolRegistry::pluginsDir();
    if (!QDir().mkpath(pluginsDir) || !QFileInfo(pluginsDir).isWritable()) {
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("插件目录不可写：%1")
                                 .arg(QDir::toNativeSeparators(pluginsDir)));
        return;
    }
    QString fileName = source.fileName();
    QString targetPath = pluginsDir + QLatin1Char('/') + fileName;
    if (QFileInfo::exists(targetPath)) {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("插件重名"));
        box.setIcon(QMessageBox::Question);
        box.setText(QStringLiteral("插件目录已存在同名文件：\n%1").arg(fileName));
        box.setInformativeText(QStringLiteral(
            "可选择覆盖、自动改名（追加 _2 等后缀）或取消。"));
        QPushButton *overwriteButton = box.addButton(QStringLiteral("覆盖"), QMessageBox::AcceptRole);
        QPushButton *renameButton = box.addButton(QStringLiteral("自动改名"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == overwriteButton) {
            // 覆盖沿用同名配置（QSaveFile 原子替换，失败不会破坏原文件）
        } else if (box.clickedButton() == renameButton) {
            bool renamed = false;
            for (int index = 2; index < 100 && !renamed; ++index) {
                const QString candidate = QStringLiteral("%1_%2.%3")
                    .arg(source.completeBaseName()).arg(index).arg(suffix);
                const QString candidatePath = pluginsDir + QLatin1Char('/') + candidate;
                if (!QFileInfo::exists(candidatePath)) {
                    fileName = candidate;
                    targetPath = candidatePath;
                    renamed = true;
                }
            }
            if (!renamed) {
                QMessageBox::warning(this, QStringLiteral("上传插件"),
                                     QStringLiteral("自动改名失败：同名文件过多，请手动清理插件目录。"));
                return;
            }
        } else {
            return;
        }
    }
    // 同名 HTTP 服务不能被上传静默改写：先拦下，避免留下未使用的插件文件
    for (const auto &spec : mcpPluginSpecs_) {
        if (spec.name.compare(fileName, Qt::CaseInsensitive) == 0 && spec.isHttp()) {
            QMessageBox::warning(this, QStringLiteral("上传插件"),
                                 QStringLiteral("已存在同名 HTTP 服务「%1」，请先删除该服务或改名后再上传。")
                                     .arg(fileName));
            return;
        }
    }
    QFile input(sourcePath);
    if (!input.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("读取插件文件失败：%1").arg(input.errorString()));
        return;
    }
    const QByteArray contents = input.readAll();
    if (contents.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("插件文件内容为空，未复制。"));
        return;
    }
    QSaveFile output(targetPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(contents) != contents.size()
        || !output.commit()) {
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("复制插件文件失败：%1").arg(output.errorString()));
        return;
    }
    bool appended = false;
    bool configured = false;
    for (auto &spec : mcpPluginSpecs_) {
        if (spec.name.compare(fileName, Qt::CaseInsensitive) == 0) {
            spec.transport = QStringLiteral("stdio");
            spec.command.clear();
            spec.url.clear();
            spec.headers.clear();
            configured = true;
            break;
        }
    }
    if (!configured) {
        McpPluginSpec spec;
        spec.name = fileName; // transport 默认 stdio
        mcpPluginSpecs_.append(spec);
        appended = true;
    }
    if (!persistMcpPlugins(QStringLiteral("已上传 stdio 插件「%1」，正在连接并拉取工具…").arg(fileName))) {
        if (appended)
            mcpPluginSpecs_.removeLast();
        QMessageBox::warning(this, QStringLiteral("上传插件"),
                             QStringLiteral("插件文件已复制到 %1，但写入配置失败，未注册该插件。")
                                 .arg(QDir::toNativeSeparators(targetPath)));
    }
}

// 删除所选工具来源：只移除配置，不删除插件目录中的文件
void MainWindow::removeSelectedMcpService()
{
    if (!mcpTools_) return;
    const int row = mcpTools_->currentRow();
    const QTableWidgetItem *sourceItem = row >= 0 ? mcpTools_->item(row, 1) : nullptr;
    const QString name = sourceItem ? sourceItem->data(Qt::UserRole).toString() : QString();
    if (name.isEmpty()) {
        if (status_)
            status_->setText(QStringLiteral("请先在 MCP 工具表中选中一行，再删除其来源。"));
        return;
    }
    const QString label = sourceItem->text();
    if (QMessageBox::question(this, QStringLiteral("删除 MCP 来源"),
                              QStringLiteral("确定删除 MCP 服务/插件「%1」吗？\n"
                                             "其提供的工具将从下一次通话的工具集中移除；"
                                             "插件目录中的文件不会被删除。")
                                  .arg(label),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }
    for (int index = 0; index < mcpPluginSpecs_.size(); ++index) {
        if (mcpPluginSpecs_.at(index).name.compare(name, Qt::CaseInsensitive) == 0) {
            const McpPluginSpec spec = mcpPluginSpecs_.takeAt(index);
            if (!persistMcpPlugins(QStringLiteral("已删除 MCP 来源「%1」（只移除配置，未删除插件文件）。").arg(label))) {
                mcpPluginSpecs_.insert(index, spec);
                QMessageBox::warning(this, QStringLiteral("删除 MCP 来源"),
                                     QStringLiteral("写入配置失败，未删除该来源。"));
            }
            return;
        }
    }
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
    if (engineBusy_) return; // 启动/停止执行中，忽略重复点击
    if (engine_.isRunning()) {
        const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
        setEngineBusy(true, QStringLiteral("停止中…"));
        // 先把进度文案与禁用态画出来，再执行阻塞的 PJSUA 销毁
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        engine_.stop();
        setEngineBusy(false, QString());
        appendLog(QStringLiteral("停止操作完成，界面阻塞 %1 ms")
                      .arg(QDateTime::currentMSecsSinceEpoch() - startedMs));
        return;
    }
    lastStatus_.clear();
    const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
    setEngineBusy(true, QStringLiteral("启动中…"));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    const bool started = engine_.start(formConfig());
    setEngineBusy(false, QString());
    appendLog(QStringLiteral("启动操作完成，界面阻塞 %1 ms")
                  .arg(QDateTime::currentMSecsSinceEpoch() - startedMs));
    if (!started) {
        QMessageBox::critical(this, QStringLiteral("启动失败"),
                              lastStatus_.isEmpty() ? QStringLiteral("机器人启动失败，请查看日志。") : lastStatus_);
    }
}

void MainWindow::setEngineBusy(bool busy, const QString &text)
{
    engineBusy_ = busy;
    if (busy) {
        appendLog(QStringLiteral("界面反馈：%1（按钮已禁用）").arg(text));
        startStop_->setText(text);
        startStop_->setEnabled(false);
        if (trayToggle_) trayToggle_->setText(text);
        if (trayToggle_) trayToggle_->setEnabled(false);
        if (localToggle_) localToggle_->setEnabled(false);
        status_->setText(text);
        return;
    }
    const QString action = engine_.isRunning()
        ? QStringLiteral("停止机器人") : QStringLiteral("启动机器人");
    startStop_->setText(action);
    startStop_->setEnabled(!localActive_);
    if (trayToggle_) {
        trayToggle_->setText(action);
        trayToggle_->setEnabled(true);
    }
    if (localToggle_) localToggle_->setEnabled(!engine_.isRunning());
    updateActions();
}

void MainWindow::applyRunning(bool running)
{
    if (running) {
        // 新一次引擎运行：呼叫 ID 会从头复用，清空映射让新通话追加新行，
        // 而不是覆盖上一次运行留下的历史行
        liveEngineRows_.clear();
    }
    configuration_->setEnabled(!running && !localActive_);
    // 工具配置已移到顶层「工具」页：通话中禁止改插件连接（会影响进行中的工具调用），
    // 工具启用开关只影响下一次通话，可继续保持可操作。
    if (mcpConfigPanel_) mcpConfigPanel_->setEnabled(!running && !localActive_);
    const QString action = running ? QStringLiteral("停止机器人") : QStringLiteral("启动机器人");
    if (engineBusy_) {
        // 启动/停止尚未完成：保持进度文案与禁用，避免重复点击
        startStop_->setEnabled(false);
    } else {
        startStop_->setText(action);
        startStop_->setEnabled(!localActive_); // 本地对话与引擎互斥
    }
    if (localToggle_) localToggle_->setEnabled(!running && !engineBusy_);
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
    if (config.allowBargeIn) {
        status_->setText(QStringLiteral(
            "本地对话正在开始：全双工（已勾选允许打断），麦克风全程开启，可直接语音打断。"));
        appendLog(QStringLiteral(
            "本地对话以全双工启动：麦克风全程开启、上行不停，收到 speech_started 立即停播。"));
    } else {
        status_->setText(QStringLiteral(
            "本地对话正在开始：半双工（未勾选允许打断），AI 播放期间上行静音。"));
        appendLog(QStringLiteral(
            "本地对话以半双工启动：AI 播放期间上行静音，播放结束立即恢复监听。"));
    }
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
        // 实时转写区也必须清空：否则两通本地对话的转写会混在一起显示
        // （通话表中各行的历史转写缓存在 addTranscript 里按行隔离，不受影响）。
        localTranscript_.clear();
        lastLocalUsage_ = ModelUsage();
        lastLocalFinishReason_.clear();
        // 每通本地对话分配独立表行与递增标识（跨停止/启动继续累加），历史行保留
        ++localCallSerial_;
        localCallLabel_ = QStringLiteral("local-%1").arg(localCallSerial_);
        localRowKey_ = createCallRow(localRowId, localCallLabel_);
        // CONFIRMED 语义：已开始计时/已接听
        updateCallRow(localRowKey_, QStringLiteral("sip:local@local"), QStringLiteral("CONFIRMED"));
        if (auto rec = calls_.find(localRowKey_); rec != calls_.end()) {
            rec->callerNumber = QStringLiteral("本地麦克风");
            rec->calleeNumber = QStringLiteral("AI 助手");
            const int row = rec->idItem->row();
            callTable_->item(row, 1)->setText(rec->callerNumber);
            callTable_->item(row, 2)->setText(rec->calleeNumber);
            callTable_->item(row, 7)->setText(QStringLiteral("已接听"));
            callTable_->selectRow(row);
        }
    } else {
        pendingLocalToolTokens_.clear(); // 对话结束，丢弃未完成的工具调用
        writeLocalCallRecord();
        // 收尾表格行：挂断方、挂断原因、最终时长
        if (auto rec = calls_.find(localRowKey_); rec != calls_.end() && !rec->ended) {
            rec->hangupBy = QStringLiteral("user");
            updateCallRow(localRowKey_, QStringLiteral("sip:local@local"), QStringLiteral("DISCONNECTED"));
            callTable_->item(rec->idItem->row(), 12)->setText(lastLocalFinishReason_.left(80));
        }
    }
    localToggle_->setText(active ? QStringLiteral("结束本地对话") : QStringLiteral("开始本地对话"));
    const bool engineRunning = engine_.isRunning();
    startStop_->setEnabled(!active);
    localToggle_->setEnabled(!engineRunning);
    configuration_->setEnabled(!active && !engineRunning);
    if (mcpConfigPanel_) mcpConfigPanel_->setEnabled(!active && !engineRunning);
    setStatusPill(active, active ? QStringLiteral("本地对话中") : QStringLiteral("未启动"));
    showTranscript();
}

void MainWindow::updateInteractionHint()
{
    if (!interactionHint_ || !allowBargeIn_)
        return;
    if (allowBargeIn_->isChecked()) {
        interactionHint_->setStyleSheet(QStringLiteral("color:#1f6f4a;"));
        interactionHint_->setText(QStringLiteral(
            "当前为全双工：AI 播放期间麦克风保持开启、上行不停，用户一说话就立即停止播报。"
            "外放时若出现 AI 自激，请改用耳机或检查输入设备是否为回环/虚拟设备。"));
    } else {
        interactionHint_->setStyleSheet(QStringLiteral("color:#555555;"));
        interactionHint_->setText(QStringLiteral(
            "当前为半双工：AI 播放期间不发送麦克风音频，播放结束立即恢复监听。"));
    }
}

void MainWindow::writeLocalCallRecord()
{
    // 仅在对话真正开始过的情况下写记录；写后置空时间戳防止重复写入
    if (!localStartTime_.isValid() || !localRecorder_)
        return;
    const BotConfig config = formConfig(); // 对话期间配置面板禁用，表单仍是启动时的值
    const QDateTime now = QDateTime::currentDateTime();
    qwenbot::CallRecord record; // 显式限定：MainWindow 内有同名嵌套 struct CallRecord（通话列表条目）
    // 与通话表展示的 local-N 保持一致；跨停止/启动继续累加
    record.callId = localCallLabel_.isEmpty()
        ? QStringLiteral("local-%1").arg(localCallSerial_)
        : localCallLabel_;
    record.caller = QStringLiteral("local");
    record.callee = QStringLiteral("local");
    record.modelId = config.effectiveRealtimeModel();
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
    if (auto rec = calls_.find(localRowKey_); rec != calls_.end() && !rec->ended) {
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
    if (localRowKey_ >= 0) addTranscript(localRowKey_, display, text);
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
    // 实时语音（WorkspaceId + 地域 + 模型 / 手动端点）的错误在这里直接弹窗，避免只在日志里出现
    if (const QString error = config.realtimeError(); !error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("不能保存"), error);
        return;
    }
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
    // MCP 远端服务必须有 URL，否则保存后无法连接
    for (const auto &plugin : config.mcpPlugins) {
        if (plugin.isHttp() && plugin.url.trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("不能保存"),
                                 QStringLiteral("MCP 远端服务「%1」未填写 URL。").arg(plugin.name));
            return;
        }
    }
    saveConfig(settings_, config);
    // MCP 插件热插拔：差异比对，仅启动新增、停止移除、配置变化重启
    if (tools_) {
        tools_->applyMcpPlugins(config.mcpPlugins);
        // 工具级启停：保存后同样即时生效（只影响下一次通话）
        tools_->setDisabledTools(config.disabledTools);
    }
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

int MainWindow::rowKeyForEngine(int engineId) const
{
    return liveEngineRows_.value(engineId, -1);
}

int MainWindow::createCallRow(int engineId, const QString &displayId)
{
    if (calls_.size() >= maxHistoryCalls) {
        // 只淘汰已结束的最旧一行；进行中的通话不回收
        for (int row = 0; row < callTable_->rowCount(); ++row) {
            const int oldKey = callTable_->item(row, 0)->data(Qt::UserRole).toInt();
            const auto oldRecord = calls_.constFind(oldKey);
            if (oldRecord != calls_.cend() && oldRecord->ended) {
                calls_.remove(oldKey);
                callTable_->removeRow(row);
                break;
            }
        }
    }
    // 行键单调递增，不与引擎重启后复用的呼叫 ID 冲突，历史行不会被新通话覆盖
    const int rowKey = nextRowKey_++;
    const int row = callTable_->rowCount();
    callTable_->insertRow(row);
    CallRecord record;
    record.engineId = engineId;
    record.idItem = new QTableWidgetItem(displayId);
    record.idItem->setData(Qt::UserRole, rowKey);
    callTable_->setItem(row, 0, record.idItem);
    for (int column = 1; column < 14; ++column)
        callTable_->setItem(row, column, new QTableWidgetItem);
    callTable_->item(row, 6)->setText(allowBargeIn_->isChecked()
        ? QStringLiteral("打断") : QStringLiteral("不打断"));
    calls_.insert(rowKey, record);
    return rowKey;
}

void MainWindow::updateCall(int id, const QString &peer, const QString &state)
{
    int rowKey = rowKeyForEngine(id);
    if (rowKey < 0) {
        // 引擎停止后的迟到非挂断事件不再生成幽灵行
        if (!engine_.isRunning() && state != QStringLiteral("DISCONNECTED"))
            return;
        rowKey = createCallRow(id, QString::number(id));
        liveEngineRows_.insert(id, rowKey);
    }
    updateCallRow(rowKey, peer, state);
}

void MainWindow::updateCallRow(int rowKey, const QString &peer, const QString &state)
{
    auto existing = calls_.find(rowKey);
    if (existing == calls_.end()) return;
    if (existing->ended && state == QStringLiteral("DISCONNECTED"))
        return;
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

void MainWindow::endCall(int rowKey)
{
    auto record = calls_.find(rowKey);
    if (record == calls_.end() || record->ended) return;
    record->ended = true;
    record->finalDuration = record->connected ? record->connectedAt.elapsed() : 0;
    const int row = record->idItem->row();
    callTable_->item(row, 3)->setText(QStringLiteral("已结束"));
    callTable_->item(row, 4)->setText(
        record->connected ? durationText(record->finalDuration) : QStringLiteral("未接通"));
    updateActions();
}

int MainWindow::selectedRowKey() const
{
    const int row = callTable_->currentRow();
    const auto *item = row >= 0 ? callTable_->item(row, 0) : nullptr;
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

int MainWindow::selectedCall() const
{
    const auto record = calls_.constFind(selectedRowKey());
    return record == calls_.cend() ? -1 : record->engineId;
}

void MainWindow::updateActions()
{
    const auto record = calls_.constFind(selectedRowKey());
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
    const int rowKey = selectedRowKey();
    // 本地对话进行中只对当前本地行展示累计转写；选中历史行时仍回看该行记录
    if (localActive_ && rowKey == localRowKey_) {
        transcript_->setPlainText(localTranscript_.join(QLatin1Char('\n')));
        return;
    }
    const auto record = calls_.constFind(rowKey);
    transcript_->setPlainText(record == calls_.cend() ? QString() : record->transcript.join(QLatin1Char('\n')));
}

void MainWindow::addTranscript(int rowKey, const QString &speaker, const QString &text)
{
    auto record = calls_.find(rowKey);
    if (record == calls_.end() || record->ended || text.isEmpty()) return;
    const QString entry = QStringLiteral("[%1] %2：%3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             safeText(speaker).left(80), safeText(text).left(maxEntryCharacters));
    record->transcript.append(entry);
    record->transcriptSize += entry.size();
    while (record->transcript.size() > maxTranscriptLines || record->transcriptSize > maxTranscriptCharacters)
        record->transcriptSize -= record->transcript.takeFirst().size();
    if (selectedRowKey() == rowKey) {
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
