#include <unistd.h>
#include <iostream>

#include "nlohmann/json.hpp"

#include "spec.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace ocijail {

void spec::init(main_app& app) {
    static spec instance{app};
}

spec::spec(main_app& app) : app_(app) {
    auto sub = app.add_subcommand(
        "spec", "Create a default config.json in the bundle directory");
    sub->add_option("--bundle,-b",
                    bundle_path_,
                    "Path to the bundle directory (default: current directory)")
        ->check(CLI::ExistingDirectory);
    sub->add_option(
        "--file,-f", file_, "Destination file (default: config.json)");
    sub->add_flag("--rootless", rootless_, "Create a spec for a rootless container");
    sub->final_callback([this] { run(); });
}

void spec::run() {
    if (rootless_) {
        throw std::runtime_error{
            "spec: rootless containers are not supported"};
    }

    if (bundle_path_ && chdir(bundle_path_->c_str()) < 0) {
        throw std::system_error{
            errno,
            std::system_category(),
            "error changing directory to " + bundle_path_->string()};
    }

    // As with crun, an existing config.json is only replaced if --file is
    // given.
    auto where = file_.value_or("config.json");
    if (!file_ && fs::exists(where)) {
        throw std::runtime_error{"spec: " + where.string() + " already exists"};
    }

    json config = {
        {"ociVersion", "1.3.0"},
        {"process",
         {
             {"terminal", true},
             {"user", {{"uid", 0}, {"gid", 0}}},
             {"args", {"sh"}},
             {"env",
              {
                  "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/"
                  "local/bin",
                  "TERM=xterm",
              }},
             {"cwd", "/"},
         }},
        {"root", {{"path", "rootfs"}, {"readonly", true}}},
        {"hostname", "ocijail"},
        {"mounts",
         {
             {
                 {"destination", "/dev"},
                 {"type", "devfs"},
                 {"source", "devfs"},
                 {"options", {"ruleset=4"}},
             },
             {
                 {"destination", "/dev/fd"},
                 {"type", "fdescfs"},
                 {"source", "fdesc"},
                 {"options", json::array()},
             },
         }},
        // The jail has its own hostname and shares the host's network.
        {"freebsd",
         {
             {"jail",
              {
                  {"host", "new"},
                  {"ip4", "inherit"},
                  {"ip6", "inherit"},
              }},
         }},
    };

    std::ofstream out{where};
    if (!out) {
        throw std::system_error{
            errno, std::system_category(), "opening " + where.string()};
    }
    out << config.dump(4) << "\n";
}

}  // namespace ocijail
