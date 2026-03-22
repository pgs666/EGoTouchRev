#include "ControlPipeServer.h"
#include "ControlRuntime.h"
#include "Logger.h"
#include "SystemStateMonitor.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <windows.h>

namespace {

constexpr const char* kPipeName = R"(\\.\pipe\EGoTouchControlService)";
std::atomic<bool> g_stopRequested{false};

BOOL WINAPI CtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int main() {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    Common::Logger::Init("EGoTouchControlService");

    LOG_INFO("Control", "ControlServiceMain::main", "Boot", "Starting control service host.");

    Control::ControlRuntime runtime;
    if (!runtime.Start()) {
        LOG_ERROR("Control", "ControlServiceMain::main", "Boot", "Failed to start runtime.");
        Common::Logger::Shutdown();
        return 1;
    }

    Host::SystemStateMonitor monitor;
    const bool monitorStarted = monitor.Start([&runtime](const Host::SystemStateEvent& event) {
        runtime.IngestSystemEvent(event);
    });

    if (!monitorStarted) {
        LOG_WARN("Control", "ControlServiceMain::main", "Boot", "SystemStateMonitor start failed; running with pipe-only control.");
    }

    Control::ControlPipeServer pipe_server(kPipeName);
    if (!pipe_server.Start(&runtime)) {
        LOG_ERROR("Control", "ControlServiceMain::main", "Boot", "Failed to start pipe server.");
        if (monitorStarted) {
            monitor.Stop();
        }
        runtime.Stop();
        Common::Logger::Shutdown();
        return 2;
    }

    LOG_INFO("Control", "ControlServiceMain::main", "Running", "Control service ready on {}.", kPipeName);

    while (!g_stopRequested.load(std::memory_order_acquire) &&
           !runtime.IsShutdownRequested() &&
           !pipe_server.IsStopRequestedByClient()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    pipe_server.Stop();
    if (monitorStarted) {
        monitor.Stop();
    }
    runtime.Stop();
    LOG_INFO("Control", "ControlServiceMain::main", "Stopped", "Control service host exited.");
    Common::Logger::Shutdown();
    return 0;
}

