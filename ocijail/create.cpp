#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "ocijail/create.h"
#include "ocijail/hook.h"
#include "ocijail/jail.h"
#include "ocijail/mount.h"
#include "ocijail/net.h"
#include "ocijail/process.h"
#include "ocijail/tty.h"

namespace fs = std::filesystem;

using nlohmann::json;

namespace {

struct oci_version {
    std::string major;
    std::string minor;
    std::string patch;
};

oci_version parse_version(std::string ociver) {
    std::vector<std::string_view> parts;
    auto tmp = std::string_view{ociver};
    // Trim off any -rc.x or -dev suffix first
    auto i = tmp.find_first_of("-");
    if (i != std::string_view::npos) {
        auto suffix = tmp.substr(i + 1);
        if (suffix.substr(0, 3) != "rc." && suffix != "dev") {
            throw std::runtime_error("malformed ociVersion " +
                                     std::string(ociver));
        }
        tmp = tmp.substr(0, i);
    }
    while (tmp.size() > 0) {
        auto i = tmp.find_first_of(".");
        if (i != std::string_view::npos) {
            parts.push_back(tmp.substr(0, i));
            tmp = tmp.substr(i + 1);
        } else {
            parts.push_back(tmp);
            tmp = "";
        }
    }
    if (parts.size() != 3) {
        throw std::runtime_error("malformed ociVersion " + std::string(ociver));
    }
    return oci_version{
        std::string{parts[0]}, std::string{parts[1]}, std::string{parts[2]}};
}

}  // namespace

