#pragma once

#include <opent1/core/base.hpp>
#include <opent1/core/types.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <numeric>

namespace opent1 {

using div_t32 = DivResult32;

// -----------------------------------------------------------------------------
// Float <-> Fixed16 / Int32 Conversions (uplifted from m_base.asm)
// -----------------------------------------------------------------------------
// Per-thread native rounding/exception control on supported CPUs. x87 precision
// selection cannot change SSE arithmetic width; WASM uses explicit conversions.
[[nodiscard]] Int16 m_fpuGetControlState() noexcept;
void m_fpuSetControlState(Int16 state) noexcept;

class ScopedFpuControl final {
public:
    explicit ScopedFpuControl(Int16 state) noexcept : saved_(m_fpuGetControlState()) {
        m_fpuSetControlState(state);
    }

    ~ScopedFpuControl() {
        m_fpuSetControlState(saved_);
    }

    ScopedFpuControl(const ScopedFpuControl&) = delete;
    ScopedFpuControl& operator=(const ScopedFpuControl&) = delete;

private:
    Int16 saved_;
};

namespace detail {
// Matches FISTP's indefinite INT_MIN result for invalid/out-of-range conversions.
[[nodiscard]] Int32 roundToInt32(RealD value) noexcept;

[[nodiscard]] constexpr Int32 low32(Int64 value) noexcept {
    return std::bit_cast<Int32>(static_cast<UInt32>(value));
}

[[nodiscard]] constexpr DivResult32 divide32(Int64 numer, Int32 denom) {
    OPENT1_ASSERT_FATAL(denom != 0, "Integer divide by zero");
    // Check INT64_MIN/-1 before division (undefined in C++).
    OPENT1_ASSERT_FATAL(!(numer == std::numeric_limits<Int64>::min() && denom == -1),
                        "Integer quotient overflow");
    const Int64 quot = numer / denom;
    return {checkedNarrow<Int32>(quot), static_cast<Int32>(numer % denom)};
}
} // namespace detail

[[nodiscard]] inline RealX m_RealF_to_RealX(RealF value) noexcept {
    return detail::roundToInt32(static_cast<RealD>(value) * 65536.0);
}

inline void m_RealF_to_RealX(RealF value, RealX& out) noexcept {
    out = m_RealF_to_RealX(value);
}

[[nodiscard]] inline Int32 m_RealF_to_Int32(RealF value) noexcept {
    return detail::roundToInt32(value);
}

inline void m_RealF_to_Int32(RealF value, Int32& out) noexcept {
    out = m_RealF_to_Int32(value);
}

// -----------------------------------------------------------------------------
// Fixed-Point 15.16 Multiply & Divide (uplifted from m_fix.asm & m_base.h)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Fix16 m_mulff16(Fix16 a, Fix16 b) {
    return detail::low32((static_cast<Int64>(a) * b) >> kFixShift);
}

[[nodiscard]] constexpr Fix16 m_mulff16R(Fix16 a, Fix16 b) {
    return detail::low32((static_cast<Int64>(a) * b + 0x8000LL) >> kFixShift);
}

[[nodiscard]] constexpr Int32 m_mulf16(Fix16 frac, Int32 val) {
    return m_mulff16(frac, val);
}

[[nodiscard]] constexpr Int32 m_mulf16(Fix16 frac, UInt32 val) {
    return m_mulf16(frac, static_cast<Int32>(val));
}

[[nodiscard]] constexpr RealF m_mulf16(Fix16 frac, RealF val) {
    return static_cast<RealF>((static_cast<RealD>(val) * static_cast<RealD>(frac)) / 65536.0);
}

[[nodiscard]] constexpr RealD m_mulf16(Fix16 frac, RealD val) {
    return (val * static_cast<RealD>(frac)) / 65536.0;
}

[[nodiscard]] constexpr Int32 m_mulf16R(Fix16 frac, Int32 val) {
    return m_mulff16R(frac, val);
}

[[nodiscard]] constexpr Fix16 m_divff16(Fix16 numer, Fix16 denom) {
    OPENT1_ASSERT_FATAL(denom != 0, "m_divff16: divide by zero");
    return detail::divide32(static_cast<Int64>(numer) * kFixOne, denom).quot;
}

