#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct spec {
    static void init(main_app& app);

   private:
    spec(main_app& app);
    void run();

    main_app& app_;
    std::optional<std::filesystem::path> bundle_path_;
    std::optional<std::filesystem::path> file_;
    bool rootless_{false};
};

}  // namespace ocijail
