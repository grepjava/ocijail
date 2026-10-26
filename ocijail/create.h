#pragma once

#include <filesystem>
#include <optional>

#include "nlohmann/json.hpp"

#include "ocijail/main.h"
#include "ocijail/process.h"

namespace ocijail {

struct create {
    static void init(main_app& app);

   private:
    // With run_mode set, this is the run command, which also starts the
    // container and, unless detached, waits for it to exit.
    create(main_app& app, bool run_mode);

    void run();
    [[noreturn]] void run_container(pid_t pid);

    main_app& app_;
    bool run_mode_;
    bool detach_{false};
    bool keep_{false};
    std::filesystem::path bundle_path_{"."};
    std::string id_;
    std::optional<std::filesystem::path> console_socket_;
    std::optional<std::filesystem::path> pid_file_;
    int preserve_fds_{0};
};

}  // namespace ocijail
