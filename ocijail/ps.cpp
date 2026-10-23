#include <algorithm>
#include <iostream>

#include "nlohmann/json.hpp"

#include "jail.h"
#include "ps.h"

using nlohmann::json;

namespace ocijail {

void ps::init(main_app& app) {
    static ps instance{app};
}

ps::ps(main_app& app) : app_(app) {
    std::map<std::string, ps_format> formats{
        {"table", ps_format::PS_TABLE},
        {"json", ps_format::PS_JSON},
    };

    auto sub = app.add_subcommand(
        "ps", "Show the processes running in the container");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_option("--format,-f",
                    format_,
                    "output format: either table or json (default: table)")
        ->transform(CLI::CheckedTransformer(formats, CLI::ignore_case));
    sub->final_callback([this] { run(); });
}

void ps::run() {
    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();

    auto j = jail::find(int(state["jid"]));
    auto pids = j.processes();
    std::sort(pids.begin(), pids.end());

    if (format_ == ps_format::PS_TABLE) {
        std::cout << "PID\n";
        for (auto pid : pids) {
            std::cout << pid << "\n";
        }
    } else {
        std::cout << json(pids) << "\n";
    }
}

}  // namespace ocijail
