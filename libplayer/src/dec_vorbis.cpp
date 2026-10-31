// Ogg Vorbis via libvorbisfile.
#include "decoders.hpp"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <climits>
#include <cstdio>

namespace lm::detail {
namespace {

class VorbisDecoder final : public Decoder {
public:
    explicit VorbisDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}
    ~VorbisDecoder() override {
        if (open_) ov_clear(&vf_);
    }

    bool init(std::string *err) {
        const ov_callbacks cbs = {onRead, onSeek, nullptr, onTell};
        const int e = ov_open_callbacks(this, &vf_, nullptr, 0, cbs);
        if (e != 0) {
            if (err) *err = "vorbisfile: open failed (" + std::to_string(e) + "): " + src_->name();
            return false;
        }
        open_ = true;
        // With a chained (multi-link) file vorbisfile 1.3.7 can leave the decode position
        // at the start of the LAST link after its open-time scan (seen with ffmpeg-made
        // chains), silently skipping the earlier links. Rewind explicitly.
        if (ov_seekable(&vf_) && ov_streams(&vf_) > 1 && ov_raw_seek(&vf_, 0) != 0) {
            if (err) *err = "vorbisfile: cannot rewind chained stream: " + src_->name();
            return false;
        }
        const vorbis_info *vi = ov_info(&vf_, -1);
        rate_ = static_cast<std::uint32_t>(vi->rate);
        channels_ = static_cast<std::uint32_t>(vi->channels);
        const ogg_int64_t total = ov_pcm_total(&vf_, -1);
        total_ = total > 0 ? static_cast<std::uint64_t>(total) : 0;
        if (const vorbis_comment *vc = ov_comment(&vf_, -1)) {
            for (int i = 0; i < vc->comments; i++) {
                if (!vc->user_comments[i] || vc->comment_lengths[i] < 0) continue;
                const std::string_view s(vc->user_comments[i], static_cast<std::size_t>(vc->comment_lengths[i]));
                if (!gain_) gain_ = parseVorbisCommentGain(s);
                applyVorbisComment(s, tags_);
            }
        }
        return true;
    }

    Format format() const override { return Format::Vorbis; }
    std::uint32_t sampleRate() const override { return rate_; }
    std::uint32_t channels() const override { return channels_; }
    std::uint64_t totalFrames() const override { return total_; }
    std::uint64_t read(float *out, std::uint64_t frames) override {
        std::uint64_t done = 0;
        while (done < frames) {
            float **pcm = nullptr;
            int link = 0;
            const long got = ov_read_float(&vf_, &pcm, static_cast<int>(std::min<std::uint64_t>(frames - done, 4096)), &link);
            if (got == OV_HOLE) continue;
            if (got < 0) {
                error_ = "vorbisfile: decode error (" + std::to_string(got) + "): " + src_->name();
                break;
            }
            if (got == 0) break;
            // A chained stream whose later link changes the layout cannot continue in the
            // same PCM stream; report it rather than mixing layouts. The frames of this
            // read already belong to the new link, so they are dropped.
            const vorbis_info *vi = ov_info(&vf_, link);
            if (!vi || vi->channels != static_cast<int>(channels_) || static_cast<std::uint32_t>(vi->rate) != rate_) {
                error_ = "vorbisfile: chained stream changes to " + std::to_string(vi ? vi->channels : 0) + " ch / " +
                         std::to_string(vi ? vi->rate : 0) + " Hz at link " + std::to_string(link) +
                         " (unsupported): " + src_->name();
                break;
            }
            for (long f = 0; f < got; f++)
                for (std::uint32_t c = 0; c < channels_; c++) out[(done + static_cast<std::uint64_t>(f)) * channels_ + c] = pcm[c][f];
            done += static_cast<std::uint64_t>(got);
        }
        return done;
    }
    bool seek(std::uint64_t frame) override { return ov_pcm_seek(&vf_, static_cast<ogg_int64_t>(frame)) == 0; }

private:
    static size_t onRead(void *dst, size_t size, size_t nmemb, void *u) {
        if (size == 0) return 0;
        return static_cast<VorbisDecoder *>(u)->src_->read(dst, size * nmemb) / size;
    }
    static int onSeek(void *u, ogg_int64_t offset, int whence) {
        const auto w = whence == SEEK_SET   ? ByteSource::Whence::Set
                       : whence == SEEK_CUR ? ByteSource::Whence::Cur
                                            : ByteSource::Whence::End;
        return static_cast<VorbisDecoder *>(u)->src_->seek(offset, w) ? 0 : -1;
    }
    static long onTell(void *u) { return static_cast<long>(static_cast<VorbisDecoder *>(u)->src_->tell()); }

    std::unique_ptr<ByteSource> src_;
    OggVorbis_File vf_{};
    bool open_ = false;
    std::uint32_t rate_ = 0, channels_ = 0;
    std::uint64_t total_ = 0;
};

} // namespace

std::unique_ptr<Decoder> openVorbis(std::unique_ptr<ByteSource> src, std::string *err) {
    auto d = std::make_unique<VorbisDecoder>(std::move(src));
    if (!d->init(err)) return nullptr;
    return d;
}

} // namespace lm::detail