[[nodiscard]] constexpr Fix16 m_divff16R(Fix16 numer, Fix16 denom) {
    OPENT1_ASSERT_FATAL(denom != 0, "m_divff16R: divide by zero");
    const bool negResult = (numer < 0) != (denom < 0);
    const UInt64 absNum =
        (numer < 0) ? static_cast<UInt64>(-static_cast<Int64>(numer)) : static_cast<UInt64>(numer);
    const UInt64 absDen =
        (denom < 0) ? static_cast<UInt64>(-static_cast<Int64>(denom)) : static_cast<UInt64>(denom);

    const UInt64 shifted = absNum << static_cast<UInt32>(kFixShift);
    UInt64 quot = shifted / absDen;
    const UInt64 rem = shifted % absDen;
    const UInt64 half = (absDen >> 1U) + (absDen & 1U) - 1U;
    if (half < rem) {
        ++quot;
    }

    // DIV checks the unsigned quotient before NEG; rounding then wraps at 32 bits.
    OPENT1_ASSERT_FATAL(shifted / absDen <= std::numeric_limits<UInt32>::max(),
                        "Rounded fixed quotient overflow");
    const UInt32 bits = static_cast<UInt32>(quot);
    return std::bit_cast<Int32>(negResult ? UInt32{0} - bits : bits);
}

// -----------------------------------------------------------------------------
// 32/64-bit Multiply, Divide, MulDiv (uplifted from m_mul.asm, m_div.asm, m_mdiv.asm)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Int64 m_mul32(Int32 a, Int32 b) {
    return static_cast<Int64>(a) * static_cast<Int64>(b);
}

[[nodiscard]] constexpr Int64 m_mul(Int32 a, Int32 b) {
    return m_mul32(a, b);
}

[[nodiscard]] constexpr DivResult32 m_div32(const Int64& numer, Int32 denom) {
    OPENT1_ASSERT_FATAL(denom != 0, "m_div32: divide by zero");
    return detail::divide32(numer, denom);
}

[[nodiscard]] constexpr DivResult32 m_div(const Int64& numer, Int32 denom) {
    return m_div32(numer, denom);
}

[[nodiscard]] constexpr DivResult32 m_div(Int32 numer, Int32 denom) {
    return m_div32(static_cast<Int64>(numer), denom);
}

[[nodiscard]] constexpr Int32 m_muldiv32(Int32 numer, Int32 multp, Int32 denom) {
    OPENT1_ASSERT_FATAL(denom != 0, "m_muldiv32: divide by zero");
    return detail::divide32(static_cast<Int64>(numer) * multp, denom).quot;
}

[[nodiscard]] constexpr Int32 m_muldiv(Int32 numer, Int32 multp, Int32 denom) {
    return m_muldiv32(numer, multp, denom);
}

[[nodiscard]] constexpr RealF m_muldiv(RealF numer, RealF multp, RealF denom) {
    return (numer * multp) / denom;
}

[[nodiscard]] constexpr RealD m_muldiv(RealD numer, RealD multp, RealD denom) {
    return (numer * multp) / denom;
}

[[nodiscard]] constexpr Int32 m_mdiv(Int32 numer, Int32 multp, Int32 denom) {
    return m_muldiv32(numer, multp, denom);
}

[[nodiscard]] constexpr RealF m_mdiv(RealF numer, RealF multp, RealF denom) {
    return m_muldiv(numer, multp, denom);
}

[[nodiscard]] constexpr RealD m_mdiv(RealD numer, RealD multp, RealD denom) {
    return m_muldiv(numer, multp, denom);
}

[[nodiscard]] constexpr Int32 m_muludiv32(Int32 numer, Int32 multp, UInt32 denom) {
    OPENT1_ASSERT_FATAL(denom != 0U, "m_muludiv32: divide by zero");
    const UInt64 uNumer = static_cast<UInt64>(static_cast<UInt32>(numer));
    const UInt64 uMultp = static_cast<UInt64>(static_cast<UInt32>(multp));
    const UInt64 quot = (uNumer * uMultp) / denom;
    OPENT1_ASSERT_FATAL(quot <= std::numeric_limits<UInt32>::max(), "Unsigned quotient overflow");
    return std::bit_cast<Int32>(static_cast<UInt32>(quot));
}