namespace ocijail {

void create::init(main_app& app) {
    static create instance{app};
}

create::create(main_app& app) : app_(app) {
    auto sub = app.add_subcommand("create",
                                  "Create a jail instance for the container "
                                  "described by the given bundle directory.");
    sub->add_option("--bundle,-b",
                    bundle_path_,
                    "Path to the OCI runtime bundle directory")
        ->check(CLI::ExistingDirectory);
    sub->add_option("container-id", id_, "Unique identifier for the container")
        ->required();
    sub->add_option(
           "--console-socket",
           console_socket_,
           "Path to a socket which will receive the console pty descriptor")
        ->check(CLI::ExistingPath);
    sub->add_option(
        "--pid-file",
        pid_file_,
        "Path to a file where the container process id will be written");
    sub->add_option("--preserve-fds",
                    preserve_fds_,
                    "Number of additional file descriptors for the container");

    sub->final_callback([this] { run(); });
}

void create::run() {
    auto state = app_.get_runtime_state(id_);

    if (app_.get_test_mode() == test_mode::NONE && state.exists()) {
        throw std::runtime_error{"container " + id_ + " exists"};
    }

    if (chdir(bundle_path_.c_str()) < 0) {
        throw std::system_error{
            errno,
            std::system_category(),
            "error changing directory to" + bundle_path_.string()};
    }

    auto config_path = bundle_path_ / "config.json";
    if (!fs::is_regular_file(config_path)) {
        throw std::runtime_error{
            "create: bundle directory must contain config.json"};
    }
    json config;
    std::ifstream{config_path} >> config;

    if (!config.contains("ociVersion")) {
        malformed_config("no ociVersion");
    }
    if (!config["ociVersion"].is_string()) {
        malformed_config("ociVersion must be a string");
    }
    // Allow 1.0.x, 1.1.x, 1.2.x and 1.3.x
    auto ver = parse_version(config["ociVersion"]);
    if (ver.major != "1" || ver.minor > "3") {
        throw std::runtime_error{"create: unsupported OCI version " +
                                 std::string{config["ociVersion"]}};
    }

    if (!config.contains("process")) {
        malformed_config("no process");
    }

    auto& config_process = config["process"];
    process proc{config_process, console_socket_, true, preserve_fds_};

    // If the config contains a root path, use that, otherwise the
    // bundle directory must have a subdirectory named "root"
    bool root_readonly = false;
    auto root_path = bundle_path_ / "root";
    auto readonly_root_path = state.get_state_dir() / "readonly_root";
    if (config.contains("root")) {
        auto& config_root = config["root"];
        if (config["root"].contains("path")) {
            root_path = fs::path{config_root["path"]};
        }
        if (config_root.contains("readonly") && config_root["readonly"]) {
            root_readonly = true;
        }
    }
    if (!fs::is_directory(root_path)) {
        std::stringstream ss;
        ss << "root directory " << root_path << " must be a directory";
        throw std::runtime_error{ss.str()};
    }

    // Validate mounts if present
    auto& config_mounts = config["mounts"];
    if (!config_mounts.is_null()) {
        if (!config_mounts.is_array()) {
            malformed_config("mounts must be an array");
        }
        for (auto& mount : config_mounts) {
            if (!mount.is_object()) {
                malformed_config("mounts must be an array of objects");
            }
            if (!mount["destination"].is_string()) {
                malformed_config("mount destination must be a string");
            }
            if (mount.contains("source")) {
                if (!mount["source"].is_string()) {
                    malformed_config(
                        "if present, mount source must be a string");
                }
            }
            if (mount.contains("type")) {
                if (!mount["type"].is_string()) {
                    malformed_config("if present, mount type must be a string");
                }
            }
            if (mount.contains("options")) {
                if (!mount["options"].is_array()) {
                    malformed_config(
                        "if present, mount options must be an array");
                }
                for (auto& opt : mount["options"]) {
                    if (!opt.is_string()) {
                        malformed_config(
                            "if present, mount options must be an array of "
                            "strings");
                    }
                }
            }
        }
    }

    // Validate hooks, if present
    auto& config_hooks = config["hooks"];
    if (!config_hooks.is_null()) {
        if (!config_hooks.is_object()) {
            malformed_config("hooks must be an object");
        }

        hook::validate_hooks(app_, config_hooks, "prestart");
        hook::validate_hooks(app_, config_hooks, "createRuntime");
        hook::validate_hooks(app_, config_hooks, "createContainer");
        hook::validate_hooks(app_, config_hooks, "startContainer");
        hook::validate_hooks(app_, config_hooks, "poststart");
        hook::validate_hooks(app_, config_hooks, "poststop");
    }

    // Jail parameters come from the freebsd section of the config, or from
    // annotations for versions of podman which predate it. Where both are
    // present, the freebsd section takes precedence.
    std::optional<std::string> parent_jail;
    auto vnet = jail::INHERIT;
    std::optional<jail::ns> host;
    std::optional<jail::ns> ip4;
    std::optional<jail::ns> ip6;
    std::optional<std::string> ip4_addr;
    std::optional<std::string> ip6_addr;
    uint32_t enforce_statfs = 1;
    // An interface to add the container's addresses to
    std::optional<std::string> interface;
    // Interfaces to move into the container's vnet
    std::vector<std::string> vnet_interfaces;
    // Boolean jail parameters such as allow.mlock
    std::map<std::string, bool> allow_params;
    std::vector<std::string> annotation_allow_params;

    std::unordered_map<std::string, std::optional<jail::ns>> known_ns_params = {
        {"org.freebsd.jail.vnet", std::nullopt},
        {"org.freebsd.jail.sysvmsg", std::nullopt},
        {"org.freebsd.jail.sysvsem", std::nullopt},
        {"org.freebsd.jail.sysvshm", std::nullopt},
    };
    if (config.contains("annotations")) {
        auto config_annotations = config["annotations"];
        if (config_annotations.contains("org.freebsd.parentJail")) {
            parent_jail = config_annotations["org.freebsd.parentJail"];
        }

        for (auto& [key, nsvalue] : known_ns_params) {
            if (config_annotations.contains(key)) {
                std::string val = config_annotations[key];
                if (val == "new") {
                    nsvalue = jail::NEW;
                } else if (val == "inherit") {
                    nsvalue = jail::INHERIT;
                } else if (val == "disable") {
                    nsvalue = jail::DISABLED;
                } else {
                    throw std::runtime_error(
                        "bad value for " + key + ": " + val);
                }
            }
        }

        vnet = known_ns_params["org.freebsd.jail.vnet"].value_or(vnet);
        if (vnet == jail::INHERIT) {
            if (config_annotations.contains("org.freebsd.jail.ip4.addr")) {
                ip4_addr = config_annotations["org.freebsd.jail.ip4.addr"];
            }
            if (config_annotations.contains("org.freebsd.jail.ip6.addr")) {
                ip6_addr = config_annotations["org.freebsd.jail.ip6.addr"];
            } else if (config_annotations.contains("org.freebsd.jail.ip6.add")) {
                ip6_addr = config_annotations["org.freebsd.jail.ip6.add"];
            }
        } else if (vnet == jail::DISABLED) {
            throw std::runtime_error(
                "bad value for org.freebsd.jail.vnet: disable");
        }

        // Check for allow.* annotations (e.g., org.freebsd.jail.allow.mlock).
        // Only parameters known to the FreeBSD jail subsystem are accepted;
        // unknown parameters are logged and ignored. The kernel remains the
        // final authority and will reject any parameter it does not recognise.
        static const std::unordered_set<std::string> known_allow_params = {
            "allow.adjtime",       "allow.chflags",
            "allow.extattr",       "allow.mlock",
            "allow.mount",         "allow.mount.devfs",
            "allow.mount.fdescfs", "allow.mount.nullfs",
            "allow.mount.procfs",  "allow.mount.tmpfs",
            "allow.mount.zfs",     "allow.nfsd",
            "allow.quotas",        "allow.raw_sockets",
            "allow.read_msgbuf",   "allow.reserved_ports",
            "allow.routing",       "allow.set_hostname",
            "allow.setaudit",      "allow.settime",
            "allow.socket_af",     "allow.suser",
            "allow.sysvipc",       "allow.unprivileged_parent_tampering",
            "allow.unprivileged_proc_debug",
        };
        const std::string allow_prefix = "org.freebsd.jail.allow.";
        for (auto& [key, value] : config_annotations.items()) {
            if (key.starts_with(allow_prefix) && value.is_string()) {
                std::string param = "allow." + key.substr(allow_prefix.size());
                std::string val = value;
                if (!known_allow_params.count(param)) {
                    app_.log() << "warning: unknown jail allow annotation '"
                               << key << "', ignoring";
                } else if (val == "true" || val == "1") {
                    annotation_allow_params.push_back(param);
                }
            }
        }
    }
    auto sysvmsg = known_ns_params["org.freebsd.jail.sysvmsg"];
    auto sysvsem = known_ns_params["org.freebsd.jail.sysvsem"];
    auto sysvshm = known_ns_params["org.freebsd.jail.sysvshm"];

    // The allow parameters which the freebsd section can set
    static const std::map<std::string, std::string> freebsd_allow_params = {
        {"setHostname", "allow.set_hostname"},
        {"rawSockets", "allow.raw_sockets"},
        {"chflags", "allow.chflags"},
        {"quotas", "allow.quotas"},
        {"socketAf", "allow.socket_af"},
        {"mlock", "allow.mlock"},
        {"reservedPorts", "allow.reserved_ports"},
        {"suser", "allow.suser"},
    };
    std::map<std::string, bool> freebsd_allow;
    bool have_freebsd_jail =
        config.contains("freebsd") && config["freebsd"].is_object() &&
        config["freebsd"].contains("jail");
    if (have_freebsd_jail) {
        auto& fj = config["freebsd"]["jail"];
        if (!fj.is_object()) {
            malformed_config("freebsd.jail must be an object");
        }
        auto mode = [&](const char* key,
                        bool can_disable) -> std::optional<jail::ns> {
            if (!fj.contains(key)) {
                return std::nullopt;
            }
            if (fj[key] == "new") {
                return jail::NEW;
            } else if (fj[key] == "inherit") {
                return jail::INHERIT;
            } else if (can_disable && fj[key] == "disable") {
                return jail::DISABLED;
            }
            malformed_config(std::string{"bad value for freebsd.jail."} + key);
            return std::nullopt;
        };
        auto addrs = [&](const char* key) -> std::optional<std::string> {
            if (!fj.contains(key)) {
                return std::nullopt;
            }
            if (!fj[key].is_array()) {
                malformed_config(std::string{"freebsd.jail."} + key +
                                 " must be an array of strings");
            }
            std::string res;
            for (auto& addr : fj[key]) {
                if (!addr.is_string()) {
                    malformed_config(std::string{"freebsd.jail."} + key +
                                     " must be an array of strings");
                }
                if (!res.empty()) {
                    res += ",";
                }
                res += addr.get<std::string>();
            }
            return res;
        };

        if (fj.contains("parent")) {
            if (!fj["parent"].is_string()) {
                malformed_config("freebsd.jail.parent must be a string");
            }
            parent_jail = fj["parent"];
        }
        if (auto v = mode("host", false)) {
            host = v;
        }
        if (auto v = mode("vnet", false)) {
            vnet = *v;
        }
        if (auto v = mode("ip4", true)) {
            ip4 = v;
        }
        if (auto v = mode("ip6", true)) {
            ip6 = v;
        }
        if (auto v = addrs("ip4Addr")) {
            ip4_addr = v;
        }
        if (auto v = addrs("ip6Addr")) {
            ip6_addr = v;
        }
        if (auto v = mode("sysvmsg", true)) {
            sysvmsg = v;
        }
        if (auto v = mode("sysvsem", true)) {
            sysvsem = v;
        }
        if (auto v = mode("sysvshm", true)) {
            sysvshm = v;
        }
        enforce_statfs = 2;
        if (fj.contains("enforceStatfs")) {
            if (!fj["enforceStatfs"].is_number_unsigned() ||
                fj["enforceStatfs"] > 2) {
                malformed_config("freebsd.jail.enforceStatfs must be 0, 1 or 2");
            }
            enforce_statfs = fj["enforceStatfs"];
        }
        if (fj.contains("interface")) {
            if (!fj["interface"].is_string()) {
                malformed_config("freebsd.jail.interface must be a string");
            }
            interface = fj["interface"];
            if (::if_nametoindex(interface->c_str()) == 0) {
                throw std::runtime_error("no such interface " + *interface);
            }
        }
        if (fj.contains("vnetInterfaces")) {
            auto& ifs = fj["vnetInterfaces"];
            if (!ifs.is_array()) {
                malformed_config(
                    "freebsd.jail.vnetInterfaces must be an array of strings");
            }
            for (auto& name : ifs) {
                if (!name.is_string()) {
                    malformed_config(
                        "freebsd.jail.vnetInterfaces must be an array of "
                        "strings");
                }
                if (::if_nametoindex(name.get<std::string>().c_str()) == 0) {
                    throw std::runtime_error("no such interface " +
                                             name.get<std::string>());
                }
                vnet_interfaces.push_back(name);
            }
        }
        if (fj.contains("allow")) {
            auto& allow = fj["allow"];
            if (!allow.is_object()) {
                malformed_config("freebsd.jail.allow must be an object");
            }
            for (auto& [key, value] : allow.items()) {
                if (key == "mount") {
                    if (!value.is_array()) {
                        malformed_config(
                            "freebsd.jail.allow.mount must be an array of "
                            "strings");
                    }
                    for (auto& fstype : value) {
                        if (!fstype.is_string()) {
                            malformed_config(
                                "freebsd.jail.allow.mount must be an array of "
                                "strings");
                        }
                        freebsd_allow["allow.mount"] = true;
                        freebsd_allow["allow.mount." +
                                      fstype.get<std::string>()] = true;
                    }
                    continue;
                }
                auto it = freebsd_allow_params.find(key);
                if (it == freebsd_allow_params.end()) {
                    malformed_config("unknown freebsd.jail.allow." + key);
                }
                if (!value.is_boolean()) {
                    malformed_config("freebsd.jail.allow." + key +
                                     " must be a boolean");
                }
                freebsd_allow[it->second] = value;
            }
        }
    }

    // Start with the defaults, which for the freebsd section are those of
    // the specification, then apply the allow annotations and finally the
    // freebsd section.
    if (have_freebsd_jail) {
        for (auto& [key, param] : freebsd_allow_params) {
            allow_params[param] = (key == "suser");
        }
    } else {
        allow_params["allow.raw_sockets"] = true;
        // Set allow.chflags unless we have a parent jail where this is not
        // set.
        bool allow_chflags = true;
        if (parent_jail) {
            auto pj = jail::find(*parent_jail);
            allow_chflags = pj.get<bool>("allow.chflags");
        }
        if (allow_chflags) {
            allow_params["allow.chflags"] = true;
        }
    }
    for (auto& param : annotation_allow_params) {
        allow_params[param] = true;
    }
    for (auto& [param, value] : freebsd_allow) {
        allow_params[param] = value;
    }

    // Create a jail config from the OCI config
    jail::config jconf;
    if (parent_jail) {
        jconf.set("name", *parent_jail + "." + id_);
    } else {
        jconf.set("name", id_);
    }
    jconf.set("persist");
    jconf.set("enforce_statfs", enforce_statfs);
    for (const auto& [param, value] : allow_params) {
        if (value) {
            jconf.set(param);
        } else {
            // Turn off a boolean parameter by adding "no" to its last part,
            // e.g. allow.noraw_sockets.
            auto i = param.rfind('.') + 1;
            jconf.set(param.substr(0, i) + "no" + param.substr(i));
        }
    }
    if (sysvmsg) {
        jconf.set("sysvmsg", *sysvmsg);
    }
    if (sysvsem) {
        jconf.set("sysvsem", *sysvsem);
    }
    if (sysvshm) {
        jconf.set("sysvshm", *sysvshm);
    }
    if (root_readonly) {
        jconf.set("path", readonly_root_path);
    } else {
        jconf.set("path", root_path);
    }
    if (vnet == jail::NEW) {
        // A vnet jail has its own addresses.
        if (ip4 || ip6 || ip4_addr || ip6_addr || interface) {
            throw std::runtime_error(
                "IP address restrictions cannot be used with a new vnet");
        }
        jconf.set("vnet", vnet);
    } else if (!vnet_interfaces.empty()) {
        throw std::runtime_error(
            "freebsd.jail.vnetInterfaces needs a new vnet");
    } else {
        auto parse_addrs = [](const std::string& addrs, int af,
                              const char* name) {
            std::vector<uint8_t> res;
            std::string::size_type start = 0;
            while (start < addrs.size()) {
                auto end = addrs.find(',', start);
                auto token = addrs.substr(
                    start, end == std::string::npos ? std::string::npos
                                                    : end - start);
                if (!token.empty()) {
                    std::array<uint8_t, sizeof(in6_addr)> addr;
                    if (inet_pton(af, token.c_str(), addr.data()) != 1) {
                        throw std::runtime_error(std::string{"bad "} + name +
                                                 " address: " + token);
                    }
                    auto len = af == AF_INET ? sizeof(in_addr)
                                             : sizeof(in6_addr);
                    res.insert(res.end(), addr.begin(), addr.begin() + len);
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
            return res;
        };
        if (ip4_addr) {
            if (ip4 && *ip4 != jail::NEW) {
                throw std::runtime_error(
                    "IPv4 addresses can only be given if ip4 is new");
            }
            jconf.set("ip4", jail::NEW);
            jconf.set("ip4.addr", parse_addrs(*ip4_addr, AF_INET, "IPv4"));
        } else {
            jconf.set("ip4", ip4.value_or(jail::INHERIT));
        }
        if (ip6_addr) {
            if (ip6 && *ip6 != jail::NEW) {
                throw std::runtime_error(
                    "IPv6 addresses can only be given if ip6 is new");
            }
            jconf.set("ip6", jail::NEW);
            jconf.set("ip6.addr", parse_addrs(*ip6_addr, AF_INET6, "IPv6"));
        } else {
            jconf.set("ip6", ip6.value_or(jail::INHERIT));
        }
    }
    if (!host) {
        host = config.contains("hostname") ? jail::NEW : jail::INHERIT;
    }
    if (config.contains("hostname")) {
        if (*host != jail::NEW) {
            throw std::runtime_error(
                "hostname can only be set if freebsd.jail.host is new");
        }
        jconf.set("host.hostname", config["hostname"].get<std::string>());
    }
    jconf.set("host", *host);

    // Unit tests for config validation stop here.
    if (app_.get_test_mode() == test_mode::VALIDATION) {
        return;
    }

    // Create a state object with initial fields from the config
    state["root_path"] = root_path;
    state["bundle"] = bundle_path_;
    state["config"] = config;
    state["status"] = "created";
    if (parent_jail) {
        state["parent_jail"] = *parent_jail;
    }

    // Create the state here in case we have a readonly root
    auto lk = state.create();

    // Mount filesystems if requested and record unmount actions in the
    // state.
    //
    // If rootfs needs to be remounted read-only, we make two passes. The first
    // prepares mount points and the second completes the mounts in our
    // read-only alias.
    state["root_readonly"] = false;
    if (root_readonly) {
        if (config_mounts.is_array()) {
            mount_volumes(app_, state, root_path, true, config_mounts);
        }
        fs::create_directory(readonly_root_path);
        std::vector<std::tuple<std::string, std::string>> mount_opts;
        mount_opts.emplace_back("fstype", "nullfs");
        mount_opts.emplace_back("fspath", readonly_root_path);
        mount_opts.emplace_back("target", root_path);
        if (do_mount(mount_opts, MNT_RDONLY) < 0) {
            throw std::system_error(errno,
                                    std::system_category(),
                                    "mounting " + readonly_root_path.native());
        }
        root_path = readonly_root_path;
        state["root_readonly"] = true;
        state["readonly_root_path"] = readonly_root_path;
    }
    if (config_mounts.is_array()) {
        mount_volumes(app_, state, root_path, false, config_mounts);
    }

    // Create the jail for our container. If we have a parent, attach
    // to that first.
    if (parent_jail) {
        auto pj = jail::find(*parent_jail);
        auto current_child_count = pj.get<uint32_t>("children.cur");
        auto max_child_count = pj.get<uint32_t>("children.max");
        if (current_child_count >= max_child_count) {
            pj.set("children.max", current_child_count + 1);
        }
    }

    // Create a socket pair for coordinating create activities with
    // our child process.
    int create_sock[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, create_sock) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error creating socket pair"};
    }

    // Add the container's addresses to the requested interface before
    // creating the jail which uses them.
    if (interface) {
        std::vector<std::pair<int, std::string>> addrs;
        for (auto [family, list] :
             {std::pair{AF_INET, ip4_addr}, std::pair{AF_INET6, ip6_addr}}) {
            std::string::size_type start = 0;
            while (list && start < list->size()) {
                auto end = list->find(',', start);
                auto token = list->substr(start, end == std::string::npos
                                                     ? std::string::npos
                                                     : end - start);
                if (!token.empty()) {
                    addrs.emplace_back(family, token);
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        }
        add_interface_aliases(state, *interface, addrs);
    }

    auto j = [&] {
        try {
            return jail::create(jconf);
        } catch (const std::exception&) {
            remove_interface_aliases(app_, state);
            throw;
        }
    }();
    state["jid"] = j.jid();
    try {
        move_interfaces_to_vnet(state, j.jid(), vnet_interfaces);
    } catch (const std::exception&) {
        return_interfaces_from_vnet(app_, state);
        j.remove();
        remove_interface_aliases(app_, state);
        throw;
    }

    // We record the container state including the bundle config. We
    // need to create the start fifo before forking - this will be
    // used to pause the container until start is called.
    auto start_wait = state.get_state_dir() / "start_wait";
    if (mkfifo(start_wait.c_str(), 0600) < 0) {
        throw std::system_error{
            errno, std::system_category(), "error creating start fifo"};
    }

    auto pid = ::fork();
    if (pid) {
        // Parent process - write to pid file if requested
        if (pid_file_) {
            std::ofstream{*pid_file_} << pid;
        }
        state["jid"] = j.jid();
        state["pid"] = pid;
        state.save();

        hook::run_hooks(app_, config_hooks, "createRuntime", state);

        lk.unlock();

        // Signal the child to execute any hooks and validate that the
        // container process can be found.
        char ch = 1;
        auto n = ::write(create_sock[0], &ch, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "write to create socket"};
        }

        // Read back the child's status - this is our exit status. The
        // child will have already written to stderr if necessary.
        char status;
        n = ::read(create_sock[0], &status, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "read from create socket"};
        }
        if (status != 0) {
            // If the create failed, we need to clean up: unmount the volumes and
            // delete the state.
            return_interfaces_from_vnet(app_, state);
            j.remove();
            remove_interface_aliases(app_, state);
            if (config_mounts.is_array()) {
                unmount_volumes(app_, state, root_path, config_mounts);
            }
            if (root_readonly) {
                if (::unmount(root_path.c_str(), MNT_FORCE) > 0) {
                    throw std::system_error{errno,
                                            std::system_category(),
                                            "unmounting " + root_path.native()};
                }
            }
            state.remove_all();
        }
        ::exit(status);
    } else {
        // Perform the console-socket hand off if process.terminal is true.
        auto [stdin_fd, stdout_fd, stderr_fd] = proc.pre_start();

        auto start_wait_fd = ::open(start_wait.c_str(), O_RDWR);
        if (start_wait_fd < 0) {
            throw std::system_error{
                errno, std::system_category(), "open start fifo"};
        }

        // Wait for our parent to signal us via the socket
        char ch;
        auto n = read(create_sock[1], &ch, 1);
        if (n < 0) {
            throw std::system_error{errno,
                                    std::system_category(),
                                    "error reading from create socket"};
        }

        char status = 0;
        try {
            // Our part of create: execute any hooks, enter the jail and
            // validate process args.

            // The specification states that for createContainer hooks:
            //
            // - The value of path MUST resolve in the container namespace.
            // - The startContainer hooks MUST be executed in the container
            //   namespace.
            //
            // This doesn't make a lot of sense but looking at other
            // implementations, runc interprets this as changing directory
            // to the container root (but not chrooting).
            if (chdir(root_path.c_str()) < 0) {
                throw std::system_error{
                    errno,
                    std::system_category(),
                    "error changing directory to" + root_path.string()};
            }
            hook::run_hooks(app_, config_hooks, "createContainer", state);

            // Enter the jail and set the requested working directory.
            j.attach();

            // Validate the process executable exists and can be executed
            proc.validate();
        } catch (const std::exception& e) {
            std::string_view msg{e.what()};
            ::write(2, msg.data(), msg.size());
            status = 1;
        }

        n = write(create_sock[1], &status, 1);
        if (n < 0) {
            throw std::system_error{errno,
                                    std::system_category(),
                                    "error writing to create socket"};
        }
        ::close(create_sock[1]);

        // If validate failed, don't wait for a start signal, just stop here.
        if (status != 0) {
            ::exit(status);
        }

        // Finished coordinating with parent - now we wait until
        // signalled by start.
        n = ::read(start_wait_fd, &ch, 1);
        if (n < 0) {
            throw std::system_error{
                errno, std::system_category(), "read from start fifo"};
        }
        ::close(start_wait_fd);

        // Run startContainer hooks inside the jail.
        hook::run_hooks(app_, config_hooks, "startContainer", state);

        // Execute the requested process inside the jail.
        proc.exec(stdin_fd, stdout_fd, stderr_fd);
    }
}

}  // namespace ocijail
