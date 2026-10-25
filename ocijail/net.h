#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ocijail/main.h"

namespace ocijail {

// Return true if the address (in numeric form) is configured on any
// interface. The family is AF_INET or AF_INET6.
bool address_configured(int family, const std::string& addr);

// Add or remove an alias address on an interface using ifconfig.
void add_interface_alias(const std::string& interface,
                         int family,
                         const std::string& addr);
void remove_interface_alias(const std::string& interface,
                            int family,
                            const std::string& addr);

// Add the addresses, each a family and numeric address, as aliases on the
// interface, recording them in the state so that they can be removed. Any
// address which is already configured is left alone.
void add_interface_aliases(
    runtime_state& state,
    const std::string& interface,
    const std::vector<std::pair<int, std::string>>& addrs);

// Remove the aliases recorded by add_interface_aliases, logging any errors.
void remove_interface_aliases(main_app& app, runtime_state& state);

}  // namespace ocijail
