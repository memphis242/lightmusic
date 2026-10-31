// Byte-level input for decoders. Decoders never touch files directly; they read through
// a ByteSource so a network stream or an offline-cache file can be plugged in later
// without changing any decoder.
//
// Implemented here: FileSource. libnet adds CachedStreamSource (lm/net/client.hpp): reads
// cached byte ranges from disk and blocks on holes while the network thread fetches them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lm {

class ByteSource {
public:
    enum class Whence { Set, Cur, End };
    virtual ~ByteSource() = default;
    // Reads up to n bytes. Returns the bytes read; fewer than n only at end of stream or
    // on error. May block (a network source would wait for data).
    virtual std::size_t read(void *dst, std::size_t n) = 0;
    // Returns false if the position is invalid or seeking is unsupported.
    virtual bool seek(std::int64_t offset, Whence whence) = 0;
    virtual std::int64_t tell() const = 0;
    // Total size in bytes, or -1 when unknown.
    virtual std::int64_t size() const = 0;
    // Human-readable origin (path or URL) for logs.
    virtual const std::string &name() const = 0;
    // Makes a read() blocked on another thread (e.g. waiting for the network) return
    // early, and every later read() return 0. Thread-safe; called by the player when it
    // retires a decoder (seek, skip, stop, shutdown). Sources that never block need not
    // implement it.
    virtual void interrupt() {}
};

// Plain file. Returns nullptr if the file cannot be opened or is not a regular file
// (FIFOs, devices and directories are rejected without blocking).
std::unique_ptr<ByteSource> openFileSource(const std::string &path);

} // namespace lm