[[nodiscard]] constexpr Int32 m_muludiv(Int32 numer, Int32 multp, UInt32 denom) {
    return m_muludiv32(numer, multp, denom);
}

[[nodiscard]] constexpr UInt32 m_muludiv(UInt32 numer, UInt32 multp, UInt32 denom) {
    return static_cast<UInt32>(
        m_muludiv32(static_cast<Int32>(numer), static_cast<Int32>(multp), denom));
}

// -----------------------------------------------------------------------------
// Bit Reinterpretation & Fast Float Approximations (Jim Blinn, m_base.asm)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Int32 asInt(RealF f) {
    return std::bit_cast<Int32>(f);
}

[[nodiscard]] constexpr RealF asFloat(Int32 i) {
    return std::bit_cast<RealF>(i);
}

namespace detail {
inline constexpr UInt32 kAsIntegerOne = 0x3F800000U;
inline constexpr UInt32 kScaleUpBits = 0x4B000000U;  // 8388608.0f (2^23)
inline constexpr UInt32 kScaleDnBits = 0x34000000U;  // 1.0f / 8388608.0f (2^-23)
inline constexpr UInt32 kNegLog2EBits = 0xBF317218U; // -log2(e)
} // namespace detail

[[nodiscard]] inline RealF m_sqrt_RealF(RealF a) {
    return std::sqrt(a);
}

[[nodiscard]] constexpr RealF m_sqrtf_RealF(RealF a) {
    const UInt32 bits = (std::bit_cast<UInt32>(a) >> 1U) + (detail::kAsIntegerOne >> 1U);
    return std::bit_cast<RealF>(bits);
}

[[nodiscard]] constexpr RealF m_invsqrtf_RealF(RealF a) {
    const UInt32 bits =
        (detail::kAsIntegerOne + (detail::kAsIntegerOne >> 1U)) - (std::bit_cast<UInt32>(a) >> 1U);
    return std::bit_cast<RealF>(bits);
}

[[nodiscard]] inline RealF m_log2_RealF(RealF a) {
    return std::log2(a);
}

[[nodiscard]] constexpr RealF m_log2f_RealF(RealF a) {
    const Int32 diff = static_cast<Int32>(std::bit_cast<UInt32>(a) - detail::kAsIntegerOne);
    const RealF scaleDn = std::bit_cast<RealF>(detail::kScaleDnBits);
    return static_cast<RealF>(diff) * scaleDn;
}

[[nodiscard]] inline RealF m_pow2f_RealF(RealF a) {
    const RealF scaleUp = std::bit_cast<RealF>(detail::kScaleUpBits);
    const Int32 scaled = detail::roundToInt32(static_cast<RealD>(a) * scaleUp);
    const UInt32 resBits = static_cast<UInt32>(scaled) + detail::kAsIntegerOne;
    return std::bit_cast<RealF>(resBits);
}

[[nodiscard]] inline RealF m_fog_RealF(RealF w, RealF d) {
    const RealF negLog2E = std::bit_cast<RealF>(detail::kNegLog2EBits);
    const RealF scaleUp = std::bit_cast<RealF>(detail::kScaleUpBits);
    const RealD exponent = (static_cast<RealD>(d) * negLog2E) * w;
    const Int32 scaled = detail::roundToInt32(exponent * scaleUp);
    const RealF f = std::bit_cast<RealF>(static_cast<UInt32>(scaled) + detail::kAsIntegerOne);
    return 1.0f - f;
}

