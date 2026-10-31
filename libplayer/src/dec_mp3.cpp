// MP3 via dr_mp3 used directly. dr_mp3 >= 0.7 reads the LAME/Xing header and trims
// encoder delay and padding, which gives the same frame counts as mpg123 in gapless
// mode (bench 2a/2c). miniaudio's ma_decoder is not used: it left ~76 ms untrimmed.
#include "decoders.hpp"

#include "dr_mp3.h"

#include <climits>
#include <string>
#include <vector>

namespace lm::detail {
namespace {

class Mp3Decoder final : public Decoder {
public:
    explicit Mp3Decoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}
    ~Mp3Decoder() override {
        if (inited_) drmp3_uninit(&mp3_);
    }

    bool init(std::string *err) {
        // Same tag reader as lm::readTags (the source is rewound before decoding).
        // TODO: APE tags (REPLAYGAIN_TRACK_GAIN) and the LAME header's RG field.
        readTagsFrom(*src_, tags_, &gain_);
        if (!src_->seek(0, ByteSource::Whence::Set) ||
            !drmp3_init(&mp3_, onRead, onSeek, onTell, nullptr, this, nullptr)) {
            if (err) *err = "dr_mp3: not a decodable MP3: " + src_->name();
            return false;
        }
        inited_ = true;
        // Known (and trimmed) only with a Xing/Info header; no full-file scan here.
        // TODO: for CBR files without Xing, estimate from byte size and bitrate.
        if (mp3_.totalPCMFrameCount != DRMP3_UINT64_MAX) total_ = drmp3_get_pcm_frame_count(&mp3_);
        return true;
    }

    Format format() const override { return Format::Mp3; }
    std::uint32_t sampleRate() const override { return mp3_.sampleRate; }
    std::uint32_t channels() const override { return mp3_.channels; }
    std::uint64_t totalFrames() const override { return total_; }
    std::uint64_t read(float *out, std::uint64_t frames) override {
        const std::uint64_t n = drmp3_read_pcm_frames_f32(&mp3_, frames, out);
        // Like dr_flac, errors show only as a short read; detectable with a known length.
        if (n < frames && error_.empty() && total_ && mp3_.currentPCMFrame < total_)
            error_ = "dr_mp3: stream ends at frame " + std::to_string(mp3_.currentPCMFrame) + " of " +
                     std::to_string(total_) + " (truncated or corrupt): " + src_->name();
        return n;
    }
    bool seek(std::uint64_t frame) override {
        // Without a seek table dr_mp3 decodes from the start to the target. Build one on
        // the first seek (one header scan of the file) so later seeks are cheap.
        if (!seekTableBuilt_) {
            seekTableBuilt_ = true;
            drmp3_uint32 count = kMaxSeekPoints;
            seekPoints_.resize(count);
            if (drmp3_calculate_seek_points(&mp3_, &count, seekPoints_.data()) && count > 0) {
                seekPoints_.resize(count);
                drmp3_bind_seek_table(&mp3_, count, seekPoints_.data());
            } else {
                seekPoints_.clear();
            }
        }
        return drmp3_seek_to_pcm_frame(&mp3_, frame) != 0;
    }

private:
    static constexpr drmp3_uint32 kMaxSeekPoints = 256;

    static size_t onRead(void *u, void *dst, size_t n) { return static_cast<Mp3Decoder *>(u)->src_->read(dst, n); }
    static drmp3_bool32 onSeek(void *u, int offset, drmp3_seek_origin o) {
        const auto w = o == DRMP3_SEEK_SET   ? ByteSource::Whence::Set
                       : o == DRMP3_SEEK_CUR ? ByteSource::Whence::Cur
                                             : ByteSource::Whence::End;
        return static_cast<Mp3Decoder *>(u)->src_->seek(offset, w) ? DRMP3_TRUE : DRMP3_FALSE;
    }
    static drmp3_bool32 onTell(void *u, drmp3_int64 *cursor) {
        *cursor = static_cast<Mp3Decoder *>(u)->src_->tell();
        return *cursor >= 0 ? DRMP3_TRUE : DRMP3_FALSE;
    }
    std::unique_ptr<ByteSource> src_;
    drmp3 mp3_{};
    bool inited_ = false;
    std::uint64_t total_ = 0;
    bool seekTableBuilt_ = false;
    std::vector<drmp3_seek_point> seekPoints_;
};

} // namespace

std::unique_ptr<Decoder> openMp3(std::unique_ptr<ByteSource> src, std::string *err) {
    auto d = std::make_unique<Mp3Decoder>(std::move(src));
    if (!d->init(err)) return nullptr;
    return d;
}

} // namespace lm::detail
