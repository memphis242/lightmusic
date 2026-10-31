// Ogg Opus via libopusfile. Always 48 kHz; decoded as stereo (opusfile downmixes
// multichannel streams). opusfile applies the OpusHead output gain and trims pre-skip.
#include "decoders.hpp"

#include <opusfile.h>

#include <algorithm>
#include <climits>
#include <cstdio>

namespace lm::detail {
namespace {

class OpusDecoder final : public Decoder {
public:
    explicit OpusDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}
    ~OpusDecoder() override {
        if (of_) op_free(of_);
    }

    bool init(std::string *err) {
        static const OpusFileCallbacks cbs = {onRead, onSeek, onTell, nullptr};
        int e = 0;
        of_ = op_open_callbacks(this, &cbs, nullptr, 0, &e);
        if (!of_) {
            if (err) *err = "opusfile: open failed (" + std::to_string(e) + "): " + src_->name();
            return false;
        }
        const ogg_int64_t total = op_pcm_total(of_, -1);
        total_ = total > 0 ? static_cast<std::uint64_t>(total) : 0;
        int q8 = 0;
        // R128_TRACK_GAIN is relative to -23 LUFS; ReplayGain's reference is ~-18 LUFS.
        if (const OpusTags *tags = op_tags(of_, -1)) {
            if (opus_tags_get_track_gain(tags, &q8) == 0) gain_ = static_cast<float>(q8) / 256.0f + 5.0f;
            for (int i = 0; i < tags->comments; i++)
                if (tags->user_comments[i] && tags->comment_lengths[i] >= 0)
                    applyVorbisComment(std::string_view(tags->user_comments[i],
                                                        static_cast<std::size_t>(tags->comment_lengths[i])),
                                       tags_);
        }
        return true;
    }

    Format format() const override { return Format::Opus; }
    std::uint32_t sampleRate() const override { return 48000; }
    std::uint32_t channels() const override { return 2; }
    std::uint64_t totalFrames() const override { return total_; }
    std::uint64_t read(float *out, std::uint64_t frames) override {
        std::uint64_t done = 0;
        while (done < frames) {
            const std::uint64_t want = std::min<std::uint64_t>(frames - done, INT_MAX / 2);
            const int got = op_read_float_stereo(of_, out + done * 2, static_cast<int>(want * 2));
            if (got == OP_HOLE) continue; // corrupt page skipped; keep going
            if (got < 0) {
                error_ = "opusfile: decode error (" + std::to_string(got) + "): " + src_->name();
                break;
            }
            if (got == 0) break;
            done += static_cast<std::uint64_t>(got);
        }
        return done;
    }
    bool seek(std::uint64_t frame) override { return op_pcm_seek(of_, static_cast<ogg_int64_t>(frame)) == 0; }

private:
    static int onRead(void *u, unsigned char *dst, int n) {
        return static_cast<int>(static_cast<OpusDecoder *>(u)->src_->read(dst, static_cast<std::size_t>(n)));
    }
    static int onSeek(void *u, opus_int64 offset, int whence) {
        const auto w = whence == SEEK_SET   ? ByteSource::Whence::Set
                       : whence == SEEK_CUR ? ByteSource::Whence::Cur
                                            : ByteSource::Whence::End;
        return static_cast<OpusDecoder *>(u)->src_->seek(offset, w) ? 0 : -1;
    }
    static opus_int64 onTell(void *u) { return static_cast<OpusDecoder *>(u)->src_->tell(); }

    std::unique_ptr<ByteSource> src_;
    OggOpusFile *of_ = nullptr;
    std::uint64_t total_ = 0;
};

} // namespace

std::unique_ptr<Decoder> openOpus(std::unique_ptr<ByteSource> src, std::string *err) {
    auto d = std::make_unique<OpusDecoder>(std::move(src));
    if (!d->init(err)) return nullptr;
    return d;
}

} // namespace lm::detail
