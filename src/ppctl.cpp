// ppctl: tiny client for the ProcessPilot control socket.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string sock = getenv("PROCESSPILOT_SOCK") ? getenv("PROCESSPILOT_SOCK") : "/tmp/processpilot.sock";
    int i = 1;
    if (argc > 2 && std::string(argv[1]) == "-s") { sock = argv[2]; i = 3; }
    if (i >= argc) {
        fprintf(stderr, "usage: ppctl [-s socket] status|start <svc|all>|stop <svc|all>|restart <svc|all>|logs [n]|shutdown\n");
        return 2;
    }
    std::string cmd;
    for (; i < argc; ++i) cmd += (cmd.empty() ? "" : " ") + std::string(argv[i]);
    cmd += "\n";

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock.size() >= sizeof addr.sun_path) { fprintf(stderr, "socket path too long\n"); return 1; }
    strcpy(addr.sun_path, sock.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        fprintf(stderr, "ppctl: cannot connect to %s (is processpilot running?)\n", sock.c_str());
        return 1;
    }
    if (write(fd, cmd.data(), cmd.size()) < 0) { perror("write"); return 1; }
    char buf[4096];
    ssize_t n;
    int rc = 0;
    bool first = true;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (first && n >= 5 && strncmp(buf, "error", 5) == 0) rc = 1;
        first = false;
        fwrite(buf, 1, n, stdout);
    }
    close(fd);
    return rc;
}
