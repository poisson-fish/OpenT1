#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <functional>
#include <list>

namespace opent1::gfx {
// Owned ARGB4444 or Terrain2 RGB565 samples. Software lighting accepts ARGB4444
// and consumes its alpha nibble; the hardware batch supports both formats.
// Offsets retain the original GFXLightMap byte-field limits. Dimensions express
// the OpenGL 256 sentinel explicitly, so owned maps never use a zero size.
struct LightMap {
    Point2I size{}, offset{};
    std::vector<UInt16> samples;
    enum class Format { Rgb444, Rgb565 };
    Format format{Format::Rgb444}; // Terrain2's native Glide maps use RGB565
    [[nodiscard]] bool valid() const noexcept;
};

struct TextureHandle {
    UInt32 first{}, second{};
    bool operator==(const TextureHandle&) const = default;
};

struct TextureSettings {
    Point2I size{};   // surface size before mip reduction; need not equal bitmap size
    Point2I offset{}; // signed texture origin before mip reduction
    Int32 lightScale{}, mipLevel{};
    UInt32 noiseKey{}; // explicit, repeatable replacement for pointer-derived noise
};

inline constexpr std::size_t TextureNoiseSize = 2047;
// Original ternary cache dither at bias .5, with the engine RNG's explicit
// default seed so cache rebuilds and captures are repeatable.
[[nodiscard]] std::span<const Int32> defaultTextureNoise();
// Original p_txcach.cpp / p_txrc.asm lighting, including distinct aligned and
// wrapping interpolation policies. Empty noise disables dithering; otherwise
// supply exactly 2047 signed values in [-65535,65535]. No arguments are retained.
// Failure publishes no partial bitmap. Output is padded, opaque indexed data.
[[nodiscard]] std::unique_ptr<Bitmap> lightTexture(const Bitmap&,
                                                   const LightMap&,
                                                   const Palette&,
                                                   const TextureSettings&,
                                                   std::span<const Int32> noise = {});

// Per-instance owned source snapshots and LRU prepared textures. Retained results
// remain valid across replacement, invalidation, eviction and cache destruction.
// Limits count cache-owned sample/pixel bytes, excluding caller-retained results.
// Methods require external synchronization; there is no global/current entry.
class TextureCache {
public:
    using Generator = std::function<std::unique_ptr<Bitmap>(TextureHandle, Point2I)>;

    struct Limits {
        std::size_t entries{3000}, sourceBytes{Bitmap::MaxFileBytes},
            preparedBytes{Bitmap::MaxFileBytes};
    };

    TextureCache();
    explicit TextureCache(Limits);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    bool setPalette(const Palette&); // snapshot; invalidates prepared textures
    bool
        setNoise(std::span<const Int32>); // snapshot; empty disables; invalidates prepared textures
    bool registerTexture(TextureHandle, const Bitmap&, const LightMap&, const TextureSettings&);
    // Deferred procedural texture generation, repeated only after invalidation
    // or prepared-image eviction. Requires square power-of-two size >=4, mip 0,
    // zero offsets and an exact light grid. Returned bitmap must match size and
    // be single-level indexed data. Null/invalid output is retryable failure.
    // Recursive preparation of the same handle fails; callback mutations are
    // allowed but discard this preparation if they change cache inputs.
    bool registerTexture(TextureHandle, Generator, const LightMap&, const TextureSettings&);
    [[nodiscard]] std::shared_ptr<const Bitmap> prepare(TextureHandle);
    bool remove(TextureHandle);
    void clearPrepared() noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::size_t sourceBytes() const noexcept {
        return sourceBytes_;
    }

    [[nodiscard]] std::size_t preparedBytes() const noexcept {
        return preparedBytes_;
    }

private:
    struct Entry;
    std::list<Entry> entries_; // most recently used first
    Limits limits_;
    std::shared_ptr<const Palette> palette_;
    std::vector<Int32> noise_;
    std::vector<TextureHandle> inFlight_;
    UInt64 revision_{};
    std::size_t sourceBytes_{}, preparedBytes_{};
    void erase(std::list<Entry>::iterator) noexcept;
    bool insert(Entry);
};
} // namespace opent1::gfx
