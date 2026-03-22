#pragma once

#include "himax/HimaxProtocol.h"
#include "btmcu/PenUsbCodec.h"
#include "btmcu/PenUsbTransport.h"
#include "btmcu/PenUsbTypes.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace Himax::Pen {

struct PenSessionStats {
    uint64_t rxPackets = 0;
    uint64_t txPackets = 0;
    uint64_t droppedPackets = 0;
    uint64_t eventPackets = 0;
};

class PenSession {
public:
    explicit PenSession(std::shared_ptr<IPenUsbTransport> transport);

    ChipResult<> Initialize();
    void Shutdown();

    ChipResult<> SendCommand(PenUsbCommandId commandId,
                             std::span<const uint8_t> payload = {},
                             uint8_t option = 0x00);

    ChipResult<> PollOnce(uint32_t timeoutMs = 10);

    void SetEventCallback(PenEventCallback callback);
    PenSessionState GetState() const;
    PenSessionStats GetStats() const;

private:
    ChipResult<> HandleIncomingPacket(const PenUsbPacket& packet);

    std::shared_ptr<IPenUsbTransport> m_transport;
    PenUsbCodec m_codec;

    mutable std::mutex m_mutex;
    PenSessionState m_state{PenSessionState::Stopped};
    PenSessionStats m_stats{};
    PenEventCallback m_eventCallback{};
};

} // namespace Himax::Pen
