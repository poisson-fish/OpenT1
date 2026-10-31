#pragma once
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <chrono>

namespace opent1::gfx {
// Owned scanline spans replace linked, process-global raster lists. The input
// x/count stay intact so interpolators can advance by left - x after clipping.
struct RasterSpan {
    Int32 y{}, x{}, count{}; // count is width - 1; negative spans are dead
    Int32 left{}, right{-1};
    bool dead{}, leftClipped{}, rightClipped{};
};

// Re-clipping is supported. Returns the visible bounding rectangle, or empty.
[[nodiscard]] std::optional<RectI> clipRasters(std::span<RasterSpan>, RectI) noexcept;

struct PaletteMap {
    using Inclusion = std::array<UInt32, 8>; // MSB-first bit order

    struct Range {
        std::array<Inclusion, 256> inclusion;
        Flt32 maximum{1};
        Int32 levels{};
        [[nodiscard]] bool allows(UInt8 source, UInt8 candidate) const noexcept;
        void allow(UInt8 source, UInt8 candidate, bool enabled) noexcept;
    };

    Range shade, haze;
    PaletteColor hazeColor{255, 255, 255, 0};
    Flt32 gamma{1};
    bool useLuv{};
    PaletteMap();
    [[nodiscard]] bool valid() const noexcept;
    // PMAP version 1 stores only the first 64 bits of each 256-bit inclusion.
    // Loading defaults omitted bits to enabled, matching the legacy constructor.
    // Writing rejects disabled omitted bits rather than silently discarding edits.
    [[nodiscard]] bool legacyRepresentable() const noexcept;
    [[nodiscard]] static std::unique_ptr<PaletteMap> load(StreamIO&, UInt32 size);
    bool write(StreamIO&) const;
};

bool registerPaletteMapLoader(ResourceManager&); // .pmp

// Per-instance, monotonic elapsed nanoseconds replace raw CPU cycles and the
// assembly's overlapping, unchecked global timer stack. Instances are unsynchronized.
class GraphicsTimer {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t Capacity = 20;
    bool push(Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] std::optional<std::chrono::nanoseconds>
    pop(Clock::time_point now = Clock::now()) noexcept;

    [[nodiscard]] std::size_t depth() const noexcept {
        return depth_;
    }

    void reset() noexcept {
        depth_ = 0;
    }

private:
    std::array<Clock::time_point, Capacity> starts_{};
    std::size_t depth_{};
};
} // namespace opent1::gfx
