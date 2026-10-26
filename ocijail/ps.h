#pragma once

#include "ocijail/main.h"

namespace ocijail {

enum class ps_format {
    PS_TABLE,
    PS_JSON,
};

struct ps {
    static void init(main_app& app);

   private:
    ps(main_app& app);
    void run();

    main_app& app_;
    std::string id_;
    ps_format format_{ps_format::PS_TABLE};
};

}  // namespace ocijail
