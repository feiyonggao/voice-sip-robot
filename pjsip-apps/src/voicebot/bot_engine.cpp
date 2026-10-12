#include "bot_engine.h"
#include "call_record.h"
#include "pcm_bridge.h"
#include "qwen_session.h"
#include "tool_registry.h"
#include <pjsua2.hpp>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <map>

namespace qwenbot {

struct BotEngine::Impl {
    struct Call;
    struct Account final : pj::Account {
        Impl *owner;
        explicit Account(Impl *o) : owner(o) {}
        void onIncomingCall(pj::OnIncomingCallParam &p) override { owner->incoming(p); }
        void onRegState(pj::OnRegStateParam &p) override {
            emit owner->q->statusChanged(QStringLiteral("SIP 注册状态：%1").arg(p.code));
        }
    };
    struct Call final : pj::Call {
        Impl *owner;
        int id;
        QString peer;
        bool closing = false, ready = false, confirmed = false;
        bool opening = false, pendingHangup = false;
        std::unique_ptr<QwenSession> session;
        std::unique_ptr<PcmBridge> bridge;
        QElapsedTimer age;
        CallRecorder *recorder = nullptr; // 指向 Impl 的 recorder，由 BotEngine 拥有
        // 通话记录字段
        QString callId;           // SIP Call-ID 头
        QString callerNumber;     // 主叫号码（From URI 用户部分）
        QString calleeNumber;     // 被叫号码（To URI 用户部分）
        QDateTime startTime;      // 来电时间
        QDateTime answerTime;     // 接听时间（200 OK）
        QDateTime hangupTime;     // 挂断时间
        int turnCount = 0;        // 对话轮次
        bool allowBargeIn = true; // 是否允许打断
        bool answered = false;    // 是否已接听
        QString hangupBy;         // 挂断方：caller/callee
        QString hangupReason;     // 挂断原因
        int sipStatusCode = 0;    // SIP 状态码
        QJsonObject customVars;   // 自定义变量（X-* 头）
        QJsonArray transcriptLog; // 对话记录（逐句转写），挂断时随通话记录写出
        ModelUsage usage;         // 模型 token 用量（session usageUpdated 累计）
        Call(Impl *o, int callId) : pj::Call(*o->account, callId), owner(o), id(callId) {
            age.start();
            startTime = QDateTime::currentDateTime();
        }
        void onCallState(pj::OnCallStateParam &) override {
            try {
                const auto info = getInfo();
                Impl *o = owner;
                const int key = id;
                Call *identity = this;
                if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
                    auto c = o->find(key, identity);
                    if (!c) return;
                    c->closing = true;
                    c->sipStatusCode = info.lastStatusCode;
                    o->clearPendingTools(key);
                    if (c->hangupReason.isEmpty()) {
                        c->hangupReason = QString::fromStdString(info.lastReason);
                    }
                    if (c->hangupBy.isEmpty()) {
                        // 根据 SIP 状态码推断挂断方
                        c->hangupBy = (info.lastStatusCode == PJSIP_SC_OK || info.lastStatusCode == PJSIP_SC_DECLINE)
                            ? QStringLiteral("callee") : QStringLiteral("caller");
                    }
                    if (c->session) {
                        c->session->close();
                        c->session.release()->deleteLater();
                    }
                    c->bridge.reset();
                    emit o->q->callChanged(key, c->peer, QStringLiteral("DISCONNECTED"));
                    emit o->q->callEnded(key, c->sipStatusCode, c->hangupBy, c->hangupReason);
                    o->calls.erase(key);
                    // PJSUA2 permits destruction in this final callback.
                    return;
                }
                QMetaObject::invokeMethod(o->q, [o, key, identity, info] {
                    auto c = o->find(key, identity);
                    if (!c) return;
                    emit o->q->callChanged(key, c->peer, QString::fromStdString(info.stateText));
                    if (info.state == PJSIP_INV_STATE_CONFIRMED) {
                        c->confirmed = true;
                        o->attach(c);
                    }
                }, Qt::QueuedConnection);
            } catch (const pj::Error &) {}
        }
        void onCallMediaState(pj::OnCallMediaStateParam &) override {
            Impl *o = owner;
            const int key = id;
            Call *identity = this;
            QMetaObject::invokeMethod(o->q, [o, key, identity] {
                if (auto c = o->find(key, identity)) o->attach(c);
            }, Qt::QueuedConnection);
        }
    };

