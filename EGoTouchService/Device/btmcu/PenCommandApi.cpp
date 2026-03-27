#include "btmcu/PenCommandApi.h"
#include "Logger.h"
#include "btmcu/PenSession.h"

namespace Himax::Pen {

PenCommandApi::PenCommandApi() = default;

PenCommandApi::~PenCommandApi() {
    Shutdown();
}

ChipResult<> PenCommandApi::Initialize(const std::wstring& devicePath) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto transport = CreatePenUsbTransportWin32();
    if (!transport) {
        return std::unexpected(ChipError::InternalError);
    }

    if (auto res = transport->Open(devicePath); !res) {
        return res;
    }

    m_transport = std::shared_ptr<IPenUsbTransport>(std::move(transport));
    m_session = std::make_shared<PenSession>(m_transport);
    if (auto res = m_session->Initialize(); !res) {
        m_transport->Close();
        m_transport.reset();
        m_session.reset();
        return res;
    }

    m_client = std::make_unique<PenUsbClient>(m_session);
    m_ready = true;
    LOG_INFO("Device", "PenCommandApi::Initialize", "Ready", "Bluetooth MCU USB session initialized.");
    return {};
}

void PenCommandApi::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_client) {
        m_client->Stop();
    }
    if (m_session) {
        m_session->Shutdown();
    }
    if (m_transport) {
        m_transport->Close();
    }

    m_client.reset();
    m_session.reset();
    m_transport.reset();
    m_ready = false;
}

ChipResult<> PenCommandApi::Start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || !m_client) {
        return std::unexpected(ChipError::InvalidOperation);
    }
    return m_client->Start();
}

void PenCommandApi::Stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client) {
        m_client->Stop();
    }
}

bool PenCommandApi::IsReady() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ready;
}

ChipResult<> PenCommandApi::QueryBootstrapInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || !m_client) {
        return std::unexpected(ChipError::InvalidOperation);
    }
    return m_client->QueryBootstrapInfo();
}

ChipResult<> PenCommandApi::SendEventAck(uint8_t ackCode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || !m_client) {
        return std::unexpected(ChipError::InvalidOperation);
    }
    return m_client->SendEventAck(ackCode);
}

void PenCommandApi::SetEventCallback(PenEventCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session) {
        m_session->SetEventCallback(std::move(callback));
    }
}

} // namespace Himax::Pen
