#include "lm/config/client.hpp"

namespace lm::config {

namespace {
bool checkUrl(const std::string &s, std::string &err) {
    if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) {
        err = "must start with http:// or https://";
        return false;
    }
    for (char c : s)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
            err = "contains spaces or control characters";
            return false;
        }
    return true;
}
} // namespace

const Schema<ClientSettings> &clientSchema() {
    static const Schema<ClientSettings> s = [] {
        using S = ClientSettings;
        Schema<S> sc;
        sc.fields = {
            stringField<S>("server.url", &S::serverUrl, false, checkUrl),
            pathField<S>("server.key_file", &S::keyFile),
            intField<S>("cache.max_size_mb", &S::cacheMaxSizeMb, 1, 1LL << 30, true),
            intField<S>("cache.min_free_mb", &S::cacheMinFreeMb, 0, 1LL << 30),
            boolField<S>("cache.background_fill", &S::backgroundFill, true),
            boolField<S>("cache.pause_on_metered", &S::pauseOnMetered, true),
            pathField<S>("cache.dir", &S::cacheDir),
            intField<S>("playback.volume", &S::volume, 0, 150, true),
            enumField<S>("playback.replaygain", &S::replayGain,
                         {{"off", ReplayGain::Off}, {"track", ReplayGain::Track}, {"album", ReplayGain::Album}}),
            boolField<S>("ui.show_cache_markers", &S::showCacheMarkers),
            boolField<S>("ui.save_settings", &S::saveSettings, true),
        };
        const std::string keyMsg = "the API key must not be stored in the config file; put it in a file only you "
                                   "can read (chmod 600) and set server.key_file to its path";
        sc.refused = {{"api_key", keyMsg}, {"apikey", keyMsg}};
        return sc;
    }();
    return s;
}

std::string defaultClientConfigPath() { return configDir() + "/cfg.yaml"; }
std::string uiOverridesPath() { return configDir() + "/ui-overrides.yaml"; }

FilePolicy clientFilePolicy() { return FilePolicy{}; }

static bool loadFileLayer(const std::string &path, bool required, Layer &file, Diags &diags) {
    bool missing = false;
    const bool ok = loadLayer(path, clientFilePolicy(), file, missing, diags);
    if (missing && required) {
        Diag d;
        d.file = path;
        d.msg = "no such file";
        diags.push_back(std::move(d));
        return false;
    }
    return ok;
}

bool loadClientSettings(const std::string &path, bool required, const Layer *overrides, const Layer &cli,
                        ClientSettings &out, Diags &diags) {
    Layer file;
    loadFileLayer(path, required, file, diags);
    out = clientSchema().resolve({&file, overrides, &cli}, diags);
    return !hasError(diags);
}

Reload reloadClientSettings(const std::string &path, const Layer *overrides, const Layer &cli,
                            const ClientSettings &running) {
    Reload r;
    r.applied = running;
    auto keep = [&](const std::string &why) {
        Diag d;
        d.level = Diag::Warning;
        d.file = path;
        d.msg = why + "; settings not reloaded (keeping the running settings)";
        r.diags.push_back(std::move(d));
        return r;
    };
    // A missing or empty file is what editors leave between steps of a save (vim: rename
    // away, create empty, write); it never resets the running settings to the defaults.
    std::string text;
    bool missing = false;
    if (!readConfigFile(path, clientFilePolicy(), text, missing, r.diags)) return missing ? keep("no such file") : r;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return keep("the file is empty");
    Layer file;
    file.name = path;
    if (!parseYaml(text, path, file, r.diags)) return r;
    const ClientSettings next = clientSchema().resolve({&file, overrides, &cli}, r.diags);
    if (hasError(r.diags)) return r;
    r.ok = true;
    r.diff = clientSchema().diff(running, next);
    r.applied = clientSchema().applyHot(running, next);
    return r;
}

} // namespace lm::config
