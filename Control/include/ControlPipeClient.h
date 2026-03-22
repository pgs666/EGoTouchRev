#pragma once

#include <string>
#include <vector>

namespace Control {

class ControlPipeClient {
public:
    explicit ControlPipeClient(std::string pipe_name);

    bool Request(const std::string& request_line, std::vector<std::string>& payload_lines, std::string& error_message) const;

private:
    std::string m_pipeName;
};

} // namespace Control

