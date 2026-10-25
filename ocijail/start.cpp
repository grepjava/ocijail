#include <fcntl.h>
#include <unistd.h>
#include <iostream>

#include "CLI/CLI.hpp"
#include "nlohmann/json.hpp"

#include "hook.h"
#include "start.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void start::init(main_app& app) {
    static start instance{app};
}

start::start(main_app& app) : app_(app) {
    auto sub =
        app.add_subcommand("start", "Start the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->final_callback([this] { run(); });
}

void start::run() {
    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();

    if (state["status"] != "created") {
        std::stringstream ss;
        ss << "start: container not in \"created\" state (currently "
           << state["status"] << ")";
        throw std::runtime_error(ss.str());
    }
    state["status"] = "running";
    state.save();

    auto& config_hooks = state["config"]["hooks"];
    hook::run_hooks(app_, config_hooks, "prestart", state);

    // Open the exec fifo before signalling the container so that nothing it
    // writes there is lost. Containers created by older versions don't have
    // one.
    auto exec_wait = state.get_state_dir() / "exec_wait";
    auto exec_wait_fd = ::open(exec_wait.c_str(), O_RDONLY | O_NONBLOCK);
    if (exec_wait_fd < 0 && errno != ENOENT) {
        throw std::system_error{
            errno, std::system_category(), "open exec fifo"};
    }

    auto start_wait = state.get_state_dir() / "start_wait";
    auto fd = ::open(start_wait.c_str(), O_RDWR);
    char ch = 0;
    if (fd < 0) {
        throw std::system_error{
            errno, std::system_category(), "open start fifo"};
    }
    auto n = ::write(fd, &ch, 1);
    if (n < 0) {
        throw std::system_error{
            errno, std::system_category(), "write to start fifo"};
    }
    ::close(fd);

    // Wait for the container to execute its command. The fifo reaches EOF
    // when it does, or if it exits; otherwise it reports an error.
    if (exec_wait_fd >= 0) {
        if (::fcntl(exec_wait_fd, F_SETFL, 0) < 0) {
            throw std::system_error{
                errno, std::system_category(), "setting exec fifo flags"};
        }
        std::string msg;
        char buf[512];
        ssize_t n;
        while ((n = ::read(exec_wait_fd, buf, sizeof(buf))) != 0) {
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error{
                    errno, std::system_category(), "read from exec fifo"};
            }
            msg.append(buf, n);
        }
        ::close(exec_wait_fd);
        if (!msg.empty()) {
            throw std::runtime_error("start: " + msg);
        }
    }

    hook::run_hooks(app_, config_hooks, "poststart", state);
}

}  // namespace ocijail
