// Metered-network detection: NetworkManager's global "Metered" property
// (org.freedesktop.NetworkManager, NMMetered: 1 yes, 3 guess-yes => metered) read over
// the system bus with sd-bus. Built only when libsystemd is available (LM_HAVE_SDBUS).
#pragma once

#include <optional>
#include <string>

namespace lm::net {

// nullopt with *err when unknown (no sd-bus support, no bus, no NetworkManager).
std::optional<bool> probeNetworkManagerMetered(std::string *err);

// Pure policy: may the idle fill run now?
inline bool idleFillAllowed(bool backgroundFill, bool pauseOnMetered, std::optional<bool> metered) {
    if (!backgroundFill) return false;
    if (pauseOnMetered && metered.value_or(false)) return false;
    return true;
}

} // namespace lm::net
