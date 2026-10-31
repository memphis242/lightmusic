// lm::net::Client — talks to lightmusicd: library sync into a local SQLite mirror, and
// track streaming through an offline cache (docs/research.md §5). UI-free.
//
// Threads and ownership:
//   network thread  one per Client (NetThread): owns the libcurl multi handle and every
//                   transfer, writes downloaded bytes into the cache files (pwrite),
//                   commits them (fdatasync, then SQLite), schedules playback fetches and
//                   the idle fill queue, evicts, probes the metered state.
//   decoder thread  (the player's) reads through CachedStreamSource: cached ranges are
//                   pread() from disk; for a hole it asks the network thread for the bytes
//                   and blocks on a condition variable (interruptible, with a timeout).
//   any thread      sync(), browsing, pin(), cacheInfo(): short SQLite queries.
// Never call a source from an audio callback.
//
// Data flow for a remote track: Player -> openSource("lm://server/track/<id>") ->
// CachedStreamSource -> Entry (one per open track: sparse .part file + in-memory range
// set) <- network thread <- GET /api/v1/stream/<id> with Range + If-Range.
#pragma once

#include "lm/source.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lm::net {


struct Track {
    std::int64_t id = 0;
    std::string title, artist, album, albumArtist, codec, suffix, etag;
    std::int64_t albumId = 0, trackNo = 0, discNo = 0, year = 0, durationMs = 0, size = 0;
};

struct Album {
    std::int64_t id = 0;
    std::string title, artist;
    std::int64_t year = 0, trackCount = 0, durationMs = 0;
};

struct ClientConfig {
    std::string serverUrl; // e.g. "http://127.0.0.1:8737" (http/https only)
    std::string apiKey;    // sent only as "Authorization: Bearer"; never logged
    // Default: $XDG_CACHE_HOME/lightmusic, else ~/.cache/lightmusic. Created 0700.
    std::string cacheDir;
    std::uint64_t cacheMaxBytes = 100ull * 1024 * 1024;
    bool backgroundFill = true;  // idle fill queue
    bool pauseOnMetered = false; // pause the idle fill on metered networks
    // Metered probe; default: NetworkManager's Metered property over D-Bus (sd-bus) when
    // built with it, else "unmetered". nullopt = unknown (treated as unmetered).
    std::function<std::optional<bool>()> meteredProbe;
    int meteredCheckMs = 30000;
    double readTimeoutSec = 20;  // a read waiting this long without progress fails
    long connectTimeoutSec = 10;
    long stallTimeoutSec = 20;   // transfers below 1 B/s this long are aborted
    std::size_t commitBytes = 256 * 1024; // batch: fdatasync + SQLite every 256 KiB ...
    int commitMs = 1000;                  // ... or 1 s, whichever first
    // A playback fetch already running at most this far before the read position is
    // kept; otherwise it is cancelled and restarted at the read position.
    std::uint64_t seekRestartBytes = 512 * 1024;
    std::function<void(const std::string &)> log;
    // Network thread: a track became cached/partial/invalidated, or a sync applied.
    std::function<void()> onCacheChange;
};

enum class CacheState { Remote, Partial, Cached };
const char *cacheStateName(CacheState s);

struct CacheInfo {
    CacheState state = CacheState::Remote;
    bool pinned = false;
    std::int64_t cachedBytes = 0, size = 0;
};

struct CacheStats {
    std::int64_t usage = 0;     // bytes reserved by cache entries (sparse file sizes)
    std::int64_t pinned = 0;
    std::uint64_t cap = 0;
    std::size_t entries = 0;
    std::uint64_t downloaded = 0; // body bytes received since open
    bool idleFillActive = false;
    bool playbackFetchActive = false;
};

// Durable cache row, for tests and diagnostics.
struct CacheEntryInfo {
    std::string etag;
    std::int64_t size = 0;
    bool complete = false;
    std::string ranges; // durable ranges "b-e,..." (end exclusive)
    bool pinned = false;
    std::string file;   // current file path (.part or final)
};

class Client {
public:
    // Opens (creates) the cache directory and client database. Does not touch the
    // network. nullptr with *err on failure.
    static std::unique_ptr<Client> open(ClientConfig cfg, std::string *err);
    ~Client(); // stops the network thread; open sources then fail their reads
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // GET /library/changes?since=<rev> into the mirror (blocking; network). Tracks
    // whose ETag changed or that were deleted lose their cache entries.
    bool sync(std::string *err);
    std::int64_t revision();

    // Mirror (works offline).
    std::vector<Album> albums();
    std::vector<Track> tracksOfAlbum(std::int64_t albumId);
    std::vector<Track> allTracks();
    std::optional<Track> track(std::int64_t id);

    CacheInfo cacheInfo(std::int64_t trackId);
    std::optional<CacheEntryInfo> cacheEntry(std::int64_t trackId);
    CacheStats stats();
    // Pinned tracks are never evicted and are filled in the background. Pinning fails
    // when the pinned total would exceed the cap.
    bool pin(std::int64_t trackId, bool pinned, std::string *err);
    // Tracks to fill in the background next (e.g. the next few in the play queue);
    // replaces the previous list.
    void setFillQueue(std::vector<std::int64_t> trackIds);

    // A ByteSource for the track (cache first, network for holes). Blocking reads;
    // decoder thread only. nullptr with *err if the track is unknown.
    std::unique_ptr<ByteSource> openTrack(std::int64_t trackId, std::string *err);
    // For lm::PlayerConfig::openSource: resolves trackUri() strings.
    std::function<std::unique_ptr<ByteSource>(const std::string &, std::string *)> sourceOpener();

    static std::string trackUri(std::int64_t trackId); // "lm://server/track/<id>"
    static std::optional<std::int64_t> parseTrackUri(const std::string &uri);

    struct Core;

private:
    explicit Client(std::shared_ptr<Core> core);
    std::shared_ptr<Core> core_;
};

} // namespace lm::net
