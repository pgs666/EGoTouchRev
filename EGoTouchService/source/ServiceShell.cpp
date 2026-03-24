#include "ServiceShell.h"
#include "Logger.h"

#include <string_view>

namespace Service {

static ServiceShell s_instance;

ServiceShell* ServiceShell::Instance() {
    return &s_instance;
}

// ─── SCM 模式 ────────────────────────────────

void WINAPI ServiceShell::SvcMain(DWORD argc, LPWSTR* argv) {
    auto* s = Instance();

    s->m_statusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName, SvcCtrlHandlerEx, s);
    if (!s->m_statusHandle) {
        LOG_ERROR("Shell", "SvcMain", "Boot",
                  "RegisterServiceCtrlHandlerExW failed.");
        return;
    }

    s->ReportStatus(SERVICE_START_PENDING, 3000);
    s->m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    LOG_INFO("Shell", "SvcMain", "Boot",
             "Starting modules...");
    if (!s->m_host.Start()) {
        LOG_ERROR("Shell", "SvcMain", "Boot",
                  "ServiceHost::Start() failed.");
        s->ReportStatus(SERVICE_STOPPED);
        return;
    }

    s->ReportStatus(SERVICE_RUNNING);
    LOG_INFO("Shell", "SvcMain", "Running",
             "Service is running. Waiting for stop signal...");
    s->WaitForStop();
    s->m_host.Stop();
    s->ReportStatus(SERVICE_STOPPED);
    LOG_INFO("Shell", "SvcMain", "Stopped", "Service stopped.");
}

DWORD WINAPI ServiceShell::SvcCtrlHandlerEx(
        DWORD ctrl, DWORD, LPVOID, LPVOID ctx) {
    auto* s = static_cast<ServiceShell*>(ctx);
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        LOG_INFO("Shell", "CtrlHandler", "Stopping",
                 "Received stop/shutdown control code={}.", ctrl);
        s->ReportStatus(SERVICE_STOP_PENDING, 5000);
        SetEvent(s->m_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// ─── 控制台模式 ──────────────────────────────

void ServiceShell::RunAsConsole() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        SetEvent(Instance()->m_stopEvent);
        return TRUE;
    }, TRUE);

    LOG_INFO("Shell", "RunAsConsole", "Boot",
             "Starting modules (console mode)...");
    if (!m_host.Start()) {
        LOG_ERROR("Shell", "RunAsConsole", "Boot",
                  "ServiceHost::Start() failed.");
        return;
    }

    LOG_INFO("Shell", "RunAsConsole", "Running",
             "Service running in console mode. Press Ctrl+C to stop.");
    WaitForStop();
    m_host.Stop();
    LOG_INFO("Shell", "RunAsConsole", "Stopped", "Console mode stopped.");
}

// ─── 辅助 ────────────────────────────────────

void ServiceShell::ReportStatus(DWORD state, DWORD waitHint) {
    if (!m_statusHandle) return;

    m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_status.dwCurrentState = state;
    m_status.dwWin32ExitCode = NO_ERROR;
    m_status.dwWaitHint = waitHint;

    if (state == SERVICE_START_PENDING) {
        m_status.dwControlsAccepted = 0;
    } else {
        m_status.dwControlsAccepted =
            SERVICE_ACCEPT_STOP |
            SERVICE_ACCEPT_SHUTDOWN |
            SERVICE_ACCEPT_PRESHUTDOWN;
    }

    static DWORD checkPoint = 1;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        m_status.dwCheckPoint = 0;
    } else {
        m_status.dwCheckPoint = checkPoint++;
    }

    SetServiceStatus(m_statusHandle, &m_status);
}

void ServiceShell::WaitForStop() {
    if (m_stopEvent) {
        WaitForSingleObject(m_stopEvent, INFINITE);
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

} // namespace Service

