#include "ControlRuntime.h"

#include "Logger.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>

namespace Control {

namespace {

constexpr std::chrono::milliseconds kDuplicateCommandWindow{200};
constexpr std::chrono::milliseconds kEventDebounceWindow{400};
constexpr std::size_t kMaxHistoryItems = 512;

} // namespace

const char* ToString(RuntimeState state) noexcept {
    switch (state) {
    case RuntimeState::Stopped:
        return "Stopped";
    case RuntimeState::Deinitialized:
        return "Deinitialized";
    case RuntimeState::Active:
        return "Active";
    case RuntimeState::Idle:
        return "Idle";
    case RuntimeState::FatalBusDead:
        return "FatalBusDead";
    case RuntimeState::ShuttingDown:
        return "ShuttingDown";
    default:
        return "Unknown";
    }
}

const char* ToString(ControlCommandKind command) noexcept {
    switch (command) {
    case ControlCommandKind::Init:
        return "Init";
    case ControlCommandKind::Deinit:
        return "Deinit";
    case ControlCommandKind::StartStreaming:
        return "StartStreaming";
    case ControlCommandKind::StopStreaming:
        return "StopStreaming";
    case ControlCommandKind::EnterIdle:
        return "EnterIdle";
    case ControlCommandKind::ExitIdle:
        return "ExitIdle";
    case ControlCommandKind::CheckBus:
        return "CheckBus";
    case ControlCommandKind::Shutdown:
        return "Shutdown";
    default:
        return "Unknown";
    }
}

const char* ToString(CommandSource source) noexcept {
    switch (source) {
    case CommandSource::External:
        return "External";
    case CommandSource::SystemPolicy:
        return "SystemPolicy";
    default:
        return "Unknown";
    }
}

ControlRuntime::ControlRuntime() = default;

ControlRuntime::~ControlRuntime() {
    Stop();
}

bool ControlRuntime::Start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_state = RuntimeState::Deinitialized;
        m_initialized = false;
        m_streaming = false;
        m_idle = false;
        m_shutdownRequested = false;
        m_lastNote = "Runtime started";
    }

    m_worker = std::thread(&ControlRuntime::WorkerLoop, this);
    LOG_INFO("Control", "ControlRuntime::Start", "Deinitialized", "Control runtime started.");
    return true;
}

void ControlRuntime::Stop() {
    m_running.store(false, std::memory_order_release);
    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    std::lock_guard<std::mutex> lock(m_mu);
    m_state = RuntimeState::Stopped;
    m_initialized = false;
    m_streaming = false;
    m_idle = false;
    if (m_lastNote.empty()) {
        m_lastNote = "Runtime stopped";
    }
}

std::uint64_t ControlRuntime::SubmitCommand(ControlCommandKind command, CommandSource source, const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    QueuedCommand queued{};
    queued.id = m_nextCommandId.fetch_add(1, std::memory_order_relaxed);
    queued.command = command;
    queued.source = source;
    queued.enqueued_at = now;
    queued.reason = (reason == nullptr) ? "" : reason;

    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_state == RuntimeState::FatalBusDead && command != ControlCommandKind::Shutdown) {
            RecordHistory(queued, false, "Rejected in FatalBusDead");
            return queued.id;
        }
        if (m_state == RuntimeState::ShuttingDown && command != ControlCommandKind::Shutdown) {
            RecordHistory(queued, false, "Rejected in ShuttingDown");
            return queued.id;
        }

        if (ShouldDropAsDuplicate(command, now)) {
            RecordHistory(queued, true, "Dropped duplicate in debounce window");
            return queued.id;
        }

        if (command == ControlCommandKind::Shutdown || command == ControlCommandKind::Deinit) {
            m_queue.erase(
                std::remove_if(
                    m_queue.begin(),
                    m_queue.end(),
                    [](const QueuedCommand& q) {
                        return q.command == ControlCommandKind::EnterIdle ||
                               q.command == ControlCommandKind::ExitIdle ||
                               q.command == ControlCommandKind::StartStreaming ||
                               q.command == ControlCommandKind::StopStreaming;
                    }),
                m_queue.end());
        } else {
            m_queue.erase(
                std::remove_if(
                    m_queue.begin(),
                    m_queue.end(),
                    [command](const QueuedCommand& q) { return q.command == command; }),
                m_queue.end());
        }

        m_lastQueuedByKind[static_cast<int>(command)] = now;
        m_queue.push_back(std::move(queued));
    }

    m_cv.notify_one();
    return m_nextCommandId.load(std::memory_order_relaxed) - 1;
}

