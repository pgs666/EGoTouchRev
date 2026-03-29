#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include "ServiceHost.h"

namespace Service {

/// 服务名称常量
inline constexpr wchar_t kServiceName[] = L"EGoTouchService";

/// 最外层壳：负责 Windows SCM 注册和控制台回退。
/// 不了解任何业务模块，只持有一个 ServiceHost。
class ServiceShell {
public:
    static ServiceShell* Instance();

    /// 控制台调试模式（--console 参数或无 SCM 时退回）
    void RunAsConsole();

    /// SCM 回调入口（静态桥接到 Instance()）
    static void WINAPI SvcMain(DWORD argc, LPWSTR* argv);

private:
    static DWORD WINAPI SvcCtrlHandlerEx(
        DWORD ctrl, DWORD evtType,
        LPVOID evtData, LPVOID ctx);

    void RegisterPowerNotifications();
    void UnregisterPowerNotifications();
    void ReportStatus(DWORD state, DWORD waitHint = 0);
    void WaitForStop();

    SERVICE_STATUS_HANDLE m_statusHandle = nullptr;
    SERVICE_STATUS        m_status{};
    HANDLE                m_stopEvent = nullptr;
    ServiceHost           m_host;

    // PBT power setting notification handles
    HPOWERNOTIFY m_hDisplayNotify = nullptr;
    HPOWERNOTIFY m_hLidNotify     = nullptr;
    HPOWERNOTIFY m_hSuspendNotify = nullptr;
};

} // namespace Service
