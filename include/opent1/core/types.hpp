#pragma once

#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

namespace opent1 {

// -----------------------------------------------------------------------------
// Fixed-Width Protocol & Engine Primitive Types (64-bit & WASM safe)
// -----------------------------------------------------------------------------
using Int8 = std::int8_t;
using Int16 = std::int16_t;
using Int32 = std::int32_t;
using Int64 = std::int64_t;

using UInt8 = std::uint8_t;
using UInt16 = std::uint16_t;
using UInt32 = std::uint32_t;
using UInt64 = std::uint64_t;

using Flt32 = float;
using Flt64 = double;
using Fix16 = std::int32_t; // 15.16 signed fixed-point
using Bool = bool;

using RealX = Fix16;
using RealF = float;
using RealD = double;

using StringTableEntry = const char*;

struct DivResult32 {
    Int32 quot{0};
    Int32 rem{0};
    constexpr bool operator==(const DivResult32&) const noexcept = default;
};

// -----------------------------------------------------------------------------
// Numeric Constants (replacing legacy macros & types.asm constants)
// -----------------------------------------------------------------------------
inline constexpr Int32 kFixShift = 16;
inline constexpr RealX kFixOne = (1 << kFixShift);
inline constexpr RealX kRealXOne = kFixOne;
inline constexpr RealX kRealXHalf = (kFixOne >> 1);
inline constexpr RealX kRealXZero = 0;

inline constexpr RealF kEqualF = 1.0e-6f;
inline constexpr RealF kRealFOne = 1.0f;
inline constexpr RealF kRealFHalf = 0.5f;
inline constexpr RealF kRealFZero = 0.0f;
inline constexpr RealF kRealFPi = std::numbers::pi_v<RealF>;
inline constexpr RealF kRealF2Pi = 2.0f * std::numbers::pi_v<RealF>;

inline constexpr RealD kEqualD = 1.0e-6;
inline constexpr RealD kRealDOne = 1.0;
inline constexpr RealD kRealDHalf = 0.5;
inline constexpr RealD kRealDZero = 0.0;
inline constexpr RealD kRealDPi = std::numbers::pi_v<RealD>;
inline constexpr RealD kRealD2Pi = 2.0 * std::numbers::pi_v<RealD>;

// -----------------------------------------------------------------------------
// Tolerance & Fixed-Point Helpers
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr bool isZero(RealX a) noexcept {
    return a == 0;
}

[[nodiscard]] constexpr bool isEqual(RealX a, RealX b) noexcept {
    return a == b;
}

template <std::floating_point T>
[[nodiscard]] constexpr bool isZero(T a, T tolerance = static_cast<T>(1.0e-6)) noexcept {
    return (a * a) < (tolerance * tolerance);
}

template <std::floating_point T>
[[nodiscard]] constexpr bool isEqual(T a, T b, T tolerance = static_cast<T>(1.0e-6)) noexcept {
    return isZero(a - b, tolerance);
}

[[nodiscard]] constexpr Int32 fix16ToInt(Fix16 value) noexcept {
    return value / kFixOne;
}

[[nodiscard]] constexpr Fix16 intToFix16(Int32 value) noexcept {
    return static_cast<Fix16>(static_cast<UInt32>(value) << kFixShift);
}

// -----------------------------------------------------------------------------
// FourCC Tag Generation (Little-Endian Tag Packing for VOL / Persist / IFF)
// -----------------------------------------------------------------------------
[[nodiscard]] consteval UInt32 makeFourCC(char ch0, char ch1, char ch2, char ch3) noexcept {
    return static_cast<UInt32>(static_cast<UInt8>(ch0)) |
           (static_cast<UInt32>(static_cast<UInt8>(ch1)) << 8U) |
           (static_cast<UInt32>(static_cast<UInt8>(ch2)) << 16U) |
           (static_cast<UInt32>(static_cast<UInt8>(ch3)) << 24U);
}

[[nodiscard]] constexpr UInt32 makeFourCCRuntime(char ch0, char ch1, char ch2, char ch3) noexcept {
    return static_cast<UInt32>(static_cast<UInt8>(ch0)) |
           (static_cast<UInt32>(static_cast<UInt8>(ch1)) << 8U) |
           (static_cast<UInt32>(static_cast<UInt8>(ch2)) << 16U) |
           (static_cast<UInt32>(static_cast<UInt8>(ch3)) << 24U);
}

// -----------------------------------------------------------------------------
// Bit Manipulation Helpers (<bit> replacements for legacy x86 bsr/bsf asm)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr UInt32 highestBitIndex(UInt32 value) noexcept {
    return value == 0U ? 0U : static_cast<UInt32>(31 - std::countl_zero(value));
}

