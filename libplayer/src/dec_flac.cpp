// FLAC via dr_flac (native FLAC stream; Ogg FLAC is not sniffed as FLAC).
#include "decoders.hpp"

#include "dr_flac.h"

#include <string>
#include <string_view>

namespace lm::detail {
namespace {

class FlacDecoder final : public Decoder {
public:
    explicit FlacDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}
    ~FlacDecoder() override {
        if (flac_) drflac_close(flac_);
    }

    bool init(std::string *err) {
        flac_ = drflac_open_with_metadata(onRead, onSeek, onTell, onMeta, this, nullptr);
        if (!flac_) {
            if (err) *err = "dr_flac: not a decodable FLAC: " + src_->name();
            return false;
        }
        return true;
    }

    Format format() const override { return Format::Flac; }
    std::uint32_t sampleRate() const override { return flac_->sampleRate; }
    std::uint32_t channels() const override { return flac_->channels; }
    std::uint64_t totalFrames() const override { return flac_->totalPCMFrameCount; }
    std::uint64_t read(float *out, std::uint64_t frames) override {
        const std::uint64_t n = drflac_read_pcm_frames_f32(flac_, frames, out);
        // dr_flac reports corrupt or missing frames only as a short read; with a known
        // length (STREAMINFO), stopping before it is an error, not the end.
        if (n < frames && error_.empty() && flac_->totalPCMFrameCount &&
            flac_->currentPCMFrame < flac_->totalPCMFrameCount)
            error_ = "dr_flac: stream ends at frame " + std::to_string(flac_->currentPCMFrame) + " of " +
                     std::to_string(flac_->totalPCMFrameCount) + " (truncated or corrupt): " + src_->name();
        return n;
    }
    bool seek(std::uint64_t frame) override { return drflac_seek_to_pcm_frame(flac_, frame) != 0; }

private:
    static size_t onRead(void *u, void *dst, size_t n) { return static_cast<FlacDecoder *>(u)->src_->read(dst, n); }
    static drflac_bool32 onSeek(void *u, int offset, drflac_seek_origin o) {
        const auto w = o == DRFLAC_SEEK_SET   ? ByteSource::Whence::Set
                       : o == DRFLAC_SEEK_CUR ? ByteSource::Whence::Cur
                                              : ByteSource::Whence::End;
        return static_cast<FlacDecoder *>(u)->src_->seek(offset, w) ? DRFLAC_TRUE : DRFLAC_FALSE;
    }
    static drflac_bool32 onTell(void *u, drflac_int64 *cursor) {
        *cursor = static_cast<FlacDecoder *>(u)->src_->tell();
        return *cursor >= 0 ? DRFLAC_TRUE : DRFLAC_FALSE;
    }
    static void onMeta(void *u, drflac_metadata *m) {
        auto *self = static_cast<FlacDecoder *>(u);
        if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
        drflac_vorbis_comment_iterator it;
        drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
        drflac_uint32 len = 0;
        while (const char *c = drflac_next_vorbis_comment(&it, &len)) {
            const std::string_view s(c, len);
            if (!self->gain_) self->gain_ = parseVorbisCommentGain(s);
            applyVorbisComment(s, self->tags_);
        }
    }

    std::unique_ptr<ByteSource> src_;
    drflac *flac_ = nullptr;
};

} // namespace

std::unique_ptr<Decoder> openFlac(std::unique_ptr<ByteSource> src, std::string *err) {
    auto d = std::make_unique<FlacDecoder>(std::move(src));
    if (!d->init(err)) return nullptr;
    return d;
}

} // namespace lm::detail
