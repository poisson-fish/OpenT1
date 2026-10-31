#pragma once

#include <opent1/core/base.hpp>
#include <opent1/core/types.hpp>
#include <opent1/math/math_Fix16_and_int.hpp>

#include <bit>
#include <cmath>

namespace opent1 {

// -----------------------------------------------------------------------------
// Angle Types & Constants (Parity with m_trig.h & m_trig.inc)
// -----------------------------------------------------------------------------
using AngleX = Int16; // 16-bit binary angle: 0x4000 = pi/2, 0x8000 (-32768) = pi
using AngleF = RealF;
using AngleD = RealD;

inline constexpr AngleX kAngleXZero = 0;
inline constexpr AngleX kAngleXQuarterPi = static_cast<AngleX>(0x2000); // 45 deg
inline constexpr AngleX kAngleXHalfPi = static_cast<AngleX>(0x4000);    // 90 deg
inline constexpr AngleX kAngleXPi = static_cast<AngleX>(-32768);        // 0x8000, 180 deg

// -----------------------------------------------------------------------------
// Fixed-Point / Binary Angle Trig (uplifted from m_trig.asm, m_atrig.asm, m_atan.asm)
// -----------------------------------------------------------------------------
[[nodiscard]] Fix16 m_AngleX_cos(AngleX angle) noexcept;
[[nodiscard]] Fix16 m_AngleX_sin(AngleX angle) noexcept;
void m_AngleX_sincos(AngleX angle, Fix16& sine, Fix16& cosine) noexcept;

// Inverse fixed inputs require [-65536, 65536]. Atan rejects INT32_MIN
// components (legacy signed absolute value cannot represent them).
[[nodiscard]] AngleX m_asin_(Fix16 x);
[[nodiscard]] AngleX m_acos_(Fix16 x);
[[nodiscard]] AngleX m_atan_(Int32 deltax, Int32 deltay);

[[nodiscard]] inline Fix16 m_sin(AngleX angle) noexcept {
    return m_AngleX_sin(angle);
}

[[nodiscard]] inline Fix16 m_cos(AngleX angle) noexcept {
    return m_AngleX_cos(angle);
}

inline void m_sincos(AngleX angle, Fix16& s, Fix16& c) noexcept {
    m_AngleX_sincos(angle, s, c);
}

[[nodiscard]] inline Fix16 m_tan(AngleX angle) {
    const Fix16 s = m_AngleX_sin(angle);
    const Fix16 c = m_AngleX_cos(angle);
    return m_divff16(s, c);
}

// -----------------------------------------------------------------------------
// Float Trig Functions (m_trig.h & m_reduce.cpp)
// -----------------------------------------------------------------------------
#if defined(OPENT1_NATIVE_X87)
extern "C" void opent1_x87_sincosf(const float*, float*, float*) noexcept;
extern "C" void opent1_x87_sincosd(const double*, double*, double*) noexcept;
extern "C" void opent1_x87_reducef(const float*, float*) noexcept;
extern "C" void opent1_x87_reduced(const double*, double*) noexcept;
#endif
inline void m_AngleF_sincos(AngleF a, RealF& sine, RealF& cosine) noexcept {
#if defined(OPENT1_NATIVE_X87)
    (void)m_fpuGetControlState();
    opent1_x87_sincosf(&a, &sine, &cosine);
#else
    // Legacy FSINCOS stores cosine first; aliased outputs finish with sine.
    const auto sinValue = static_cast<RealF>(std::sin(static_cast<RealD>(a)));
    cosine = static_cast<RealF>(std::cos(static_cast<RealD>(a)));
    sine = sinValue;
#endif
}

// Both legacy variants load the single precision 2*pi constant.
// Native x87 builds preserve the single FPREM instruction, including partial
// reduction of huge inputs. Other targets retain the portable fmod fallback.
[[nodiscard]] inline AngleF m_AngleF_reduce(AngleF a) noexcept {
#if defined(OPENT1_NATIVE_X87)
    (void)m_fpuGetControlState();
    float result{};
    opent1_x87_reducef(&a, &result);
    return result;
#else
    return std::fmod(a, static_cast<RealF>(kRealF2Pi));
#endif
}

[[nodiscard]] AngleF m_reduce(AngleF a) noexcept;

[[nodiscard]] inline RealF m_sin(AngleF a) noexcept {
    return std::sin(a);
}

[[nodiscard]] inline RealF m_cos(AngleF a) noexcept {
    return std::cos(a);
}

[[nodiscard]] inline RealF m_tan(AngleF a) noexcept {
    return std::tan(a);
}

inline void m_sincos(AngleF a, RealF& sine, RealF& cosine) noexcept {
    m_AngleF_sincos(a, sine, cosine);
}

[[nodiscard]] inline AngleF m_asin(RealF val) {
    OPENT1_ASSERT_FATAL(val >= -1.0f && val <= 1.0f, "asin domain error");
    return std::asin(val);
}

[[nodiscard]] inline AngleF m_acos(RealF val) {
    OPENT1_ASSERT_FATAL(val >= -1.0f && val <= 1.0f, "acos domain error");
    return std::acos(val);
}

// Note Darkstar parameter order: m_atan(x, y) computes atan2(y, x)!
[[nodiscard]] inline AngleF m_atan(RealF x, RealF y) {
    OPENT1_ASSERT_FATAL((y != 0.0f) || (x != 0.0f), "X and Y are both zero in atan");
    return std::atan2(y, x);
}

[[nodiscard]] inline AngleF m_atan(RealF val) noexcept {
    return std::atan(val);
}

[[nodiscard]] inline RealF m_ptox(RealF r, AngleF a) noexcept {
    return r * std::cos(a);
}

[[nodiscard]] inline RealF m_ptoy(RealF r, AngleF a) noexcept {
    return r * std::sin(a);
}

// -----------------------------------------------------------------------------
// Double Trig Functions (m_trig.h & m_reduce.cpp)
// -----------------------------------------------------------------------------
inline void m_AngleD_sincos(AngleD a, RealD& sine, RealD& cosine) noexcept {
#if defined(OPENT1_NATIVE_X87)
    (void)m_fpuGetControlState();
    opent1_x87_sincosd(&a, &sine, &cosine);
#else
    // Legacy FSINCOS stores cosine first; aliased outputs finish with sine.
    const auto sinValue = static_cast<RealD>(std::sin(static_cast<RealD>(a)));
    cosine = static_cast<RealD>(std::cos(static_cast<RealD>(a)));
    sine = sinValue;
#endif
}

// Both legacy variants load the single precision 2*pi constant.
// Native x87 builds preserve the single FPREM instruction, including partial
// reduction of huge inputs. Other targets retain the portable fmod fallback.
[[nodiscard]] inline AngleD m_AngleD_reduce(AngleD a) noexcept {
#if defined(OPENT1_NATIVE_X87)
    (void)m_fpuGetControlState();
    double result{};
    opent1_x87_reduced(&a, &result);
    return result;
#else
    return std::fmod(a, static_cast<RealD>(kRealF2Pi));
#endif
}

[[nodiscard]] AngleD m_reduce(AngleD a) noexcept;

[[nodiscard]] inline RealD m_sin(AngleD a) noexcept {
    return std::sin(a);
}

[[nodiscard]] inline RealD m_cos(AngleD a) noexcept {
    return std::cos(a);
}

[[nodiscard]] inline RealD m_tan(AngleD a) noexcept {
    return std::tan(a);
}

inline void m_sincos(AngleD a, RealD& sine, RealD& cosine) noexcept {
    m_AngleD_sincos(a, sine, cosine);
}

[[nodiscard]] inline AngleD m_asin(RealD val) {
    return std::asin(val);
}

[[nodiscard]] inline AngleD m_acos(RealD val) {
    return std::acos(val);
}

// Note Darkstar parameter order: m_atan(x, y) computes atan2(y, x)!
[[nodiscard]] inline AngleD m_atan(RealD x, RealD y) {
    return std::atan2(y, x);
}

[[nodiscard]] inline AngleD m_atan(RealD val) noexcept {
    return std::atan(val);
}

[[nodiscard]] inline RealD m_ptox(RealD r, AngleD a) noexcept {
    return r * std::cos(a);
}

[[nodiscard]] inline RealD m_ptoy(RealD r, AngleD a) noexcept {
    return r * std::sin(a);
}

// -----------------------------------------------------------------------------
// Degree / Radian / AngleX Conversion Helpers (OpenT1 conveniences)
// -----------------------------------------------------------------------------
[[nodiscard]] constexpr RealF m_degToRad(RealF deg) noexcept {
    return deg * (kRealFPi / 180.0f);
}

[[nodiscard]] constexpr RealD m_degToRad(RealD deg) noexcept {
    return deg * (kRealDPi / 180.0);
}

[[nodiscard]] constexpr RealF m_radToDeg(RealF rad) noexcept {
    return rad * (180.0f / kRealFPi);
}

[[nodiscard]] constexpr RealD m_radToDeg(RealD rad) noexcept {
    return rad * (180.0 / kRealDPi);
}

// Conversions honor the logical FPU rounding mode; invalid/out-of-Int32
// results use FISTP's indefinite value, whose low 16 bits are zero.
[[nodiscard]] inline AngleX m_radToAngleX(RealF rad) noexcept {
    return std::bit_cast<AngleX>(
        static_cast<UInt16>(detail::roundToInt32(static_cast<RealD>(rad) * (32768.0 / kRealDPi))));
}

[[nodiscard]] constexpr RealF m_angleXToRad(AngleX ang) noexcept {
    return static_cast<RealF>(ang) * (kRealFPi / 32768.0f);
}

[[nodiscard]] inline AngleX m_degToAngleX(RealF deg) noexcept {
    return std::bit_cast<AngleX>(
        static_cast<UInt16>(detail::roundToInt32(static_cast<RealD>(deg) * (32768.0 / 180.0))));
}

[[nodiscard]] constexpr RealF m_angleXToDeg(AngleX ang) noexcept {
    return static_cast<RealF>(ang) * (180.0f / 32768.0f);
}

} // namespace opent1
