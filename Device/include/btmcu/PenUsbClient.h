#pragma once

#include "himax/HimaxProtocol.h"
#include "btmcu/PenSession.h"
#include <atomic>
#include <memory>
#include <thread>

namespace Himax::Pen {

class PenUsbClient {
public:
    explicit PenUsbClient(std::shared_ptr<PenSession> session);
    ~PenUsbClient();

    ChipResult<> Start();
    void Stop();
    bool IsRunning() const;

    ChipResult<> QueryBootstrapInfo();
    ChipResult<> SendEventAck(uint8_t ackCode);

private:
    void WorkerLoop();

    std::shared_ptr<PenSession> m_session;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
};

} // namespace Himax::Pen
