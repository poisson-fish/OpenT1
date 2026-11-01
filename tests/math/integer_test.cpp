#include "test_support.hpp"
#include <opent1/core/crc16.hpp>
#include <opent1/math/math_Fix16_and_int.hpp>
#include <array>
#include <limits>
#include <ranges>
#include <thread>

#if defined(_MSC_VER)
// This case intentionally changes rounding between ordinary FP expressions.
// Volatile operands alone do not declare floating-environment access to MSVC.
#pragma fenv_access(on)
#endif

namespace {
using namespace opent1;
using test::rejects;
constexpr auto low = std::numeric_limits<Int32>::min();
constexpr auto high = std::numeric_limits<Int32>::max();

void tests() {
    static_assert(m_mulff16(65536, 32768) == 32768);
    static_assert(m_mulff16(-1, 32768) == -1);
    static_assert(m_mulff16R(-1, 32768) == 0); // multiply ties go toward +infinity
    static_assert(m_divff16(-65536, 3 * 65536) == -21845);
    static_assert(m_divff16R(1, 131072) == 1); // divide ties go away from zero
    static_assert(m_divff16R(-1, 131072) == -1);
    static_assert(m_mul32(low, low) == 4611686018427387904LL);
    static_assert(m_bsr_(0) == -1 && m_bsf_(0) == -1);
    static_assert(m_BSR(0x80000000U) == 31 && m_BSF(0x80000000U) == 31);
    static_assert(m_log2_(1) == 0 && m_log2_(3) == 0x18000);
    static_assert(m_map(1, 0, 3, 0, 10) == 4);
    static_assert(m_map(1, 0, 3, 10, 0) == 6);
    static_assert(m_map(low, high, low + 1, low, high) == low);
    static_assert(m_sqr(high) == 1 && m_cube(low) == 0);
    static_assert(m_gcd(low, 1) == 1);
    static_assert(fix16ToInt(low) == -32768);
    static_assert(std::bit_cast<UInt32>(m_sqrtf_RealF(0.0f)) == 0x1FC00000U);
    static_assert(asFloat(asInt(1.25f)) == 1.25f);
    static_assert(m_sar(-4, 33) == -2 && m_shr(-1, 1) == high);
    static_assert(m_shl(low, 1) == 0 && m_rol(1, 32) == 1 && m_ror(1, 1) == 0x80000000U);
    static_assert(UNIT_TO_UFIX(0.5f, 8) == 127 && UFIX_TO_UNIT(127, 8) == 0.5f);
    static_assert(UNIT_TO_SFIX(-0.5f, 8) == -63); // preserve historical +0.5 truncation bias
    static_assert(SFIX_TO_UNIT(0x1C0, 8) == -0.5f);

    CHECK(m_mulff16(low, low) == 0); // SHRD low word wraps rather than saturates
    CHECK(m_div(-7, 3) == DivResult32{-2, -1});
    CHECK(m_div(Int64{7}, -3) == DivResult32{-2, 1});
    CHECK(m_div(low, 1) == DivResult32{low, 0});
    CHECK(m_muludiv(UInt32{0xFFFFFFFFU}, UInt32{0xFFFFFFFFU}, UInt32{0xFFFFFFFFU}) == 0xFFFFFFFFU);
    rejects([] {
        (void)m_div(1, 0);
    });
    rejects([] {
        (void)m_div(low, -1);
    });
    rejects([] {
        (void)m_div(std::numeric_limits<Int64>::min(), -1);
    });
    rejects([] {
        (void)m_muldiv(high, high, 1);
    });
    rejects([] {
        (void)m_muludiv32(-1, -1, 1);
    });
    rejects([] {
        (void)m_divff16(1, 0);
    });
    rejects([] {
        (void)m_divff16(high, 1);
    });
    rejects([] {
        (void)m_divff16R(high, 1);
    });
    rejects([] {
        (void)m_map(1, 0, 0, 1, 10);
    });
    rejects([] {
        (void)m_abs(low);
    });
    rejects([] {
        (void)m_gcd(low, 0);
    });
    rejects([] {
        (void)m_lcm(high, high - 1);
    });
    rejects([] {
        (void)m_dist(std::numeric_limits<Int64>::min(), std::numeric_limits<Int64>::max());
    });
    rejects([] {
        (void)m_limit_range(0, low);
    });
    rejects([] {
        (void)m_hyp(high, high);
    });
    rejects([] {
        (void)m_sqrt(-1);
    });
    rejects([] {
        (void)m_bsf(-1);
    });
    rejects([] {
        (void)UNIT_TO_UFIX(0.5f, 32);
    });
    rejects([] {
        (void)SFIX_TO_UNIT(0, 0);
    });

    std::ranges::for_each(std::views::iota(0, 100000), [](Int32 value) {
        const auto root = m_sqrt(value);
        CHECK(static_cast<Int64>(root) * root <= value);
        CHECK(static_cast<Int64>(root + 1) * (root + 1) > value);
    });
    CHECK(m_sqrt(high) == 46340);
    CHECK(m_sqrt(0.25f) == 0.5f && m_sqrt(0.25) == 0.5);
    CHECK(m_log2(8.0f) == 3.0f && m_pow2(3.0f) == 8.0f);
    CHECK(m_invsqrt(4.0f) == 0.5f && m_hyp(3, 4) == 5);
    CHECK(m_hyp(2, 3, 6) == 7 && m_hypf(3, 4) == 5 && m_hypvf(3, 4, 5) == 5);
    CHECK(m_gcd(24, 18) == 6 && m_lcm(6, 8) == 24);
    CHECK(m_dist(UInt64{0}, std::numeric_limits<UInt64>::max()) ==
          std::numeric_limits<UInt64>::max());

    const auto original = m_fpuGetControlState();
    {
        const ScopedFpuControl rounding(0x0E7F); // truncate
        CHECK(m_RealF_to_Int32(-1.9f) == -1);
        CHECK(m_RealF_to_Int32(std::numeric_limits<float>::infinity()) == low);
        CHECK(m_RealF_to_Int32(std::numeric_limits<float>::quiet_NaN()) == low);
        CHECK(m_RealF_to_RealX(-32768.0f) == low);
        Int32 out{};
        m_RealF_to_Int32(2.5f, out);
        CHECK(out == 2);
        // Logical control state is local to a thread and doesn't alter another thread's math.
        std::jthread other([] {
            // POSIX threads inherit the creator's floating-point environment.
            // Set an explicit baseline before checking independent control.
            const ScopedFpuControl baseline(0x027F);
            CHECK(m_fpuGetControlState() == 0x027F);
            m_fpuSetControlState(0x0A7F);
            CHECK(m_RealF_to_Int32(1.1f) == 2);
        });
        other.join();
        CHECK(m_fpuGetControlState() == 0x0E7F);
        CHECK(m_RealF_to_Int32(1.9f) == 1);
    }
    CHECK(m_fpuGetControlState() == original);
#if !defined(__EMSCRIPTEN__)
    // Volatile operands ensure ordinary CPU arithmetic, not constant folding or
    // the explicit integer-conversion helper, observes the requested mode.
    volatile double one = 1.0, halfUlp = 0x1p-53;
    double down{}, up{};
    {
        const ScopedFpuControl state(0x067f);
        down = one + halfUlp;
    }
    {
        const ScopedFpuControl state(0x0a7f);
        up = one + halfUlp;
    }
    CHECK(down == 1.0 && up == std::nextafter(1.0, 2.0));
    CHECK(m_fpuGetControlState() == original);
#endif
    CHECK(m_RealF_to_Int32(1.5f) == 2 && m_RealF_to_Int32(2.5f) == 2);
    constexpr std::array<UInt8, 4> sample{'P', 'V', 'O', 'L'};
    static_assert(computeCRC16(sample) == 0x030AU);
    CHECK(computeCRC16(std::span<const std::byte>{}) == 0U);
    const std::string_view text = "123456789";
    CHECK(computeTableCRC16(std::as_bytes(std::span(text))) == 0x906EU);
}
}

int opent1_test_math_integer() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("math", "math_integer", opent1_test_math_integer);
