#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

// The pause and resume commands
struct pause {
    static void init(main_app& app);

   private:
    pause(main_app& app, bool resume);
    void run();

    main_app& app_;
    bool resume_;
    std::string id_;
};

}  // namespace ocijail
