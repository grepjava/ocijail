#pragma once

#include <chrono>
#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct events {
    static void init(main_app& app);

   private:
    events(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
    bool stats_{false};
    std::chrono::milliseconds interval_{5000};
};

}  // namespace ocijail
