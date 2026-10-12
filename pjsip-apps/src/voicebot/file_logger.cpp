#include "file_logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace qwenbot {

namespace {
FileLogger *s_instance = nullptr;
}

FileLogger::FileLogger(QObject *parent) : QObject(parent)
{
    s_instance = this;
}

FileLogger::~FileLogger()
{
    if (s_instance == this)
        s_instance = nullptr;
}

FileLogger *FileLogger::instance()
{
    return s_instance;
}

QString FileLogger::filePath() const
{
    QMutexLocker locker(&mutex_);
    return filePath_;
}

void FileLogger::init(const BotConfig &config)
{
    QString path = config.logPath.trimmed();
    if (!path.isEmpty() && QDir(path).isRelative())
        path = QCoreApplication::applicationDirPath() + QLatin1Char('/') + path;
    QMutexLocker locker(&mutex_);
    filePath_ = QDir::cleanPath(path);
    maxSize_ = config.logMaxSize > 0 ? config.logMaxSize : 10LL * 1024 * 1024;
    maxFiles_ = config.logMaxFiles > 0 ? config.logMaxFiles : 5;
}

void FileLogger::write(const QString &text)
{
    QString line = QStringLiteral("[%1] %2\n")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             text);
    QMutexLocker locker(&mutex_);
    if (filePath_.isEmpty())
        return;
    const QByteArray data = line.toUtf8();
    QFileInfo fi(filePath_);
    if (!fi.dir().exists() && !fi.dir().mkpath(QStringLiteral(".")))
        return; // 目录创建失败时静默丢弃，避免日志故障影响主流程
    // 超大小先滚动：voicebot.log -> voicebot.log.1 -> ...
    if (fi.exists() && fi.size() + data.size() > maxSize_) {
        const QString last = QStringLiteral("%1.%2").arg(filePath_).arg(maxFiles_);
        QFile::remove(last);
        for (int i = maxFiles_ - 1; i >= 1; --i) {
            const QString src = QStringLiteral("%1.%2").arg(filePath_).arg(i);
            const QString dst = QStringLiteral("%1.%2").arg(filePath_).arg(i + 1);
            if (QFile::exists(src)) {
                QFile::remove(dst);
                QFile::rename(src, dst);
            }
        }
        QFile::remove(QStringLiteral("%1.1").arg(filePath_));
        QFile::rename(filePath_, QStringLiteral("%1.1").arg(filePath_));
    }
    QFile file(filePath_);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        file.write(data);
}

} // namespace qwenbot
