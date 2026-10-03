#pragma once
#include <stdexcept>
#include <string>
#include <vector>
#include "config.hpp"

namespace pp {
struct GraphError : std::runtime_error { using std::runtime_error::runtime_error; };

// Kahn's algorithm. Returns service names, dependencies first.
// Ties are broken by config order so the result is deterministic.
std::vector<std::string> topoSort(const std::vector<ServiceConfig>& cfgs);
}  // namespace pp
