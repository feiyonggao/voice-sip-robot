#include "bot_engine.h"
#include "file_logger.h"
#include "main_window.h"
#include "tool_registry.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageLogContext>
#include <QSettings>
#include <QStyleFactory>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <exception>
#include <cstdio>
#include <memory>

namespace {
volatile std::sig_atomic_t interrupted = 0;

void interruptHandler(int signal)
{
    interrupted = signal;
}

QString withoutSecrets(QString text, const qwenbot::BotConfig &config)
{
    if (!config.apiKey.isEmpty())
        text.replace(config.apiKey, QStringLiteral("[redacted]"));
    if (!config.sipPassword.isEmpty())
        text.replace(config.sipPassword, QStringLiteral("[redacted]"));
    return text;
}

void printError(const QString &text)
{
    QTextStream stream(stderr);
    stream << text << Qt::endl;
    // 同步写入统一运行日志文件（logger 尚未创建时静默跳过）
    if (auto *logger = qwenbot::FileLogger::instance())
        logger->write(text);
}

// 兜底捕获 Qt 内部告警（网络、QProcess 等），写入日志文件并保留 stderr 输出
void qtMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    QString prefix;
    switch (type) {
    case QtWarningMsg: prefix = QStringLiteral("[Qt WARN] "); break;
    case QtCriticalMsg: prefix = QStringLiteral("[Qt CRIT] "); break;
    case QtFatalMsg: prefix = QStringLiteral("[Qt FATAL] "); break;
    default: prefix = QStringLiteral("[Qt] "); break;
    }
    if (auto *logger = qwenbot::FileLogger::instance())
        logger->write(prefix + msg);
    fprintf(stderr, "%s\n", (prefix + msg).toLocal8Bit().constData());
    if (type == QtFatalMsg)
        abort();
}
} // namespace

