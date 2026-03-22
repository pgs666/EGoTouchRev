#include "ControlPipeServer.h"

#include "Logger.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>

namespace Control {

namespace {

std::string Trim(std::string input) {
    while (!input.empty() && (input.back() == '\r' || input.back() == '\n' || input.back() == ' ' || input.back() == '\t')) {
        input.pop_back();
    }
    std::size_t start = 0;
    while (start < input.size() && (input[start] == ' ' || input[start] == '\t')) {
        ++start;
    }
    return input.substr(start);
}

std::vector<std::string> SplitTokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        out.push_back(token);
    }
    return out;
}

std::string BuildResponseOk(const std::vector<std::string>& lines = {}) {
    std::string response = "OK\n";
    for (const auto& line : lines) {
        response += line;
        response += '\n';
    }
    response += "END\n";
    return response;
}

std::string BuildResponseErr(const std::string& message) {
    return std::format("ERR {}\nEND\n", message);
}

bool ParseCommandKind(const std::string& token, ControlCommandKind& out) {
    if (token == "Init") { out = ControlCommandKind::Init; return true; }
    if (token == "Deinit") { out = ControlCommandKind::Deinit; return true; }
    if (token == "StartStreaming") { out = ControlCommandKind::StartStreaming; return true; }
    if (token == "StopStreaming") { out = ControlCommandKind::StopStreaming; return true; }
    if (token == "EnterIdle") { out = ControlCommandKind::EnterIdle; return true; }
    if (token == "ExitIdle") { out = ControlCommandKind::ExitIdle; return true; }
    if (token == "CheckBus") { out = ControlCommandKind::CheckBus; return true; }
    if (token == "Shutdown") { out = ControlCommandKind::Shutdown; return true; }
    return false;
}

bool ParseEventType(const std::string& token, Host::SystemStateEventType& out) {
    if (token == "DisplayOn") { out = Host::SystemStateEventType::DisplayOn; return true; }
    if (token == "DisplayOff") { out = Host::SystemStateEventType::DisplayOff; return true; }
    if (token == "LidOn") { out = Host::SystemStateEventType::LidOn; return true; }
    if (token == "LidOff") { out = Host::SystemStateEventType::LidOff; return true; }
    if (token == "ResumeAutomatic") { out = Host::SystemStateEventType::ResumeAutomatic; return true; }
    if (token == "Shutdown") { out = Host::SystemStateEventType::Shutdown; return true; }
    return false;
}

std::string TimeToString(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    tm local_tm{};
    localtime_s(&local_tm, &tt);
    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buffer;
}

SECURITY_ATTRIBUTES BuildPermissiveSa(SECURITY_DESCRIPTOR& sd) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = nullptr;

    if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) != 0 &&
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE) != 0) {
        sa.lpSecurityDescriptor = &sd;
    }
    return sa;
}

} // namespace

ControlPipeServer::ControlPipeServer(std::string pipe_name) : m_pipeName(std::move(pipe_name)) {}

ControlPipeServer::~ControlPipeServer() {
    Stop();
}

bool ControlPipeServer::Start(ControlRuntime* runtime) {
    if (runtime == nullptr) {
        return false;
    }
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    m_runtime = runtime;
    m_stopRequestedByClient.store(false, std::memory_order_release);
    m_thread = std::thread(&ControlPipeServer::ServerLoop, this);
    return true;
}

