// Client (TUI/GUI) settings: schema of ~/.config/lightmusic/cfg.yaml.
#pragma once

#include "lm/config/config.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace lm::config {

enum class ReplayGain { Off, Track, Album };

struct ClientSettings {
    // server
    std::string serverUrl; // empty: no server
    std::string keyFile;   // file holding the API key (must be 0600); never the key itself
    // cache (hot: maxSizeMb, backgroundFill, pauseOnMetered)
    std::int64_t cacheMaxSizeMb = 1024; // unpinned tracks only; "saved offline" ones are excluded
    std::int64_t cacheMinFreeMb = 1024; // disk-space floor for saved-offline tracks (also >= 5% of the disk)
    bool backgroundFill = true;
    bool pauseOnMetered = false;
    std::string cacheDir; // empty: $XDG_CACHE_HOME/lightmusic
    // playback (hot: volume)
    int volume = 100; // percent, 0..100
    ReplayGain replayGain = ReplayGain::Off;
    // ui
    bool showCacheMarkers = true;
    bool saveSettings = true; // write UI changes (volume) back into cfg.yaml
    bool operator==(const ClientSettings &) const = default;
};

const Schema<ClientSettings> &clientSchema();

std::string defaultClientConfigPath(); // configDir() + "/cfg.yaml"
// Reserved for the UI write-back option "overrides layered on top" (not used yet).
std::string uiOverridesPath(); // configDir() + "/ui-overrides.yaml"

// Client config files: no group/world write (refused), read warns.
FilePolicy clientFilePolicy();

// Resolves defaults < file < overrides < cli. A missing file is fine unless `required`.
// The parsed file layer is returned in `fileLayer` (for diagnostics/reload).
// Returns false if any error was reported (settings are then not to be used).
bool loadClientSettings(const std::string &path, bool required, const Layer *overrides, const Layer &cli,
                        ClientSettings &out, Diags &diags);

// Re-reads the file for a hot reload. `running` is what the process uses now. On
// success, `applied` = running with the hot fields from the new file (+ overrides +
// cli, which still win), and `diff` lists hot changes and keys needing a restart.
// Otherwise ok = false, `applied` = running and `diags` says why: errors in the file,
// or (a warning) the file is missing or empty, which is never taken as "use the
// defaults" since editors pass through both states while saving.
struct Reload {
    bool ok = false;
    ClientSettings applied;
    Schema<ClientSettings>::Diff diff;
    Diags diags;
};
Reload reloadClientSettings(const std::string &path, const Layer *overrides, const Layer &cli,
                            const ClientSettings &running);

} // namespace lm::config
