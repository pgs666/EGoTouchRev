#include "btmcu/PenUsbClient.h"
#include <array>
#include <chrono>

namespace Himax::Pen {

PenUsbClient::PenUsbClient(std::shared_ptr<PenSession> session)
    : m_session(std::move(session)) {}

PenUsbClient::~PenUsbClient() {
    Stop();
}

ChipResult<> PenUsbClient::Start() {
    if (!m_session) {
        return std::unexpected(ChipError::InvalidOperation);
    }
    if (m_running.exchange(true)) {
        return {};
    }

    m_worker = std::thread(&PenUsbClient::WorkerLoop, this);
    return {};
}

void PenUsbClient::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

bool PenUsbClient::IsRunning() const {
    return m_running.load();
}

ChipResult<> PenUsbClient::QueryBootstrapInfo() {
    if (!m_session) {
        return std::unexpected(ChipError::InvalidOperation);
    }

    if (auto res = m_session->SendCommand(PenUsbCommandId::QueryPenStatus); !res) {
        return res;
    }
    return m_session->SendCommand(PenUsbCommandId::QueryPenInfo);
}

ChipResult<> PenUsbClient::SendEventAck(uint8_t ackCode) {
    if (!m_session) {
        return std::unexpected(ChipError::InvalidOperation);
    }

    const std::array<uint8_t, 1> payload{ackCode};
    return m_session->SendCommand(PenUsbCommandId::EventAck, payload);
}

void PenUsbClient::WorkerLoop() {
    while (m_running.load()) {
        if (!m_session) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto res = m_session->PollOnce(10);
        if (!res && res.error() != ChipError::Timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

} // namespace Himax::Pen
