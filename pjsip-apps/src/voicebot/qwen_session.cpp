#include "qwen_session.h"

#include "pcm_bridge.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDnsLookup>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QSslError>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QWebSocket>

namespace qwenbot {
namespace {
constexpr qint64 MaxPendingBytes = 128 * 1024;
constexpr quint64 MaxMessageBytes = 2 * 1024 * 1024;
constexpr qsizetype MaxInputBytes = 16000 * 2;
constexpr qsizetype MaxOutputBytes = 24000 * 2 * 10;
constexpr qsizetype MaxEncodedOutput = ((MaxOutputBytes + 2) / 3) * 4;
constexpr qsizetype MaxPendingInterruptions = 128;

qint64 usageNumber(const QJsonObject &obj, const char *key)
{
    return static_cast<qint64>(obj.value(QLatin1String(key)).toDouble());
}

// 内置挂机工具定义：与 ToolRegistry 注册的 hang_up 描述保持一致。
// tools 快照为空时（未接入注册表）仍以此兜底，保证挂机能力不丢失。
QJsonObject hangUpToolJson()
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("function")},
        {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("hang_up")},
            {QStringLiteral("description"), QStringLiteral(
                "挂断电话，结束本次通话。当用户明确道别、表示不再需要帮助，"
                "或本次任务已完成时，先向用户简短道别，随后立即调用此工具。")},
            {QStringLiteral("parameters"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"), QJsonObject{}},
                {QStringLiteral("required"), QJsonArray{}}}}}}};
}
} // namespace

// 解析单次 response.done 的 usage 并累计；字段缺失按 0 处理（兼容不同服务端实现）
void ModelUsage::accumulate(const QJsonObject &usage)
{
    if (usage.isEmpty())
        return;
    valid = true;
    inputTokens += usageNumber(usage, "input_tokens");
    outputTokens += usageNumber(usage, "output_tokens");
    const QJsonObject in = usage.value(QStringLiteral("input_token_details")).toObject();
    inputAudioTokens += usageNumber(in, "audio_tokens");
    inputTextTokens += usageNumber(in, "text_tokens");
    cachedTokens += usageNumber(in, "cached_tokens");
    const QJsonObject out = usage.value(QStringLiteral("output_token_details")).toObject();
    outputAudioTokens += usageNumber(out, "audio_tokens");
    outputTextTokens += usageNumber(out, "text_tokens");
}

QJsonObject ModelUsage::toJson() const
{
    if (!valid)
        return {};
    return QJsonObject{
        {QStringLiteral("input_tokens"), inputTokens},
        {QStringLiteral("output_tokens"), outputTokens},
        {QStringLiteral("total_tokens"), inputTokens + outputTokens},
        {QStringLiteral("input_audio_tokens"), inputAudioTokens},
        {QStringLiteral("input_text_tokens"), inputTextTokens},
        {QStringLiteral("cached_tokens"), cachedTokens},
        {QStringLiteral("output_audio_tokens"), outputAudioTokens},
        {QStringLiteral("output_text_tokens"), outputTextTokens},
    };
}

struct QwenSession::Impl : QObject {
    enum class State { Closed, Connecting, Updating, Ready, Failed };

    explicit Impl(QwenSession *session) : owner(session)
    {
        deadline.setSingleShot(true);
        deadline.setInterval(15000);
        connect(&deadline, &QTimer::timeout, this, [this] {
            fail(QStringLiteral("Qwen session initialization timed out (15 seconds)."));
        });
        silenceTimer.setSingleShot(true);
        connect(&silenceTimer, &QTimer::timeout, this, [this] {
            if (state != State::Ready || endingCall || farewellPending)
                return;
            if (pendingToolCalls > 0)
                return; // 工具执行中：暂停无声计时，结果回传后自然重新开始
            ++silenceRetries;
            if (silenceRetries >= maxSilenceRetries) {
                endingCall = true;
                sendFarewell();
            } else if (!silencePrompt.trimmed().isEmpty()) {
                sendSystemPrompt(silencePrompt);
            } else {
                silenceTimer.start();
            }
        });
    }

