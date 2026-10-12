#pragma once

#include "bot_config.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>

namespace qwenbot {

struct CallRecord {
    QString callId;
    QString caller;
    QString callee;
    QString modelId = QStringLiteral("qwen-audio-3.1-realtime-plus");
    QString voice;
    QDateTime startTime;
    QDateTime answerTime;
    QDateTime hangupTime;
    qint64 durationMs = 0;
    qint64 callDurationMs = 0;
    int turns = 0;
    bool allowBargeIn = true;
    bool answered = false;
    QString hangupBy = QStringLiteral("caller");
    QString hangupReason;
    int sipStatusCode = 0;
    QJsonObject customVars;
    // 对话记录（逐句转写），元素：{"time": ISO8601ms, "speaker": "caller"|"AI", "text": ...}
    // 随通话记录同条保存/回调，无需独立文件
    QJsonArray transcript;
    // 模型 token 用量（usage 字段，阿里计费以控制台单价为准）；服务端未上报时为空
    QJsonObject usage;

    QJsonObject toJson() const;
};

class CallRecorder : public QObject {
    Q_OBJECT
public:
    explicit CallRecorder(QObject *parent = nullptr);
    ~CallRecorder() override;
    void init(const BotConfig &config);
    void write(const CallRecord &record);

signals:
    void logMessage(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qwenbot
