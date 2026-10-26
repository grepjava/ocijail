#include <sys/types.h>
#include <sys/rctl.h>
#include <cerrno>
#include <system_error>

#include "ocijail/main.h"
#include "ocijail/rctl.h"

using nlohmann::json;

namespace ocijail {

namespace {

const json* member(const json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return nullptr;
    }
    return &obj[key];
}

std::optional<int64_t> integer(const json& obj,
                               const char* key,
                               const std::string& name) {
    auto value = member(obj, key);
    if (!value) {
        return std::nullopt;
    }
    if (!value->is_number_integer()) {
        malformed_config(name + " must be an integer");
    }
    return value->get<int64_t>();
}

// rctl throttles the whole jail rather than each device, so use the lowest
// rate given for any device.
std::optional<uint64_t> throttle(const json& block_io, const char* key) {
    auto devices = member(block_io, key);
    if (!devices) {
        return std::nullopt;
    }
    auto name = std::string{"linux.resources.blockIO."} + key;
    if (!devices->is_array()) {
        malformed_config(name + " must be an array");
    }
    std::optional<uint64_t> res;
    for (auto& device : *devices) {
        auto rate = integer(device, "rate", name + " rate");
        if (!rate || *rate < 0) {
            malformed_config(name + " must have a non-negative rate");
        }
        if (!res || uint64_t(*rate) < *res) {
            res = *rate;
        }
    }
    return res;
}

void rctl_error(const std::string& what) {
    if (errno == ENOSYS) {
        throw std::runtime_error(
            what + ": resource limits need kern.racct.enable=1 in "
                   "/boot/loader.conf");
    }
    throw std::system_error{errno, std::system_category(), what};
}

}  // namespace

resource_limits resource_limits::parse(const json& resources) {
    resource_limits res;
    if (resources.is_null()) {
        return res;
    }
    if (!resources.is_object()) {
        malformed_config("linux.resources must be an object");
    }
    if (auto memory = member(resources, "memory")) {
        res.memory = integer(*memory, "limit", "linux.resources.memory.limit");
        res.memory_swap =
            integer(*memory, "swap", "linux.resources.memory.swap");
        if (res.memory && res.memory_swap && *res.memory > 0 &&
            *res.memory_swap >= 0 && *res.memory_swap < *res.memory) {
            throw std::runtime_error(
                "linux.resources.memory.swap must be at least "
                "linux.resources.memory.limit");
        }
    }
    if (auto pids = member(resources, "pids")) {
        res.pids = integer(*pids, "limit", "linux.resources.pids.limit");
    }
    if (auto cpu = member(resources, "cpu")) {
        res.cpu_quota = integer(*cpu, "quota", "linux.resources.cpu.quota");
        auto period = integer(*cpu, "period", "linux.resources.cpu.period");
        if (period) {
            if (*period < 0) {
                malformed_config(
                    "linux.resources.cpu.period must not be negative");
            }
            res.cpu_period = *period;
        }
    }
    if (auto block_io = member(resources, "blockIO")) {
        res.read_bps = throttle(*block_io, "throttleReadBpsDevice");
        res.write_bps = throttle(*block_io, "throttleWriteBpsDevice");
        res.read_iops = throttle(*block_io, "throttleReadIOPSDevice");
        res.write_iops = throttle(*block_io, "throttleWriteIOPSDevice");
    }
    return res;
}

std::vector<std::string> resource_limits::rules() const {
    std::vector<std::string> res;
    // As with the Linux OOM killer, a process which takes the container
    // over its memory limit is killed. A deny rule wouldn't stop it, since
    // memory becomes resident when it is used rather than allocated.
    if (memory && *memory > 0) {
        res.push_back("memoryuse:sigkill=" + std::to_string(*memory));
    }
    // memory.swap is the limit for memory and swap together.
    if (memory_swap && *memory_swap >= 0) {
        auto swap = *memory_swap;
        if (memory && *memory > 0) {
            swap -= *memory;
        }
        res.push_back("swapuse:deny=" + std::to_string(swap));
    }
    if (pids && *pids > 0) {
        res.push_back("maxproc:deny=" + std::to_string(*pids));
    }
    // pcpu is a percentage of one CPU.
    if (cpu_quota && *cpu_quota > 0) {
        uint64_t period = cpu_period && *cpu_period > 0 ? *cpu_period : 100000;
        auto pcpu = (uint64_t(*cpu_quota) * 100 + period - 1) / period;
        res.push_back("pcpu:deny=" + std::to_string(pcpu));
    }
    for (auto [resource, value] : {std::pair{"readbps", read_bps},
                                   std::pair{"writebps", write_bps},
                                   std::pair{"readiops", read_iops},
                                   std::pair{"writeiops", write_iops}}) {
        if (value) {
            res.push_back(std::string{resource} +
                          ":throttle=" + std::to_string(*value));
        }
    }
    return res;
}

void set_resource_limits(const std::string& jail_name,
                         const resource_limits& limits) {
    remove_resource_limits(jail_name);
    for (auto& rule : limits.rules()) {
        auto full_rule = "jail:" + jail_name + ":" + rule;
        if (::rctl_add_rule(
                full_rule.c_str(), full_rule.size() + 1, nullptr, 0) < 0) {
            auto saved_errno = errno;
            remove_resource_limits(jail_name);
            errno = saved_errno;
            rctl_error("adding rctl rule " + full_rule);
        }
    }
}

void remove_resource_limits(const std::string& jail_name) {
    auto filter = "jail:" + jail_name;
    if (::rctl_remove_rule(filter.c_str(), filter.size() + 1, nullptr, 0) <
        0) {
        // ESRCH means there were no rules, and ENOSYS that racct is off, so
        // there can't be any.
        if (errno != ESRCH && errno != ENOSYS) {
            rctl_error("removing rctl rules for " + filter);
        }
    }
}

std::map<std::string, uint64_t> resource_usage(const std::string& jail_name) {
    auto filter = "jail:" + jail_name;
    std::vector<char> buf(4096);
    for (;;) {
        if (::rctl_get_racct(filter.c_str(),
                             filter.size() + 1,
                             buf.data(),
                             buf.size()) == 0) {
            break;
        }
        if (errno != ERANGE) {
            rctl_error("getting resource usage for " + filter);
        }
        buf.resize(buf.size() * 2);
    }

    // The usage is a comma separated list of resource=value
    std::map<std::string, uint64_t> res;
    std::string_view usage{buf.data()};
    while (!usage.empty()) {
        auto end = usage.find(',');
        auto item = usage.substr(0, end);
        auto eq = item.find('=');
        if (eq != std::string_view::npos) {
            res[std::string{item.substr(0, eq)}] =
                std::stoull(std::string{item.substr(eq + 1)});
        }
        if (end == std::string_view::npos) {
            break;
        }
        usage = usage.substr(end + 1);
    }
    return res;
}

}  // namespace ocijail
