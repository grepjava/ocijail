#include <iostream>
#include <thread>

#include "nlohmann/json.hpp"

#include "events.h"
#include "rctl.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

namespace {

// Parse a duration such as 5s, 500ms or 1.5m, as runc does.
std::chrono::milliseconds parse_interval(const std::string& value) {
    static const std::pair<const char*, double> units[] = {
        {"ms", 1}, {"s", 1000}, {"m", 60000}, {"h", 3600000}};
    std::size_t end = 0;
    double n = -1;
    try {
        n = std::stod(value, &end);
    } catch (const std::exception&) {
    }
    auto suffix = value.substr(end);
    for (auto [unit, ms] : units) {
        if (n > 0 && suffix == unit) {
            return std::chrono::milliseconds{int64_t(n * ms)};
        }
    }
    throw std::runtime_error("events: invalid interval " + value);
}

// The container's statistics in the form runc reports them, as far as
// racct can provide them.
json stats(runtime_state& state, const std::string& jail_name) {
    auto usage = resource_usage(jail_name);
    resource_limits limits;
    auto& config = state["config"];
    if (config.contains("linux") && config["linux"].is_object() &&
        config["linux"].contains("resources")) {
        limits = resource_limits::parse(config["linux"]["resources"]);
    }
    auto limit = [](const std::optional<int64_t>& value) -> uint64_t {
        return value && *value > 0 ? *value : 0;
    };

    json data;
    // racct counts CPU time in seconds, runc in nanoseconds
    data["cpu"]["usage"]["total"] = usage["cputime"] * 1000000000ull;
    data["memory"]["usage"]["usage"] = usage["memoryuse"];
    data["memory"]["usage"]["limit"] = limit(limits.memory);
    uint64_t swap_limit = 0;
    if (limits.memory_swap && *limits.memory_swap >= 0) {
        swap_limit = *limits.memory_swap - limit(limits.memory);
    }
    data["memory"]["swap"]["usage"] = usage["swapuse"];
    data["memory"]["swap"]["limit"] = swap_limit;
    data["pids"]["current"] = usage["maxproc"];
    data["pids"]["limit"] = limit(limits.pids);

    json event;
    event["type"] = "stats";
    event["id"] = state.get_id();
    event["data"] = data;
    return event;
}

}  // namespace

void events::init(main_app& app) {
    static events instance{app};
}

events::events(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "events", "Report the resource usage of the container");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_flag("--stats", stats_, "Report the usage once and exit");
    sub->add_option_function<std::string>(
        "--interval",
        [this](const std::string& value) { interval_ = parse_interval(value); },
        "How often to report the usage (default: 5s)");
    sub->final_callback([this] { run(); });
}

void events::run() {
    auto state = app_.get_runtime_state(id_);
    for (bool first = true;; first = false) {
        {
            auto lk = state.lock();
            state.load();
            state.check_status();
            // Stop reporting once the container stops.
            if (state["status"] == "stopped") {
                if (first) {
                    throw std::runtime_error("events: container " + id_ +
                                             " is stopped");
                }
                return;
            }
        }
        std::string jail_name{id_};
        if (state.contains("parent_jail")) {
            jail_name =
                state["parent_jail"].get<std::string>() + "." + jail_name;
        }
        std::cout << stats(state, jail_name) << std::endl;
        if (stats_) {
            return;
        }
        std::this_thread::sleep_for(interval_);
    }
}

}  // namespace ocijail
