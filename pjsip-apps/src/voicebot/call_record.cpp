#include "call_record.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

namespace qwenbot {

QJsonObject CallRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("call_id")] = callId;
    obj[QStringLiteral("caller")] = caller;
    obj[QStringLiteral("callee")] = callee;
    obj[QStringLiteral("model_id")] = modelId;
    obj[QStringLiteral("voice")] = voice;
    obj[QStringLiteral("start_time")] = startTime.toString(Qt::ISODateWithMs);
    obj[QStringLiteral("answer_time")] = answerTime.toString(Qt::ISODateWithMs);
    obj[QStringLiteral("hangup_time")] = hangupTime.toString(Qt::ISODateWithMs);
    obj[QStringLiteral("duration_ms")] = durationMs;
    obj[QStringLiteral("call_duration_ms")] = callDurationMs;
    obj[QStringLiteral("duration_sec")] = durationMs / 1000.0;
    obj[QStringLiteral("call_duration_sec")] = callDurationMs / 1000.0;
    obj[QStringLiteral("turns")] = turns;
    obj[QStringLiteral("barge_in_mode")] = allowBargeIn ? QStringLiteral("interrupt") : QStringLiteral("no_interrupt");
    obj[QStringLiteral("answered")] = answered;
    obj[QStringLiteral("hangup_by")] = hangupBy;
    obj[QStringLiteral("hangup_reason")] = hangupReason;
    obj[QStringLiteral("sip_status_code")] = sipStatusCode;
    if (!customVars.isEmpty())
        obj[QStringLiteral("custom_variables")] = customVars;
    if (!transcript.isEmpty())
        obj[QStringLiteral("transcript")] = transcript;
    if (!usage.isEmpty())
        obj[QStringLiteral("usage")] = usage;
    return obj;
}

struct CallRecorder::Impl {
    QString filePath;
    QString webhookUrl;
    qint64 maxSize = 10 * 1024 * 1024;
    int maxFiles = 5;
    QNetworkAccessManager *nam = nullptr;
    bool initialized = false;
    CallRecorder *q = nullptr;

    void writeToFile(const QJsonObject &json);
    void rotateFiles();
    void postToWebhook(const QJsonObject &json);
    // 一次性升级迁移：旧版默认文件 <appdir>/call_records.jsonl(.N) 搬到 <appdir>/cdr/
    void migrateLegacyFile();
};

CallRecorder::CallRecorder(QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->q = this;
}
CallRecorder::~CallRecorder() = default;

void CallRecorder::init(const BotConfig &config)
{
    // 相对路径统一基于程序所在目录解析，避免依赖进程工作目录
    QString path = config.callRecordPath.trimmed();
    if (!path.isEmpty() && QDir(path).isRelative())
        path = QCoreApplication::applicationDirPath() + QLatin1Char('/') + path;
    impl_->filePath = QDir::cleanPath(path);
    impl_->webhookUrl = config.callRecordWebhook;
    impl_->maxSize = config.callRecordMaxSize > 0 ? config.callRecordMaxSize : 10 * 1024 * 1024;
    impl_->maxFiles = config.callRecordMaxFiles > 0 ? config.callRecordMaxFiles : 5;
    if (!impl_->nam)
        impl_->nam = new QNetworkAccessManager(this);
    impl_->initialized = true;
    impl_->migrateLegacyFile();
    if (!impl_->filePath.isEmpty())
        emit logMessage(QStringLiteral("通话记录将写入：%1（单文件上限 %2 MB，保留 %3 份，含对话转写）")
            .arg(impl_->filePath).arg(impl_->maxSize / (1024 * 1024)).arg(impl_->maxFiles));
    if (!impl_->webhookUrl.isEmpty())
        emit logMessage(QStringLiteral("通话记录将推送至：%1").arg(impl_->webhookUrl));
}

void CallRecorder::Impl::migrateLegacyFile()
{
    // 仅当目标仍是新版默认布局 <appdir>/cdr/calls.jsonl 时才迁移，自定义路径不动
    const QString newDefault = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/cdr/calls.jsonl"));
    if (filePath != newDefault)
        return;
    QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList legacy = appDir.entryList(
        {QStringLiteral("call_records.jsonl*")}, QDir::Files);
    if (legacy.isEmpty())
        return;
    if (!appDir.mkpath(QStringLiteral("cdr")))
        return;
    const QString legacyBase = QStringLiteral("call_records.jsonl");
    for (const QString &name : legacy) {
        const QString suffix = name.mid(legacyBase.size());
        const QString targetRel = QStringLiteral("cdr/calls.jsonl") + suffix;
        if (QFile::exists(appDir.filePath(targetRel)))
            continue; // 目标已有内容，不覆盖，旧文件保留原位
        if (QFile::rename(appDir.filePath(name), appDir.filePath(targetRel)))
            emit q->logMessage(QStringLiteral("已迁移旧通话记录：%1 → %2").arg(name, targetRel));
    }
}

void CallRecorder::Impl::rotateFiles()
{
    const QString last = QStringLiteral("%1.%2").arg(filePath).arg(maxFiles);
    if (QFile::exists(last)) {
        QFile::remove(last);
    }
    for (int i = maxFiles - 1; i >= 1; --i) {
        const QString src = QStringLiteral("%1.%2").arg(filePath).arg(i);
        const QString dst = QStringLiteral("%1.%2").arg(filePath).arg(i + 1);
        if (QFile::exists(src)) {
            QFile::remove(dst);
            QFile::rename(src, dst);
        }
    }
    const QString first = QStringLiteral("%1.1").arg(filePath);
    QFile::remove(first);
    QFile::rename(filePath, first);
}

void CallRecorder::Impl::writeToFile(const QJsonObject &json)
{
    if (filePath.isEmpty()) return;
    const QByteArray line = QJsonDocument(json).toJson(QJsonDocument::Compact) + "\n";
    QFileInfo fi(filePath);
    // 写入前确保目录存在（如 cdr/ 子目录）
    if (!fi.dir().exists() && !fi.dir().mkpath(QStringLiteral("."))) {
        emit q->logMessage(QStringLiteral("无法创建通话记录目录：%1").arg(fi.dir().absolutePath()));
        return;
    }
    // 写入前检查是否需要轮转：若当前文件存在且加上新行后超过上限，则轮转
    if (fi.exists() && fi.size() + line.size() > maxSize) {
        rotateFiles();
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        emit q->logMessage(QStringLiteral("无法写入通话记录文件：%1").arg(filePath));
        return;
    }
    file.write(line);
    file.close();
}

void CallRecorder::Impl::postToWebhook(const QJsonObject &json)
{
    if (webhookUrl.isEmpty() || !nam) return;
    QNetworkRequest request;
    request.setUrl(QUrl(webhookUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", "qwenbot/1.0");
    auto *reply = nam->post(request, QJsonDocument(json).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, [reply, this] {
        if (reply->error() != QNetworkReply::NoError) {
            emit q->logMessage(QStringLiteral("通话记录推送失败：%1").arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

void CallRecorder::write(const CallRecord &record)
{
    if (!impl_->initialized) return;
    const auto json = record.toJson();
    impl_->writeToFile(json);
    impl_->postToWebhook(json);
}

} // namespace qwenbot
