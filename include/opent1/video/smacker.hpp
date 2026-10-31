#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>

namespace opent1::video {
// PCM bytes are interleaved unsigned 8 or signed little-endian 16, independently
// owned. No decoder object borrows an input stream, manager, or native backend.
struct AudioFormat {
    UInt32 rate{};
    UInt16 channels{}, bits{};
    bool operator==(const AudioFormat&) const = default;
};

struct Pcm {
    AudioFormat format;
    UInt64 firstSample{};
    std::vector<std::byte> bytes;
};

struct Frame {
    UInt32 index{};
    UInt64 timeUs{};
    gfx::Bitmap bitmap;
    std::array<Pcm, 7> audio;
};
class Decoder;

class Movie {
public:
    static constexpr UInt32 MaxFileBytes = 64U * 1024 * 1024, MaxFrames = 100000;
    static std::unique_ptr<Movie> load(StreamIO&, UInt32 size);
    [[nodiscard]] UInt32 width() const;
    [[nodiscard]] UInt32 height() const;
    [[nodiscard]] UInt32 frameCount() const; // excludes the optional ring frame
    [[nodiscard]] UInt64 frameTimeUs() const;
    [[nodiscard]] UInt64 durationUs() const;
    [[nodiscard]] UInt32 verticalScale() const; // authored double/interlaced aspect
    [[nodiscard]] bool ringFrame() const;
    [[nodiscard]] AudioFormat audioFormat(UInt32 track) const;
    [[nodiscard]] UInt64 audioSamples(UInt32 track) const;
    // Bounded random PCM window, at most one second. Short/end windows contain
    // only available samples. Throws on malformed compressed data.
    [[nodiscard]] Pcm audio(UInt32 track, UInt64 firstSample, UInt32 samples) const;

private:
    friend class Decoder;
    struct Data;
    explicit Movie(std::shared_ptr<const Data>);
    std::shared_ptr<const Data> data_;
};

bool registerSmackerLoader(ResourceManager&);

class Decoder {
public:
    explicit Decoder(std::shared_ptr<const Movie>);
    explicit Decoder(Resource<Movie>);
    // next/seek publish only complete immutable frames; failure retains both
    // cursor and last frame. Seek replays delta/palette history from frame zero.
    bool next();
    bool seek(UInt32 frame);

    [[nodiscard]] std::shared_ptr<const Frame> frame() const {
        return frame_;
    }

    [[nodiscard]] std::shared_ptr<const Movie> movie() const {
        return movie_;
    }

private:
    bool decode(UInt32, std::shared_ptr<const Frame>&, bool audio = true) const;
    std::shared_ptr<const Movie> movie_;
    std::shared_ptr<const Frame> frame_;
};
} // namespace opent1::video
