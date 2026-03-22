#include "SystemStateMonitor.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

bool SignalNamedEvent(const wchar_t* event_name) {
    HANDLE event_handle = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (event_handle == nullptr) {
        std::wcerr << L"[TEST] OpenEvent failed: " << event_name << L", gle=" << GetLastError() << L"\n";
        return false;
    }

    const BOOL ok = SetEvent(event_handle);
    const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(event_handle);

    if (!ok) {
        std::wcerr << L"[TEST] SetEvent failed: " << event_name << L", gle=" << err << L"\n";
    }
    return ok == TRUE;
}

void ResetNamedEventBestEffort(const wchar_t* event_name) {
    HANDLE event_handle = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (event_handle == nullptr) {
        return;
    }
    ResetEvent(event_handle);
    CloseHandle(event_handle);
}

bool ExpectSequence(const std::vector<Host::SystemStateEventType>& expected,
                    const std::vector<Host::SystemStateEventType>& actual) {
    if (actual.size() < expected.size()) {
        return false;
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            return false;
        }
    }

    return true;
}

} // namespace

int main() {
    using namespace std::chrono_literals;

    Host::SystemStateMonitor monitor;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Host::SystemStateEventType> observed;

    const bool started = monitor.Start([&](const Host::SystemStateEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mu);
            observed.push_back(event.type);
        }
        cv.notify_all();
    });

    if (!started) {
        std::cerr << "[TEST] SystemStateMonitor start failed.\n";
        return 1;
    }

    const auto& named_events = Host::SystemStateMonitor::NamedEventList();
    for (const wchar_t* event_name : named_events) {
        ResetNamedEventBestEffort(event_name);
    }

    // Give worker thread a short warm-up window before injecting events.
    std::this_thread::sleep_for(80ms);

    const std::vector<std::pair<std::size_t, Host::SystemStateEventType>> script = {
        {3, Host::SystemStateEventType::DisplayOff},
        {2, Host::SystemStateEventType::DisplayOn},
        {5, Host::SystemStateEventType::LidOff},
        {7, Host::SystemStateEventType::ResumeAutomatic},
    };

    for (const auto& step : script) {
        const std::size_t index = step.first;
        if (!SignalNamedEvent(named_events[index])) {
            monitor.Stop();
            return 2;
        }
        std::this_thread::sleep_for(20ms);
    }

    const std::vector<Host::SystemStateEventType> expected = {
        Host::SystemStateEventType::DisplayOff,
        Host::SystemStateEventType::DisplayOn,
        Host::SystemStateEventType::LidOff,
        Host::SystemStateEventType::ResumeAutomatic,
    };

    {
        std::unique_lock<std::mutex> lock(mu);
        const bool received_all = cv.wait_for(lock, 2s, [&] {
            return observed.size() >= expected.size();
        });
        if (!received_all) {
            monitor.Stop();
            std::cerr << "[TEST] Timeout waiting for detector events. observed=" << observed.size() << "\n";
            return 3;
        }
    }

    monitor.Stop();

    if (!ExpectSequence(expected, observed)) {
        std::cerr << "[TEST] Event sequence mismatch.\n";
        for (std::size_t i = 0; i < observed.size(); ++i) {
            std::cerr << "  observed[" << i << "]=" << Host::ToString(observed[i]) << "\n";
        }
        return 4;
    }

    std::cout << "[TEST] SystemStateMonitor event test passed. observed=" << observed.size() << "\n";
    return 0;
}
