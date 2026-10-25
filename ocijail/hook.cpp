#include <err.h>
#include <poll.h>
#include <signal.h>
#include <sys/procdesc.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ocijail/hook.h"

extern "C" char** environ;

using nlohmann::json;

namespace ocijail {

hook::hook(const json& hook_config) {
    // We can assume that validate_hooks has ensured that this is well-formed
    path_ = hook_config["path"];

    if (hook_config.contains("args")) {
        auto& config_args = hook_config["args"];
        args_ = std::vector<std::string>();
        for (auto& arg : config_args) {
            args_->push_back(arg.get<std::string>());
        }
    }

    if (hook_config.contains("env")) {
        auto& config_env = hook_config["env"];
        env_ = std::vector<std::string>();
        for (auto& arg : config_env) {
            env_->push_back(arg.get<std::string>());
        }
    }

    if (hook_config.contains("timeout")) {
        auto& config_timeout = hook_config["timeout"];
        if (!config_timeout.is_number()) {
            malformed_config("process.env must be a number");
        }
        timeout_ = config_timeout;
    }
}

void hook::validate_hooks(main_app& app,
                          const nlohmann::json& hooks,
                          const char* phase) {
    if (hooks.is_null() || !hooks.contains(phase)) {
        return;
    }
    auto& a = hooks[phase];
    if (!a.is_array()) {
        malformed_config("hook lists must be arrays");
    }
    for (auto& hook : a) {
        if (!hook.contains("path")) {
            malformed_config("hook must have a path property");
        }
        if (hook.contains("args")) {
            auto& args = hook["args"];
            if (!args.is_array()) {
                malformed_config("hook.args must be an array");
            }
            for (auto& s : args) {
                if (!s.is_string()) {
                    malformed_config("hook.args elements must be strings");
                }
            }
        }
        if (hook.contains("env")) {
            auto& env = hook["env"];
            if (!env.is_array()) {
                malformed_config("hook.env must be an array");
            }
            for (auto& s : env) {
                if (!s.is_string()) {
                    malformed_config("hook.env elements must be strings");
                }
            }
        }
        if (hook.contains("timeout")) {
            if (!hook["timeout"].is_number_integer()) {
                malformed_config("hook.timeout must be an integer");
            }
            if (hook["timeout"] <= 0) {
                malformed_config("hook.timeout must be greater than zero");
            }
        }
    }
}

void hook::run_hooks(main_app& app,
                     const nlohmann::json& hooks,
                     const char* phase,
                     const runtime_state& state) {
    if (hooks.is_null() || !hooks.contains(phase)) {
        return;
    }

    bool warn_only = std::string_view{phase} == "poststart" ||
                     std::string_view{phase} == "poststop";
    for (auto& hook_config : hooks[phase]) {
        hook h{hook_config};
        std::stringstream ss;
        try {
            auto ret = h.run(app, state);
            if (ret == 0) {
                continue;
            }
            ss << phase << " hook " << h.path_ << " failed with status "
               << ret;
        } catch (const std::exception& e) {
            if (!warn_only) {
                throw;
            }
            ss << e.what();
        }
        if (!warn_only) {
            throw std::runtime_error(ss.str());
        }
        app.log() << "warning: " << ss.str();
    }
}

int hook::run(main_app& app, const runtime_state& state) {
    std::vector<char*> argv;
    std::vector<char*> envv;
    if (env_) {
        for (auto& s : *env_) {
            envv.push_back(const_cast<char*>(s.c_str()));
        }
        envv.push_back(nullptr);
    }

    argv.push_back(const_cast<char*>(path_.c_str()));
    if (args_) {
        for (auto& s : *args_) {
            argv.push_back(const_cast<char*>(s.c_str()));
        }
    }
    argv.push_back(nullptr);

    int stdin[2];
    if (::pipe(stdin) < 0) {
        throw std::system_error{errno,
                                std::system_category(),
                                "error creating pipe for executing hook"};
    }

    std::stringstream ss;
    ss << state.report();
    // Use a process descriptor so that we can wait for the hook with a
    // timeout. Closing it kills the hook if it is still running.
    int pd;
    auto pid = ::pdfork(&pd, 0);
    if (pid < 0) {
        throw std::system_error{
            errno, std::system_category(), "error forking hook"};
    }
    if (pid) {
        // Parent process - write the state json
        auto s = ss.str();
        auto len = s.size();
        auto p = s.data();
        while (len > 0) {
            auto n = ::write(stdin[1], p, len);
            if (n < 0) {
                throw std::system_error{errno,
                                        std::system_category(),
                                        "error writing state to hook"};
            }
            len -= n;
            p += n;
        }
        if (::close(stdin[0]) < 0 || ::close(stdin[1]) < 0) {
            throw std::system_error{
                errno, std::system_category(), "error closing hook stdin pipe"};
        }
        if (timeout_) {
            struct pollfd pfd = {.fd = pd, .events = POLLHUP};
            int res;
            do {
                res = ::poll(&pfd, 1, *timeout_ * 1000);
            } while (res < 0 && errno == EINTR);
            if (res < 0) {
                throw std::system_error{
                    errno, std::system_category(), "error waiting for hook"};
            }
            if (res == 0) {
                ::close(pd);
                ::waitpid(pid, nullptr, 0);
                std::stringstream ss;
                ss << "hook " << path_ << " timed out after " << *timeout_
                   << " seconds";
                throw std::runtime_error(ss.str());
            }
        }
        int status;
        auto res = ::waitpid(pid, &status, 0);
        ::close(pd);
        if (res < 0) {
            throw std::system_error{
                errno, std::system_category(), "error waiting for hook"};
        }
        int ret =
            WIFEXITED(status) ? WEXITSTATUS(status) : 127 + WTERMSIG(status);
        return ret;
    } else {
        // Child process - setup descriptors and go
        ::dup2(stdin[0], 0);
        ::close(stdin[1]);
        ::close_range(3, INT_MAX, CLOSE_RANGE_CLOEXEC);
        // Don't override environment unless it was in the config
        char** envp;
        if (env_) {
            envp = &envv[0];
        } else {
            envp = environ;
        }
        // The path should be absolute - no PATH lookup is needed
        ::execve(argv[0], &argv[0], envp);
        ::err(127, "error executing hook %s", path_.c_str());
    }
}

}  // namespace ocijail