void ControlRuntime::IngestSystemEvent(const Host::SystemStateEvent& event) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mu);
        const int key = static_cast<int>(event.type);
        const auto it = m_lastEventByType.find(key);
        if (it != m_lastEventByType.end() && now - it->second < kEventDebounceWindow) {
            return;
        }
        m_lastEventByType[key] = now;
    }

    switch (event.type) {
    case Host::SystemStateEventType::DisplayOff:
        SubmitCommand(ControlCommandKind::StopStreaming, CommandSource::SystemPolicy, "DisplayOff");
        SubmitCommand(ControlCommandKind::Deinit, CommandSource::SystemPolicy, "DisplayOff");
        break;
    case Host::SystemStateEventType::DisplayOn:
    case Host::SystemStateEventType::ResumeAutomatic:
        SubmitCommand(ControlCommandKind::Init, CommandSource::SystemPolicy, "DisplayOn/Resume");
        SubmitCommand(ControlCommandKind::StartStreaming, CommandSource::SystemPolicy, "DisplayOn/Resume");
        break;
    case Host::SystemStateEventType::LidOff:
        SubmitCommand(ControlCommandKind::EnterIdle, CommandSource::SystemPolicy, "LidOff");
        break;
    case Host::SystemStateEventType::LidOn:
        SubmitCommand(ControlCommandKind::ExitIdle, CommandSource::SystemPolicy, "LidOn");
        break;
    case Host::SystemStateEventType::Shutdown:
        SubmitCommand(ControlCommandKind::Shutdown, CommandSource::SystemPolicy, "Shutdown");
        break;
    default:
        break;
    }
}

void ControlRuntime::SetSimulatedBusDead(bool dead) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_busDead = dead;
    m_lastNote = dead ? "Simulated bus set to dead" : "Simulated bus set to alive";
}

void ControlRuntime::ClearHistory() {
    std::lock_guard<std::mutex> lock(m_mu);
    m_history.clear();
}

RuntimeSnapshot ControlRuntime::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mu);
    RuntimeSnapshot snapshot{};
    snapshot.state = m_state;
    snapshot.initialized = m_initialized;
    snapshot.streaming = m_streaming;
    snapshot.idle = m_idle;
    snapshot.bus_dead = m_busDead;
    snapshot.queue_depth = m_queue.size();
    snapshot.last_command_id = m_lastCommandId;
    snapshot.last_note = m_lastNote;
    return snapshot;
}

std::vector<HistoryEntry> ControlRuntime::GetHistory(std::size_t max_items) const {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_history.empty()) {
        return {};
    }

    if (max_items >= m_history.size()) {
        return m_history;
    }

    return std::vector<HistoryEntry>(m_history.end() - static_cast<std::ptrdiff_t>(max_items), m_history.end());
}

bool ControlRuntime::IsShutdownRequested() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_shutdownRequested;
}

void ControlRuntime::WorkerLoop() {
    while (true) {
        QueuedCommand cmd{};
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) || !m_queue.empty();
            });

            if (!m_running.load(std::memory_order_acquire) && m_queue.empty()) {
                break;
            }

            if (!TryPopNextCommand(cmd)) {
                continue;
            }
        }

        Execute(cmd);
    }
}

bool ControlRuntime::TryPopNextCommand(QueuedCommand& out) {
    if (m_queue.empty()) {
        return false;
    }

    auto bestIt = m_queue.begin();
    int bestPriority = PriorityOf(bestIt->command);
    for (auto it = std::next(m_queue.begin()); it != m_queue.end(); ++it) {
        const int p = PriorityOf(it->command);
        if (p < bestPriority || (p == bestPriority && it->enqueued_at < bestIt->enqueued_at)) {
            bestPriority = p;
            bestIt = it;
        }
    }

    out = *bestIt;
    m_queue.erase(bestIt);
    return true;
}

