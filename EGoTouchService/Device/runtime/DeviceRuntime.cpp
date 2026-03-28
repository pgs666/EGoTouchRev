#include "runtime/DeviceRuntime.h"
#include "EngineTypes.h"
#include "Logger.h"


#include <format>

namespace {
constexpr std::size_t kMaxHistoryItems = 512;
constexpr std::chrono::milliseconds kEventDebounce{400};
} // namespace

// --------------- ToString helpers ---------------

const char* ToString(workerState s) noexcept {
    switch (s) {
    case workerState::quit:      return "quit";
    case workerState::ready:     return "ready";
    case workerState::streaming: return "streaming";
    case workerState::recover:   return "recover";
    default:                     return "unknown";
    }
}

const char* ToString(CommandSource s) noexcept {
    switch (s) {
    case CommandSource::External:     return "External";
    case CommandSource::SystemPolicy: return "SystemPolicy";
    default:                          return "Unknown";
    }
}

// --------------- Lifecycle ---------------

DeviceRuntime::DeviceRuntime(
        const std::wstring& master,
        const std::wstring& slave,
        const std::wstring& interrupt)
    : m_chip(master, slave, interrupt) {}

DeviceRuntime::~DeviceRuntime() { Stop(); }

bool DeviceRuntime::Start() {
    if (m_running.exchange(true)) return false;
    m_stopReq.store(false);       // ← critical: clear stop flag for restart
    m_state.store(workerState::ready);
    m_shutdownReq = false;
    m_recoverCount = 0;
    m_lastNote = "Runtime started";
    m_thread = std::thread(&DeviceRuntime::WorkerMain, this);
    LOG_INFO("Device", "DeviceRuntime::Start", "ready",
             "Worker thread launched.");
    return true;
}

void DeviceRuntime::Stop() {
    m_stopReq.store(true);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
    m_state.store(workerState::quit);
    m_lastNote = "Runtime stopped";
}

bool DeviceRuntime::IsShutdownRequested() const {
    return m_shutdownReq;
}

// --------------- 命令注入 ---------------

uint64_t DeviceRuntime::SubmitCommand(
        command cmd, CommandSource src, const char* reason) {
    QueuedCommand qc{};
    qc.id = m_nextCmdId.fetch_add(1);
    qc.cmd = cmd;
    qc.source = src;
    qc.enqueued_at = std::chrono::steady_clock::now();
    qc.reason = reason ? reason : "";
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_cmdQueue.push_back(std::move(qc));
    }
    return qc.id;
}

void DeviceRuntime::IngestSystemEvent(
        const Host::SystemStateEvent& ev) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        int key = static_cast<int>(ev.type);
        auto it = m_lastEventByType.find(key);
        if (it != m_lastEventByType.end() &&
            now - it->second < kEventDebounce) return;
        m_lastEventByType[key] = now;
    }
    using ET = Host::SystemStateEventType;
    switch (ev.type) {
    case ET::DisplayOff:
    case ET::LidOff:
        LOG_INFO("Device", "IngestSystemEvent", "Policy",
                 "Sleep event ({}), requesting worker stop.",
                 Host::ToString(ev.type));
        m_stopReq.store(true);
        break;
    case ET::DisplayOn:
    case ET::LidOn:
    case ET::ResumeAutomatic:
        LOG_INFO("Device", "IngestSystemEvent", "Policy",
                 "Wake event ({}), attempting restart.",
                 Host::ToString(ev.type));
        // Ensure old worker thread is fully joined before restarting
        Stop();
        Start();
        break;
    case ET::Shutdown:
        LOG_INFO("Device", "IngestSystemEvent", "Policy",
                 "Shutdown event, requesting termination.");
        m_shutdownReq = true;
        m_stopReq.store(true);
        break;
    default: break;
    }
}

// --------------- Pipe 查询 ---------------

RuntimeSnapshot DeviceRuntime::GetSnapshot() const {
    std::lock_guard<std::mutex> lk(m_mu);
    RuntimeSnapshot s;
    s.state = m_state.load();
    s.stylus_connected = m_chip.m_stylus.connected;
    s.recover_count = m_recoverCount;
    s.queue_depth = m_cmdQueue.size();
    s.last_command_id = m_lastCmdId;
    s.last_note = m_lastNote;
    return s;
}