    ~Impl() override { shutdown(); }

    QwenSession *owner;
    std::unique_ptr<QWebSocket> socket;
    QTimer deadline;
    QTimer silenceTimer;
    QTimer drainTimer;
    State state = State::Closed;
    QString voice;
    QString instructions;
    QString greeting;
    QString turnMode;
    QString activeResponse;
    QSet<QString> blockedResponses;
    bool awaitingUserResponse = false;
    bool responseRequested = false;
    bool greetingRequested = false;
    bool greetingSent = false;
    bool allowBargeIn = true;
    int silenceTimeoutMs = 6000;
    int maxSilenceRetries = 3;
    int maxTurns = 40;
    QString silencePrompt;
    QString farewellPrompt;
    int turnCount = 0;
    ModelUsage usage;         // 累计 token 用量（response.done usage），通话中随信号上报
    int maxHistoryTurns = 20;
    int silenceRetries = 0;
    int pendingToolCalls = 0; // 已上抛、尚未回传结果的工具调用数（挂起期间暂停无声计时）
    QJsonArray tools;         // 本次会话锁定的工具集快照
    bool endingCall = false;
    bool farewellPending = false;
    bool systemResponsePending = false;
    bool hangupAfterResponse = false;
    bool pendingSilenceStart = false;

    void shutdown()
    {
        state = State::Closed;
        deadline.stop();
        silenceTimer.stop();
        drainTimer.stop();
        if (socket) {
            socket->disconnect(this);
            socket->abort();
            // close() may run synchronously from a socket error callback.
            // Retire the sender after its current signal stack has unwound.
            socket.release()->deleteLater();
        }
        activeResponse.clear();
        blockedResponses.clear();
        awaitingUserResponse = false;
        responseRequested = false;
        greetingRequested = false;
        greetingSent = false;
        turnCount = 0;
        silenceRetries = 0;
        pendingToolCalls = 0;
        endingCall = false;
        farewellPending = false;
        systemResponsePending = false;
        hangupAfterResponse = false;
        pendingSilenceStart = false;
    }

    void fail(const QString &message)
    {
        if (state == State::Closed || state == State::Failed)
            return;
        state = State::Failed;
        deadline.stop();
        silenceTimer.stop();
        if (socket) {
            socket->disconnect(this);
            socket->abort();
        }
        // Do not expose raw network/server errors: they can echo credentials.
        emit owner->failed(message);
    }

    bool send(const QJsonObject &event)
    {
        if (!socket || state == State::Closed || state == State::Failed)
            return false;
        const QByteArray json = QJsonDocument(event).toJson(QJsonDocument::Compact);
        if (socket->state() != QAbstractSocket::ConnectedState) {
            fail(QStringLiteral("Qwen WebSocket is not connected."));
            return false;
        }
        // Include framing headroom; never accumulate seconds of stale caller audio.
        if (json.size() > MaxPendingBytes - 16 ||
            socket->bytesToWrite() > MaxPendingBytes - 16 - json.size()) {
            fail(QStringLiteral("Qwen upload buffer exceeded its limit; connection is too slow."));
            return false;
        }
        if (socket->sendTextMessage(QString::fromUtf8(json)) != json.size()) {
            fail(QStringLiteral("Could not send a complete Qwen WebSocket event."));
            return false;
        }
        return true;
    }

