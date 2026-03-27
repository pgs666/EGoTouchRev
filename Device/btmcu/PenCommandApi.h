#pragma once

#include "btmcu/PenSession.h"
#include "btmcu/PenUsbClient.h"
#include "btmcu/PenUsbTransport.h"
#include "btmcu/PenUsbTypes.h"
#include <memory>
#include <mutex>
#include <string>

namespace Himax::Pen {

class PenCommandApi {
public:
    PenCommandApi();
    ~PenCommandApi();

    ChipResult<> Initialize(const std::wstring& devicePath);
    void Shutdown();

    ChipResult<> Start();
    void Stop();
    bool IsReady() const;

    ChipResult<> QueryBootstrapInfo();
    ChipResult<> SendEventAck(uint8_t ackCode);

    void SetEventCallback(PenEventCallback callback);

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<IPenUsbTransport> m_transport;
    std::shared_ptr<PenSession> m_session;
    std::unique_ptr<PenUsbClient> m_client;
    bool m_ready{false};
};

} // namespace Himax::Pen
