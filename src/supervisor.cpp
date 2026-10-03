#include "supervisor.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>

#include "graph.hpp"

namespace pp {

using std::string;
using namespace std::chrono;

static const int kStableMs = 10000;   // running this long resets the restart counter
static const int kMaxBackoffMs = 30000;

const char* toString(State s) {
    switch (s) {
        case State::Stopped: return "Stopped";
        case State::Running: return "Running";
        case State::Backoff: return "Backoff";
        case State::Stopping: return "Stopping";
        case State::Failed: return "Failed";
    }
    return "?";
}

static string describeStatus(int st) {
    if (WIFEXITED(st)) return "exited with code " + std::to_string(WEXITSTATUS(st));
    if (WIFSIGNALED(st)) return "killed by signal " + std::to_string(WTERMSIG(st));
    return "status " + std::to_string(st);
}

Supervisor::Supervisor(std::vector<ServiceConfig> cfgs, Options opt) : opt_(std::move(opt)) {
    order_ = topoSort(cfgs);  // throws GraphError on cycles / unknown deps
    for (auto& c : cfgs) {
        Service s;
        s.cfg = c;
        svcs_.emplace(c.name, std::move(s));
    }
    for (auto& kv : svcs_)
        for (auto& d : kv.second.cfg.depends) dependents_[d].push_back(kv.first);
}

Supervisor::~Supervisor() {
    if (sigFd_ >= 0) close(sigFd_);
    if (listenFd_ >= 0) { close(listenFd_); unlink(opt_.sockPath.c_str()); }
}

void Supervisor::event(const string& msg) {
    char ts[32];
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    string line = string(ts) + " " + msg;
    fprintf(stderr, "[processpilot] %s\n", line.c_str());
    journal_.push_back(line);
    if (journal_.size() > 200) journal_.pop_front();
}

std::vector<string> Supervisor::closure(const string& name, bool dependents) const {
    std::vector<string> out{name};
    std::set<string> seen{name};
    for (size_t i = 0; i < out.size(); ++i) {
        std::vector<string> next;
        if (dependents) {
            auto it = dependents_.find(out[i]);
            if (it != dependents_.end()) next = it->second;
        } else {
            next = svcs_.at(out[i]).cfg.depends;
        }
        for (auto& n : next)
            if (seen.insert(n).second) out.push_back(n);
    }
    return out;
}

bool Supervisor::depsReady(const Service& s, Clock::time_point now) const {
    for (auto& d : s.cfg.depends) {
        const Service& dep = svcs_.at(d);
        if (dep.state != State::Running) return false;
        if (now - dep.startedAt < milliseconds(dep.cfg.readyDelayMs)) return false;
    }
    return true;
}

bool Supervisor::hasAliveDependent(const string& name) const {
    auto it = dependents_.find(name);
    if (it == dependents_.end()) return false;
    for (auto& d : it->second) {
        State st = svcs_.at(d).state;
        if (st == State::Running || st == State::Stopping) return true;
    }
    return false;
}

bool Supervisor::anyAlive() const {
    for (auto& kv : svcs_)
        if (kv.second.state == State::Running || kv.second.state == State::Stopping) return true;
    return false;
}

bool Supervisor::spawn(Service& s) {
    std::vector<string> args = splitArgs(s.cfg.command);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        event("fork failed for '" + s.cfg.name + "': " + strerror(errno));
        return false;
    }
    if (pid == 0) {
        setpgid(0, 0);
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        signal(SIGPIPE, SIG_DFL);
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) { dup2(in, 0); close(in); }
        if (!opt_.logDir.empty()) {
            string path = opt_.logDir + "/" + s.cfg.name + ".log";
            int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        }
        execvp(argv[0], argv.data());
        const char* m = "processpilot: exec failed\n";
        ssize_t r = write(2, m, strlen(m));
        (void)r;
        _exit(127);
    }
    setpgid(pid, pid);  // avoid race with child's own setpgid
    s.pid = pid;
    s.state = State::Running;
    s.startedAt = Clock::now();
    s.killSent = false;
    event("started '" + s.cfg.name + "' pid=" + std::to_string(pid));
    return true;
}

