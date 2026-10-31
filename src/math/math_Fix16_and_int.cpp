#include <opent1/math/math_Fix16_and_int.hpp>
#include <cfenv>
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE__)
#include <xmmintrin.h>
#endif
#if defined(_MSC_VER) && defined(_M_IX86)
#include <float.h>
#endif

namespace opent1 {
#if defined(OPENT1_NATIVE_X87)
extern "C" void opent1_x87_control(UInt16) noexcept;
#endif
namespace {
// The portable word retains x87 precision bits where the target instruction set
// cannot change precision. Rounding and exception masks also govern CPU math on
// x86/SSE; WebAssembly retains explicit conversion behavior only.
thread_local UInt16 controlState = [] {
#if defined(OPENT1_NATIVE_X87)
    // Match the original x86 CRT's initial double-precision control word.
    opent1_x87_control(0x027f);
#endif
    return UInt16{0x027f};
}();
}

Int16 m_fpuGetControlState() noexcept {
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE__)
    const auto csr = _mm_getcsr();
    controlState =
        UInt16((controlState & ~0x0c3fU) | ((csr >> 7) & 0x3fU) | ((csr >> 3) & 0x0c00U));
#endif
    return std::bit_cast<Int16>(controlState);
}

void m_fpuSetControlState(Int16 state) noexcept {
    controlState = std::bit_cast<UInt16>(state);
#if defined(OPENT1_NATIVE_X87)
    opent1_x87_control(controlState);
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("fnclex\n\tfldcw %0" : : "m"(controlState));
#elif defined(_MSC_VER) && defined(_M_IX86)
    _clearfp();
    unsigned mask{};
    constexpr unsigned exceptions[] = {
        _EM_INVALID, _EM_DENORMAL, _EM_ZERODIVIDE, _EM_OVERFLOW, _EM_UNDERFLOW, _EM_INEXACT};
    for (unsigned i = 0; i < 6; ++i) {
        if (controlState & (1U << i)) {
            mask |= exceptions[i];
        }
    }
    mask |= ((controlState >> 10) & 3U) << 8;
    mask |= (controlState & 0x300U) == 0        ? _PC_24
            : (controlState & 0x300U) == 0x200U ? _PC_53
                                                : _PC_64;
    _control87(mask, _MCW_EM | _MCW_RC | _MCW_PC);
#elif !defined(__EMSCRIPTEN__)
    std::feclearexcept(FE_ALL_EXCEPT);
    constexpr int rounding[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    std::fesetround(rounding[(controlState >> 10) & 3U]);
#endif
#if defined(_M_IX86) || defined(_M_X64) || defined(__SSE__)
    // Clear pending status before unmasking exceptions, as native fclex/fldcw do.
    const auto csr = (_mm_getcsr() & ~0x7fbfU) | (UInt32(controlState & 0x3fU) << 7) |
                     (UInt32(controlState & 0xc00U) << 3);
    _mm_setcsr(csr);
#endif
}

Int32 detail::roundToInt32(RealD value) noexcept {
    if (!std::isfinite(value)) {
        return std::numeric_limits<Int32>::min();
    }
    RealD rounded{};
    switch ((std::bit_cast<UInt16>(m_fpuGetControlState()) >> 10U) & 3U) {
        case 1:
            rounded = std::floor(value);
            break;
        case 2:
            rounded = std::ceil(value);
            break;
        case 3:
            rounded = std::trunc(value);
            break;
        default: {
            const RealD low = std::floor(value);
            const RealD fraction = value - low;
            rounded = low;
            if (fraction > 0.5 || (fraction == 0.5 && std::fmod(low, 2.0) != 0.0)) {
                rounded += 1.0;
            }
            break;
        }
    }
    if (rounded < std::numeric_limits<Int32>::min() ||
        rounded > std::numeric_limits<Int32>::max()) {
        return std::numeric_limits<Int32>::min();
    }
    return static_cast<Int32>(rounded);
}

} // namespace opent1
