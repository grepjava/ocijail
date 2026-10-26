#include <signal.h>
#include <unistd.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "jail.h"
#include "pause.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void pause::init(main_app& app) {
    static pause pause_instance{app, false};
    static pause resume_instance{app, true};
}

pause::pause(main_app& app, bool resume) : app_(app), resume_(resume) {
    auto sub = resume_ ? app.add_subcommand(
                             "resume", "Resume the paused container")
                       : app.add_subcommand(
                             "pause",
                             "Suspend all processes in the container");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->final_callback([this] { run(); });
}

void pause::run() {
    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();
    state.check_status();

    auto expected = resume_ ? "paused" : "running";
    if (state["status"] != expected) {
        std::stringstream ss;
        ss << (resume_ ? "resume" : "pause") << ": container not in \""
           << expected << "\" state (currently " << state["status"] << ")";
        throw std::runtime_error(ss.str());
    }

    auto j = jail::find(int(state["jid"]));
    auto signal = [](pid_t pid, int sig) {
        if (::kill(pid, sig) < 0 && errno != ESRCH) {
            throw std::system_error(errno,
                                    std::system_category(),
                                    "sending signal to pid " +
                                        std::to_string(pid));
        }
    };
    if (resume_) {
        // Stopped processes can't fork, so one pass is enough.
        for (auto& p : j.process_info()) {
            signal(p.pid, SIGCONT);
        }
        state["status"] = "running";
    } else {
        // Without a freezer, stop the processes one by one. A process may
        // fork before it stops, so keep going until every process in the
        // jail has stopped.
        bool all_stopped = false;
        while (!all_stopped) {
            all_stopped = true;
            for (auto& p : j.process_info()) {
                if (!p.stopped) {
                    all_stopped = false;
                    signal(p.pid, SIGSTOP);
                }
            }
            if (!all_stopped) {
                ::usleep(1000);
            }
        }
        state["status"] = "paused";
    }
    state.save();
}

}  // namespace ocijail
