#pragma once

#include "ControlRuntime.h"

#include <atomic>
#include <string>
#include <thread>

namespace Control {

class ControlPipeServer {
public:
    explicit ControlPipeServer(std::string pipe_name);
    ~ControlPipeServer();

    ControlPipeServer(const ControlPipeServer&) = delete;
    ControlPipeServer& operator=(const ControlPipeServer&) = delete;

    bool Start(ControlRuntime* runtime);
    void Stop();

    bool IsStopRequestedByClient() const noexcept { return m_stopRequestedByClient.load(std::memory_order_acquire); }

private:
    void ServerLoop();
    std::string HandleCommand(const std::string& request_line);

private:
    std::string m_pipeName;
    ControlRuntime* m_runtime = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequestedByClient{false};
    std::thread m_thread;
};

} // namespace Control