// -----------------------------------------------------------------------------
// Integer Square Root & Log2 (uplifted from m_sqrt.asm & m_log.asm)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Int32 m_sqrt_(Int32 n) {
    if (n <= 0) {
        return 0;
    }
    const UInt32 un = static_cast<UInt32>(n);
    UInt32 curBit = highestBitIndex(un) >> 1U;
    UInt32 curRoot = 1U << curBit;
    if (curBit == 0U) {
        return static_cast<Int32>(curRoot);
    }

    UInt32 curSq = curRoot << curBit;
    UInt32 curMask = curRoot >> 1U;
    --curBit;

    while (curMask != 0U) {
        const UInt32 shiftedRoot = (curRoot << curBit) << 1U;
        const UInt32 newSq = (curMask << curBit) + curSq + shiftedRoot;
        if (newSq <= un) {
            curRoot += curMask;
            curSq = newSq;
        }
        curMask >>= 1U;
        if (curBit > 0U) {
            --curBit;
        }
    }
    return static_cast<Int32>(curRoot);
}

[[nodiscard]] constexpr Int32 m_log2_(Int32 l) {
    if (l <= 0) {
        return 0;
    }
    const UInt32 ul = static_cast<UInt32>(l);
    const UInt32 bsr = highestBitIndex(ul);
    const UInt32 hi = bsr << 16U;
    // Note: x86 `shl edx, cl` masks shift count by 31, so when bsr == 0, cl == 32 -> shift by 0
    const UInt32 shift = (32U - bsr) & 31U;
    const UInt32 lo = ((ul << shift) >> 16U) & 0xFFFFU;
    return static_cast<Int32>(hi | lo);
}

// -----------------------------------------------------------------------------
// Basic Scalar Math Overloads (m_abs, m_min, m_max, m_clamp, m_sgn, m_sqr, m_cube, etc.)
// -----------------------------------------------------------------------------
[[nodiscard]] inline RealF m_abs(RealF val) {
    return std::fabs(val);
}

[[nodiscard]] inline RealD m_abs(RealD val) {
    return std::fabs(val);
}

[[nodiscard]] constexpr Int32 m_abs(Int32 val) {
    OPENT1_ASSERT_FATAL(val != std::numeric_limits<decltype(val)>::min(),
                        "Absolute value overflow");
    return (val >= 0) ? val : -val;
}

[[nodiscard]] constexpr Int64 m_abs(Int64 val) {
    OPENT1_ASSERT_FATAL(val != std::numeric_limits<decltype(val)>::min(),
                        "Absolute value overflow");
    return (val >= 0) ? val : -val;
}

template <typename T> [[nodiscard]] constexpr T m_min(T a, T b) {
    return (a < b) ? a : b;
}

template <typename T> [[nodiscard]] constexpr T m_max(T a, T b) {
    return (a > b) ? a : b;
}

template <typename T> [[nodiscard]] constexpr T m_clamp(T val, T low, T high) {
    return (val < low) ? low : ((val > high) ? high : val);
}

template <typename T> [[nodiscard]] constexpr T m_sgn(T val) {
    return static_cast<T>((T{0} < val) - (val < T{0}));
}

template <typename T> [[nodiscard]] constexpr T m_sqr(T val) {
    if constexpr (std::integral<T> && !std::same_as<T, bool>) {
        using U = std::make_unsigned_t<T>;
        const auto bits =
            static_cast<U>(static_cast<UInt64>(static_cast<U>(val)) * static_cast<U>(val));
        return std::bit_cast<T>(bits);
    } else {
        return val * val;
    }
}

