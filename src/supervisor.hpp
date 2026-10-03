#pragma once
#include <sys/types.h>

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"

namespace pp {

using Clock = std::chrono::steady_clock;

enum class State { Stopped, Running, Backoff, Stopping, Failed };
const char* toString(State s);

struct Service {
    ServiceConfig cfg;
    State state = State::Stopped;
    bool desiredUp = true;       // what the operator wants
    bool restartPending = false; // restart once fully stopped (restart cmd / dependency cascade)
    bool killSent = false;
    pid_t pid = -1;
    int restarts = 0;            // consecutive unexpected restarts
    int lastStatus = 0;
    bool everExited = false;
    Clock::time_point startedAt{}, nextStart{}, killDeadline{};
};

struct Options {
    std::string sockPath = "/tmp/processpilot.sock";
    std::string logDir;      // per-service stdout/stderr logs; empty = inherit
};

class Supervisor {
public:
    Supervisor(std::vector<ServiceConfig> cfgs, Options opt);
    ~Supervisor();
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    int run();  // blocks until shutdown completes
    std::string handleCommand(const std::string& line);

private:
    void setupSignals();
    int setupSocket();
    void drainSignals();
    void serveClient();
    void reapChildren();
    void onExit(pid_t pid, int status);
    void tick();
    bool spawn(Service& s);
    bool depsReady(const Service& s, Clock::time_point now) const;
    bool hasAliveDependent(const std::string& name) const;
    bool anyAlive() const;
    void beginShutdown();
    void applyAction(const std::string& cmd, const std::string& name);
    std::vector<std::string> closure(const std::string& name, bool dependents) const;
    std::string statusTable() const;
    void event(const std::string& msg);

    Options opt_;
    std::map<std::string, Service> svcs_;
    std::map<std::string, std::vector<std::string>> dependents_;
    std::vector<std::string> order_;  // dependencies first
    std::deque<std::string> journal_;
    bool shuttingDown_ = false;
    int sigFd_ = -1, listenFd_ = -1;
};

}  // namespace pp
