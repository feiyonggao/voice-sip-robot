#pragma once

#include "bot_config.h"
#include <QObject>
#include <QMutex>
#include <QString>
#include <memory>

namespace qwenbot {

// 统一运行日志文件：相对路径基于程序目录解析（默认 log/voicebot.log），
// 目录自动创建；按大小滚动，voicebot.log -> .1 -> .2 ...，与通话记录规则一致。
// write() 线程安全，可在任意线程直接调用（含 Qt 消息处理器）。
class FileLogger : public QObject {
    Q_OBJECT
public:
    explicit FileLogger(QObject *parent = nullptr);
    ~FileLogger() override;

    // 按配置初始化/重配（logPath/logMaxSize/logMaxFiles）；空路径表示禁用文件日志
    void init(const BotConfig &config);
    QString filePath() const;

    // 进程级单例：供 Qt 消息处理器和各日志漏斗使用；未创建时为 nullptr
    static FileLogger *instance();

public slots:
    // 追加一行日志，自动补全时间戳前缀，落盘失败静默忽略（不得影响主流程）
    void write(const QString &text);

private:
    mutable QMutex mutex_;
    QString filePath_;
    qint64 maxSize_ = 10 * 1024 * 1024;
    int maxFiles_ = 5;
};

} // namespace qwenbot