template <typename T> [[nodiscard]] constexpr T m_cube(T val) {
    if constexpr (std::integral<T> && !std::same_as<T, bool>) {
        using U = std::make_unsigned_t<T>;
        const auto bits =
            static_cast<U>(static_cast<UInt64>(static_cast<U>(m_sqr(val))) * static_cast<U>(val));
        return std::bit_cast<T>(bits);
    } else {
        return val * val * val;
    }
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
[[nodiscard]] constexpr T m_gcd(T a, T b) {
    using U = std::make_unsigned_t<T>;
    const auto magnitude = [](T value) {
        if constexpr (std::signed_integral<T>) {
            return value < 0 ? static_cast<U>(U{0} - static_cast<U>(value)) : static_cast<U>(value);
        } else {
            return value;
        }
    };
    return checkedNarrow<T>(std::gcd(magnitude(a), magnitude(b)));
}

template <std::integral T>
    requires(!std::same_as<T, bool>)
[[nodiscard]] constexpr T m_lcm(T a, T b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    if constexpr (std::signed_integral<T>) {
        OPENT1_ASSERT_FATAL(a != std::numeric_limits<T>::min() &&
                                b != std::numeric_limits<T>::min(),
                            "LCM overflow");
        a = a < 0 ? static_cast<T>(-a) : a;
        b = b < 0 ? static_cast<T>(-b) : b;
    }
    const auto divided = static_cast<T>(a / m_gcd(a, b));
    OPENT1_ASSERT_FATAL(divided <= std::numeric_limits<T>::max() / b, "LCM overflow");
    return static_cast<T>(divided * b);
}

// Bitwise shifts & rotations
[[nodiscard]] constexpr Int32 m_sar(Int32 val, Int32 shift) {
    return val >> (shift & 31);
}

[[nodiscard]] constexpr Int32 m_shl(Int32 val, Int32 shift) {
    return static_cast<Int32>(static_cast<UInt32>(val) << (shift & 31));
}

[[nodiscard]] constexpr UInt32 m_shr(UInt32 val, Int32 shift) {
    return val >> (shift & 31);
}

[[nodiscard]] constexpr Int32 m_shr(Int32 val, Int32 shift) {
    return static_cast<Int32>(static_cast<UInt32>(val) >> (shift & 31));
}

[[nodiscard]] constexpr UInt32 m_rol(UInt32 val, Int32 shift) {
    return std::rotl(val, shift);
}

[[nodiscard]] constexpr UInt32 m_ror(UInt32 val, Int32 shift) {
    return std::rotr(val, shift);
}

// Square root, inverse square root, log2, pow2, fog
[[nodiscard]] inline RealF m_sqrt(RealF val) {
    OPENT1_ASSERT_FATAL(val >= 0.0f, "m_sqrt: domain error");
    return m_sqrt_RealF(val);
}

[[nodiscard]] inline RealD m_sqrt(RealD val) {
    OPENT1_ASSERT_FATAL(val >= 0.0, "m_sqrt: domain error");
    return std::sqrt(val);
}

[[nodiscard]] inline Int32 m_sqrt(Int32 val) {
    OPENT1_ASSERT_FATAL(val >= 0, "m_sqrt: domain error");
    return m_sqrt_(val);
}

[[nodiscard]] inline RealF m_sqrtf(RealF val) {
    OPENT1_ASSERT_FATAL(val >= 0.0f, "m_sqrtf: domain error");
    return m_sqrtf_RealF(val);
}

[[nodiscard]] inline RealD m_sqrtf(RealD val) {
    OPENT1_ASSERT_FATAL(val >= 0.0, "m_sqrtf: domain error");
    return std::sqrt(val);
}

[[nodiscard]] inline RealF m_invsqrt(RealF val) {
    OPENT1_ASSERT_FATAL(val >= 0.0f, "m_invsqrt: domain error");
    return 1.0f / m_sqrt_RealF(val);
}

[[nodiscard]] inline RealD m_invsqrt(RealD val) {
    OPENT1_ASSERT_FATAL(val >= 0.0, "m_invsqrt: domain error");
    return 1.0 / std::sqrt(val);
}

[[nodiscard]] inline RealF m_invsqrtf(RealF val) {
    OPENT1_ASSERT_FATAL(val >= 0.0f, "m_invsqrtf: domain error");
    return m_invsqrtf_RealF(val);
}

[[nodiscard]] inline RealD m_invsqrtf(RealD val) {
    OPENT1_ASSERT_FATAL(val >= 0.0, "m_invsqrtf: domain error");
    return 1.0 / std::sqrt(val);
}

[[nodiscard]] inline RealF m_log2(RealF val) {
    OPENT1_ASSERT_FATAL(val >= 0.0f, "m_log2: domain error");
    return m_log2_RealF(val);
}

[[nodiscard]] inline RealD m_log2(RealD val) {
    OPENT1_ASSERT_FATAL(val >= 0.0, "m_log2: domain error");
    return std::log2(val);
}

[[nodiscard]] inline Int32 m_log2(Int32 val) {
    OPENT1_ASSERT_FATAL(val >= 0, "m_log2: domain error");
    return m_log2_(val);
}

[[nodiscard]] inline RealF m_log2f(RealF val) {
    OPENT1_ASSERT_FATAL(val >= 0.0f, "m_log2f: domain error");
    return m_log2f_RealF(val);
}

[[nodiscard]] inline RealD m_log2f(RealD val) {
    return m_log2(val);
}

[[nodiscard]] inline RealF m_pow2(RealF val) {
    return std::exp2(val);
}

[[nodiscard]] inline RealD m_pow2(RealD val) {
    return std::exp2(val);
}

[[nodiscard]] inline RealF m_pow2f(RealF val) {
    return m_pow2f_RealF(val);
}

[[nodiscard]] inline RealD m_pow2f(RealD val) {
    return std::exp2(val);
}

[[nodiscard]] inline RealF m_fog(RealF w, RealF density) {
    return m_fog_RealF(w, density);
}

// -----------------------------------------------------------------------------
// Hypotenuse Functions (from m_base.h)
// -----------------------------------------------------------------------------
[[nodiscard]] inline RealF m_hyp(RealF a, RealF b) {
    return m_sqrt(a * a + b * b);
}

[[nodiscard]] inline RealF m_hyp(RealF a, RealF b, RealF c) {
    return m_sqrt(a * a + b * b + c * c);
}

[[nodiscard]] inline RealF m_hypf(RealF a, RealF b) {
    return m_hyp(a, b);
}

[[nodiscard]] inline RealF m_hypf(RealF a, RealF b, RealF c) {
    return m_hyp(a, b, c);
}

[[nodiscard]] inline RealF m_hypvf(RealF a, RealF b) {
    return m_hypf(a, b);
}

[[nodiscard]] inline RealF m_hypvf(RealF a, RealF b, RealF c) {
    return m_hypf(a, b, c);
}

[[nodiscard]] inline RealD m_hyp(RealD a, RealD b) {
    return m_sqrt(a * a + b * b);
}

[[nodiscard]] inline RealD m_hyp(RealD a, RealD b, RealD c) {
    return m_sqrt(a * a + b * b + c * c);
}

[[nodiscard]] inline RealD m_hypf(RealD a, RealD b) {
    return m_hyp(a, b);
}

[[nodiscard]] inline RealD m_hypf(RealD a, RealD b, RealD c) {
    return m_hyp(a, b, c);
}

[[nodiscard]] inline RealD m_hypvf(RealD a, RealD b) {
    return m_hypf(a, b);
}

[[nodiscard]] inline RealD m_hypvf(RealD a, RealD b, RealD c) {
    return m_hypf(a, b, c);
}

[[nodiscard]] inline Int32 m_hyp(Int32 a, Int32 b) {
    const auto aa = static_cast<Int64>(a) * a;
    const auto bb = static_cast<Int64>(b) * b;
    OPENT1_ASSERT_FATAL(aa <= std::numeric_limits<Int32>::max() &&
                            bb <= std::numeric_limits<Int32>::max() - aa,
                        "Integer hypotenuse overflow");
    return m_sqrt(static_cast<Int32>(aa + bb));
}

[[nodiscard]] inline Int32 m_hyp(Int32 a, Int32 b, Int32 c) {
    const auto aa = static_cast<Int64>(a) * a;
    const auto bb = static_cast<Int64>(b) * b;
    const auto cc = static_cast<Int64>(c) * c;
    OPENT1_ASSERT_FATAL(aa <= std::numeric_limits<Int32>::max() &&
                            bb <= std::numeric_limits<Int32>::max() - aa &&
                            cc <= std::numeric_limits<Int32>::max() - aa - bb,
                        "Integer hypotenuse overflow");
    return m_sqrt(static_cast<Int32>(aa + bb + cc));
}

[[nodiscard]] constexpr Int32 m_hypf(Int32 x, Int32 y) {
    const Int32 ax = m_abs(x);
    const Int32 ay = m_abs(y);
    return checkedNarrow<Int32>(static_cast<Int64>(std::max(ax, ay)) + std::min(ax, ay) / 2);
}

[[nodiscard]] constexpr Int32 m_hypf(Int32 x, Int32 y, Int32 z) {
    std::array sorted{m_abs(x), m_abs(y), m_abs(z)};
    std::ranges::sort(sorted, std::greater{});
    const auto [ax, ay, az] = sorted;
    return checkedNarrow<Int32>(static_cast<Int64>(ax) + (static_cast<Int64>(ay) * 11) / 32 +
                                az / 4);
}

[[nodiscard]] constexpr Int32 m_hypvf(Int32 x, Int32 y) {
    const Int32 ax = m_abs(x);
    const Int32 ay = m_abs(y);
    return (ax > ay) ? ax : ay;
}

[[nodiscard]] constexpr Int32 m_hypvf(Int32 x, Int32 y, Int32 z) {
    const Int32 ax = m_abs(x);
    const Int32 ay = m_abs(y);
    const Int32 az = m_abs(z);
    return m_max(ax, m_max(ay, az));
}

// -----------------------------------------------------------------------------
// Range Check & Limit Functions (from m_base.h)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Bool m_check_range(RealF val, RealF range) {
    return (val <= range) && (val >= -range);
}

