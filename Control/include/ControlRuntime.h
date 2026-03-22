#pragma once

#include "ControlTypes.h"
#include "SystemStateEvent.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Control {

class ControlRuntime {
public:
    ControlRuntime();
    ~ControlRuntime();

    ControlRuntime(const ControlRuntime&) = delete;
    ControlRuntime& operator=(const ControlRuntime&) = delete;

    bool Start();
    void Stop();

    std::uint64_t SubmitCommand(ControlCommandKind command, CommandSource source, const char* reason = "");
    void IngestSystemEvent(const Host::SystemStateEvent& event);
    void SetSimulatedBusDead(bool dead);
    void ClearHistory();

    RuntimeSnapshot GetSnapshot() const;
    std::vector<HistoryEntry> GetHistory(std::size_t max_items = 200) const;
    bool IsShutdownRequested() const;

private:
    struct QueuedCommand {
        std::uint64_t id = 0;
        ControlCommandKind command = ControlCommandKind::Init;
        CommandSource source = CommandSource::External;
        std::chrono::steady_clock::time_point enqueued_at{};
        std::string reason;
    };

    void WorkerLoop();
    bool TryPopNextCommand(QueuedCommand& out);
    int PriorityOf(ControlCommandKind command) const noexcept;
    void Execute(const QueuedCommand& cmd);
    bool ShouldDropAsDuplicate(ControlCommandKind command, std::chrono::steady_clock::time_point now);
    void RecordHistory(const QueuedCommand& cmd, bool success, const std::string& detail);

private:
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<QueuedCommand> m_queue;
    std::vector<HistoryEntry> m_history;

    std::unordered_map<int, std::chrono::steady_clock::time_point> m_lastQueuedByKind;
    std::unordered_map<int, std::chrono::steady_clock::time_point> m_lastEventByType;

    RuntimeState m_state = RuntimeState::Stopped;
    bool m_initialized = false;
    bool m_streaming = false;
    bool m_idle = false;
    bool m_busDead = false;
    bool m_shutdownRequested = false;
    std::uint64_t m_lastCommandId = 0;
    std::string m_lastNote;

    std::atomic<bool> m_running{false};
    std::thread m_worker;
    std::atomic<std::uint64_t> m_nextCommandId{1};
};

} // namespace Control

