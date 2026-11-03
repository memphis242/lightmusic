// Client (TUI/GUI) settings: schema of ~/.config/lightmusic/cfg.yaml.
#pragma once

#include "lm/config/config.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace lm::config {

enum class ReplayGain { Off, Track, Album };
enum class RepeatMode { Off, All, One }; // playback.repeat: off | all | one
enum class CoverResolution { Normal, High }; // ui.cover_resolution: normal (64x64) | high (128x128)

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
    // playback (hot: volume, gap, repeat, shuffle)
    int volume = 100; // percent, 0..150 (above 100: software gain, as the volume keys)
    ReplayGain replayGain = ReplayGain::Off;
    double replayGainPreampDb = 0; // -15..15 dB, added to the tagged gain (tagged files only)
    bool replayGainPreventClipping = true; // cap the gain by the tagged peak
    bool pauseOnDeviceLoss = true;     // pause when the output goes away (headphones, Bluetooth)
    bool resumeOnDeviceReturn = false; // ... and resume when it comes back
    std::chrono::milliseconds gap{0}; // silence between tracks, 0..30 s (0: gapless); the file says seconds
    RepeatMode repeat = RepeatMode::Off;
    bool shuffle = false;
    // ui
    bool showCacheMarkers = true;
    std::string musicDir; // folder browser root (hot); empty: $XDG_MUSIC_DIR, else ~/Music
    bool saveSettings = true; // write UI changes (volume, repeat, shuffle) back into cfg.yaml
    bool resumeSession = true; // started without files: restore the last list, queue and position
    bool notifications = false; // desktop notification on track change (hot)
    bool coverArt = true;       // album cover pane (half-block thumbnail) when the terminal is wide enough (hot)
    // Thumbnail resolution: normal 64x64 pixels (pane up to 64x32 cells, 12 KiB each),
    // high 128x128 (up to 128x64 cells, 48 KiB each). Applies on restart.
    CoverResolution coverResolution = CoverResolution::Normal;
    // keys (hot): action name -> keys bound to it, replacing that action's defaults
    // (keys.queue_add: [q], keys.quit: []). Names and keys are checked by the frontend.
    std::map<std::string, NamedList> keys;
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
[[nodiscard]] bool loadClientSettings(const std::string &path, bool required, const Layer *overrides, const Layer &cli,
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