    void sendGreeting()
    {
        if (state != State::Ready || !greetingRequested || greetingSent ||
            awaitingUserResponse || responseRequested || !activeResponse.isEmpty() ||
            !blockedResponses.isEmpty())
            return;
        greetingSent = true;
        greetingRequested = false;
        if (greeting.trimmed().isEmpty())
            return;
        responseRequested = true;
        const QJsonObject item{
            {QStringLiteral("type"), QStringLiteral("message")},
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("input_text")},
                {QStringLiteral("text"), greeting}}}}};
        if (!send({{QStringLiteral("type"), QStringLiteral("conversation.item.create")},
                   {QStringLiteral("item"), item}}))
            return;
        send({{QStringLiteral("type"), QStringLiteral("response.create")}});
    }

    void sendSystemPrompt(const QString &text)
    {
        if (state != State::Ready ||
            responseRequested || awaitingUserResponse ||
            !activeResponse.isEmpty() || !blockedResponses.isEmpty())
            return;
        if (text.trimmed().isEmpty())
            return;
        systemResponsePending = true;
        responseRequested = true;
        const QJsonObject item{
            {QStringLiteral("type"), QStringLiteral("message")},
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("input_text")},
                {QStringLiteral("text"), text}}}}};
        if (!send({{QStringLiteral("type"), QStringLiteral("conversation.item.create")},
                   {QStringLiteral("item"), item}}))
            return;
        send({{QStringLiteral("type"), QStringLiteral("response.create")}});
    }

    void sendFarewell()
    {
        if (farewellPrompt.trimmed().isEmpty()) {
            emit owner->endRequested(QStringLiteral("达到结束条件，挂断通话"));
            return;
        }
        farewellPending = true;
        sendSystemPrompt(farewellPrompt);
    }

    bool isCurrentResponse(const QJsonObject &event) const
    {
        const QString id = event.value(QStringLiteral("response_id")).toString();
        return !id.isEmpty() && id == activeResponse && !blockedResponses.contains(id);
    }

    void receiveAudio(const QJsonObject &event)
    {
        if (!isCurrentResponse(event))
            return;
        const QJsonValue value = event.value(QStringLiteral("delta"));
        if (!value.isString()) {
            fail(QStringLiteral("Qwen sent an invalid audio delta."));
            return;
        }
        const QString encoded = value.toString();
        if (encoded.isEmpty() || encoded.size() > MaxEncodedOutput ||
            encoded.size() % 4 != 0) {
            fail(QStringLiteral("Qwen audio delta has an invalid or excessive size."));
            return;
        }
        // Qt's default decoder silently ignores errors. Require the standard
        // padded alphabet, including canonical zero bits in the final quartet.
        int padding = 0;
        for (qsizetype i = 0; i < encoded.size(); ++i) {
            const ushort c = encoded.at(i).unicode();
            const bool alphabet = (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '+' || c == '/';
            if (c == '=' && i >= encoded.size() - 2) {
                ++padding;
            } else if (!alphabet || padding != 0) {
                fail(QStringLiteral("Qwen audio delta is not valid Base64."));
                return;
            }
        }
        if (padding != 0) {
            const ushort c = encoded.at(encoded.size() - padding - 1).unicode();
            const int last = c >= 'A' && c <= 'Z' ? c - 'A' :
                c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : 63;
            if ((last & (padding == 2 ? 15 : 3)) != 0) {
                fail(QStringLiteral("Qwen audio delta has noncanonical Base64 padding."));
                return;
            }
        }
        const QByteArray base64 = encoded.toLatin1();
        auto decoded = QByteArray::fromBase64Encoding(
            base64, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() ||
            decoded.decoded.size() > MaxOutputBytes ||
            decoded.decoded.size() % 2 != 0) {
            fail(QStringLiteral("Qwen audio delta is not valid 16-bit PCM Base64."));
            return;
        }
        emit owner->audioReceived(decoded.decoded);
    }

    void receive(const QString &message)
    {
        if (state == State::Closed || state == State::Failed)
            return;
        if (message.size() > static_cast<qsizetype>(MaxMessageBytes)) {
            fail(QStringLiteral("Qwen event exceeded the incoming message limit."));
            return;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("Qwen sent an invalid JSON event."));
            return;
        }
        const QJsonObject event = document.object();
        const QString type = event.value(QStringLiteral("type")).toString();
        if (type.isEmpty()) {
            fail(QStringLiteral("Qwen event is missing its type."));
            return;
        }
        if (type == QStringLiteral("error")) {
            // [debug-point evt-error] 保留服务端错误码，便于区分被拒原因（不含密钥）
            emit owner->diagLog(QStringLiteral("[server error] %1")
                .arg(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))));
            fail(QStringLiteral("Qwen rejected a request or reported a server error."));
            return;
        }
        if (type == QStringLiteral("session.created")) {
            if (state != State::Connecting) {
                fail(QStringLiteral("Qwen sent session.created out of sequence."));
                return;
            }
            state = State::Updating;
            QJsonArray toolsPayload = tools;
            if (toolsPayload.isEmpty())
                toolsPayload.append(hangUpToolJson()); // 兜底：未接入注册表时保留挂机工具
            const QJsonObject session{
                {QStringLiteral("modalities"), QJsonArray{QStringLiteral("text"), QStringLiteral("audio")}},
                {QStringLiteral("voice"), voice},
                {QStringLiteral("instructions"), instructions},
                {QStringLiteral("input_audio_format"), QStringLiteral("pcm")},
                {QStringLiteral("output_audio_format"), QStringLiteral("pcm")},
                {QStringLiteral("input_audio_transcription"), QJsonObject{{QStringLiteral("language"), QStringLiteral("zh")}}},
                {QStringLiteral("output_audio"), QJsonObject{{QStringLiteral("language"), QStringLiteral("zh")}}},
                {QStringLiteral("turn_detection"), QJsonObject{{QStringLiteral("type"), turnMode}}},
                {QStringLiteral("max_history_turns"), maxHistoryTurns},
                {QStringLiteral("tools"), toolsPayload}};
            if (send({{QStringLiteral("type"), QStringLiteral("session.update")},
                      {QStringLiteral("session"), session}}))
                emit owner->statusChanged(QStringLiteral("Configuring Qwen session"));
            return;
        }
        if (type == QStringLiteral("session.updated")) {
            if (state != State::Updating) {
                fail(QStringLiteral("Qwen sent session.updated out of sequence."));
                return;
            }
            deadline.stop();
            state = State::Ready;
            QPointer<QwenSession> guard(owner);
            emit owner->statusChanged(QStringLiteral("Qwen session ready"));
            if (!guard || state != State::Ready)
                return;
            emit owner->ready();
            if (guard)
                sendGreeting();
            return;
        }
        if (state != State::Ready)
            return;
        if (type == QStringLiteral("input_audio_buffer.speech_started")) {
            // [debug-point speech-started]
            emit owner->diagLog(QStringLiteral("[speech_started] activeResp=%1 ending=%2 allowBarge=%3")
                .arg(activeResponse.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes"))
                .arg(endingCall ? 1 : 0).arg(allowBargeIn ? 1 : 0));
            if (endingCall)
                return;
            silenceRetries = 0;
            silenceTimer.stop();
            if (!allowBargeIn && !activeResponse.isEmpty())
                return; //不打断模式：AI 正在说话时不处理用户语音（音频已在 appendAudio 丢弃）
            if (!activeResponse.isEmpty()) {
                if (blockedResponses.size() >= MaxPendingInterruptions) {
                    fail(QStringLiteral("Qwen has too many unfinished interrupted responses."));
                    return;
                }
                blockedResponses.insert(activeResponse);
                activeResponse.clear();
            }
            awaitingUserResponse = true;
            // Always clear local playback, even if response.done already arrived.
            // VAD cancels server generation itself; response.cancel would race it.
            emit owner->interrupted();
            return;
        }
        if (type == QStringLiteral("input_audio_buffer.speech_stopped")) {
            // [debug-point speech-stopped]
            emit owner->diagLog(QStringLiteral("[speech_stopped] reason=%1")
                .arg(event.value(QStringLiteral("reason")).toString()));
            if (event.value(QStringLiteral("reason")).toString() == QStringLiteral("turn_invalid")) {
                awaitingUserResponse = false;
                sendGreeting();
            }
            return;
        }
        if (type == QStringLiteral("input_audio_buffer.committed")) {
            awaitingUserResponse = true;
            return;
        }
        if (type == QStringLiteral("response.created")) {
            const QString id = event.value(QStringLiteral("response")).toObject()
                .value(QStringLiteral("id")).toString();
            // [debug-point response-created]
            emit owner->diagLog(QStringLiteral("[response.created] id=%1 blocked=%2")
                .arg(id, blockedResponses.contains(id) ? QStringLiteral("yes") : QStringLiteral("no")));
            if (id.isEmpty()) {
                fail(QStringLiteral("Qwen response is missing its ID."));
                return;
            }
            if (blockedResponses.contains(id))
                return;
            silenceTimer.stop();
            if (systemResponsePending) {
                //系统发起的响应（静默提示/挂机话术），不计入对话轮次
                systemResponsePending = false;
            } else {
                ++turnCount;
                if (turnCount > maxTurns)
                    endingCall = true;
            }
            activeResponse = id;
            awaitingUserResponse = false;
            responseRequested = false;
            return;
        }
        if (type == QStringLiteral("response.audio.delta")) {
            receiveAudio(event);
            return;
        }
        if (type == QStringLiteral("response.function_call_arguments.done")) {
            //模型调用挂机工具：本轮响应（含道别语音）播完后挂断
            if (isCurrentResponse(event) &&
                event.value(QStringLiteral("name")).toString() == QStringLiteral("hang_up")) {
                hangupAfterResponse = true;
                endingCall = true;
                return;
            }
            // 其他工具（MCP 插件）：上抛引擎取回结果，经 completeToolCall 回传后模型继续作答
            if (!isCurrentResponse(event))
                return;
            const QString callId = event.value(QStringLiteral("call_id")).toString();
            if (callId.isEmpty())
                return; // 缺少 call_id 无法回传结果，只能放弃该轮
            ++pendingToolCalls;
            emit owner->toolCallRequested(callId,
                event.value(QStringLiteral("name")).toString(),
                event.value(QStringLiteral("arguments")).toString());
            return;
        }
        if (type == QStringLiteral("conversation.item.input_audio_transcription.completed")) {
            const QString text = event.value(QStringLiteral("transcript")).toString();
            if (!text.isEmpty())
                emit owner->transcript(QStringLiteral("caller"), text);
            return;
        }
        if (type == QStringLiteral("conversation.item.input_audio_transcription.failed")) {
            fail(QStringLiteral("Qwen could not transcribe the caller's audio."));
            return;
        }
        if (type == QStringLiteral("response.audio_transcript.done") ||
            type == QStringLiteral("response.text.done")) {
            if (!isCurrentResponse(event))
                return;
            const QString text = event.value(type == QStringLiteral("response.text.done")
                ? QStringLiteral("text") : QStringLiteral("transcript")).toString();
            if (!text.isEmpty())
                emit owner->transcript(QStringLiteral("AI"), text);
            return;
        }
        if (type == QStringLiteral("response.done")) {
            const QJsonObject response = event.value(QStringLiteral("response")).toObject();
            const QString id = response.value(QStringLiteral("id")).toString();
            const QString status = response.value(QStringLiteral("status")).toString();
            // [debug-point response-done]
            emit owner->diagLog(QStringLiteral("[response.done] id=%1 status=%2 wasActive=%3 blocked=%4")
                .arg(id, status).arg(id == activeResponse ? 1 : 0)
                .arg(blockedResponses.contains(id) ? 1 : 0));
            if (status == QStringLiteral("failed")) {
                fail(QStringLiteral("Qwen response generation failed."));
                return;
            }
            if (id.isEmpty() || (status != QStringLiteral("completed") &&
                                 status != QStringLiteral("cancelled"))) {
                fail(QStringLiteral("Qwen sent an invalid response completion."));
                return;
            }
            // 累计本次响应的 token 用量并上报（cancelled 的已产出部分也计费）
            usage.accumulate(response.value(QStringLiteral("usage")).toObject());
            if (usage.valid)
                emit owner->usageUpdated(usage);
            const bool wasBlocked = blockedResponses.remove(id);
            const bool wasActive = id == activeResponse;
            if (wasActive)
                activeResponse.clear();
            if (!wasActive && !wasBlocked)
                return;
            // A late cancellation for an old response must not erase new audio.
            QPointer<QwenSession> guard(owner);
            if (status == QStringLiteral("cancelled") &&
                (wasActive || activeResponse.isEmpty()))
                emit owner->interrupted();
            if (guard)
                sendGreeting();
            if (!guard)
                return;
            if (farewellPending) {
                farewellPending = false;
                emit owner->endRequested(QStringLiteral("挂机话术播放完毕"));
                return;
            }
            if (hangupAfterResponse) {
                hangupAfterResponse = false;
                emit owner->endRequested(QStringLiteral("模型请求挂断电话"));
                return;
            }
            if (endingCall) {
                sendFarewell();
                return;
            }
            if (state == State::Ready) {
                pendingSilenceStart = true;
                drainTimer.start();
            }
            if (status == QStringLiteral("completed"))
                emit owner->turnCompleted(turnCount);
        }
    }

    void checkDrainAndStartSilence(bool outputEmpty)
    {
        if (!pendingSilenceStart)
            return;
        if (!outputEmpty) {
            drainTimer.start();
            return;
        }
        pendingSilenceStart = false;
        silenceTimer.start();
    }
};