void Supervisor::onExit(pid_t pid, int status) {
    Service* sp = nullptr;
    for (auto& kv : svcs_)
        if (kv.second.pid == pid) { sp = &kv.second; break; }
    if (!sp) return;  // not ours (grandchild reparented to us)
    Service& s = *sp;
    auto now = Clock::now();
    const bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const auto ranMs = duration_cast<milliseconds>(now - s.startedAt).count();
    s.pid = -1;
    s.lastStatus = status;
    s.everExited = true;

    if (s.state == State::Stopping || !s.desiredUp) {
        s.state = State::Stopped;
        event("'" + s.cfg.name + "' stopped (" + describeStatus(status) + ")");
        if (s.restartPending) {
            s.restartPending = false;
            s.desiredUp = !shuttingDown_;
            s.restarts = 0;
        }
        return;
    }

    event("'" + s.cfg.name + "' " + describeStatus(status) + " after " + std::to_string(ranMs) + "ms");
    if (ranMs >= kStableMs) s.restarts = 0;

    const bool wantRestart = s.cfg.restart == RestartPolicy::Always ||
                             (s.cfg.restart == RestartPolicy::OnFailure && !success);
    if (wantRestart) {
        s.restarts++;
        if (s.restarts > s.cfg.maxRestarts) {
            s.state = State::Failed;
            event("'" + s.cfg.name + "' FAILED: exceeded max_restarts=" + std::to_string(s.cfg.maxRestarts));
        } else {
            long long delay = static_cast<long long>(s.cfg.backoffMs) << std::min(s.restarts - 1, 16);
            delay = std::min<long long>(delay, kMaxBackoffMs);
            s.state = State::Backoff;
            s.nextStart = now + milliseconds(delay);
            event("'" + s.cfg.name + "' restart #" + std::to_string(s.restarts) + " in " +
                  std::to_string(delay) + "ms");
        }
    } else {
        s.state = success ? State::Stopped : State::Failed;
        s.desiredUp = false;
    }

    // Self-healing cascade: dependents of a dead service are bounced so they
    // come back only after the dependency is Running again.
    for (auto& n : closure(s.cfg.name, true)) {
        if (n == s.cfg.name) continue;
        Service& d = svcs_.at(n);
        if (d.state == State::Running) {
            d.desiredUp = false;
            d.restartPending = true;
            event("'" + n + "' will restart because dependency '" + s.cfg.name + "' went down");
        }
    }
}

void Supervisor::reapChildren() {
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) onExit(pid, st);
}

void Supervisor::tick() {
    auto now = Clock::now();
    // Stop pass: dependents first (reverse topological order).
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        Service& s = svcs_.at(*it);
        if (s.state == State::Running && !s.desiredUp && !hasAliveDependent(s.cfg.name)) {
            killpg(s.pid, SIGTERM);
            s.state = State::Stopping;
            s.killDeadline = now + milliseconds(s.cfg.stopTimeoutMs);
            s.killSent = false;
            event("stopping '" + s.cfg.name + "' (SIGTERM)");
        }
        if (s.state == State::Stopping && !s.killSent && now >= s.killDeadline) {
            killpg(s.pid, SIGKILL);
            s.killSent = true;
            event("'" + s.cfg.name + "' ignored SIGTERM, sent SIGKILL");
        }
    }
    // Start pass: dependencies first.
    for (auto& name : order_) {
        Service& s = svcs_.at(name);
        if (s.state == State::Backoff && !s.desiredUp) s.state = State::Stopped;
        bool due = s.state == State::Stopped || (s.state == State::Backoff && now >= s.nextStart);
        if (s.desiredUp && due && depsReady(s, now)) {
            if (!spawn(s)) {
                s.state = State::Backoff;
                s.nextStart = now + seconds(1);
            }
        }
    }
}

void Supervisor::beginShutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    event("shutdown requested");
    for (auto& kv : svcs_) {
        kv.second.desiredUp = false;
        kv.second.restartPending = false;
    }
}

void Supervisor::applyAction(const string& cmd, const string& name) {
    event("ctl: " + cmd + " " + name);
    if (cmd == "start") {
        for (auto& n : closure(name, false)) {
            Service& s = svcs_.at(n);
            if (s.state == State::Failed) { s.state = State::Stopped; }
            if (s.state != State::Running) s.restarts = 0;
            s.desiredUp = true;
            s.restartPending = false;
        }
    } else if (cmd == "stop") {
        for (auto& n : closure(name, true)) {
            Service& s = svcs_.at(n);
            s.desiredUp = false;
            s.restartPending = false;
        }
    } else if (cmd == "restart") {
        for (auto& n : closure(name, true)) {
            Service& s = svcs_.at(n);
            if (s.state == State::Running || s.state == State::Stopping) {
                s.desiredUp = false;
                s.restartPending = true;
            } else if (n == name || s.desiredUp) {
                s.state = State::Stopped;
                s.restarts = 0;
                s.desiredUp = true;
                s.restartPending = false;
            }
        }
    }
}

static string fmtUptime(long long sec) {
    std::ostringstream o;
    if (sec >= 3600) o << sec / 3600 << "h" << (sec % 3600) / 60 << "m";
    else if (sec >= 60) o << sec / 60 << "m" << sec % 60 << "s";
    else o << sec << "s";
    return o.str();
}

