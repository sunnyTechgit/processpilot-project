#include "graph.hpp"
#include <map>

namespace pp {

std::vector<std::string> topoSort(const std::vector<ServiceConfig>& cfgs) {
    const size_t n = cfgs.size();
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < n; ++i) idx[cfgs[i].name] = i;

    std::vector<int> indeg(n, 0);
    std::vector<std::vector<size_t>> out(n);
    for (size_t i = 0; i < n; ++i) {
        for (const auto& d : cfgs[i].depends) {
            auto it = idx.find(d);
            if (it == idx.end())
                throw GraphError("service '" + cfgs[i].name + "' depends on unknown service '" + d + "'");
            if (it->second == i) throw GraphError("service '" + d + "' depends on itself");
            out[it->second].push_back(i);
            indeg[i]++;
        }
    }
    std::vector<bool> done(n, false);
    std::vector<std::string> order;
    for (size_t step = 0; step < n; ++step) {
        size_t pick = n;
        for (size_t i = 0; i < n; ++i)
            if (!done[i] && indeg[i] == 0) { pick = i; break; }
        if (pick == n) {
            std::string rem;
            for (size_t i = 0; i < n; ++i)
                if (!done[i]) rem += (rem.empty() ? "" : ", ") + cfgs[i].name;
            throw GraphError("dependency cycle among: " + rem);
        }
        done[pick] = true;
        order.push_back(cfgs[pick].name);
        for (size_t j : out[pick]) indeg[j]--;
    }
    return order;
}

}  // namespace pp