[[nodiscard]] constexpr UInt32 lowestBitIndex(UInt32 value) noexcept {
    return value == 0U ? 0U : static_cast<UInt32>(std::countr_zero(value));
}

[[nodiscard]] constexpr bool isPowerOfTwo(UInt32 value) noexcept {
    return std::has_single_bit(value);
}

// -----------------------------------------------------------------------------
// Color Types (Modernized from darkstar/Core/inc/types.h)
// -----------------------------------------------------------------------------
struct ColorX {
    RealX red{0};
    RealX green{0};
    RealX blue{0};

    constexpr ColorX() noexcept = default;

    constexpr ColorX(RealX r, RealX g, RealX b) noexcept : red(r), green(g), blue(b) {}

    constexpr ColorX& interpolate(const ColorX& c1, const ColorX& c2, RealF t1) noexcept {
        const RealF t2 = 1.0f - t1;
        red = static_cast<RealX>(static_cast<RealF>(c1.red) * t2 + static_cast<RealF>(c2.red) * t1);
        green = static_cast<RealX>(static_cast<RealF>(c1.green) * t2 +
                                   static_cast<RealF>(c2.green) * t1);
        blue =
            static_cast<RealX>(static_cast<RealF>(c1.blue) * t2 + static_cast<RealF>(c2.blue) * t1);
        return *this;
    }

    constexpr bool operator==(const ColorX&) const noexcept = default;
};

struct ColorF {
    RealF red{0.0f};
    RealF green{0.0f};
    RealF blue{0.0f};

    constexpr ColorF() noexcept = default;

    constexpr ColorF(RealF r, RealF g, RealF b) noexcept : red(r), green(g), blue(b) {}

    constexpr void set(RealF r = 0.0f, RealF g = 0.0f, RealF b = 0.0f) noexcept {
        red = r;
        green = g;
        blue = b;
    }

    constexpr ColorF& interpolate(const ColorF& c1, const ColorF& c2, RealF t1) noexcept {
        const RealF t2 = 1.0f - t1;
        red = (c1.red * t2) + (c2.red * t1);
        green = (c1.green * t2) + (c2.green * t1);
        blue = (c1.blue * t2) + (c2.blue * t1);
        return *this;
    }

    constexpr ColorF& operator+=(const ColorF& c) noexcept {
        red += c.red;
        green += c.green;
        blue += c.blue;
        return *this;
    }

    [[nodiscard]] constexpr ColorF operator+(const ColorF& c) const noexcept {
        return {red + c.red, green + c.green, blue + c.blue};
    }

    constexpr ColorF& operator*=(RealF factor) noexcept {
        red *= factor;
        green *= factor;
        blue *= factor;
        return *this;
    }

    [[nodiscard]] constexpr ColorF operator*(RealF factor) const noexcept {
        return {red * factor, green * factor, blue * factor};
    }

    constexpr bool operator==(const ColorF&) const noexcept = default;
};

struct GFXColorInfoF {
    RealF haze{0.0f};
    ColorF color{};
    RealF alpha{1.0f};

    constexpr bool operator==(const GFXColorInfoF&) const noexcept = default;
};

[[nodiscard]] std::string_view engineVersionString() noexcept;

} // namespace opent1
