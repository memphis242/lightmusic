#include "tags.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/opusfile.h>
#include <taglib/tag.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>

#include <fcntl.h>
#include <unistd.h>

namespace lm::server {

bool is_audio_path(const std::string &path) {
    auto dot = path.rfind('.');
    auto slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e == "mp3" || e == "flac" || e == "opus" || e == "ogg" || e == "oga";
}

static std::string s(const TagLib::String &t) { return t.to8Bit(true); }

static int leading_int(const TagLib::PropertyMap &p, const char *key) {
    auto it = p.find(key);
    if (it == p.end() || it->second.isEmpty()) return 0;
    return std::atoi(it->second.front().toCString(true));  // "1/2" -> 1
}

bool read_track_meta(TrackMeta &m) {
    TagLib::FileRef f(m.path.c_str(), true, TagLib::AudioProperties::Average);
    if (f.isNull() || !f.file() || !f.file()->isValid()) return false;
    TagLib::File *file = f.file();
    if (dynamic_cast<TagLib::MPEG::File *>(file)) m.codec = "mp3";
    else if (dynamic_cast<TagLib::FLAC::File *>(file)) m.codec = "flac";
    else if (dynamic_cast<TagLib::Ogg::Opus::File *>(file)) m.codec = "opus";
    else if (dynamic_cast<TagLib::Ogg::Vorbis::File *>(file)) m.codec = "vorbis";
    else m.codec = "unknown";
    if (auto *t = f.tag()) {
        m.title = s(t->title());
        m.artist = s(t->artist());
        m.album = s(t->album());
        m.genre = s(t->genre());
        m.year = int(t->year());
        m.track_no = int(t->track());
    }
    TagLib::PropertyMap props = file->properties();
    if (auto it = props.find("ALBUMARTIST"); it != props.end() && !it->second.isEmpty())
        m.album_artist = s(it->second.front());
    m.disc_no = leading_int(props, "DISCNUMBER");
    if (auto *a = f.audioProperties()) {
        m.duration_ms = a->lengthInMilliseconds();
        m.bitrate = a->bitrate();
        m.sample_rate = a->sampleRate();
        m.channels = a->channels();
    }
    if (m.title.empty()) {  // fall back to the file name without extension
        auto slash = m.path.rfind('/');
        std::string base = m.path.substr(slash == std::string::npos ? 0 : slash + 1);
        m.title = base.substr(0, base.rfind('.'));
    }
    return true;
}

namespace {
std::optional<Picture> pick_flac(const TagLib::List<TagLib::FLAC::Picture *> &list) {
    const TagLib::FLAC::Picture *best = nullptr;
    for (auto *p : list) {
        if (!best || p->type() == TagLib::FLAC::Picture::FrontCover) best = p;
        if (p->type() == TagLib::FLAC::Picture::FrontCover) break;
    }
    if (!best) return std::nullopt;
    return Picture{best->mimeType().to8Bit(), std::string(best->data().data(), best->data().size())};
}
}  // namespace

std::optional<Picture> read_embedded_picture(int fd, const std::string &name) {
    if (!is_audio_path(name)) return std::nullopt;
    std::string ext = name.substr(name.rfind('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    // TagLib's FileStream(fd) fdopen()s the descriptor and fclose()s it: hand it a dup.
    int dfd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dfd < 0) return std::nullopt;
    if (::lseek(dfd, 0, SEEK_SET) < 0) {
        ::close(dfd);
        return std::nullopt;
    }
    TagLib::FileStream stream(dfd, true);
    if (!stream.isOpen()) {  // fdopen() failed: the stream does not own dfd
        ::close(dfd);
        return std::nullopt;
    }
    if (ext == "mp3") {
        TagLib::MPEG::File mp(&stream, TagLib::ID3v2::FrameFactory::instance(), false);
        if (!mp.isValid()) return std::nullopt;
        auto *id3 = mp.ID3v2Tag(false);
        if (!id3) return std::nullopt;
        const TagLib::ID3v2::AttachedPictureFrame *best = nullptr;
        for (auto *fr : id3->frameList("APIC")) {
            auto *ap = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame *>(fr);
            if (!ap) continue;
            if (!best || ap->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) best = ap;
            if (ap->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) break;
        }
        if (!best) return std::nullopt;
        return Picture{best->mimeType().to8Bit(),
                       std::string(best->picture().data(), best->picture().size())};
    }
    if (ext == "flac") {
        TagLib::FLAC::File fl(&stream, TagLib::ID3v2::FrameFactory::instance(), false);
        if (!fl.isValid()) return std::nullopt;
        if (auto p = pick_flac(fl.pictureList())) return p;
        if (auto *x = fl.xiphComment(false)) return pick_flac(x->pictureList());
        return std::nullopt;
    }
    if (ext == "opus") {
        TagLib::Ogg::Opus::File op(&stream, false);
        if (!op.isValid() || !op.tag()) return std::nullopt;
        return pick_flac(op.tag()->pictureList());
    }
    // .ogg / .oga: Vorbis (an Opus stream in .ogg is tried second).
    {
        TagLib::Ogg::Vorbis::File vb(&stream, false);
        if (vb.isValid() && vb.tag()) return pick_flac(vb.tag()->pictureList());
    }
    TagLib::Ogg::Opus::File op(&stream, false);
    if (op.isValid() && op.tag()) return pick_flac(op.tag()->pictureList());
    return std::nullopt;
}

}  // namespace lm::server