std::vector<HistoryEntry> DeviceRuntime::GetHistory(
        std::size_t n) const {
    std::lock_guard<std::mutex> lk(m_mu);
    if (n >= m_history.size()) return m_history;
    return {m_history.end() - static_cast<ptrdiff_t>(n),
            m_history.end()};
}

void DeviceRuntime::ClearHistory() {
    std::lock_guard<std::mutex> lk(m_mu);
    m_history.clear();
}



// --------------- 审计日志 ---------------

void DeviceRuntime::RecordHistory(
        const QueuedCommand& qc, bool ok,
        const std::string& det) {
    HistoryEntry e;
    e.timestamp = std::chrono::system_clock::now();
    e.command_id = qc.id;
    e.command_name = qc.reason;
    e.source = qc.source;
    e.success = ok;
    e.detail = det;
    m_history.push_back(std::move(e));
    if (m_history.size() > kMaxHistoryItems)
        m_history.erase(m_history.begin(),
            m_history.begin() +
            static_cast<ptrdiff_t>(
                m_history.size() - kMaxHistoryItems));
}

// --------------- 命令执行 ---------------

bool DeviceRuntime::DrainCommands() {
    std::lock_guard<std::mutex> lk(m_mu);
    while (!m_cmdQueue.empty()) {
        auto qc = m_cmdQueue.front();
        m_cmdQueue.pop_front();
        m_lastCmdId = qc.id;
        if (auto r = m_chip.afe_sendCommand(qc.cmd); !r) {
            RecordHistory(qc, false, "afe_sendCommand failed");
            m_state.store(workerState::recover);
            return false;
        }
        RecordHistory(qc, true, "OK");
    }
    return true;
}

// ----------- Worker 核心循环 -----------

ThreadResult DeviceRuntime::WorkerMain() {
    while (true) {
        if (m_stopReq.load(std::memory_order_acquire)) {
            m_state.store(workerState::quit);
            m_stopReq.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> lk(m_mu);
            m_cmdQueue.clear();
        }

        DrainCommands();

        switch (m_state.load(std::memory_order_acquire)) {
        case workerState::ready:     OnReady();     break;
        case workerState::streaming: OnStreaming();  break;
        case workerState::recover:   OnRecover();   break;
        case workerState::quit:
            if (OnQuit()) {
                m_running.store(false);  // allow restart via Start()
                LOG_INFO("Device", "WorkerMain", "quit",
                         "Worker exited, m_running=false.");
                return ThreadResult();
            }
            break;
        }
    }
    return ThreadResult();
}

// ----------- 状态处理 -----------

void DeviceRuntime::OnReady() {
    if (m_autoMode.load()) {
        if (auto r = m_chip.Init(); !r) {
            m_state.store(workerState::recover);
            return;
        }
        m_state.store(workerState::streaming);
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void DeviceRuntime::OnStreaming() {
    auto res = m_chip.GetFrame();
    if (res == std::unexpected(ChipError::Timeout)) return;
    if (!res) {
        m_state.store(workerState::recover);
        return;
    }

    const auto& rawData = m_chip.back_data;

    // 1. Stylus pipeline (independent, works directly from slave frame)
    Engine::StylusPacket stylusPacket{};
    m_stylusPipeline.Process(
        std::span<const uint8_t>(rawData.data(), rawData.size()),
        stylusPacket);
    // m_vhfReporter.DispatchStylus(stylusPacket);  // disabled for debugging

    // 2. Touch pipeline (independent, works from master frame)
    Engine::HeatmapFrame touchFrame;
    touchFrame.rawData.assign(rawData.begin(), rawData.end());
    m_touchPipeline.Execute(touchFrame);
    m_vhfReporter.DispatchTouch(touchFrame);

    // 3. Merge results for UI push
    if (m_framePushCb) {
        touchFrame.stylus = m_stylusPipeline.GetLastResult();
        touchFrame.stylus.packet = stylusPacket;
        m_framePushCb(touchFrame);
    }
}

bool DeviceRuntime::OnQuit() {
    if (m_autoMode.load()) {
        (void)m_chip.Deinit(false);
    }
    return true;  // signal WorkerMain to return
}

void DeviceRuntime::OnRecover() {
    m_recoverCount++;
    if (m_recoverCount > 10) {
        m_state.store(workerState::quit);
        return;
    }
    if (auto res = m_chip.check_bus(); !res) return;
    if (auto res = m_chip.Init(); !res) return;
    m_state.store(workerState::streaming);
    m_recoverCount = 0;
}