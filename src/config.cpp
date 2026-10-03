#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace pp {

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static int toInt(const std::string& v, int line, const std::string& key) {
    try {
        size_t used = 0;
        int n = std::stoi(v, &used);
        if (used != v.size() || n < 0) throw std::invalid_argument("bad");
        return n;
    } catch (...) {
        throw ConfigError("line " + std::to_string(line) + ": '" + key + "' needs a non-negative integer");
    }
}

const char* toString(RestartPolicy p) {
    switch (p) {
        case RestartPolicy::Never: return "never";
        case RestartPolicy::OnFailure: return "on-failure";
        case RestartPolicy::Always: return "always";
    }
    return "?";
}

std::vector<std::string> splitArgs(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool inTok = false;
    char quote = 0;
    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < cmd.size()) cur += cmd[++i];
            else cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            inTok = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inTok) { out.push_back(cur); cur.clear(); inTok = false; }
        } else {
            cur += c;
            inTok = true;
        }
    }
    if (quote) throw ConfigError("unterminated quote in command: " + cmd);
    if (inTok) out.push_back(cur);
    return out;
}

std::vector<ServiceConfig> parseConfig(std::istream& in) {
    std::vector<ServiceConfig> out;
    std::set<std::string> seen;
    ServiceConfig* cur = nullptr;
    std::string line;
    int ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const std::string where = "line " + std::to_string(ln) + ": ";
        if (line.front() == '[') {
            if (line.back() != ']') throw ConfigError(where + "unterminated section header");
            std::string name = trim(line.substr(1, line.size() - 2));
            if (name.rfind("service:", 0) == 0) name = trim(name.substr(8));
            if (name.empty() || name.find_first_of(" \t") != std::string::npos)
                throw ConfigError(where + "invalid service name");
            if (!seen.insert(name).second) throw ConfigError(where + "duplicate service '" + name + "'");
            out.emplace_back();
            out.back().name = name;
            cur = &out.back();
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) throw ConfigError(where + "expected key=value");
        if (!cur) throw ConfigError(where + "key outside any [service] section");
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (key == "command") cur->command = val;
        else if (key == "depends") {
            std::replace(val.begin(), val.end(), ',', ' ');
            std::istringstream is(val);
            std::string d;
            while (is >> d) cur->depends.push_back(d);
        } else if (key == "restart") {
            if (val == "never") cur->restart = RestartPolicy::Never;
            else if (val == "on-failure") cur->restart = RestartPolicy::OnFailure;
            else if (val == "always") cur->restart = RestartPolicy::Always;
            else throw ConfigError(where + "restart must be never|on-failure|always");
        } else if (key == "max_restarts") cur->maxRestarts = toInt(val, ln, key);
        else if (key == "backoff_ms") cur->backoffMs = toInt(val, ln, key);
        else if (key == "ready_delay_ms") cur->readyDelayMs = toInt(val, ln, key);
        else if (key == "stop_timeout_ms") cur->stopTimeoutMs = toInt(val, ln, key);
        else throw ConfigError(where + "unknown key '" + key + "'");
    }
    for (auto& s : out) {
        if (s.command.empty()) throw ConfigError("service '" + s.name + "' has no command");
        splitArgs(s.command);  // validates quoting
    }
    if (out.empty()) throw ConfigError("no services defined");
    return out;
}

std::vector<ServiceConfig> loadConfigFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw ConfigError("cannot open config file: " + path);
    return parseConfig(f);
}

}  // namespace pp
