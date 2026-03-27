#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include <deque>
#include <expected>
#include <mutex>

#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <functional>

#include "Device.h"
#include "himax/HimaxChip.h"
#include "SystemStateEvent.h"
#include "FramePipeline.h"
#include "vhf/VhfReporter.h"

// --------------- 基础类型 ---------------

enum class result { error, timeout };
using ThreadResult = std::expected<void, result>;

enum class workerState {
    quit = -1,
    ready = 0,
    streaming,
    recover,
};

const char* ToString(workerState s) noexcept;

enum class CommandSource : uint8_t {
    External = 0,
    SystemPolicy,
};

const char* ToString(CommandSource s) noexcept;
// --------------- 审计日志 ---------------

struct HistoryEntry {
    std::chrono::system_clock::time_point timestamp{};
    uint64_t command_id = 0;
    std::string command_name;
    CommandSource source = CommandSource::External;
    bool success = false;
    std::string detail;
};

struct RuntimeSnapshot {
    workerState state = workerState::quit;
    bool stylus_connected = false;
    uint8_t recover_count = 0;
    std::size_t queue_depth = 0;
    uint64_t last_command_id = 0;
    std::string last_note;
};

// --------------- DeviceRuntime ---------------

class DeviceRuntime {
public:
    DeviceRuntime(const std::wstring& master,
                  const std::wstring& slave,
                  const std::wstring& interrupt);
    ~DeviceRuntime();
    DeviceRuntime(const DeviceRuntime&) = delete;
    DeviceRuntime& operator=(const DeviceRuntime&) = delete;

    bool Start();
    void Stop();
    bool IsShutdownRequested() const;
    bool IsRunning() const { return m_running.load(); }

    // Auto/Manual 模式
    void SetAutoMode(bool enabled) { m_autoMode.store(enabled); }
    bool IsAutoMode() const { return m_autoMode.load(); }

    // Pipeline / VHF 配置 — 仅在 Start() 前调用
    Engine::FramePipeline& GetPipeline() { return m_pipeline; }
    VhfReporter& GetVhfReporter() { return m_vhfReporter; }

    // Frame push callback for IPC (called after pipeline+VHF in worker loop)
    using FramePushCallback = std::function<void(const Engine::HeatmapFrame&)>;
    void SetFramePushCallback(FramePushCallback cb) { m_framePushCb = std::move(cb); }

    void IngestSystemEvent(const Host::SystemStateEvent& ev);
    uint64_t SubmitCommand(command cmd, CommandSource src,
                           const char* reason = "");

    RuntimeSnapshot GetSnapshot() const;
    std::vector<HistoryEntry> GetHistory(std::size_t n = 200) const;
    void ClearHistory();

private:
    ThreadResult WorkerMain();

    // ── Worker 状态处理（每个状态一个入口，Worker 只做调度） ──
    void OnReady();              // ready → 尝试 auto init
    void OnStreaming();          // streaming → 采帧 + 处理
    void OnRecover();            // recover → 重试恢复
    bool OnQuit();               // quit → 清理并退出

    struct QueuedCommand {
        uint64_t id = 0;
        command cmd{};
        CommandSource source = CommandSource::External;
        std::chrono::steady_clock::time_point enqueued_at{};
        std::string reason;
    };

    bool DrainCommands();
    void RecordHistory(const QueuedCommand& qc,
                       bool ok, const std::string& det);

    std::atomic<workerState> m_state{workerState::quit};
    std::atomic<bool> m_stopReq{false};
    std::atomic<bool> m_autoMode{false};
    Himax::Chip m_chip;
    Engine::FramePipeline m_pipeline;
    VhfReporter m_vhfReporter;
    uint8_t m_recoverCount = 0;

    mutable std::mutex m_mu;
    std::deque<QueuedCommand> m_cmdQueue;

    std::vector<HistoryEntry> m_history;
    std::unordered_map<int, std::chrono::steady_clock::time_point>
        m_lastEventByType;
    uint64_t m_lastCmdId = 0;
    std::string m_lastNote;
    std::atomic<uint64_t> m_nextCmdId{1};
    FramePushCallback m_framePushCb;

    std::atomic<bool> m_running{false};
    bool m_shutdownReq = false;
    std::thread m_thread;
};