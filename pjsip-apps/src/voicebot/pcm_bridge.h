#pragma once

#include <pjmedia.h>
#include <pjsua-lib/pjsua.h>
#include <QByteArray>
#include <array>
#include <memory>
#include <mutex>

namespace qwenbot {

// Media callbacks only copy PCM into preallocated bounded storage.
class PcmBridge {
public:
    PcmBridge();
    ~PcmBridge();
    void connect(int callSlot);
    void disconnect();
    // chunkMs: 上行分片时长（毫秒），16kHz 16bit 单声道，20ms=640B，100ms=3200B
    void setUplinkChunkMs(int chunkMs);
    QByteArray takeInput();
    bool appendOutput(const QByteArray &pcm);
    void interrupt();
    bool overflowed() const;
    bool outputEmpty() const;
private:
    struct State;
    struct Port;
    std::shared_ptr<State> state_;
    Port *input_ = nullptr;
    Port *output_ = nullptr;
    int callSlot_ = PJSUA_INVALID_ID;
    static Port *createPort(const std::shared_ptr<State> &, bool input);
    static void releasePort(Port *);
};

} // namespace qwenbot
