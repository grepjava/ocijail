#include <charconv>
#include <cmath>
#include <iostream>

#include "nlohmann/json.hpp"

#include "rctl.h"
#include "update.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

namespace {

// Parse a memory size, which may have a unit suffix such as 512k or 1.5G,
// as crun and runc do.
std::string parse_memory_size(const std::string& opt,
                              const std::string& value) {
    if (value == "-1") {
        return value;
    }
    auto invalid = [&] {
        throw std::runtime_error("update: invalid value for --" + opt + ": " +
                                 value);
    };
    std::size_t end = 0;
    double size = 0;
    try {
        size = std::stod(value, &end);
    } catch (const std::exception&) {
        invalid();
    }
    if (value[0] == '-' || value[0] == '+' || size < 0) {
        invalid();
    }
    std::string suffix = value.substr(end);
    if (!suffix.empty() && suffix[0] == ' ') {
        suffix = suffix.substr(1);
    }
    for (auto& c : suffix) {
        c = std::tolower(c);
    }
    static const std::map<std::string, double> units = {
        {"", 1},
        {"b", 1},
        {"k", 1ull << 10},
        {"m", 1ull << 20},
        {"g", 1ull << 30},
        {"t", 1ull << 40},
        {"p", 1ull << 50},
    };
    double mul = 0;
    for (auto& [unit, m] : units) {
        if (suffix == unit ||
            (unit.size() == 1 && unit != "b" &&
             (suffix == unit + "b" || suffix == unit + "ib"))) {
            mul = m;
        }
    }
    if (mul == 0 || size * mul >= 9223372036854775808.0) {
        invalid();
    }
    return std::to_string(int64_t(size * mul));
}

std::string check_integer(const std::string& opt, const std::string& value) {
    int64_t n;
    auto [ptr, ec] =
        std::from_chars(value.data(), value.data() + value.size(), n);
    if (value.empty() || ec != std::errc{} ||
        ptr != value.data() + value.size()) {
        throw std::runtime_error("update: invalid value for --" + opt + ": " +
                                 value);
    }
    return value;
}

}  // namespace

void update::init(main_app& app) {
    static update instance{app};
}

update::update(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "update", "Update the resource limits of the container");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_option("--resources,-r",
                    resources_,
                    "Path to a file containing the resources to set, in the "
                    "form of linux.resources")
        ->check(CLI::ExistingFile);

    struct value_option {
        const char* option;
        const char* section;
        const char* name;
        bool memory;
        const char* help;
    };
    static const value_option options[] = {
        {"memory", "memory", "limit", true, "Memory limit"},
        {"memory-swap",
         "memory",
         "swap",
         true,
         "Limit for memory and swap together"},
        {"pids-limit",
         "pids",
         "limit",
         false,
         "Maximum number of processes in the container"},
        {"cpu-quota",
         "cpu",
         "quota",
         false,
         "CPU time allowed in each period, in microseconds"},
        {"cpu-period", "cpu", "period", false, "CPU period, in microseconds"},
    };
    for (auto& opt : options) {
        sub->add_option_function<std::string>(
            std::string{"--"} + opt.option,
            [this, &opt](const std::string& value) {
                values_[{opt.section, opt.name}] =
                    opt.memory ? parse_memory_size(opt.option, value)
                               : check_integer(opt.option, value);
            },
            opt.help);
    }
    // crun's other options control cgroup features which have no
    // equivalent here.
    for (auto opt : {"blkio-weight",
                     "cpu-share",
                     "cpu-rt-period",
                     "cpu-rt-runtime",
                     "cpu-burst",
                     "cpu-idle",
                     "cpuset-cpus",
                     "cpuset-mems",
                     "kernel-memory",
                     "kernel-memory-tcp",
                     "memory-reservation",
                     "l3-cache-schema",
                     "mem-bw-schema"}) {
        sub->add_option_function<std::string>(
               std::string{"--"} + opt,
               [this, opt](const std::string&) {
                   unsupported_.push_back(std::string{"--"} + opt);
               },
               "Not supported")
            ->group("");
    }
    sub->final_callback([this] { run(); });
}

void update::run() {
    if (!unsupported_.empty()) {
        throw std::runtime_error("update: " + unsupported_[0] +
                                 " is not supported");
    }

    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();
    state.check_status();
    if (state["status"] == "stopped") {
        throw std::runtime_error("update: container " + id_ + " is stopped");
    }

    auto& config = state["config"];
    json resources = json::object();
    if (resources_) {
        std::ifstream{*resources_} >> resources;
    } else if (config.contains("linux") && config["linux"].is_object() &&
               config["linux"].contains("resources") &&
               config["linux"]["resources"].is_object()) {
        resources = config["linux"]["resources"];
    }
    for (auto& [key, value] : values_) {
        auto& [section, name] = key;
        resources[section][name] = std::stoll(value);
    }

    auto limits = resource_limits::parse(resources);
    std::string jail_name{id_};
    if (state.contains("parent_jail")) {
        jail_name = state["parent_jail"].get<std::string>() + "." + jail_name;
    }
    set_resource_limits(jail_name, limits);

    if (!config.contains("linux") || !config["linux"].is_object()) {
        config["linux"] = json::object();
    }
    config["linux"]["resources"] = resources;
    state.save();
}

}  // namespace ocijail