void ControlPipeServer::Stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    HANDLE wake = CreateFileA(
        m_pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (wake != INVALID_HANDLE_VALUE) {
        CloseHandle(wake);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void ControlPipeServer::ServerLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        SECURITY_DESCRIPTOR sd{};
        SECURITY_ATTRIBUTES sa = BuildPermissiveSa(sd);
        LPSECURITY_ATTRIBUTES sa_ptr = (sa.lpSecurityDescriptor != nullptr) ? &sa : nullptr;

        HANDLE pipe = CreateNamedPipeA(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            500,
            sa_ptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            LOG_ERROR("Control", "ControlPipeServer::ServerLoop", "Error", "CreateNamedPipe failed: {}", static_cast<int>(GetLastError()));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        std::string request;
        {
            char buf[512]{};
            DWORD bytesRead = 0;
            while (ReadFile(pipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
                request.append(buf, buf + bytesRead);
                if (request.find('\n') != std::string::npos) {
                    break;
                }
            }
        }

        const std::string response = HandleCommand(request);
        DWORD bytesWritten = 0;
        WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &bytesWritten, nullptr);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

std::string ControlPipeServer::HandleCommand(const std::string& request_line) {
    if (m_runtime == nullptr) {
        return BuildResponseErr("runtime_not_ready");
    }

    const std::string trimmed = Trim(request_line);
    if (trimmed.empty()) {
        return BuildResponseErr("empty_request");
    }

    const auto tokens = SplitTokens(trimmed);
    if (tokens.empty()) {
        return BuildResponseErr("invalid_request");
    }

    const std::string& op = tokens[0];

    if (op == "PING") {
        return BuildResponseOk({"PONG"});
    }

    if (op == "STATE") {
        const RuntimeSnapshot s = m_runtime->GetSnapshot();
        const std::string stateLine = std::format(
            "state={} initialized={} streaming={} idle={} bus_dead={} queue={} last_id={} note={}",
            ToString(s.state),
            s.initialized ? 1 : 0,
            s.streaming ? 1 : 0,
            s.idle ? 1 : 0,
            s.bus_dead ? 1 : 0,
            s.queue_depth,
            s.last_command_id,
            s.last_note);
        return BuildResponseOk({stateLine});
    }

    if (op == "HISTORY") {
        auto history = m_runtime->GetHistory(120);
        std::vector<std::string> lines;
        lines.reserve(history.size() + 1);
        lines.push_back(std::format("count={}", history.size()));
        for (const auto& h : history) {
            lines.push_back(std::format(
                "{} | id={} | {} | {} | ok={} | {}",
                TimeToString(h.timestamp),
                h.command_id,
                ToString(h.command),
                ToString(h.source),
                h.success ? 1 : 0,
                h.detail));
        }
        return BuildResponseOk(lines);
    }

    if (op == "CLEAR_HISTORY") {
        m_runtime->ClearHistory();
        return BuildResponseOk({"cleared=1"});
    }

    if (op == "CMD") {
        if (tokens.size() < 2) {
            return BuildResponseErr("missing_command");
        }
        ControlCommandKind cmd{};
        if (!ParseCommandKind(tokens[1], cmd)) {
            return BuildResponseErr("unknown_command");
        }
        const auto id = m_runtime->SubmitCommand(cmd, CommandSource::External, "pipe_cmd");
        return BuildResponseOk({std::format("queued_id={}", id)});
    }

    if (op == "EVENT") {
        if (tokens.size() < 2) {
            return BuildResponseErr("missing_event");
        }

        Host::SystemStateEventType eventType{};
        if (!ParseEventType(tokens[1], eventType)) {
            return BuildResponseErr("unknown_event");
        }

        Host::SystemStateEvent event{};
        event.type = eventType;
        event.source = Host::SystemStateEventSource::ThpServiceNamedEvent;
        event.timestamp = std::chrono::system_clock::now();
        event.raw_name = L"PipeInjectedEvent";
        m_runtime->IngestSystemEvent(event);
        return BuildResponseOk({std::format("event={}", tokens[1])});
    }

    if (op == "BUS") {
        if (tokens.size() < 2) {
            return BuildResponseErr("missing_bus_state");
        }
        if (tokens[1] == "alive") {
            m_runtime->SetSimulatedBusDead(false);
            return BuildResponseOk({"bus=alive"});
        }
        if (tokens[1] == "dead") {
            m_runtime->SetSimulatedBusDead(true);
            return BuildResponseOk({"bus=dead"});
        }
        return BuildResponseErr("invalid_bus_state");
    }

    if (op == "STOP_SERVICE") {
        m_stopRequestedByClient.store(true, std::memory_order_release);
        m_runtime->SubmitCommand(ControlCommandKind::Shutdown, CommandSource::External, "stop_service");
        return BuildResponseOk({"shutdown=queued"});
    }

    return BuildResponseErr("unknown_operation");
}

} // namespace Control
