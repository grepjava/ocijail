#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct start {
    static void init(main_app& app);

    // Start the created container with the given id.
    static void start_container(main_app& app, std::string_view id);

   private:
    start(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
};

}  // namespace ocijail
