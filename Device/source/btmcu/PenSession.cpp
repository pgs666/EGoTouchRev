#include "btmcu/PenSession.h"
#include <chrono>

namespace Himax::Pen {

PenSession::PenSession(std::shared_ptr<IPenUsbTransport> transport)
    : m_transport(std::move(transport)) {}

ChipResult<> PenSession::Initialize() {
    if (!m_transport || !m_transport->IsOpen()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = PenSessionState::Error;
        return std::unexpected(ChipError::InvalidOperation);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = PenSessionState::Running;
    return {};
}

void PenSession::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = PenSessionState::Stopped;
}

ChipResult<> PenSession::SendCommand(PenUsbCommandId commandId,
                                     std::span<const uint8_t> payload,
                                     uint8_t option) {
    if (!m_transport || !m_transport->IsOpen()) {
        return std::unexpected(ChipError::InvalidOperation);
    }

    auto encoded = m_codec.BuildCommand(commandId, payload, option);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }

    auto writeRes = m_transport->WritePacket(*encoded);
    if (!writeRes) {
        return writeRes;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.txPackets;
    return {};
}

ChipResult<> PenSession::PollOnce(uint32_t timeoutMs) {
    if (!m_transport || !m_transport->IsOpen()) {
        return std::unexpected(ChipError::InvalidOperation);
    }

    std::vector<uint8_t> packetBytes;
    auto readRes = m_transport->ReadPacket(packetBytes, timeoutMs);
    if (!readRes) {
        return readRes;
    }

    if (packetBytes.empty()) {
        return std::unexpected(ChipError::Timeout);
    }

    auto packet = m_codec.ParsePacket(packetBytes);
    if (!packet) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.droppedPackets;
        return std::unexpected(packet.error());
    }

    return HandleIncomingPacket(*packet);
}

void PenSession::SetEventCallback(PenEventCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_eventCallback = std::move(callback);
}

PenSessionState PenSession::GetState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

PenSessionStats PenSession::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

ChipResult<> PenSession::HandleIncomingPacket(const PenUsbPacket& packet) {
    PenEventCallback callbackCopy{};
    PenEvent event{};
    bool isEvent = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.rxPackets;

        PenUsbEventCode eventCode = PenUsbEventCode::Unknown;
        if (PenUsbCodec::IsEventPacket(packet, &eventCode)) {
            ++m_stats.eventPackets;
            isEvent = true;
            event.code = eventCode;
            event.payload = packet.payload;
            event.receivedAt = std::chrono::steady_clock::now();
            callbackCopy = m_eventCallback;
        }
    }

    if (isEvent && callbackCopy) {
        callbackCopy(event);
    }

    return {};
}

} // namespace Himax::Pen
