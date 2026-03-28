/// EGoTouchService 入口点
/// 双模式启动：Windows SCM 服务 或 --console 控制台调试

#include "ServiceShell.h"
#include "Logger.h"
#include "GuiLogSink.h"

#include <string_view>

int wmain(int argc, wchar_t* argv[]) {
    // Hide console window — logs are forwarded to App via IPC GetLogs
    if (HWND hw = GetConsoleWindow()) ShowWindow(hw, SW_HIDE);

    Common::Logger::Init("EGoTouchService", "C:/ProgramData/EGoTouchRev/logs/",
                          Common::GuiLogSink::Instance());

    const bool consoleMode =
        (argc >= 2 && std::wstring_view(argv[1]) == L"--console");

    if (consoleMode) {
        Service::ServiceShell::Instance()->RunAsConsole();
    } else {
        SERVICE_TABLE_ENTRYW table[] = {
            { const_cast<wchar_t*>(Service::kServiceName),
              Service::ServiceShell::SvcMain },
            { nullptr, nullptr }
        };

        if (!StartServiceCtrlDispatcherW(table)) {
            DWORD err = GetLastError();
            if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                // 双击运行或无 SCM 环境 → 退回控制台
                LOG_WARN("Shell", "wmain", "Boot",
                         "Not launched by SCM (err={}), "
                         "falling back to console mode.", err);
                Service::ServiceShell::Instance()->RunAsConsole();
            } else {
                LOG_ERROR("Shell", "wmain", "Boot",
                          "StartServiceCtrlDispatcherW failed, "
                          "err={}.", err);
            }
        }
    }

    Common::Logger::Shutdown();
    return 0;
}
