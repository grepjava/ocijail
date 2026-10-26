#pragma once

#include <map>
#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct update {
    static void init(main_app& app);

   private:
    update(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
    std::optional<std::filesystem::path> resources_;
    // Values given on the command line, by their section and name in
    // linux.resources
    std::map<std::pair<std::string, std::string>, std::string> values_;
    // Options with no equivalent on FreeBSD which were given
    std::vector<std::string> unsupported_;
};

}  // namespace ocijail
