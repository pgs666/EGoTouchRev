#include "ControlPipeClient.h"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <windows.h>

namespace Control {

namespace {

std::string Trim(const std::string& line) {
    std::size_t start = 0;
    while (start < line.size() && (line[start] == '\r' || line[start] == '\n' || line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    std::size_t end = line.size();
    while (end > start && (line[end - 1] == '\r' || line[end - 1] == '\n' || line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    return line.substr(start, end - start);
}

} // namespace

ControlPipeClient::ControlPipeClient(std::string pipe_name) : m_pipeName(std::move(pipe_name)) {}

bool ControlPipeClient::Request(const std::string& request_line, std::vector<std::string>& payload_lines, std::string& error_message) const {
    payload_lines.clear();
    error_message.clear();

    if (!WaitNamedPipeA(m_pipeName.c_str(), 700)) {
        error_message = "service_not_available";
        return false;
    }

    HANDLE pipe = CreateFileA(
        m_pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        error_message = "open_pipe_failed";
        return false;
    }

    const std::string req = request_line + "\n";
    DWORD bytesWritten = 0;
    if (!WriteFile(pipe, req.data(), static_cast<DWORD>(req.size()), &bytesWritten, nullptr)) {
        CloseHandle(pipe);
        error_message = "write_failed";
        return false;
    }

    std::string raw;
    char buffer[512]{};
    DWORD bytesRead = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        raw.append(buffer, buffer + bytesRead);
        if (raw.find("\nEND\n") != std::string::npos || raw.ends_with("END\n")) {
            break;
        }
    }
    CloseHandle(pipe);

    std::istringstream iss(raw);
    std::string line;
    if (!std::getline(iss, line)) {
        error_message = "empty_response";
        return false;
    }

    line = Trim(line);
    if (line.rfind("ERR", 0) == 0) {
        error_message = line;
        return false;
    }
    if (line != "OK") {
        error_message = "invalid_response_header";
        return false;
    }

    while (std::getline(iss, line)) {
        line = Trim(line);
        if (line == "END") {
            return true;
        }
        if (!line.empty()) {
            payload_lines.push_back(line);
        }
    }

    error_message = "missing_response_end";
    return false;
}

} // namespace Control

