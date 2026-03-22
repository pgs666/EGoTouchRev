#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace Control {

enum class RuntimeState : std::uint8_t {
    Stopped = 0,
    Deinitialized,
    Active,
    Idle,
    FatalBusDead,
    ShuttingDown,
};

enum class ControlCommandKind : std::uint8_t {
    Init = 0,
    Deinit,
    StartStreaming,
    StopStreaming,
    EnterIdle,
    ExitIdle,
    CheckBus,
    Shutdown,
};

enum class CommandSource : std::uint8_t {
    External = 0,
    SystemPolicy,
};

struct RuntimeSnapshot {
    RuntimeState state = RuntimeState::Stopped;
    bool initialized = false;
    bool streaming = false;
    bool idle = false;
    bool bus_dead = false;
    std::size_t queue_depth = 0;
    std::uint64_t last_command_id = 0;
    std::string last_note;
};

struct HistoryEntry {
    std::chrono::system_clock::time_point timestamp{};
    std::uint64_t command_id = 0;
    ControlCommandKind command = ControlCommandKind::Init;
    CommandSource source = CommandSource::External;
    bool success = false;
    std::string detail;
};

const char* ToString(RuntimeState state) noexcept;
const char* ToString(ControlCommandKind command) noexcept;
const char* ToString(CommandSource source) noexcept;

} // namespace Control

