#include "pcm_bridge.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace qwenbot {
namespace {
void check(pj_status_t status, const char *operation)
{
    if (status != PJ_SUCCESS) {
        char text[PJ_ERR_MSG_SIZE];
        pj_strerror(status, text, sizeof(text));
        throw std::runtime_error(std::string(operation) + ": " + text);
    }
}

template<size_t Capacity> struct Ring {
    std::array<char, Capacity> data{};
    size_t read = 0, count = 0;
    bool push(const void *src, size_t length) {
        if (length > Capacity - count) return false;
        const size_t end = (read + count) % Capacity;
        const size_t first = std::min(length, Capacity - end);
        std::memcpy(data.data() + end, src, first);
        std::memcpy(data.data(), static_cast<const char *>(src) + first, length - first);
        count += length;
        return true;
    }
    size_t pop(void *dst, size_t length) {
        length = std::min(length, count);
        const size_t first = std::min(length, Capacity - read);
        std::memcpy(dst, data.data() + read, first);
        std::memcpy(static_cast<char *>(dst) + first, data.data(), length - first);
        read = (read + length) % Capacity;
        count -= length;
        return length;
    }
    void clear() { read = count = 0; }
};
}

struct PcmBridge::State {
    std::mutex mutex;
    Ring<32000> input;       // One second at 16 kHz.
    Ring<48000 * 30> output; // Bounded burst allowance at 24 kHz.
    bool active = false;
    bool overflow = false;
    int chunkBytes = 640;    // 上行分片字节数：16kHz×16bit×2B，20ms=640B（可配置至 100ms=3200B）
};

struct PcmBridge::Port {
    pjmedia_port base{};
    pj_pool_t *pool = nullptr;
    std::shared_ptr<State> state;
    bool input = false;
    int slot = PJSUA_INVALID_ID;

    static pj_status_t put(pjmedia_port *base, pjmedia_frame *frame) {
        auto *port = static_cast<Port *>(base->port_data.pdata);
        auto &s = *port->state;
        if (!port->input) return PJ_SUCCESS;
        std::lock_guard<std::mutex> guard(s.mutex);
        if (!s.active) return PJ_SUCCESS;
        if (frame->type == PJMEDIA_FRAME_TYPE_AUDIO && frame->buf && frame->size) {
            if (!s.input.push(frame->buf, frame->size)) s.overflow = true;
        } else {
            // Preserve server VAD's audio clock during RTP silence/DTX.
            static const std::array<char, 640> silence{};
            if (!s.input.push(silence.data(), silence.size())) s.overflow = true;
        }
        return PJ_SUCCESS;
    }
    static pj_status_t get(pjmedia_port *base, pjmedia_frame *frame) {
        auto *port = static_cast<Port *>(base->port_data.pdata);
        auto &s = *port->state;
        std::lock_guard<std::mutex> guard(s.mutex);
        const size_t length = frame->size;
        size_t read = 0;
        if (s.active && !port->input) read = s.output.pop(frame->buf, length);
        std::memset(static_cast<char *>(frame->buf) + read, 0, length - read);
        frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
        return PJ_SUCCESS;
    }
    static pj_status_t destroy(pjmedia_port *base) {
        auto *port = static_cast<Port *>(base->port_data.pdata);
        pj_pool_t *pool = port->pool;
        delete port;
        pj_pool_release(pool);
        return PJ_SUCCESS;
    }
};

PcmBridge::Port *PcmBridge::createPort(const std::shared_ptr<State> &state, bool input)
{
    auto port = std::make_unique<Port>();
    port->pool = pjsua_pool_create(input ? "qwen-in" : "qwen-out", 512, 512);
    if (!port->pool) throw std::bad_alloc();
    port->state = state;
    port->input = input;
    port->base.port_data.pdata = port.get();
    port->base.get_frame = &Port::get;
    port->base.put_frame = &Port::put;
    port->base.on_destroy = &Port::destroy;
    pj_str_t name = pj_str(const_cast<char *>(input ? "qwen-input16k" : "qwen-output24k"));
    const unsigned rate = input ? 16000 : 24000;
    pj_status_t status = pjmedia_port_info_init(&port->base.info, &name,
        PJMEDIA_SIG_CLASS_APP('Q', 'W', 'N'), rate, 1, 16, rate / 50);
    if (status == PJ_SUCCESS)
        status = pjmedia_port_init_grp_lock(&port->base, port->pool, nullptr);
    if (status != PJ_SUCCESS) {
        pj_pool_release(port->pool);
        check(status, "initialize PCM port");
    }
    Port *result = port.release();
    status = pjsua_conf_add_port(result->pool, &result->base, &result->slot);
    if (status != PJ_SUCCESS) {
        pjmedia_port_destroy(&result->base);
        check(status, "register PCM port");
    }
    return result;
}

void PcmBridge::releasePort(Port *port)
{
    if (!port) return;
    pjsua_conf_remove_port(port->slot);
    // Bridge holds a group-lock reference until asynchronous removal completes.
    pjmedia_port_destroy(&port->base);
}

PcmBridge::PcmBridge() : state_(std::make_shared<State>())
{
    input_ = createPort(state_, true);
    try { output_ = createPort(state_, false); }
    catch (...) { releasePort(input_); throw; }
}
PcmBridge::~PcmBridge()
{
    disconnect();
    releasePort(input_);
    releasePort(output_);
}
void PcmBridge::connect(int slot)
{
    if (slot == callSlot_) return;
    disconnect();
    check(pjsua_conf_connect(slot, input_->slot), "connect incoming audio");
    const auto status = pjsua_conf_connect(output_->slot, slot);
    if (status != PJ_SUCCESS) {
        pjsua_conf_disconnect(slot, input_->slot);
        check(status, "connect outgoing audio");
    }
    callSlot_ = slot;
    std::lock_guard<std::mutex> guard(state_->mutex);
    state_->active = true;
}
void PcmBridge::disconnect()
{
    {
        std::lock_guard<std::mutex> guard(state_->mutex);
        state_->active = false;
        state_->input.clear();
        state_->output.clear();
    }
    if (callSlot_ != PJSUA_INVALID_ID) {
        pjsua_conf_disconnect(callSlot_, input_->slot);
        pjsua_conf_disconnect(output_->slot, callSlot_);
        callSlot_ = PJSUA_INVALID_ID;
    }
}
void PcmBridge::setUplinkChunkMs(int chunkMs)
{
    std::lock_guard<std::mutex> guard(state_->mutex);
    state_->chunkBytes = (16000 / 1000) * 2 * chunkMs;
}

QByteArray PcmBridge::takeInput()
{
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (state_->input.count < size_t(state_->chunkBytes)) return {};
    QByteArray frame(state_->chunkBytes, Qt::Uninitialized);
    state_->input.pop(frame.data(), size_t(state_->chunkBytes));
    return frame;
}
bool PcmBridge::appendOutput(const QByteArray &pcm)
{
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (!state_->active) return true;
    if (pcm.size() % 2 || !state_->output.push(pcm.constData(), size_t(pcm.size()))) {
        state_->overflow = true;
        return false;
    }
    return true;
}
void PcmBridge::interrupt()
{
    std::lock_guard<std::mutex> guard(state_->mutex);
    state_->output.clear();
}
bool PcmBridge::overflowed() const
{
    std::lock_guard<std::mutex> guard(state_->mutex);
    return state_->overflow;
}
bool PcmBridge::outputEmpty() const
{
    std::lock_guard<std::mutex> guard(state_->mutex);
    return state_->output.count == 0;
}
} // namespace qwenbot
