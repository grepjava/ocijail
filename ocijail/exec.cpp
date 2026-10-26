#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <charconv>
#include <iostream>

#include "nlohmann/json.hpp"

#include "ocijail/exec.h"
#include "ocijail/jail.h"
#include "ocijail/process.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void exec::init(main_app& app) {
    static exec instance{app};
}

exec::exec(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "exec", "Execute a command in the container with the given id");
    sub->add_option("container-id", id_, "Unique identifier for the exec")
        ->required();
    // Anything after the container id is the command to execute, including
    // arguments which look like options.
    sub->prefix_command();
    sub->add_option(
           "--process", process_, "Path to a file containing the process json")
        ->check(CLI::ExistingPath);
    sub->add_option(
           "--console-socket",
           console_socket_,
           "Path to a socket which will receive the console pty descriptor")
        ->check(CLI::ExistingPath);
    sub->add_option(
        "--pid-file",
        pid_file_,
        "Path to a file where the container process id will be written");
    sub->add_flag("--tty,-t", tty_, "Allocate a pty for the exec process");
    sub->add_flag("--detach,-d",
                  detach_,
                  "Detach the command and execute in the background");
    sub->add_option("--preserve-fds",
                    preserve_fds_,
                    "Number of additional file descriptors for the container");
    sub->add_option(
        "--cwd", cwd_, "Current working directory for the exec process");
    sub->add_option(
        "--env,-e", env_, "Add an environment variable, in the form KEY=VALUE");
    sub->add_option("--user,-u",
                    user_,
                    "User and optionally group to run as, in the form "
                    "UID[:GID]");

    sub->final_callback([this, sub] {
        args_ = sub->remaining();
        run();
    });
}

static int parse_id(const std::string& s) {
    int id;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size() ||
        id < 0) {
        throw std::runtime_error("exec: invalid user " + s);
    }
    return id;
}

void exec::run() {
    // The parser leaves unrecognized options in the command instead of
    // reporting them, so check for them here.
    for (auto& arg : {id_, args_.empty() ? std::string{} : args_[0]}) {
        if (arg.starts_with("-")) {
            throw std::runtime_error("exec: unknown option " + arg);
        }
    }

    json process_json;
    if (process_) {
        if (!args_.empty()) {
            throw std::runtime_error(
                "exec: cannot use both --process and a command");
        }
        if (cwd_ || !env_.empty() || user_) {
            throw std::runtime_error(
                "exec: --cwd, --env and --user cannot be used with --process");
        }
        std::ifstream{*process_} >> process_json;
    } else {
        if (args_.empty()) {
            throw std::runtime_error(
                "exec: either --process or a command must be given");
        }
        // As with crun, start from the container's process, replacing its
        // arguments with the given command.
        auto state = app_.get_runtime_state(id_);
        auto lk = state.lock();
        state.load();
        process_json = state["config"]["process"];
        process_json["args"] = args_;
        process_json.erase("terminal");
        process_json.erase("consoleSize");
        if (cwd_) {
            process_json["cwd"] = *cwd_;
        }
        if (!env_.empty()) {
            if (!process_json["env"].is_array()) {
                process_json["env"] = json::array();
            }
            auto& env = process_json["env"];
            for (auto& var : env_) {
                // Replace any existing value for this variable.
                auto key = var.substr(0, var.find('=') + 1);
                auto it = std::find_if(env.begin(), env.end(), [&](auto& v) {
                    return v.is_string() &&
                           v.template get<std::string>().starts_with(key);
                });
                if (it != env.end()) {
                    *it = var;
                } else {
                    env.push_back(var);
                }
            }
        }
        if (user_) {
            auto pos = user_->find(':');
            auto& user = process_json["user"];
            if (!user.is_object()) {
                user = json::object();
            }
            user["uid"] = parse_id(user_->substr(0, pos));
            user["gid"] = pos == std::string::npos
                              ? 0
                              : parse_id(user_->substr(pos + 1));
            user.erase("additionalGids");
        }
    }
    if (tty_) {
        process_json["terminal"] = *tty_;
    }
    process proc{process_json, console_socket_, detach_, preserve_fds_};

    // Unit tests for config validation stop here.
    if (app_.get_test_mode() == test_mode::VALIDATION) {
        return;
    }

    auto state = app_.get_runtime_state(id_);
    auto lk = state.lock();
    state.load();
    state.check_status();

    auto j = state.find_jail();
    if (!j || (state["status"] != "created" && state["status"] != "running")) {
        throw std::runtime_error("exec: container " + id_ + " is not running");
    }

    if (detach_) {
        // Create a socket pair for coordinating create activities with
        // our child process.
        int create_sock[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, create_sock) < 0) {
            throw std::system_error{
                errno, std::system_category(), "error creating socket pair"};
        }

        // Detach from parent and send the pid which will perform the
        // exec (if requested).
        auto pid = ::fork();
        if (pid) {
            // Parent process - write to pid file if requested
            if (pid_file_) {
                std::ofstream{*pid_file_} << pid;
            }

            // Signal the child to validate that the container process can be
            // found.
            char ch = 1;
            auto n = ::write(create_sock[0], &ch, 1);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "write to exec create socket"};
            }

            // Read back the child's status - this is our exit status. The
            // child will have already written to stderr if necessary.
            char status;
            n = ::read(create_sock[0], &status, 1);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "read from exec create socket"};
            }
            ::exit(status);
        } else {
            // Setup the tty if requested
            auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();

            // Wait for our parent to signal us via the socket
            char ch;
            auto n = read(create_sock[1], &ch, 1);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "error reading from create socket"};
            }

            char status = 0;
            try {
                // Our part of exec: validate process args.

                // Enter the jail and set the requested working directory.
                proc.set_scheduler();
                j->attach();

                // Validate the process executable exists and can be executed
                proc.validate();
            } catch (const std::exception& e) {
                std::string_view msg{e.what()};
                ::write(2, msg.data(), msg.size());
                status = 1;
            }

            n = write(create_sock[1], &status, 1);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "error writing to exec create socket"};
            }
            ::close(create_sock[1]);

            // If validate failed, don't try to exec - it will fail and double
            // the error message reported to the user in a confusing way.
            if (status != 0) {
                ::exit(status);
            }

            // Run the process inside the jail
            proc.exec(stdin_fd, stdout_fd, stderr_fd);
        }
    } else {
        // Otherwise, just exec in this process
        auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();
        proc.set_scheduler();
        j->attach();
        proc.validate();
        proc.exec(stdin_fd, stdout_fd, stderr_fd);
    }
}

}  // namespace ocijail