std::string Supervisor::statusTable() const {
    std::ostringstream o;
    auto now = Clock::now();
    o << std::left << std::setw(16) << "NAME" << std::setw(10) << "STATE" << std::setw(8) << "PID"
      << std::setw(10) << "RESTARTS" << std::setw(9) << "UPTIME" << "DEPENDS\n";
    for (auto& name : order_) {
        const Service& s = svcs_.at(name);
        string st = toString(s.state);
        if (s.state == State::Stopped && s.desiredUp) st = "Waiting";
        string up = "-", deps = "-";
        if (s.state == State::Running)
            up = fmtUptime(duration_cast<seconds>(now - s.startedAt).count());
        if (!s.cfg.depends.empty()) {
            deps.clear();
            for (auto& d : s.cfg.depends) deps += (deps.empty() ? "" : ",") + d;
        }
        o << std::left << std::setw(16) << name << std::setw(10) << st << std::setw(8)
          << (s.pid > 0 ? std::to_string(s.pid) : "-") << std::setw(10) << s.restarts << std::setw(9) << up
          << deps << "\n";
    }
    return o.str();
}

string Supervisor::handleCommand(const string& line) {
    std::istringstream is(line);
    string cmd, arg;
    is >> cmd >> arg;
    if (cmd == "status") return statusTable();
    if (cmd == "logs") {
        size_t n = 20;
        if (!arg.empty()) { try { n = std::stoul(arg); } catch (...) {} }
        std::ostringstream o;
        size_t start = journal_.size() > n ? journal_.size() - n : 0;
        for (size_t i = start; i < journal_.size(); ++i) o << journal_[i] << "\n";
        return o.str();
    }
    if (cmd == "shutdown") { beginShutdown(); return "shutting down\n"; }
    if (cmd == "start" || cmd == "stop" || cmd == "restart") {
        if (arg.empty()) return "error: " + cmd + " needs a service name or 'all'\n";
        if (shuttingDown_) return "error: shutting down\n";
        std::vector<string> targets;
        if (arg == "all") targets = order_;
        else if (svcs_.count(arg)) targets = {arg};
        else return "error: unknown service '" + arg + "'\n";
        for (auto& t : targets) applyAction(cmd, t);
        return "ok\n";
    }
    return "commands: status | start <svc|all> | stop <svc|all> | restart <svc|all> | logs [n] | shutdown\n";
}

void Supervisor::setupSignals() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    signal(SIGPIPE, SIG_IGN);
    sigFd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

int Supervisor::setupSocket() {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (opt_.sockPath.size() >= sizeof addr.sun_path) {
        fprintf(stderr, "socket path too long\n");
        return -1;
    }
    strcpy(addr.sun_path, opt_.sockPath.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(opt_.sockPath.c_str());
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(fd, 8) < 0) {
        perror("processpilot: bind/listen");
        close(fd);
        return -1;
    }
    chmod(opt_.sockPath.c_str(), 0600);
    return fd;
}

void Supervisor::drainSignals() {
    signalfd_siginfo si;
    while (read(sigFd_, &si, sizeof si) == static_cast<ssize_t>(sizeof si)) {
        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) beginShutdown();
    }
}

void Supervisor::serveClient() {
    int c = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) return;
    timeval tv{0, 500000};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    string line;
    char buf[256];
    ssize_t n;
    while (line.size() < 1024 && (n = read(c, buf, sizeof buf)) > 0) {
        line.append(buf, n);
        if (line.find('\n') != string::npos) break;
    }
    string resp = handleCommand(line);
    size_t off = 0;
    while (off < resp.size()) {
        ssize_t w = write(c, resp.data() + off, resp.size() - off);
        if (w <= 0) break;
        off += w;
    }
    close(c);
}

int Supervisor::run() {
    setupSignals();
    if (sigFd_ < 0) { perror("signalfd"); return 1; }
    listenFd_ = setupSocket();
    if (listenFd_ < 0) return 1;
    if (!opt_.logDir.empty()) mkdir(opt_.logDir.c_str(), 0755);
    event("supervisor up, " + std::to_string(svcs_.size()) + " services, socket " + opt_.sockPath);

    while (true) {
        pollfd fds[2] = {{sigFd_, POLLIN, 0}, {listenFd_, POLLIN, 0}};
        int r = poll(fds, 2, 100);
        if (r < 0 && errno != EINTR) break;
        if (fds[0].revents & POLLIN) drainSignals();
        if (fds[1].revents & POLLIN) serveClient();
        reapChildren();
        tick();
        if (shuttingDown_ && !anyAlive()) break;
    }
    event("supervisor exit");
    return 0;
}

}  // namespace pp
