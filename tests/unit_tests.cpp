// Minimal dependency-free unit tests (config parser, arg splitter, topological sort).
#include <cstdio>
#include <sstream>
#include "config.hpp"
#include "graph.hpp"

static int failed = 0, total = 0;
#define CHECK(c) do { ++total; if (!(c)) { ++failed; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define THROWS(expr, T) do { ++total; bool ok = false; try { expr; } catch (const T&) { ok = true; } \
    if (!ok) { ++failed; printf("FAIL %s:%d  expected %s\n", __FILE__, __LINE__, #T); } } while (0)

static std::vector<pp::ServiceConfig> parse(const std::string& s) { std::istringstream is(s); return pp::parseConfig(is); }

int main() {
    // splitArgs
    auto a = pp::splitArgs("echo 'hello world'  \"a b\" c");
    CHECK(a.size() == 4 && a[1] == "hello world" && a[2] == "a b" && a[3] == "c");
    THROWS(pp::splitArgs("echo 'oops"), pp::ConfigError);

    // parseConfig
    auto c = parse("# c\n[service:db]\ncommand=sleep 1\n[web]\ncommand = sleep 2\ndepends = db, cache\nrestart=always\nmax_restarts=3\nbackoff_ms=100\n");
    CHECK(c.size() == 2 && c[0].name == "db" && c[1].name == "web");
    CHECK(c[1].depends.size() == 2 && c[1].depends[1] == "cache");
    CHECK(c[1].restart == pp::RestartPolicy::Always && c[1].maxRestarts == 3 && c[1].backoffMs == 100);
    CHECK(c[0].restart == pp::RestartPolicy::OnFailure);  // default
    THROWS(parse("[a]\n"), pp::ConfigError);                       // no command
    THROWS(parse("[a]\ncommand=x\n[a]\ncommand=y\n"), pp::ConfigError);  // duplicate
    THROWS(parse("command=x\n"), pp::ConfigError);                 // outside section
    THROWS(parse("[a]\ncommand=x\nbogus=1\n"), pp::ConfigError);   // unknown key
    THROWS(parse("[a]\ncommand=x\nrestart=maybe\n"), pp::ConfigError);
    THROWS(parse("[a]\ncommand=x\nmax_restarts=-1\n"), pp::ConfigError);
    THROWS(parse(""), pp::ConfigError);

    // topoSort
    auto g = parse("[web]\ncommand=x\ndepends=api\n[api]\ncommand=x\ndepends=db,cache\n[db]\ncommand=x\n[cache]\ncommand=x\n");
    auto order = pp::topoSort(g);
    auto pos = [&](const char* n) { for (size_t i = 0; i < order.size(); ++i) if (order[i] == n) return (int)i; return -1; };
    CHECK(order.size() == 4);
    CHECK(pos("db") < pos("api") && pos("cache") < pos("api") && pos("api") < pos("web"));
    CHECK(order == pp::topoSort(g));  // deterministic
    THROWS(pp::topoSort(parse("[a]\ncommand=x\ndepends=b\n[b]\ncommand=x\ndepends=a\n")), pp::GraphError);  // cycle
    THROWS(pp::topoSort(parse("[a]\ncommand=x\ndepends=zzz\n")), pp::GraphError);  // unknown
    THROWS(pp::topoSort(parse("[a]\ncommand=x\ndepends=a\n")), pp::GraphError);    // self

    printf("%d/%d unit checks passed\n", total - failed, total);
    return failed ? 1 : 0;
}
