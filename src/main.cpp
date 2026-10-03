#include <cstdio>
#include <cstring>
#include <string>

#include "config.hpp"
#include "graph.hpp"
#include "supervisor.hpp"

static void usage() {
    fprintf(stderr,
            "usage: processpilot -c <config> [-s <socket>] [-l <logdir>] [--check]\n"
            "  --check   validate config and print start order, then exit\n");
}

int main(int argc, char** argv) {
    std::string conf;
    bool check = false;
    pp::Options opt;
    if (const char* e = getenv("PROCESSPILOT_SOCK")) opt.sockPath = e;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : (usage(), exit(2), ""); };
        if (a == "-c") conf = next();
        else if (a == "-s") opt.sockPath = next();
        else if (a == "-l") opt.logDir = next();
        else if (a == "--check") check = true;
        else { usage(); return 2; }
    }
    if (conf.empty()) { usage(); return 2; }
    try {
        auto cfgs = pp::loadConfigFile(conf);
        auto order = pp::topoSort(cfgs);
        if (check) {
            printf("config OK, start order:");
            for (auto& n : order) printf(" %s", n.c_str());
            printf("\n");
            return 0;
        }
        pp::Supervisor sup(std::move(cfgs), opt);
        return sup.run();
    } catch (const std::exception& e) {
        fprintf(stderr, "processpilot: %s\n", e.what());
        return 1;
    }
}
