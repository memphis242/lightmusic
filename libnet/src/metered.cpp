#include "metered.hpp"

#ifdef LM_HAVE_SDBUS
#include <systemd/sd-bus.h>

#include <cstdint>

#include <cstring>
#endif

namespace lm::net {

std::optional<bool> probeNetworkManagerMetered(std::string *err) {
#ifdef LM_HAVE_SDBUS
    sd_bus *bus = nullptr;
    int r = sd_bus_open_system(&bus);
    if (r < 0) {
        if (err) *err = std::string("no system bus: ") + std::strerror(-r);
        return std::nullopt;
    }
    sd_bus_error e = SD_BUS_ERROR_NULL;
    std::uint32_t v = 0;
    r = sd_bus_get_property_trivial(bus, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                                    "org.freedesktop.NetworkManager", "Metered", &e, 'u', &v);
    std::optional<bool> out;
    if (r < 0) {
        if (err) *err = std::string("NetworkManager Metered: ") + (e.message ? e.message : std::strerror(-r));
    } else {
        out = v == 1 || v == 3; // NM_METERED_YES / NM_METERED_GUESS_YES
    }
    sd_bus_error_free(&e);
    sd_bus_unref(bus);
    return out;
#else
    if (err) *err = "built without sd-bus";
    return std::nullopt;
#endif
}

} // namespace lm::net