    BotEngine *q;
    BotConfig config;
    pj::Endpoint endpoint;
    std::unique_ptr<Account> account;
    std::map<int, std::shared_ptr<Call>> calls;
    QTimer pump;
    bool created = false, running = false, stopping = false;
    CallRecorder *recorder = nullptr;
    ToolRegistry *registry = nullptr; // 主线程对象，由外部注入
    QMetaObject::Connection registryConnection;
    // 工具调用令牌 → (通话 id, Qwen call_id)
    std::map<int, std::pair<int, QString>> pendingToolTokens;
    explicit Impl(BotEngine *object) : q(object) {
        pump.setInterval(10);
        pump.setTimerType(Qt::PreciseTimer);
        QObject::connect(&pump, &QTimer::timeout, q, [this] { tick(); });
        recorder = new CallRecorder(object);
        QObject::connect(recorder, &CallRecorder::logMessage, object, &BotEngine::logMessage);
    }
    std::shared_ptr<Call> find(int id, Call *identity) {
        const auto it = calls.find(id);
        return it != calls.end() && it->second.get() == identity ? it->second : nullptr;
    }
    void report(const QString &text) { emit q->logMessage(text); }
    void initRecorder() {
        if (recorder) recorder->init(config);
    }
    // 通话结束时清理其挂起的工具调用令牌，防止 PJSUA 复用 call id 后误投递
    void clearPendingTools(int callKey) {
        for (auto it = pendingToolTokens.begin(); it != pendingToolTokens.end();) {
            if (it->second.first == callKey) it = pendingToolTokens.erase(it);
            else ++it;
        }
    }
    // MCP 工具结果到达：找到对应通话并回传给模型继续作答
    void toolResultArrived(int token, const QString &name, const QString &outputJson) {
        const auto it = pendingToolTokens.find(token);
        if (it == pendingToolTokens.end()) return;
        const int callKey = it->second.first;
        const QString qwenCallId = it->second.second;
        pendingToolTokens.erase(it);
        const auto cit = calls.find(callKey);
        if (cit == calls.end() || !cit->second->session || cit->second->closing) return;
        report(QStringLiteral("通话 %1：工具 %2 返回结果。").arg(callKey).arg(name));
        cit->second->session->completeToolCall(qwenCallId, outputJson);
    }
    // 模型请求调用非内置工具：解析参数后转发注册表（MCP 插件）
    void handleToolCall(Call &c, const QString &qwenCallId, const QString &name,
                        const QString &argumentsText) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(argumentsText.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            report(QStringLiteral("通话 %1：工具 %2 的参数不是有效 JSON 对象。").arg(c.id).arg(name));
            c.session->completeToolCall(qwenCallId, QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QStringLiteral("工具参数不是有效的 JSON 对象")}})
                    .toJson(QJsonDocument::Compact)));
            return;
        }
        if (!registry) {
            c.session->completeToolCall(qwenCallId, QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QStringLiteral("工具服务未启用")}})
                    .toJson(QJsonDocument::Compact)));
            return;
        }
        const int token = registry->callMcpTool(name, document.object());
        if (token < 0) {
            report(QStringLiteral("通话 %1：工具 %2 不可用（未注册或插件离线）。").arg(c.id).arg(name));
            c.session->completeToolCall(qwenCallId, QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QStringLiteral("该工具当前不可用，请向用户致歉并改用其他方式帮助")}})
                    .toJson(QJsonDocument::Compact)));
            return;
        }
        pendingToolTokens.emplace(token, std::make_pair(c.id, qwenCallId));
        report(QStringLiteral("通话 %1：调用工具 %2（插件执行中，10 秒超时）。").arg(c.id).arg(name));
    }
    QString extractUserFromUri(const std::string &uri) {
        // 从 SIP URI 提取用户部分：sip:1000@192.168.1.1 -> 1000
        const QString s = QString::fromStdString(uri);
        static const QRegularExpression re(QStringLiteral("sip:([^@;>]+)"));
        const auto match = re.match(s);
        return match.hasMatch() ? match.captured(1) : s;
    }
    void parseCustomHeaders(const pjsip_rx_data *rdata, Call &call) {
        // 解析 X-* 自定义头，支持配置通配符匹配（如 X-Custom-*）
        if (!rdata || !rdata->msg_info.msg) return;
        const pjsip_msg *msg = rdata->msg_info.msg;
        const pjsip_hdr *hdr = msg->hdr.next;
        const pjsip_hdr *end = &msg->hdr;
        while (hdr != end) {
            const pj_str_t *name = &hdr->name;
            const QString headerName = QString::fromLatin1(name->ptr, int(name->slen));
            if (headerName.startsWith(QStringLiteral("X-"), Qt::CaseInsensitive)) {
                bool matched = false;
                for (const auto &pattern : config.customVariables) {
                    if (pattern.endsWith(QLatin1Char('*'))) {
                        const QString prefix = pattern.left(pattern.size() - 1);
                        if (headerName.startsWith(prefix, Qt::CaseInsensitive)) { matched = true; break; }
                    } else if (headerName.compare(pattern, Qt::CaseInsensitive) == 0) {
                        matched = true; break;
                    }
                }
                if (matched) {
                    char buf[1024];
                    const int len = pjsip_hdr_print_on(const_cast<pjsip_hdr*>(hdr), buf, sizeof(buf));
                    if (len > 0) {
                        // buf 格式为 "Header-Name: value"；值按 UTF-8 解码以支持中文头
                        const QString line = QString::fromUtf8(buf, len);
                        const int colon = line.indexOf(QLatin1Char(':'));
                        if (colon > 0) {
                            const QString value = line.mid(colon + 1).trimmed();
                            call.customVars[headerName] = value;
                        }
                    }
                }
            }
            hdr = hdr->next;
        }
    }
    // 将 {{X-Custom-Name}} 占位符替换为对应 SIP 头值（头名不区分大小写），
    // 未命中的变量记入 missing，由调用方上报。
    QString expandTemplate(const QString &text, const QJsonObject &vars, QStringList &missing) const {
        if (!text.contains(QLatin1String("{{"))) return text;
        static const QRegularExpression re(QStringLiteral("\\{\\{\\s*([^{}]+?)\\s*\\}\\}"));
        QString result;
        qsizetype pos = 0;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const auto m = it.next();
            result += text.mid(pos, m.capturedStart() - pos);
            const QString name = m.captured(1).trimmed();
            bool found = false;
            for (auto vit = vars.constBegin(); vit != vars.constEnd(); ++vit) {
                if (vit.key().compare(name, Qt::CaseInsensitive) == 0) {
                    result += vit.value().toString();
                    found = true;
                    break;
                }
            }
            if (!found && !missing.contains(name)) missing.append(name);
            pos = m.capturedEnd();
        }
        result += text.mid(pos);
        return result;
    }
    void writeCallRecord(const std::shared_ptr<Call> &c) {
        if (!c->recorder) return;
        CallRecord record;
        record.callId = c->callId.isEmpty() ? QString::number(c->id) : c->callId;
        record.caller = c->callerNumber;
        record.callee = c->calleeNumber;
        record.voice = config.voice;
        record.startTime = c->startTime;
        record.answerTime = c->answerTime;
        record.hangupTime = c->hangupTime.isValid() ? c->hangupTime : QDateTime::currentDateTime();
        record.answered = c->answered;
        record.allowBargeIn = c->allowBargeIn;
        record.turns = c->turnCount;
        record.hangupBy = c->hangupBy.isEmpty() ? QStringLiteral("caller") : c->hangupBy;
        record.hangupReason = c->hangupReason;
        record.sipStatusCode = c->sipStatusCode;
        record.customVars = c->customVars;
        record.transcript = c->transcriptLog;
        record.usage = c->usage.toJson();
        if (c->answered && c->answerTime.isValid()) {
            record.durationMs = c->answerTime.msecsTo(record.hangupTime);
        }
        record.callDurationMs = c->startTime.msecsTo(record.hangupTime);
        c->recorder->write(record);
    }
    void fail(const std::shared_ptr<Call> &c, const QString &text) {
        if (c->closing) return;
        report(QStringLiteral("通话 %1：%2").arg(c->id).arg(text));
        end(c, PJSIP_SC_BAD_GATEWAY);
    }
    void end(std::shared_ptr<Call> c, pjsip_status_code code = PJSIP_SC_DECLINE) {
        if (c->closing) return;
        c->closing = true;
        c->hangupTime = QDateTime::currentDateTime();
        clearPendingTools(c->id);
        if (c->session) c->session->close();
        c->bridge.reset();
        try { pj::CallOpParam p; p.statusCode = code; c->hangup(p); }
        catch (const pj::Error &e) { report(QString::fromStdString(e.info())); }
        // 通话结束后写入记录（仅当已接听或明确拒绝时）
        if (c->answered || code != PJSIP_SC_RINGING)
            writeCallRecord(c);
    }
    void incoming(pj::OnIncomingCallParam &p) {
        try {
            auto c = std::make_shared<Call>(this, p.callId);
            c->recorder = recorder;
            c->allowBargeIn = config.allowBargeIn;
            // Source address comes from the transport, never from From/Contact.
            const auto *rx = static_cast<const pjsip_rx_data *>(p.rdata.pjRxData);
            char host[PJ_INET6_ADDRSTRLEN] = {};
            if (rx) pj_sockaddr_print(&rx->pkt_info.src_addr, host, sizeof(host), 0);
            const QHostAddress source(QString::fromLatin1(host));
            bool allowed = false;
            if (!source.isNull()) {
                for (const auto &peer : config.allowedPeers) {
                    // 条目支持 IPv4 或 CIDR 网段；手工掩码匹配，兼容 Qt 6.4–6.8
                    const QStringList parts = peer.split('/');
                    const QHostAddress subnet(parts.value(0).trimmed());
                    if (subnet.isNull() || subnet.protocol() != QAbstractSocket::IPv4Protocol ||
                        source.protocol() != QAbstractSocket::IPv4Protocol)
                        continue;
                    int prefix = 32;
                    if (parts.size() == 2) {
                        bool ok = false;
                        prefix = parts.value(1).trimmed().toInt(&ok);
                        if (!ok || prefix < 0 || prefix > 32) continue;
                    }
                    const quint32 s = source.toIPv4Address();
                    const quint32 n = subnet.toIPv4Address();
                    const quint32 mask = prefix == 0 ? 0u : prefix >= 32 ? 0xFFFFFFFFu
                                                                        : ~((1u << (32 - prefix)) - 1);
                    allowed = allowed || ((s & mask) == (n & mask));
                }
            }
            const auto info = c->getInfo();
            c->peer = QString::fromStdString(info.remoteUri);
            c->callerNumber = extractUserFromUri(info.remoteUri);
            c->calleeNumber = extractUserFromUri(info.localUri);
            c->callId = QString::fromStdString(info.callIdString);
            // 解析 X-* 自定义头
            if (rx) parseCustomHeaders(rx, *c);
            calls.emplace(p.callId, c);
            emit q->callChanged(c->id, c->peer, QStringLiteral("INCOMING"));
            if (!allowed || stopping || !running) {
                report(QStringLiteral("拒绝未授权来源：%1").arg(QString::fromLatin1(host)));
                c->hangupReason = QStringLiteral("unauthorized_source");
                end(c, PJSIP_SC_FORBIDDEN);
                return;
            }
            pj::CallOpParam ringing;
            ringing.statusCode = PJSIP_SC_RINGING;
            c->answer(ringing);
            if (config.autoAnswer) open(c);
        } catch (const pj::Error &e) { report(QString::fromStdString(e.info())); }
          catch (const std::exception &e) { report(QString::fromUtf8(e.what())); }
    }
    void open(const std::shared_ptr<Call> &c) {
        if (c->closing || c->opening) return;
        c->opening = true;
        c->session = std::make_unique<QwenSession>();
        const std::weak_ptr<Call> weak = c;
        QObject::connect(c->session.get(), &QwenSession::ready, q, [this, weak] {
            if (auto call = weak.lock(); call && !call->closing) {
                call->ready = true;
                call->answered = true;
                call->answerTime = QDateTime::currentDateTime();
                try {
                    pj::CallOpParam answer;
                    answer.statusCode = PJSIP_SC_OK;
                    answer.opt.audioCount = 1;
                    answer.opt.videoCount = 0;
                    call->answer(answer);
                } catch (const pj::Error &e) { fail(call, QString::fromStdString(e.info())); }
            }
        });
        QObject::connect(c->session.get(), &QwenSession::audioReceived, q, [this, weak](const QByteArray &pcm) {
            if (auto call = weak.lock(); call && !call->closing && call->bridge) {
                if (!call->bridge->appendOutput(pcm)) fail(call, QStringLiteral("下行音频缓冲溢出，结束通话。"));
            }
        });
        QObject::connect(c->session.get(), &QwenSession::interrupted, q, [weak] {
            if (auto call = weak.lock(); call && call->bridge) call->bridge->interrupt();
        });
        QObject::connect(c->session.get(), &QwenSession::transcript, q,
            [this, weak](const QString &speaker, const QString &text) {
                if (auto call = weak.lock(); call && !call->closing) {
                    // 同步收集对话记录（元素与 CallRecord::transcript 约定一致）
                    QJsonObject entry;
                    entry[QStringLiteral("time")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
                    entry[QStringLiteral("speaker")] = speaker;
                    entry[QStringLiteral("text")] = text;
                    call->transcriptLog.append(entry);
                    emit q->transcript(call->id, speaker, text);
                }
            });
        QObject::connect(c->session.get(), &QwenSession::statusChanged, q, [this, weak](const QString &text) {
            if (auto call = weak.lock(); call && !call->closing)
                report(QStringLiteral("通话 %1：%2").arg(call->id).arg(text));
        });
        QObject::connect(c->session.get(), &QwenSession::failed, q, [this, weak](const QString &text) {
            if (auto call = weak.lock()) fail(call, text);
        });
        QObject::connect(c->session.get(), &QwenSession::endRequested, q,
            [this, weak](const QString &reason) {
                if (auto call = weak.lock(); call && !call->closing) {
                    report(QStringLiteral("通话 %1：%2，等待挂机话术播放完成").arg(call->id).arg(reason));
                    call->hangupBy = QStringLiteral("callee");
                    call->hangupReason = reason;
                    call->pendingHangup = true;
                    if (call->session) call->session->close();
                }
            });
        QObject::connect(c->session.get(), &QwenSession::turnCompleted, q,
            [this, weak](int count) {
                if (auto call = weak.lock(); call) {
                    call->turnCount = count;
                    emit q->callTurnsUpdated(call->id, count);
                }
            });
        // token 用量：保存到通话并转发界面实时显示
        QObject::connect(c->session.get(), &QwenSession::usageUpdated, q,
            [this, weak](const ModelUsage &usage) {
                if (auto call = weak.lock(); call) {
                    call->usage = usage;
                    emit q->callUsageUpdated(call->id, usage);
                }
            });
        // 模型调用非内置工具：路由到 MCP 插件，结果经 completeToolCall 回传
        QObject::connect(c->session.get(), &QwenSession::toolCallRequested, q,
            [this, weak](const QString &qwenCallId, const QString &name, const QString &arguments) {
                if (auto call = weak.lock(); call && !call->closing && call->session)
                    handleToolCall(*call, qwenCallId, name, arguments);
            });
        // 每通电话独立提示词：系统指令/开场白中的 {{X-Custom-*}} 替换为来话自定义头值
        BotConfig callConfig = config;
        QStringList missingVars;
        callConfig.instructions = expandTemplate(config.instructions, c->customVars, missingVars);
        callConfig.greeting = expandTemplate(config.greeting, c->customVars, missingVars);
        if (!missingVars.isEmpty())
            report(QStringLiteral("通话 %1：提示词变量未由来话提供，已替换为空：%2")
                       .arg(c->id).arg(missingVars.join(QStringLiteral(", "))));
        // 本次通话锁定的工具集：打开会话前从注册表取快照（热插拔不影响进行中的通话）
        const QJsonArray tools = registry ? registry->toolsJson() : QJsonArray();
        c->session->open(callConfig, tools);
    }
    void attach(const std::shared_ptr<Call> &c) {
        if (c->closing || !c->ready || !c->confirmed) return;
        try {
            auto info = c->getInfo();
            for (const auto &media : info.media) {
                if (media.type != PJMEDIA_TYPE_AUDIO || media.status != PJSUA_CALL_MEDIA_ACTIVE) continue;
                const auto audio = c->getAudioMedia(int(media.index));
                if (!c->bridge) {
                    c->bridge = std::make_unique<PcmBridge>();
                    c->bridge->setUplinkChunkMs(config.uplinkChunkMs);
                }
                c->bridge->connect(audio.getPortId());
                c->session->greet();
                return;
            }
            if (c->bridge) c->bridge->disconnect();
        } catch (const pj::Error &e) { fail(c, QString::fromStdString(e.info())); }
          catch (const std::exception &e) { fail(c, QString::fromUtf8(e.what())); }
    }
    void tick() {
        if (!created || stopping) return;
        try { endpoint.libHandleEvents(0); }
        catch (const pj::Error &e) { report(QString::fromStdString(e.info())); }
        for (auto it = calls.begin(); it != calls.end();) {
            auto c = (it++)->second;
            if (c->closing) continue;
            if (c->pendingHangup) {
                //等待挂机话术 PCM 输出缓冲排空后再挂断，避免话术被截断
                if (!c->bridge || c->bridge->outputEmpty()) {
                    report(QStringLiteral("通话 %1：挂机话术播放完成，挂断。").arg(c->id));
                    end(c);
                }
                continue;
            }
            if (!c->confirmed && c->age.elapsed() > 45000) {
                end(c, PJSIP_SC_REQUEST_TIMEOUT);
                continue;
            }
            if (!c->bridge || !c->ready) continue;
            // 每次 tick 检查是否需要延迟启动无声计时（等 TTS 缓冲排空）
            c->session->checkDrainAndStartSilence(c->bridge->outputEmpty());
            if (c->bridge->overflowed()) { fail(c, QStringLiteral("音频处理速度不足，结束通话。")); continue; }
            for (int n = 0; n < 5 && !c->closing; ++n) {
                const auto pcm = c->bridge->takeInput();
                if (pcm.isEmpty()) break;
                c->session->appendAudio(pcm);
            }
        }
    }
};

