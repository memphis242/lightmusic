#include "lm/source.hpp"

#include <cstdio>

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
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::int64_t size = -1;
    if (fseeko(f, 0, SEEK_END) == 0) size = static_cast<std::int64_t>(ftello(f));
    if (fseeko(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return nullptr;
    }
    return std::make_unique<FileSource>(f, path, size);
}

} // namespace lm