[[nodiscard]] constexpr RealF m_limit_range(RealF val, RealF range) {
    if (val >= range) {
        return range;
    }
    if (val <= -range) {
        return -range;
    }
    return val;
}

[[nodiscard]] constexpr Bool m_check_range(RealD val, RealD range) {
    return (val <= range) && (val >= -range);
}

[[nodiscard]] constexpr RealD m_limit_range(RealD val, RealD range) {
    if (val >= range) {
        return range;
    }
    if (val <= -range) {
        return -range;
    }
    return val;
}

[[nodiscard]] constexpr Bool m_check_range(Int32 val, Int32 range) {
    OPENT1_ASSERT_FATAL(range >= 0, "Negative scalar range");
    return (val <= range) && (val >= -range);
}

[[nodiscard]] constexpr Int32 m_limit_range(Int32 val, Int32 range) {
    OPENT1_ASSERT_FATAL(range >= 0, "Negative scalar range");
    if (val >= range) {
        return range;
    }
    if (val <= -range) {
        return -range;
    }
    return val;
}

// -----------------------------------------------------------------------------
// Bit Scanning Functions (uplifted from m_bscan.asm)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Int32 m_bsr_(Int32 val) {
    if (val == 0) {
        return -1;
    }
    return static_cast<Int32>(31 - std::countl_zero(static_cast<UInt32>(val)));
}