BotEngine::BotEngine(QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
BotEngine::~BotEngine() { stop(); }
bool BotEngine::isRunning() const { return impl_->running; }
bool BotEngine::start(const BotConfig &config)
{
    auto &d = *impl_;
    if (d.running) return true;
    const auto error = config.validate();
    if (!error.isEmpty()) { emit logMessage(error); emit statusChanged(error); return false; }
    d.config = config;
    d.initRecorder();
    try {
        d.endpoint.libCreate();
        d.created = true;
        pj::EpConfig ep;
        ep.uaConfig.threadCnt = 0;
        ep.uaConfig.mainThreadOnly = true;
        ep.uaConfig.maxCalls = unsigned(config.maxCalls);
        ep.medConfig.clockRate = 24000;
        ep.medConfig.sndClockRate = 24000;
        ep.medConfig.channelCount = 1;
        ep.medConfig.audioFramePtime = 20;
        ep.medConfig.maxMediaPorts = unsigned(config.maxCalls * 3 + 8);
        ep.medConfig.noVad = true;
        // Native parse-error logs can dump SIP Authorization even with
        // msgLogging disabled. Report checked API errors through the UI only.
        ep.logConfig.level = 0;
        ep.logConfig.consoleLevel = 0;
        ep.logConfig.msgLogging = false;
        d.endpoint.libInit(ep);
        pj::TransportConfig transport;
        transport.port = config.sipPort;
        transport.boundAddress = config.listenAddress.toStdString();
        const int transportId = d.endpoint.transportCreate(PJSIP_TRANSPORT_UDP, transport);
        d.endpoint.libStart();
        d.endpoint.audDevManager().setNullDev();
        for (const auto &codec : d.endpoint.codecEnum2())
            d.endpoint.codecSetPriority(codec.codecId, codec.codecId == "PCMA/8000/1" ? 255 :
                codec.codecId == "PCMU/8000/1" ? 254 : 0);
        pj::AccountConfig account;
        account.idUri = config.sipIdentity.toStdString();
        account.sipConfig.transportId = transportId;
        account.regConfig.registrarUri = config.registrar.toStdString();
        account.regConfig.registerOnAdd = !config.registrar.isEmpty();
        if (!config.sipUser.isEmpty())
            account.sipConfig.authCreds.emplace_back("digest", "*", config.sipUser.toStdString(),
                0, config.sipPassword.toStdString());
        account.mediaConfig.transportConfig.port = config.rtpPort;
        account.mediaConfig.transportConfig.portRange = unsigned(config.maxCalls * 2 - 1);
        account.mediaConfig.transportConfig.boundAddress = config.listenAddress.toStdString();
        account.videoConfig.autoShowIncoming = false;
        account.videoConfig.autoTransmitOutgoing = false;
        d.account = std::make_unique<Impl::Account>(&d);
        d.account->create(account, true);
        d.running = true;
        d.pump.start();
        emit runningChanged(true);
        emit statusChanged(QStringLiteral("监听 SIP UDP %1:%2").arg(config.listenAddress).arg(config.sipPort));
        return true;
    } catch (const pj::Error &e) { emit logMessage(QString::fromStdString(e.info())); }
      catch (const std::exception &e) { emit logMessage(QString::fromUtf8(e.what())); }
    stop();
    emit statusChanged(QStringLiteral("启动失败，请查看日志。"));
    return false;
}
void BotEngine::stop()
{
    auto &d = *impl_;
    if (!d.created || d.stopping) return;
    d.stopping = true;
    d.pump.stop();
    for (auto it = d.calls.begin(); it != d.calls.end();) {
        auto c = (it++)->second;
        d.end(c);
    }
    // No Qt event pumping here: queued callbacks see an empty map afterwards.
    try {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 150) d.endpoint.libHandleEvents(10);
    } catch (const pj::Error &) {}
    d.calls.clear();
    if (d.account) {
        try { d.account->shutdown(); } catch (const pj::Error &) {}
        d.account.reset();
    }
    try { d.endpoint.libDestroy(); }
    catch (const pj::Error &e) { emit logMessage(QString::fromStdString(e.info())); }
    d.created = d.running = d.stopping = false;
    d.config.apiKey.clear();
    d.config.sipPassword.clear();
    emit runningChanged(false);
    emit statusChanged(QStringLiteral("已停止"));
}
void BotEngine::answer(int id)
{
    auto &d = *impl_;
    if (const auto it = d.calls.find(id); it != d.calls.end()) d.open(it->second);
}
void BotEngine::hangup(int id)
{
    auto &d = *impl_;
    if (const auto it = d.calls.find(id); it != d.calls.end()) d.end(it->second);
}
void BotEngine::setToolRegistry(ToolRegistry *registry)
{
    auto &d = *impl_;
    if (d.registryConnection)
        QObject::disconnect(d.registryConnection);
    d.registry = registry;
    d.pendingToolTokens.clear();
    if (registry)
        d.registryConnection = QObject::connect(registry, &ToolRegistry::toolResult, d.q,
            [&d](int token, const QString &name, const QString &outputJson) {
                d.toolResultArrived(token, name, outputJson);
            });
}
} // namespace qwenbot