QwenSession::QwenSession(QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this))
{
}

QwenSession::~QwenSession() = default;

void QwenSession::open(const BotConfig &config, const QJsonArray &tools)
{
    Q_ASSERT(QThread::currentThread() == thread());
    impl_->shutdown();
    impl_->state = Impl::State::Connecting;
    impl_->tools = tools;
    if (!config.endpoint.isValid() || config.endpoint.scheme() != QStringLiteral("wss") ||
        config.endpoint.host().isEmpty() || !config.endpoint.userInfo().isEmpty() ||
        config.endpoint.hasFragment() || config.apiKey.trimmed().isEmpty() ||
        config.apiKey.contains(QLatin1Char('\r')) || config.apiKey.contains(QLatin1Char('\n'))) {
        impl_->fail(QStringLiteral("Qwen requires a valid WSS endpoint and Bearer API key."));
        return;
    }
    if (config.turnMode != QStringLiteral("smart_turn") &&
        config.turnMode != QStringLiteral("server_vad")) {
        impl_->fail(QStringLiteral("Qwen turn mode must be smart_turn or server_vad."));
        return;
    }
    if (!QSslSocket::supportsSsl()) {
        impl_->fail(QStringLiteral("No working TLS backend is available for Qwen WSS."));
        return;
    }
    impl_->voice = config.voice;
    impl_->instructions = config.instructions;
    impl_->greeting = config.greeting;
    impl_->turnMode = config.turnMode;
    impl_->allowBargeIn = config.allowBargeIn;
    impl_->silenceTimeoutMs = config.silenceTimeoutSec * 1000;
    impl_->maxSilenceRetries = config.maxSilenceRetries;
    impl_->maxTurns = config.maxTurns;
    impl_->maxHistoryTurns = config.maxHistoryTurns;
    impl_->silencePrompt = config.silencePrompt;
    impl_->farewellPrompt = config.farewellPrompt;
    impl_->silenceTimer.setInterval(impl_->silenceTimeoutMs);
    impl_->socket = std::make_unique<QWebSocket>();
    auto *socket = impl_->socket.get();
    auto *implementation = impl_.get();
    socket->setMaxAllowedIncomingFrameSize(MaxMessageBytes);
    socket->setMaxAllowedIncomingMessageSize(MaxMessageBytes);
    connect(socket, &QWebSocket::textMessageReceived, implementation,
            [implementation](const QString &message) { implementation->receive(message); });
    connect(socket, &QWebSocket::binaryMessageReceived, implementation,
            [implementation](const QByteArray &) {
        implementation->fail(QStringLiteral("Qwen sent an unexpected binary WebSocket message."));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(socket, &QWebSocket::errorOccurred, implementation,
#else
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), implementation,
#endif
            [implementation, socket](QAbstractSocket::SocketError error) {
        //Qt 的 errorString 含 HTTP 状态行（如 401），不会回显 Authorization 头
        const QString detail = socket->errorString().trimmed();
        if (detail.isEmpty())
            implementation->fail(QStringLiteral("Qwen WebSocket connection failed (network error %1).")
                                 .arg(static_cast<int>(error)));
        else
            implementation->fail(QStringLiteral("Qwen WebSocket connection failed (network error %1): %2")
                                 .arg(static_cast<int>(error)).arg(detail));
    });
    connect(socket, &QWebSocket::sslErrors, implementation,
            [implementation](const QList<QSslError> &) {
        implementation->fail(QStringLiteral("Qwen TLS certificate verification failed."));
    });
    connect(socket, &QWebSocket::disconnected, implementation, [implementation, socket] {
        const QString reason = socket->closeReason();
        const int code = static_cast<int>(socket->closeCode());
        if (reason.isEmpty())
            implementation->fail(QStringLiteral("Qwen WebSocket disconnected unexpectedly (close code %1).").arg(code));
        else
            implementation->fail(QStringLiteral("Qwen WebSocket disconnected unexpectedly (close code %1): %2").arg(code).arg(reason));
    });
    QNetworkRequest request(config.endpoint);
    request.setRawHeader("Authorization", QByteArray("Bearer ") + config.apiKey.toUtf8());
    impl_->deadline.start();
    QPointer<QwenSession> guard(this);
    QPointer<QWebSocket> openingSocket(socket);
    emit statusChanged(QStringLiteral("Connecting to Qwen"));
    if (guard && openingSocket && impl_->socket.get() == openingSocket.data() &&
        impl_->state == Impl::State::Connecting)
        openingSocket->open(request);
}

void QwenSession::close()
{
    Q_ASSERT(QThread::currentThread() == thread());
    const bool wasOpen = impl_->state != Impl::State::Closed;
    impl_->shutdown();
    if (wasOpen)
        emit statusChanged(QStringLiteral("Qwen session closed"));
}

void QwenSession::appendAudio(const QByteArray &pcm16k)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->state != Impl::State::Ready || pcm16k.isEmpty())
        return;
    if (impl_->farewellPending || impl_->hangupAfterResponse)
        return; //挂机话术已注入/挂断工具已调用：不再发送用户音频，保护话术完整播放
    // endingCall 决策后、话术注入前的窗口内仍转发音频，
    // 保证用户最后一句话能被服务端转写进通话记录；话术会在该轮 response.done 后重试发送
    if (!impl_->allowBargeIn &&
        (!impl_->activeResponse.isEmpty() || !impl_->blockedResponses.isEmpty()))
        return; //不打断模式：AI 正在说话时丢弃用户音频，防止服务端 VAD 触发
    if (pcm16k.size() > MaxInputBytes || pcm16k.size() % 2 != 0) {
        impl_->fail(QStringLiteral("Caller audio is not a bounded 16-bit PCM frame."));
        return;
    }
    impl_->send({{QStringLiteral("type"), QStringLiteral("input_audio_buffer.append")},
                 {QStringLiteral("audio"), QString::fromLatin1(pcm16k.toBase64())}});
}