[[nodiscard]] constexpr Int32 m_bsf_(Int32 val) {
    if (val == 0) {
        return -1;
    }
    return static_cast<Int32>(std::countr_zero(static_cast<UInt32>(val)));
}

[[nodiscard]] inline Int32 m_bsr(Int32 val) {
    OPENT1_ASSERT_FATAL(val >= 0, "m_bsr: domain error");
    return m_bsr_(val);
}

[[nodiscard]] inline Int32 m_bsf(Int32 val) {
    OPENT1_ASSERT_FATAL(val >= 0, "m_bsf: domain error");
    return m_bsf_(val);
}

[[nodiscard]] constexpr Int32 m_BSR(Int32 val) {
    return m_bsr_(val);
}

[[nodiscard]] constexpr Int32 m_BSR(UInt32 val) {
    return val == 0U ? -1 : static_cast<Int32>(31 - std::countl_zero(val));
}

[[nodiscard]] constexpr Int32 m_BSF(Int32 val) {
    return m_bsf_(val);
}

[[nodiscard]] constexpr Int32 m_BSF(UInt32 val) {
    return val == 0U ? -1 : static_cast<Int32>(std::countr_zero(val));
}

// -----------------------------------------------------------------------------
// Range Mapping Functions (uplifted from m_map.asm)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Int32 m_map32(Int32 dval, Int32 dmin, Int32 dmax, Int32 rmin, Int32 rmax) {
    const Int32 dRange =
        std::bit_cast<Int32>(static_cast<UInt32>(dmax) - static_cast<UInt32>(dmin));
    OPENT1_ASSERT_FATAL(dRange != 0, "m_map32: zero domain range");
    const Int64 absD = (dRange < 0) ? -static_cast<Int64>(dRange) : dRange;
    Int64 correction = absD - 1;
    const Int32 rRange =
        std::bit_cast<Int32>(static_cast<UInt32>(rmax) - static_cast<UInt32>(rmin));
    // JGE in the original tests the signed subtraction flags, even when it wraps.
    if (rmax < rmin) {
        correction = -correction;
    }
    const Int32 offset =
        std::bit_cast<Int32>(static_cast<UInt32>(dval) - static_cast<UInt32>(dmin));
    const Int64 num = static_cast<Int64>(offset) * rRange + correction;
    const auto quotient = detail::divide32(num, dRange).quot;
    return std::bit_cast<Int32>(static_cast<UInt32>(rmin) + static_cast<UInt32>(quotient));
}

