#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace ocijail {

// Resource limits for a container, enforced with rctl rules on its jail.
// These are read from the portable part of the linux.resources object,
// which is what crun and runc enforce with cgroups.
struct resource_limits {
    // Parse a linux.resources object, ignoring the parts which have no
    // FreeBSD equivalent.
    static resource_limits parse(const nlohmann::json& resources);

    // The rctl rules for these limits, without the subject, e.g.
    // "maxproc:deny=100".
    std::vector<std::string> rules() const;

    bool empty() const { return rules().empty(); }

    std::optional<int64_t> memory;
    std::optional<int64_t> memory_swap;
    std::optional<int64_t> pids;
    std::optional<int64_t> cpu_quota;
    std::optional<uint64_t> cpu_period;
    std::optional<uint64_t> read_bps;
    std::optional<uint64_t> write_bps;
    std::optional<uint64_t> read_iops;
    std::optional<uint64_t> write_iops;
};

// Replace the rctl rules for the named jail with the given limits.
void set_resource_limits(const std::string& jail_name,
                         const resource_limits& limits);

// Remove all rctl rules for the named jail.
void remove_resource_limits(const std::string& jail_name);

// Return the jail's resource usage, e.g. {"memoryuse": 1234, ...}.
std::map<std::string, uint64_t> resource_usage(const std::string& jail_name);

}  // namespace ocijail
