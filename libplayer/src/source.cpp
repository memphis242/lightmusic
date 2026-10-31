#include "lm/source.hpp"

#include <cstdio>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lm {
namespace {

class FileSource final : public ByteSource {
public:
    FileSource(std::FILE *f, std::string path, std::int64_t size) : f_(f), path_(std::move(path)), size_(size) {}
    ~FileSource() override { std::fclose(f_); }

    std::size_t read(void *dst, std::size_t n) override { return std::fread(dst, 1, n, f_); }
    bool seek(std::int64_t offset, Whence whence) override {
        const int w = whence == Whence::Set ? SEEK_SET : whence == Whence::Cur ? SEEK_CUR : SEEK_END;
        return fseeko(f_, static_cast<off_t>(offset), w) == 0;
    }
    std::int64_t tell() const override { return static_cast<std::int64_t>(ftello(f_)); }
    std::int64_t size() const override { return size_; }
    const std::string &name() const override { return path_; }

private:
    std::FILE *f_;
    std::string path_;
    std::int64_t size_;
};

} // namespace

std::unique_ptr<ByteSource> openFileSource(const std::string &path) {
    // Regular files only: opening a FIFO (or a device) would block or never end. The
    // open itself is non-blocking so a FIFO without a writer is rejected, not waited on.
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return nullptr;
    }
    const int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    std::FILE *f = fdopen(fd, "rb");
    if (!f) {
        ::close(fd);
        return nullptr;
    }
    std::int64_t size = -1;
    if (fseeko(f, 0, SEEK_END) == 0) size = static_cast<std::int64_t>(ftello(f));
    if (fseeko(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return nullptr;
    }
    return std::make_unique<FileSource>(f, path, size);
}

} // namespace lm