[[nodiscard]] constexpr Int32 m_map(Int32 dval, Int32 dmin, Int32 dmax, Int32 rmin, Int32 rmax) {
    return m_map32(dval, dmin, dmax, rmin, rmax);
}

[[nodiscard]] constexpr Int32 m_map2(Int32 dval, Int32 dmin, Int32 dmax, Int32 rmin, Int32 rmax) {
    return m_map32(dval, dmin, dmax, rmin, rmax);
}

// -----------------------------------------------------------------------------
// Scalar Distance Helper (Point overloads in <opent1/math/point.hpp>)
// -----------------------------------------------------------------------------
template <typename T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] inline T m_dist(T a, T b) {
    if constexpr (std::integral<T>) {
        if constexpr (std::same_as<T, bool>) {
            return a != b;
        } else {
            using U = std::make_unsigned_t<T>;
            const U difference = a < b ? static_cast<U>(static_cast<U>(b) - static_cast<U>(a))
                                       : static_cast<U>(static_cast<U>(a) - static_cast<U>(b));
            return checkedNarrow<T>(difference);
        }
    } else {
        return m_abs(a - b);
    }
}

// -----------------------------------------------------------------------------
// Unit Float <-> Fixed-Point Helpers (from m_base.h)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr Int32 UNIT_TO_UFIX(RealF f, Int32 bits) {
    OPENT1_ASSERT_FATAL(bits >= 2 && bits <= 30 && f >= 0.0f && f <= 1.0f,
                        "Invalid unsigned unit field");
    return static_cast<Int32>(static_cast<RealD>(f * static_cast<RealF>((1U << bits) - 2U)) + 0.5);
}

[[nodiscard]] constexpr RealF UFIX_TO_UNIT(Int32 f, Int32 bits) {
    OPENT1_ASSERT_FATAL(bits >= 2 && bits <= 30, "Invalid unsigned unit width");
    return static_cast<RealF>(f) / static_cast<RealF>((1U << bits) - 2U);
}

[[nodiscard]] constexpr Int32 UNIT_TO_SFIX(RealF f, Int32 bits) {
    OPENT1_ASSERT_FATAL(bits >= 1 && bits <= 30 && f >= -1.0f && f <= 1.0f,
                        "Invalid signed unit field");
    return static_cast<Int32>(static_cast<RealD>(f * static_cast<RealF>(1U << (bits - 1))) + 0.5);
}

[[nodiscard]] constexpr RealF SFIX_TO_UNIT(Int32 f, Int32 bits) {
    OPENT1_ASSERT_FATAL(bits >= 1 && bits <= 30, "Invalid signed unit width");
    const UInt32 mask = 1U << static_cast<UInt32>(bits);
    UInt32 uf = static_cast<UInt32>(f);
    if ((uf & mask) != 0U) {
        uf |= ~(mask - 1U);
    }
    return static_cast<RealF>(static_cast<Int32>(uf)) / static_cast<RealF>(1 << (bits - 1));
}

} // namespace opent1