void QwenSession::greet()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->state == Impl::State::Closed || impl_->state == Impl::State::Failed ||
        impl_->greetingSent)
        return;
    impl_->greetingRequested = true;
    impl_->sendGreeting();
}

void QwenSession::checkDrainAndStartSilence(bool outputEmpty)
{
    Q_ASSERT(QThread::currentThread() == thread());
    impl_->checkDrainAndStartSilence(outputEmpty);
}

void QwenSession::completeToolCall(const QString &callId, const QString &outputJson)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto &d = *impl_;
    // 会话已收尾（挂机/道别进行中）时丢弃迟到的工具结果
    if (d.state != Impl::State::Ready || d.endingCall || d.farewellPending ||
        d.hangupAfterResponse || callId.isEmpty())
        return;
    if (d.pendingToolCalls > 0)
        --d.pendingToolCalls;
    const QJsonObject item{
        {QStringLiteral("type"), QStringLiteral("function_call_output")},
        {QStringLiteral("call_id"), callId},
        {QStringLiteral("output"), outputJson}};
    if (!d.send({{QStringLiteral("type"), QStringLiteral("conversation.item.create")},
                 {QStringLiteral("item"), item}}))
        return;
    d.send({{QStringLiteral("type"), QStringLiteral("response.create")}});
}

} // namespace qwenbot
