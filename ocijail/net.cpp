#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "ocijail/net.h"

extern "C" char** environ;

namespace ocijail {

namespace {

void ifconfig(std::vector<std::string> args) {
    args.insert(args.begin(), "ifconfig");
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid;
    auto res = ::posix_spawn(
        &pid, "/sbin/ifconfig", nullptr, nullptr, &argv[0], environ);
    if (res != 0) {
        throw std::system_error{res, std::system_category(), "posix_spawn"};
    }
    int status;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::system_error{errno, std::system_category(), "waitpid"};
    }
    if (status != 0) {
        std::string cmd;
        for (auto& arg : args) {
            cmd += (cmd.empty() ? "" : " ") + arg;
        }
        throw std::runtime_error{cmd + " failed"};
    }
}

const char* family_name(int family) {
    return family == AF_INET ? "inet" : "inet6";
}

}  // namespace

bool address_configured(int family, const std::string& addr) {
    in6_addr want;
    if (::inet_pton(family, addr.c_str(), &want) != 1) {
        throw std::runtime_error{"bad address " + addr};
    }
    ifaddrs* ifap;
    if (::getifaddrs(&ifap) < 0) {
        throw std::system_error{errno, std::system_category(), "getifaddrs"};
    }
    bool found = false;
    for (auto ifa = ifap; ifa && !found; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != family) {
            continue;
        }
        if (family == AF_INET) {
            auto sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            found = std::memcmp(&sin->sin_addr, &want, sizeof(in_addr)) == 0;
        } else {
            auto sin6 = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
            found =
                std::memcmp(&sin6->sin6_addr, &want, sizeof(in6_addr)) == 0;
        }
    }
    ::freeifaddrs(ifap);
    return found;
}

void add_interface_alias(const std::string& interface,
                         int family,
                         const std::string& addr) {
    auto prefix = family == AF_INET ? "/32" : "/128";
    ifconfig({interface, family_name(family), addr + prefix, "alias"});
}

void remove_interface_alias(const std::string& interface,
                            int family,
                            const std::string& addr) {
    ifconfig({interface, family_name(family), addr, "-alias"});
}

void add_interface_aliases(
    runtime_state& state,
    const std::string& interface,
    const std::vector<std::pair<int, std::string>>& addrs) {
    for (auto& [family, addr] : addrs) {
        if (address_configured(family, addr)) {
            continue;
        }
        add_interface_alias(interface, family, addr);
        state["interface_aliases"].push_back(
            {{"interface", interface}, {"family", family}, {"address", addr}});
        state.save();
    }
}

void remove_interface_aliases(main_app& app, runtime_state& state) {
    if (!state.contains("interface_aliases")) {
        return;
    }
    for (auto& alias : state["interface_aliases"]) {
        try {
            remove_interface_alias(alias["interface"],
                                   alias["family"],
                                   alias["address"]);
        } catch (const std::exception& e) {
            app.log() << "warning: " << e.what();
        }
    }
}

}  // namespace ocijail
