#pragma once

#include <optional>

#include "ocijail/main.h"

namespace ocijail {

struct delete_ {
    static void init(main_app& app);

    // Delete the container with the given id.
    static void delete_container(main_app& app,
                                 std::string_view id,
                                 bool force);

   private:
    delete_(main_app& app);
    void run();

    main_app& app_;
    bool force_{false};
    std::string id_;
};

}  // namespace ocijail