int main(int argc, char *argv[])
{
    //强制 Qt 网络栈仅使用 IPv4（QWebSocket 无公开的 IP 族控制 API）。
    //SIP/RTP 走 PJSIP 自带网络栈，本就是 IPv4-only，不受影响。
    qputenv("QT_NO_IPV6", "1");
    bool headless = false;
    bool commandHelp = false;
    for (int index = 1; index < argc; ++index) {
        const QByteArray argument(argv[index]);
        if (argument == "--") break;
        if (argument == "--headless") headless = true;
        if (argument == "--help" || argument == "-h" ||
            argument == "--help-all" || argument == "--version" || argument == "-v")
            commandHelp = true;
    }

    std::unique_ptr<QCoreApplication> application;
    if (headless || commandHelp)
        application = std::make_unique<QCoreApplication>(argc, argv);
    else
        application = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("QwenSipRobot"));
    QCoreApplication::setApplicationName(QStringLiteral("Qwen SIP Robot"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    // 统一运行日志：QCoreApplication 就绪后立即创建，尽早捕获启动阶段日志；
    // 先用内置默认路径 log/voicebot.log，loadConfig 后再按用户配置重配
    qwenbot::FileLogger fileLogger;
    fileLogger.init(qwenbot::BotConfig{});
    qInstallMessageHandler(qtMessageHandler);
    fileLogger.write(QStringLiteral("========== Voice SIP Robot 启动 =========="));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Voice SIP Robot — SIP UDP / IPv4 入站语音机器人\n"
        "GUI 默认不启动；--headless 从配置读取并立即启动（不需要显示服务）。\n"
        "无界面模式首次运行（配置为空）会自动生成默认配置文件并退出，编辑后重新启动。\n"
        "密钥可通过配置文件字段或环境变量提供（优先级：环境变量 > 配置文件）：\n"
        "  - DASHSCOPE_API_KEY / qwen.apiKey：阿里 DashScope API Key\n"
        "  - QWENBOT_SIP_PASSWORD / sip.password：SIP 注册密码\n"
        "【建议】生产环境使用环境变量，避免密钥明文落盘。Ctrl+C / SIGTERM 停止无界面服务。\n\n"
        "--config 指定非敏感 INI 文件，亦可在 GUI 中编辑和保存。\n"
        "[sip]: listenAddress, port, rtpPort, maxCalls, allowedPeers, autoAnswer,\n"
        "       identity, registrar, user\n"
        "[qwen]: endpoint, voice, instructions, greeting, turnMode\n"
        "[tools]: enabledPlugins（启用的插件文件名，逗号分隔；插件位于打包的 plugins 目录，留空禁用）\n"
        "allowedPeers 使用逗号分隔的 IPv4 地址；autoAnswer 为 true/false。\n"
        "endpoint 必须是工作空间提供的完整 WSS 端点，不含密钥；\n"
        "model 参数固定为 qwen-audio-3.1-realtime-plus。\n"
        "不指定 --config 时使用当前用户的 QSettings 配置。"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption headlessOption(QStringLiteral("headless"),
        QStringLiteral("使用 QCoreApplication 运行无界面服务并立即启动。"));
    const QCommandLineOption configOption(QStringLiteral("config"),
        QStringLiteral("读取/保存指定的非敏感 INI 配置。"), QStringLiteral("path"));
    parser.addOption(headlessOption);
    parser.addOption(configOption);
    parser.process(*application);
    if (!parser.positionalArguments().isEmpty()) {
        printError(QStringLiteral("不接受位置参数；请使用 --help 查看用法。"));
        return 2;
    }

    std::unique_ptr<QSettings> settings;
    if (parser.isSet(configOption)) {
        const QString path = parser.value(configOption);
        const QFileInfo file(path);
        if (path.isEmpty() || (file.exists() && (!file.isFile() || !file.isReadable()))) {
            printError(QStringLiteral("配置文件不可读或不是普通文件。"));
            return 2;
        }
        settings = std::make_unique<QSettings>(file.absoluteFilePath(), QSettings::IniFormat);
    } else if (const QString envPath = qEnvironmentVariable("VOICEBOT_CONFIG"); !envPath.isEmpty()) {
        // 环境变量指定的 INI 配置（优先级低于 --config），便于容器/服务部署
        settings = std::make_unique<QSettings>(QFileInfo(envPath).absoluteFilePath(), QSettings::IniFormat);
    } else {
        settings = std::make_unique<QSettings>();
    }
    settings->setFallbacksEnabled(false);

    // 无界面模式首次启动：配置为空时生成默认配置文件并退出，供用户编辑后重新启动。
    if (headless && settings->allKeys().isEmpty()) {
        qwenbot::saveConfig(*settings, qwenbot::BotConfig{});
        settings->sync();
        printError(QStringLiteral(
            "已生成默认配置文件：%1\n"
            "请编辑其中的 SIP / Qwen 配置项后重新启动；\n"
            "密钥仅通过环境变量 DASHSCOPE_API_KEY / QWENBOT_SIP_PASSWORD 提供。")
            .arg(settings->fileName()));
        return 2;
    }

    qwenbot::BotConfig config;
    try {
        config = qwenbot::loadConfig(*settings);
    } catch (const std::exception &error) {
        printError(QString::fromUtf8(error.what()));
        return 2;
    }
    // 按用户配置重配日志路径/轮转参数（相对路径基于程序目录，目录自动创建）
    fileLogger.init(config);

    qwenbot::BotEngine engine;
    QObject::connect(application.get(), &QCoreApplication::aboutToQuit,
                     &engine, &qwenbot::BotEngine::stop);
    std::signal(SIGINT, interruptHandler);
    std::signal(SIGTERM, interruptHandler);
    QTimer signalTimer;
    signalTimer.setInterval(100);
    QObject::connect(&signalTimer, &QTimer::timeout, application.get(), [&] {
        if (interrupted != 0) application->quit();
    });
    signalTimer.start();

    if (headless) {
        // MCP 工具注册表：随启动发现插件工具，通话打开时由引擎取快照
        auto registry = std::make_unique<qwenbot::ToolRegistry>();
        QObject::connect(registry.get(), &qwenbot::ToolRegistry::logMessage,
                         application.get(), [&](const QString &text) {
            printError(withoutSecrets(text, config));
        });
        engine.setToolRegistry(registry.get());
        registry->applyMcpCommands(config.mcpCommands);
        // 等待各插件完成首次工具发现（最多 5 秒，超时照常启动，插件继续后台重试）
        QElapsedTimer discovery;
        discovery.start();
        while (registry->pendingMcpCount() > 0 && discovery.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const auto tools = registry->tools();
        QStringList toolNames;
        for (const auto &tool : tools)
            toolNames.append(tool.builtIn || tool.online
                                 ? tool.name : tool.name + QStringLiteral("（离线）"));
        printError(QStringLiteral("可用工具 %1 个：%2")
                       .arg(toolNames.size()).arg(toolNames.join(QStringLiteral(", "))));
        // registry 保持作用域存活：exec 返回后析构，顺带停止 MCP 插件子进程

        QObject::connect(&engine, &qwenbot::BotEngine::statusChanged,
                         application.get(), [&](const QString &status) {
            printError(withoutSecrets(status, config));
        });
        QObject::connect(&engine, &qwenbot::BotEngine::logMessage,
                         application.get(), [&](const QString &message) {
            printError(withoutSecrets(message, config));
        });
        if (!engine.start(config)) {
            engine.stop();
            return 1;
        }
        return application->exec();
    }

    qwenbot::MainWindow window(engine, *settings, config);

    // 现代化 UI：Fusion 基底 + 柔和浅色卡片风（单一 accent、大圆角、克制留白）
    static const char *kAppStyle = R"(
    /* ---------- 基础 ---------- */
    QMainWindow, QDialog { background-color: #f3f5f9; }
    QWidget {
        color: #1f2937;
        font-family: "Segoe UI", "Microsoft YaHei UI", "PingFang SC", sans-serif;
        font-size: 13px;
    }
    QLabel, QCheckBox { background-color: transparent; }
    QToolTip {
        background-color: #111827;
        color: #f3f4f6;
        border: none;
        border-radius: 6px;
        padding: 6px 10px;
        font-size: 12px;
    }

    /* ---------- 顶部品牌区 ---------- */
    QLabel#appAvatar {
        background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                    stop:0 #6366f1, stop:1 #8b5cf6);
        border-radius: 11px;
        color: #ffffff;
        font-size: 17px;
        font-weight: 700;
        min-width: 38px; max-width: 38px;
        min-height: 38px; max-height: 38px;
    }
    QLabel#appTitle { font-size: 16px; font-weight: 700; color: #0f172a; }
    QLabel#appSubtitle { font-size: 12px; color: #64748b; }

    /* ---------- 关于页 ---------- */
    QFrame#aboutHero {
        background-color: #ffffff;
        border: 1px solid #e7e9ef;
        border-radius: 12px;
    }
    QLabel#aboutAvatar {
        background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                    stop:0 #6062f0, stop:1 #4f46e5);
        color: #ffffff;
        font-size: 24px;
        font-weight: 800;
        border-radius: 12px;
        min-width: 52px; max-width: 52px;
        min-height: 52px; max-height: 52px;
    }
    QLabel#aboutName { font-size: 19px; font-weight: 800; color: #0f172a; }
    QLabel#aboutSubtitle { font-size: 12px; color: #64748b; }
    QFrame#aboutBadge {
        background-color: #eef2ff;
        border: 1px solid #c7d2fe;
        border-radius: 11px;
    }
    QLabel#aboutBadgeText { color: #4f46e5; font-size: 12px; font-weight: 700; }
    QLabel#aboutKey { color: #64748b; font-size: 12px; }
    QLabel#aboutValue { color: #0f172a; font-size: 12px; font-weight: 600; }
    QLabel#aboutBody { color: #475569; font-size: 12px; }

    /* 状态胶囊 */
    QFrame#statusPill {
        background-color: #ecedf1;
        border: 1px solid #e0e3e9;
        border-radius: 13px;
    }
    QFrame#statusPill[running="true"] {
        background-color: #ecfdf5;
        border: 1px solid #a7f3d0;
    }
    QLabel#statusDot {
        background-color: #b6bcc7;
        border-radius: 4px;
        min-width: 8px; max-width: 8px;
        min-height: 8px; max-height: 8px;
    }
    QLabel#statusDot[running="true"] { background-color: #10b981; }
    QLabel#statusPillText { color: #5f6875; font-size: 12px; font-weight: 600; }
    QFrame#statusPill[running="true"] QLabel#statusPillText { color: #047857; }

    /* ---------- 按钮 ---------- */
    QPushButton {
        background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                    stop:0 #6062f0, stop:1 #4f46e5);
        color: #ffffff;
        border: none;
        border-radius: 8px;
        padding: 8px 18px;
        font-weight: 600;
    }
    QPushButton:hover { background-color: #4f46e5; }
    QPushButton:pressed { background-color: #4338ca; }
    QPushButton:disabled { background: #e5e7eb; color: #9ca3af; }
    QPushButton#secondary {
        background-color: #ffffff;
        color: #374151;
        border: 1px solid #d7dae0;
        font-weight: 500;
    }
    QPushButton#secondary:hover {
        background-color: #f6f7f9;
        border-color: #b6bcc7;
        color: #111827;
    }
    QPushButton#secondary:pressed { background-color: #eceef2; }
    QPushButton#secondary:disabled {
        background-color: #f3f4f6;
        color: #9ca3af;
        border-color: #e5e7eb;
    }
    QPushButton#danger {
        background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                    stop:0 #f76b6b, stop:1 #ef4444);
    }
    QPushButton#danger:hover { background-color: #dc2626; }
    QPushButton#danger:pressed { background-color: #b91c1c; }
    QPushButton#danger:disabled { background: #e5e7eb; color: #9ca3af; }

    /* ---------- 输入控件 ---------- */
    QLineEdit, QSpinBox, QComboBox {
        background-color: #ffffff;
        border: 1px solid #d7dae0;
        border-radius: 8px;
        padding: 7px 11px;
        selection-background-color: #c7d2fe;
        selection-color: #1e1b4b;
    }
    QLineEdit:hover:!focus, QSpinBox:hover:!focus, QComboBox:hover:!focus {
        border-color: #b6bcc7;
    }
    QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #6366f1; }
    QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
        background-color: #f3f4f6;
        color: #9ca3af;
        border-color: #e5e7eb;
    }
    QSpinBox { padding: 7px 2px 7px 8px; }
    QSpinBox::up-button, QSpinBox::down-button {
        background-color: transparent;
        border: none;
        width: 20px;
    }
    QSpinBox::up-button:hover, QSpinBox::down-button:hover { background-color: #eef2ff; }
    QComboBox { padding: 7px 2px 7px 8px; }
    QComboBox::drop-down {
        border: none;
        border-left: 1px solid #e5e7eb;
        width: 26px;
        subcontrol-origin: padding;
        subcontrol-position: center right;
    }
    QComboBox QAbstractItemView {
        background-color: #ffffff;
        border: 1px solid #e2e5ea;
        border-radius: 8px;
        padding: 4px;
        selection-background-color: #eef2ff;
        selection-color: #1e1b4b;
        outline: 0;
    }
    QComboBox QAbstractItemView::item {
        min-height: 26px;
        padding: 2px 10px;
        border-radius: 5px;
    }

    QPlainTextEdit {
        background-color: #ffffff;
        border: 1px solid #e2e5ea;
        border-radius: 10px;
        padding: 8px 10px;
        selection-background-color: #c7d2fe;
        selection-color: #1e1b4b;
    }
    QPlainTextEdit:focus { border-color: #6366f1; }
    QPlainTextEdit:disabled { background-color: #f3f4f6; color: #9ca3af; }

    /* ---------- 卡片分组 ---------- */
    QGroupBox {
        background-color: #ffffff;
        border: 1px solid #e7e9ef;
        border-radius: 12px;
        margin-top: 16px;
        padding: 18px 18px 10px 18px;
        font-weight: 700;
        color: #0f172a;
    }
    QGroupBox::title {
        subcontrol-origin: margin;
        subcontrol-position: top left;
        left: 14px;
        padding: 1px 8px;
        background-color: #ffffff;
        border-radius: 4px;
        color: #0f172a;
    }

    /* ---------- Tab（药丸式） ---------- */
    QTabWidget { background: transparent; }
    QTabWidget::pane { border: none; background: transparent; }
    QTabBar { background: transparent; }
    QTabBar::tab {
        background: transparent;
        border: 1px solid transparent;
        border-radius: 8px;
        padding: 7px 16px;
        margin-right: 4px;
        color: #64748b;
        font-weight: 600;
    }
    QTabBar::tab:selected {
        background-color: #ffffff;
        border: 1px solid #e7e9ef;
        color: #4f46e5;
    }
    QTabBar::tab:hover:!selected { color: #1f2937; background-color: #e9ebf0; }

    /* ---------- 表格 ---------- */
    QTableWidget {
        background-color: #ffffff;
        alternate-background-color: #fafbfc;
        border: 1px solid #e7e9ef;
        border-radius: 12px;
        selection-background-color: #e0e7ff;
        selection-color: #1e1b4b;
    }
    QTableWidget::item { padding: 6px 10px; border: none; }
    QTableWidget::item:selected { background-color: #e0e7ff; }
    QHeaderView::section {
        background-color: #ffffff;
        color: #64748b;
        font-size: 12px;
        font-weight: 600;
        padding: 10px;
        border: none;
        border-bottom: 1px solid #eceef2;
    }

    /* ---------- 滚动条 ---------- */
    QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px; }
    QScrollBar::handle:vertical {
        background: #d4d8e0;
        border-radius: 5px;
        min-height: 32px;
    }
    QScrollBar::handle:vertical:hover { background: #a8afbd; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px 4px; }
    QScrollBar::handle:horizontal {
        background: #d4d8e0;
        border-radius: 5px;
        min-width: 32px;
    }
    QScrollBar::handle:horizontal:hover { background: #a8afbd; }
    QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

    /* ---------- 复选框 ---------- */
    QCheckBox { color: #374151; spacing: 8px; }
    QCheckBox::indicator {
        width: 17px; height: 17px;
        border-radius: 5px;
        border: 1px solid #c9ced8;
        background-color: #ffffff;
    }
    QCheckBox::indicator:hover { border-color: #6366f1; }
    QCheckBox::indicator:checked { background-color: #4f46e5; border-color: #4f46e5; }
    QCheckBox::indicator:checked:disabled { background-color: #c7d2fe; border-color: #c7d2fe; }
    QCheckBox::indicator:disabled { background-color: #f3f4f6; border-color: #e5e7eb; }

    /* ---------- 容器与其它 ---------- */
    QScrollArea { background: transparent; border: none; }
    QWidget#qt_scrollarea_viewport { background: transparent; }
    QSplitter::handle { background: transparent; }
    QSplitter::handle:vertical { height: 10px; }
    QSplitter::handle:horizontal { width: 10px; }
    QMenu {
        background-color: #ffffff;
        border: 1px solid #e7e9ef;
        border-radius: 10px;
        padding: 6px;
    }
    QMenu::item { padding: 7px 26px 7px 14px; border-radius: 6px; }
    QMenu::item:selected { background-color: #eef2ff; color: #1e1b4b; }
    QMenu::separator { height: 1px; background-color: #eceef2; margin: 5px 8px; }

    QLabel#hint { color: #94a3b8; font-size: 12px; }
    QLabel#footerStatus { color: #8a91a0; font-size: 12px; padding: 0 4px; }
    )";
    if (auto *app = qobject_cast<QApplication *>(application.get())) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        app->setStyleSheet(QString::fromLatin1(kAppStyle));
    }

    window.show();
    return application->exec();
}
