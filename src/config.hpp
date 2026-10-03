#pragma once
#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pp {

enum class RestartPolicy { Never, OnFailure, Always };

struct ServiceConfig {
    std::string name;
    std::string command;
    std::vector<std::string> depends;
    RestartPolicy restart = RestartPolicy::OnFailure;
    int maxRestarts = 5;       // consecutive restarts before entering Failed
    int backoffMs = 500;       // base delay, doubled per consecutive restart
    int readyDelayMs = 0;      // how long a service must run before dependents start
    int stopTimeoutMs = 3000;  // SIGTERM -> SIGKILL escalation
};

struct ConfigError : std::runtime_error { using std::runtime_error::runtime_error; };

std::vector<ServiceConfig> parseConfig(std::istream& in);
std::vector<ServiceConfig> loadConfigFile(const std::string& path);
std::vector<std::string> splitArgs(const std::string& cmd);
const char* toString(RestartPolicy p);

}  // namespace pp