int ControlRuntime::PriorityOf(ControlCommandKind command) const noexcept {
    switch (command) {
    case ControlCommandKind::Shutdown:
    case ControlCommandKind::Deinit:
        return 0;
    case ControlCommandKind::Init:
    case ControlCommandKind::StopStreaming:
        return 1;
    case ControlCommandKind::CheckBus:
        return 2;
    case ControlCommandKind::EnterIdle:
    case ControlCommandKind::ExitIdle:
    case ControlCommandKind::StartStreaming:
    default:
        return 3;
    }
}

void ControlRuntime::Execute(const QueuedCommand& cmd) {
    bool success = true;
    std::string detail;

    std::lock_guard<std::mutex> lock(m_mu);
    m_lastCommandId = cmd.id;

    switch (cmd.command) {
    case ControlCommandKind::Init:
        if (m_busDead) {
            success = false;
            m_state = RuntimeState::FatalBusDead;
            detail = "Init failed: bus dead, reboot required";
            break;
        }
        m_initialized = true;
        m_idle = false;
        if (!m_streaming) {
            m_state = RuntimeState::Deinitialized;
        } else {
            m_state = RuntimeState::Active;
        }
        detail = "Initialized (mock)";
        break;
    case ControlCommandKind::Deinit:
        m_initialized = false;
        m_streaming = false;
        m_idle = false;
        m_state = RuntimeState::Deinitialized;
        detail = "Deinitialized (mock)";
        break;
    case ControlCommandKind::StartStreaming:
        if (!m_initialized) {
            success = false;
            detail = "StartStreaming rejected: not initialized";
            break;
        }
        if (m_busDead) {
            success = false;
            m_state = RuntimeState::FatalBusDead;
            detail = "StartStreaming failed: bus dead, reboot required";
            break;
        }
        m_streaming = true;
        m_state = m_idle ? RuntimeState::Idle : RuntimeState::Active;
        detail = "Streaming started (mock)";
        break;
    case ControlCommandKind::StopStreaming:
        m_streaming = false;
        m_state = m_initialized ? (m_idle ? RuntimeState::Idle : RuntimeState::Deinitialized) : RuntimeState::Deinitialized;
        detail = "Streaming stopped (mock)";
        break;
    case ControlCommandKind::EnterIdle:
        if (!m_initialized) {
            success = false;
            detail = "EnterIdle rejected: not initialized";
            break;
        }
        m_idle = true;
        m_state = RuntimeState::Idle;
        detail = "Entered idle (policy timeout=200ms, block=1)";
        break;
    case ControlCommandKind::ExitIdle:
        if (!m_initialized) {
            success = false;
            detail = "ExitIdle rejected: not initialized";
            break;
        }
        m_idle = false;
        m_state = m_streaming ? RuntimeState::Active : RuntimeState::Deinitialized;
        detail = "Exited idle (policy timeout=0ms, block=1)";
        break;
    case ControlCommandKind::CheckBus:
        if (m_busDead) {
            success = false;
            m_state = RuntimeState::FatalBusDead;
            detail = "check_bus=dead, reboot required";
        } else {
            detail = "check_bus=alive";
        }
        break;
    case ControlCommandKind::Shutdown:
        m_state = RuntimeState::ShuttingDown;
        m_shutdownRequested = true;
        m_running.store(false, std::memory_order_release);
        detail = "Shutdown requested";
        break;
    default:
        success = false;
        detail = "Unknown command";
        break;
    }

    m_lastNote = detail;
    RecordHistory(cmd, success, detail);
}

bool ControlRuntime::ShouldDropAsDuplicate(ControlCommandKind command, std::chrono::steady_clock::time_point now) {
    const auto it = m_lastQueuedByKind.find(static_cast<int>(command));
    if (it == m_lastQueuedByKind.end()) {
        return false;
    }
    return now - it->second < kDuplicateCommandWindow;
}

void ControlRuntime::RecordHistory(const QueuedCommand& cmd, bool success, const std::string& detail) {
    HistoryEntry entry{};
    entry.timestamp = std::chrono::system_clock::now();
    entry.command_id = cmd.id;
    entry.command = cmd.command;
    entry.source = cmd.source;
    entry.success = success;
    entry.detail = detail.empty() ? cmd.reason : detail;
    m_history.push_back(std::move(entry));
    if (m_history.size() > kMaxHistoryItems) {
        m_history.erase(m_history.begin(), m_history.begin() + static_cast<std::ptrdiff_t>(m_history.size() - kMaxHistoryItems));
    }
}

} // namespace Control
