#pragma once
#include <opent1/math/math_Fix16_and_int.hpp>
#include <opent1/core/stream_io.hpp>
#include <array>
#include <limits>
#include <ranges>

namespace opent1 {
namespace detail {
// The original x86 point operators wrap at 32 bits. Do so without signed overflow.
constexpr Int32 pointAdd(Int32 a, Int32 b) {
    return low32(static_cast<Int64>(a) + b);
}

constexpr Int32 pointSub(Int32 a, Int32 b) {
    return low32(static_cast<Int64>(a) - b);
}

constexpr Int32 pointMul(Int32 a, Int32 b) {
    return low32(static_cast<Int64>(a) * b);
}

constexpr Int32 pointDiv(Int32 a, Int32 b) {
    return divide32(a, b).quot;
}

constexpr Int32 pointShift(Int32 a, Int32 b, bool left) {
    OPENT1_ASSERT_FATAL(b >= 0 && b < 32, "Point shift outside [0,31]");
    return left ? std::bit_cast<Int32>(static_cast<UInt32>(a) << b) : a >> b;
}

inline Int32 pointTruncate(RealF value) {
    OPENT1_ASSERT_FATAL(std::isfinite(value) && static_cast<RealD>(value) >= -2147483648.0 &&
                            static_cast<RealD>(value) < 2147483648.0,
                        "Point conversion outside Int32");
    return static_cast<Int32>(value);
}
} // namespace detail

// Named components retain the legacy coordinate API. Indexing selects members;
// it never treats adjacent members as an array. Defaults are explicitly zero.

class Point2I {
public:
    using Value = Int32;
    Int32 x{};
    Int32 y{};
    constexpr Point2I() noexcept = default;

    constexpr Point2I(Int32 inX, Int32 inY) noexcept : x(inX), y(inY) {}

    [[nodiscard]] constexpr Int32& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    [[nodiscard]] constexpr const Int32& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    constexpr Point2I& set(Int32 inX = 0, Int32 inY = 0) noexcept {
        x = inX;
        y = inY;
        return *this;
    }

    constexpr Point2I& operator()(Int32 inX, Int32 inY) noexcept {
        return set(inX, inY);
    }

    constexpr Point2I& operator()(const Point2I& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point2I&) const noexcept = default;

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return x == 0 && y == 0;
    }

    constexpr Point2I& neg() noexcept {
        x = static_cast<Int32>(detail::pointSub(0, x));
        y = static_cast<Int32>(detail::pointSub(0, y));
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point2I& setMin(const Point2I& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2I& setMax(const Point2I& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2I& average(const Point2I& a, const Point2I& b) noexcept {
        x = static_cast<Int32>(static_cast<Int32>(detail::pointAdd(a.x, b.x)) / 2);
        y = static_cast<Int32>(static_cast<Int32>(detail::pointAdd(a.y, b.y)) / 2);
        return *this;
    }

    constexpr Point2I& operator+=(Int32 v) {
        const Point2I result(static_cast<Int32>(detail::pointAdd(x, v)),
                             static_cast<Int32>(detail::pointAdd(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator+(Int32 v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2I& operator+=(const Point2I& v) {
        const Point2I result(static_cast<Int32>(detail::pointAdd(x, v.x)),
                             static_cast<Int32>(detail::pointAdd(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator+(const Point2I& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2I& operator-=(Int32 v) {
        const Point2I result(static_cast<Int32>(detail::pointSub(x, v)),
                             static_cast<Int32>(detail::pointSub(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator-(Int32 v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2I& operator-=(const Point2I& v) {
        const Point2I result(static_cast<Int32>(detail::pointSub(x, v.x)),
                             static_cast<Int32>(detail::pointSub(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator-(const Point2I& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2I& operator*=(Int32 v) {
        const Point2I result(static_cast<Int32>(detail::pointMul(x, v)),
                             static_cast<Int32>(detail::pointMul(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator*(Int32 v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2I& operator*=(const Point2I& v) {
        const Point2I result(static_cast<Int32>(detail::pointMul(x, v.x)),
                             static_cast<Int32>(detail::pointMul(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator*(const Point2I& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2I& operator/=(Int32 v) {
        const Point2I result(static_cast<Int32>(detail::pointDiv(x, v)),
                             static_cast<Int32>(detail::pointDiv(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator/(Int32 v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point2I& operator/=(const Point2I& v) {
        const Point2I result(static_cast<Int32>(detail::pointDiv(x, v.x)),
                             static_cast<Int32>(detail::pointDiv(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator/(const Point2I& v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point2I& operator<<=(Int32 count) {
        const Point2I result(static_cast<Int32>(detail::pointShift(x, count, true)),
                             static_cast<Int32>(detail::pointShift(y, count, true)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator<<(Int32 count) const {
        auto p = *this;
        return p <<= count;
    }

    constexpr Point2I& operator>>=(Int32 count) {
        const Point2I result(static_cast<Int32>(detail::pointShift(x, count, false)),
                             static_cast<Int32>(detail::pointShift(y, count, false)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2I operator>>(Int32 count) const {
        auto p = *this;
        return p >>= count;
    }

    [[nodiscard]] Int32 len() const {
        return m_hyp(x, y);
    }

    [[nodiscard]] constexpr Int32 lenf() const {
        return m_hypf(x, y);
    }

    [[nodiscard]] constexpr Int32 lenvf() const {
        return m_hypvf(x, y);
    }

    [[nodiscard]] constexpr Int32 lenSq() const {
        return static_cast<Int32>(detail::pointAdd(static_cast<Int32>(detail::pointMul(x, x)),
                                                   static_cast<Int32>(detail::pointMul(y, y))));
    }

    Point2I& normalize(Int32 newLen, Int32 curLen) {
        const Point2I result(static_cast<Int32>(m_muldiv(x, newLen, curLen)),
                             static_cast<Int32>(m_muldiv(y, newLen, curLen)));
        *this = result;
        return *this;
    }

    Point2I& normalizef(Int32 newLen, Int32 curLen) {
        const auto factor = detail::pointDiv(detail::pointMul(2048, newLen), curLen);
        const Point2I result(static_cast<Int32>(detail::pointMul(x, factor) >> 11),
                             static_cast<Int32>(detail::pointMul(y, factor) >> 11));
        *this = result;
        return *this;
    }

    Point2I& normalize(Int32 newLen) {
        return normalize(newLen, len());
    }

    Point2I& normalizef(Int32 newLen) {
        return normalize(newLen, lenf());
    }

    [[nodiscard]] Int32 len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(Int32 limit) const {
        return lenvf() <= limit;
    }

    Point2I& interpolate(const Point2I& a, const Point2I& b, RealF t) {
        // Legacy Point2I truncates the first weight to Int32 (unlike Point3I).
        const Int32 u = detail::pointTruncate(1.0f - t);
        const Point2I result(detail::pointTruncate(static_cast<RealF>(detail::pointMul(a.x, u)) +
                                                   static_cast<RealF>(b.x) * t),
                             detail::pointTruncate(static_cast<RealF>(detail::pointMul(a.y, u)) +
                                                   static_cast<RealF>(b.y) * t));
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point2I result;
        if (!(stream.read(result.x) && stream.read(result.y))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y);
    }
};

static_assert(sizeof(Point2I) == 2 * sizeof(Int32));

class Point2S {
public:
    using Value = Int16;
    Int16 x{};
    Int16 y{};
    constexpr Point2S() noexcept = default;

    constexpr Point2S(Int16 inX, Int16 inY) noexcept : x(inX), y(inY) {}

    [[nodiscard]] constexpr Int16& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    [[nodiscard]] constexpr const Int16& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    constexpr Point2S& set(Int16 inX = 0, Int16 inY = 0) noexcept {
        x = inX;
        y = inY;
        return *this;
    }

    constexpr Point2S& operator()(Int16 inX, Int16 inY) noexcept {
        return set(inX, inY);
    }

    constexpr Point2S& operator()(const Point2S& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point2S&) const noexcept = default;

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return x == 0 && y == 0;
    }

    constexpr Point2S& neg() noexcept {
        x = static_cast<Int16>(detail::pointSub(0, x));
        y = static_cast<Int16>(detail::pointSub(0, y));
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point2S& setMin(const Point2S& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2S& setMax(const Point2S& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2S& average(const Point2S& a, const Point2S& b) noexcept {
        x = static_cast<Int16>((a.x + b.x) / 2);
        y = static_cast<Int16>((a.y + b.y) / 2);
        return *this;
    }

    constexpr Point2S& operator+=(Int16 v) {
        const Point2S result(static_cast<Int16>(detail::pointAdd(x, v)),
                             static_cast<Int16>(detail::pointAdd(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator+(Int16 v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2S& operator+=(const Point2S& v) {
        const Point2S result(static_cast<Int16>(detail::pointAdd(x, v.x)),
                             static_cast<Int16>(detail::pointAdd(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator+(const Point2S& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2S& operator-=(Int16 v) {
        const Point2S result(static_cast<Int16>(detail::pointSub(x, v)),
                             static_cast<Int16>(detail::pointSub(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator-(Int16 v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2S& operator-=(const Point2S& v) {
        const Point2S result(static_cast<Int16>(detail::pointSub(x, v.x)),
                             static_cast<Int16>(detail::pointSub(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator-(const Point2S& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2S& operator*=(Int16 v) {
        const Point2S result(static_cast<Int16>(detail::pointMul(x, v)),
                             static_cast<Int16>(detail::pointMul(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator*(Int16 v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2S& operator*=(const Point2S& v) {
        const Point2S result(static_cast<Int16>(detail::pointMul(x, v.x)),
                             static_cast<Int16>(detail::pointMul(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator*(const Point2S& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2S& operator/=(Int16 v) {
        const Point2S result(static_cast<Int16>(detail::pointDiv(x, v)),
                             static_cast<Int16>(detail::pointDiv(y, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator/(Int16 v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point2S& operator/=(const Point2S& v) {
        const Point2S result(static_cast<Int16>(detail::pointDiv(x, v.x)),
                             static_cast<Int16>(detail::pointDiv(y, v.y)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator/(const Point2S& v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point2S& operator<<=(Int32 count) {
        const Point2S result(static_cast<Int16>(detail::pointShift(x, count, true)),
                             static_cast<Int16>(detail::pointShift(y, count, true)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator<<(Int32 count) const {
        auto p = *this;
        return p <<= count;
    }

    constexpr Point2S& operator>>=(Int32 count) {
        const Point2S result(static_cast<Int16>(detail::pointShift(x, count, false)),
                             static_cast<Int16>(detail::pointShift(y, count, false)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2S operator>>(Int32 count) const {
        auto p = *this;
        return p >>= count;
    }

    [[nodiscard]] Int32 len() const {
        return m_hyp(x, y);
    }

    [[nodiscard]] constexpr Int32 lenf() const {
        return m_hypf(x, y);
    }

    [[nodiscard]] constexpr Int32 lenvf() const {
        return m_hypvf(x, y);
    }

    [[nodiscard]] constexpr Int32 lenSq() const {
        return static_cast<Int16>(detail::pointAdd(static_cast<Int16>(detail::pointMul(x, x)),
                                                   static_cast<Int16>(detail::pointMul(y, y))));
    }

    Point2S& normalize(Int32 newLen, Int32 curLen) {
        const Point2S result(static_cast<Int16>(m_muldiv(x, newLen, curLen)),
                             static_cast<Int16>(m_muldiv(y, newLen, curLen)));
        *this = result;
        return *this;
    }

    Point2S& normalizef(Int32 newLen, Int32 curLen) {
        const auto factor = detail::pointDiv(detail::pointMul(2048, newLen), curLen);
        const Point2S result(static_cast<Int16>(detail::pointMul(x, factor) >> 11),
                             static_cast<Int16>(detail::pointMul(y, factor) >> 11));
        *this = result;
        return *this;
    }

    Point2S& normalize(Int32 newLen) {
        return normalize(newLen, len());
    }

    Point2S& normalizef(Int32 newLen) {
        return normalize(newLen, lenf());
    }

    [[nodiscard]] Int32 len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(Int32 limit) const {
        return lenvf() <= limit;
    }

    Point2S& interpolate(const Point2S& a, const Point2S& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point2S result(static_cast<Int16>(detail::pointTruncate(static_cast<RealF>(a.x) * u +
                                                                      static_cast<RealF>(b.x) * t)),
                             static_cast<Int16>(detail::pointTruncate(
                                 static_cast<RealF>(a.y) * u + static_cast<RealF>(b.y) * t)));
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point2S result;
        if (!(stream.read(result.x) && stream.read(result.y))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y);
    }
};

static_assert(sizeof(Point2S) == 2 * sizeof(Int16));

class Point3I {
public:
    using Value = Int32;
    Int32 x{};
    Int32 y{};
    Int32 z{};
    constexpr Point3I() noexcept = default;

    constexpr Point3I(Int32 inX, Int32 inY, Int32 inZ) noexcept : x(inX), y(inY), z(inZ) {}

    [[nodiscard]] constexpr Int32& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 3, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        return z;
    }

    [[nodiscard]] constexpr const Int32& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 3, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        return z;
    }

    constexpr Point3I& set(Int32 inX = 0, Int32 inY = 0, Int32 inZ = 0) noexcept {
        x = inX;
        y = inY;
        z = inZ;
        return *this;
    }

    constexpr Point3I& operator()(Int32 inX, Int32 inY, Int32 inZ) noexcept {
        return set(inX, inY, inZ);
    }

    constexpr Point3I& operator()(const Point3I& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point3I&) const noexcept = default;

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return x == 0 && y == 0 && z == 0;
    }

    constexpr Point3I& neg() noexcept {
        x = static_cast<Int32>(detail::pointSub(0, x));
        y = static_cast<Int32>(detail::pointSub(0, y));
        z = static_cast<Int32>(detail::pointSub(0, z));
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point3I& setMin(const Point3I& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        if (z > p.z) {
            z = p.z;
        }
        return *this;
    }

    constexpr Point3I& setMax(const Point3I& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        if (z < p.z) {
            z = p.z;
        }
        return *this;
    }

    constexpr Point3I& average(const Point3I& a, const Point3I& b) noexcept {
        x = static_cast<Int32>(static_cast<Int32>(detail::pointAdd(a.x, b.x)) / 2);
        y = static_cast<Int32>(static_cast<Int32>(detail::pointAdd(a.y, b.y)) / 2);
        z = static_cast<Int32>(static_cast<Int32>(detail::pointAdd(a.z, b.z)) / 2);
        return *this;
    }

    constexpr Point3I& operator+=(Int32 v) {
        const Point3I result(static_cast<Int32>(detail::pointAdd(x, v)),
                             static_cast<Int32>(detail::pointAdd(y, v)),
                             static_cast<Int32>(detail::pointAdd(z, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator+(Int32 v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point3I& operator+=(const Point3I& v) {
        const Point3I result(static_cast<Int32>(detail::pointAdd(x, v.x)),
                             static_cast<Int32>(detail::pointAdd(y, v.y)),
                             static_cast<Int32>(detail::pointAdd(z, v.z)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator+(const Point3I& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point3I& operator-=(Int32 v) {
        const Point3I result(static_cast<Int32>(detail::pointSub(x, v)),
                             static_cast<Int32>(detail::pointSub(y, v)),
                             static_cast<Int32>(detail::pointSub(z, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator-(Int32 v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point3I& operator-=(const Point3I& v) {
        const Point3I result(static_cast<Int32>(detail::pointSub(x, v.x)),
                             static_cast<Int32>(detail::pointSub(y, v.y)),
                             static_cast<Int32>(detail::pointSub(z, v.z)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator-(const Point3I& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point3I& operator*=(Int32 v) {
        const Point3I result(static_cast<Int32>(detail::pointMul(x, v)),
                             static_cast<Int32>(detail::pointMul(y, v)),
                             static_cast<Int32>(detail::pointMul(z, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator*(Int32 v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point3I& operator*=(const Point3I& v) {
        const Point3I result(static_cast<Int32>(detail::pointMul(x, v.x)),
                             static_cast<Int32>(detail::pointMul(y, v.y)),
                             static_cast<Int32>(detail::pointMul(z, v.z)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator*(const Point3I& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point3I& operator/=(Int32 v) {
        const Point3I result(static_cast<Int32>(detail::pointDiv(x, v)),
                             static_cast<Int32>(detail::pointDiv(y, v)),
                             static_cast<Int32>(detail::pointDiv(z, v)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator/(Int32 v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point3I& operator/=(const Point3I& v) {
        const Point3I result(static_cast<Int32>(detail::pointDiv(x, v.x)),
                             static_cast<Int32>(detail::pointDiv(y, v.y)),
                             static_cast<Int32>(detail::pointDiv(z, v.z)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator/(const Point3I& v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point3I& operator<<=(Int32 count) {
        const Point3I result(static_cast<Int32>(detail::pointShift(x, count, true)),
                             static_cast<Int32>(detail::pointShift(y, count, true)),
                             static_cast<Int32>(detail::pointShift(z, count, true)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator<<(Int32 count) const {
        auto p = *this;
        return p <<= count;
    }

    constexpr Point3I& operator>>=(Int32 count) {
        const Point3I result(static_cast<Int32>(detail::pointShift(x, count, false)),
                             static_cast<Int32>(detail::pointShift(y, count, false)),
                             static_cast<Int32>(detail::pointShift(z, count, false)));
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3I operator>>(Int32 count) const {
        auto p = *this;
        return p >>= count;
    }

    [[nodiscard]] Int32 len() const {
        return m_hyp(x, y, z);
    }

    [[nodiscard]] constexpr Int32 lenf() const {
        return m_hypf(x, y, z);
    }

    [[nodiscard]] constexpr Int32 lenvf() const {
        return m_hypvf(x, y, z);
    }

    [[nodiscard]] constexpr Int32 lenSq() const {
        return static_cast<Int32>(detail::pointAdd(
            static_cast<Int32>(detail::pointAdd(static_cast<Int32>(detail::pointMul(x, x)),
                                                static_cast<Int32>(detail::pointMul(y, y)))),
            static_cast<Int32>(detail::pointMul(z, z))));
    }

    Point3I& normalize(Int32 newLen, Int32 curLen) {
        const Point3I result(static_cast<Int32>(m_muldiv(x, newLen, curLen)),
                             static_cast<Int32>(m_muldiv(y, newLen, curLen)),
                             static_cast<Int32>(m_muldiv(z, newLen, curLen)));
        *this = result;
        return *this;
    }

    Point3I& normalizef(Int32 newLen, Int32 curLen) {
        const auto factor = detail::pointDiv(detail::pointMul(2048, newLen), curLen);
        const Point3I result(static_cast<Int32>(detail::pointMul(x, factor) >> 11),
                             static_cast<Int32>(detail::pointMul(y, factor) >> 11),
                             static_cast<Int32>(detail::pointMul(z, factor) >> 11));
        *this = result;
        return *this;
    }

    Point3I& normalize(Int32 newLen) {
        return normalize(newLen, len());
    }

    Point3I& normalizef(Int32 newLen) {
        return normalize(newLen, lenf());
    }

    [[nodiscard]] Int32 len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(Int32 limit) const {
        return lenvf() <= limit;
    }

    Point3I& interpolate(const Point3I& a, const Point3I& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point3I result(static_cast<Int32>(detail::pointTruncate(static_cast<RealF>(a.x) * u +
                                                                      static_cast<RealF>(b.x) * t)),
                             static_cast<Int32>(detail::pointTruncate(static_cast<RealF>(a.y) * u +
                                                                      static_cast<RealF>(b.y) * t)),
                             static_cast<Int32>(detail::pointTruncate(
                                 static_cast<RealF>(a.z) * u + static_cast<RealF>(b.z) * t)));
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point3I result;
        if (!(stream.read(result.x) && stream.read(result.y) && stream.read(result.z))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y) && stream.write(z);
    }
};

static_assert(sizeof(Point3I) == 3 * sizeof(Int32));

class Point2F {
public:
    using Value = RealF;
    RealF x{};
    RealF y{};
    constexpr Point2F() noexcept = default;

    constexpr Point2F(RealF inX, RealF inY) noexcept : x(inX), y(inY) {}

    [[nodiscard]] constexpr RealF& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    [[nodiscard]] constexpr const RealF& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    constexpr Point2F& set(RealF inX = 0, RealF inY = 0) noexcept {
        x = inX;
        y = inY;
        return *this;
    }

    constexpr Point2F& operator()(RealF inX, RealF inY) noexcept {
        return set(inX, inY);
    }

    constexpr Point2F& operator()(const Point2F& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point2F& other) const noexcept {
        return isEqual(x, other.x) && isEqual(y, other.y);
    }

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return opent1::isZero(x) && opent1::isZero(y);
    }

    constexpr Point2F& neg() noexcept {
        x = 0 - x;
        y = 0 - y;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point2F& setMin(const Point2F& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2F& setMax(const Point2F& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2F& average(const Point2F& a, const Point2F& b) noexcept {
        x = static_cast<RealF>((a.x + b.x) * RealF(0.5));
        y = static_cast<RealF>((a.y + b.y) * RealF(0.5));
        return *this;
    }

    constexpr Point2F& operator+=(RealF v) {
        const Point2F result(x + v, y + v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator+(RealF v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2F& operator+=(const Point2F& v) {
        const Point2F result(x + v.x, y + v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator+(const Point2F& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2F& operator-=(RealF v) {
        const Point2F result(x - v, y - v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator-(RealF v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2F& operator-=(const Point2F& v) {
        const Point2F result(x - v.x, y - v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator-(const Point2F& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2F& operator*=(RealF v) {
        const Point2F result(x * v, y * v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator*(RealF v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2F& operator*=(const Point2F& v) {
        const Point2F result(x * v.x, y * v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator*(const Point2F& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2F& operator/=(RealF v) {
        const Point2F result(x / v, y / v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator/(RealF v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point2F& operator/=(const Point2F& v) {
        const Point2F result(x / v.x, y / v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2F operator/(const Point2F& v) const {
        auto p = *this;
        return p /= v;
    }

    [[nodiscard]] constexpr RealF lenSq() const noexcept {
        return x * x + y * y;
    }

    [[nodiscard]] RealF len() const {
        return std::sqrt(lenSq());
    }

    [[nodiscard]] RealF lenf() const {
        return len();
    }

    Point2F& normalizef() {
        return normalizef(1);
    }

    [[nodiscard]] RealF lenvf() const {
        return lenf();
    }

    Point2F& normalize(RealF newLen, RealF curLen) {
        OPENT1_ASSERT_FATAL(std::isfinite(curLen) && curLen != 0 && std::isfinite(newLen),
                            "Invalid point normalization");
        return *this *= newLen / curLen;
    }

    Point2F& normalizef(RealF newLen, RealF curLen) {
        return normalize(newLen, curLen);
    }

    Point2F& normalize() {
        return normalize(1);
    }

    Point2F& normalize(RealF newLen) {
        return normalize(newLen, len());
    }

    Point2F& normalizef(RealF newLen) {
        return normalizef(newLen, lenf());
    }

    [[nodiscard]] RealF len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(RealF limit) const {
        return lenvf() <= limit;
    }

    Point2F& interpolate(const Point2F& a, const Point2F& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point2F result(a.x * u + b.x * t, a.y * u + b.y * t);
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point2F result;
        if (!(stream.read(result.x) && stream.read(result.y))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y);
    }
};

static_assert(sizeof(Point2F) == 2 * sizeof(RealF));

class Point3F {
public:
    using Value = RealF;
    RealF x{};
    RealF y{};
    RealF z{};
    constexpr Point3F() noexcept = default;

    constexpr Point3F(RealF inX, RealF inY, RealF inZ) noexcept : x(inX), y(inY), z(inZ) {}

    [[nodiscard]] constexpr RealF& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 3, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        return z;
    }

    [[nodiscard]] constexpr const RealF& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 3, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        return z;
    }

    constexpr Point3F& set(RealF inX = 0, RealF inY = 0, RealF inZ = 0) noexcept {
        x = inX;
        y = inY;
        z = inZ;
        return *this;
    }

    constexpr Point3F& operator()(RealF inX, RealF inY, RealF inZ) noexcept {
        return set(inX, inY, inZ);
    }

    constexpr Point3F& operator()(const Point3F& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point3F& other) const noexcept {
        return isEqual(x, other.x) && isEqual(y, other.y) && isEqual(z, other.z);
    }

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return opent1::isZero(x) && opent1::isZero(y) && opent1::isZero(z);
    }

    constexpr Point3F& neg() noexcept {
        x = 0 - x;
        y = 0 - y;
        z = 0 - z;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point3F& setMin(const Point3F& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        if (z > p.z) {
            z = p.z;
        }
        return *this;
    }

    constexpr Point3F& setMax(const Point3F& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        if (z < p.z) {
            z = p.z;
        }
        return *this;
    }

    constexpr Point3F& average(const Point3F& a, const Point3F& b) noexcept {
        x = static_cast<RealF>((a.x + b.x) * RealF(0.5));
        y = static_cast<RealF>((a.y + b.y) * RealF(0.5));
        z = static_cast<RealF>((a.z + b.z) * RealF(0.5));
        return *this;
    }

    constexpr Point3F& operator+=(RealF v) {
        const Point3F result(x + v, y + v, z + v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator+(RealF v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point3F& operator+=(const Point3F& v) {
        const Point3F result(x + v.x, y + v.y, z + v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator+(const Point3F& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point3F& operator-=(RealF v) {
        const Point3F result(x - v, y - v, z - v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator-(RealF v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point3F& operator-=(const Point3F& v) {
        const Point3F result(x - v.x, y - v.y, z - v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator-(const Point3F& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point3F& operator*=(RealF v) {
        const Point3F result(x * v, y * v, z * v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator*(RealF v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point3F& operator*=(const Point3F& v) {
        const Point3F result(x * v.x, y * v.y, z * v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator*(const Point3F& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point3F& operator/=(RealF v) {
        const Point3F result(x / v, y / v, z / v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator/(RealF v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point3F& operator/=(const Point3F& v) {
        const Point3F result(x / v.x, y / v.y, z / v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3F operator/(const Point3F& v) const {
        auto p = *this;
        return p /= v;
    }

    [[nodiscard]] constexpr RealF lenSq() const noexcept {
        return x * x + y * y + z * z;
    }

    [[nodiscard]] RealF len() const {
        return static_cast<RealF>(std::sqrt(static_cast<RealD>(x) * x + static_cast<RealD>(y) * y +
                                            static_cast<RealD>(z) * z));
    }

    [[nodiscard]] RealF lenf() const {
        return m_sqrtf(static_cast<RealF>(static_cast<RealD>(x) * x + static_cast<RealD>(y) * y +
                                          static_cast<RealD>(z) * z));
    }

    Point3F& normalizef() {
        return *this *=
               m_invsqrtf(static_cast<RealF>(static_cast<RealD>(x) * x + static_cast<RealD>(y) * y +
                                             static_cast<RealD>(z) * z));
    }

    [[nodiscard]] RealF lenvf() const {
        return lenf();
    }

    Point3F& normalize(RealF newLen, RealF curLen) {
        OPENT1_ASSERT_FATAL(std::isfinite(curLen) && curLen != 0 && std::isfinite(newLen),
                            "Invalid point normalization");
        return *this *= newLen / curLen;
    }

    Point3F& normalizef(RealF newLen, RealF curLen) {
        return normalize(newLen, curLen);
    }

    Point3F& normalize() {
        return normalize(1);
    }

    Point3F& normalize(RealF newLen) {
        return normalize(newLen, len());
    }

    Point3F& normalizef(RealF newLen) {
        return normalizef(newLen, lenf());
    }

    [[nodiscard]] RealF len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(RealF limit) const {
        return m_hypvf(x, y, z) <= limit;
    }

    Point3F& interpolate(const Point3F& a, const Point3F& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point3F result(a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t);
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point3F result;
        if (!(stream.read(result.x) && stream.read(result.y) && stream.read(result.z))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y) && stream.write(z);
    }
};

static_assert(sizeof(Point3F) == 3 * sizeof(RealF));

class Point4F {
public:
    using Value = RealF;
    RealF x{};
    RealF y{};
    RealF z{};
    RealF w{};
    constexpr Point4F() noexcept = default;

    constexpr Point4F(RealF inX, RealF inY, RealF inZ, RealF inW) noexcept
        : x(inX), y(inY), z(inZ), w(inW) {}

    [[nodiscard]] constexpr RealF& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 4, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        if (i == 2) {
            return z;
        }
        return w;
    }

    [[nodiscard]] constexpr const RealF& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 4, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        if (i == 2) {
            return z;
        }
        return w;
    }

    constexpr Point4F& set(RealF inX = 0, RealF inY = 0, RealF inZ = 0, RealF inW = 0) noexcept {
        x = inX;
        y = inY;
        z = inZ;
        w = inW;
        return *this;
    }

    constexpr Point4F& operator()(RealF inX, RealF inY, RealF inZ, RealF inW) noexcept {
        return set(inX, inY, inZ, inW);
    }

    constexpr Point4F& operator()(const Point4F& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point4F&) const noexcept = default;

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return x == 0 && y == 0 && z == 0 && w == 0;
    }

    constexpr Point4F& neg() noexcept {
        x = 0 - x;
        y = 0 - y;
        z = 0 - z;
        w = 0 - w;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point4F& setMin(const Point4F& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        if (z > p.z) {
            z = p.z;
        }
        if (w > p.w) {
            w = p.w;
        }
        return *this;
    }

    constexpr Point4F& setMax(const Point4F& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        if (z < p.z) {
            z = p.z;
        }
        if (w < p.w) {
            w = p.w;
        }
        return *this;
    }

    constexpr Point4F& average(const Point4F& a, const Point4F& b) noexcept {
        x = static_cast<RealF>((a.x + b.x) * RealF(0.5));
        y = static_cast<RealF>((a.y + b.y) * RealF(0.5));
        z = static_cast<RealF>((a.z + b.z) * RealF(0.5));
        w = static_cast<RealF>((a.w + b.w) * RealF(0.5));
        return *this;
    }

    constexpr Point4F& operator+=(RealF v) {
        const Point4F result(x + v, y + v, z + v, w + v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator+(RealF v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point4F& operator+=(const Point4F& v) {
        const Point4F result(x + v.x, y + v.y, z + v.z, w + v.w);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator+(const Point4F& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point4F& operator-=(RealF v) {
        const Point4F result(x - v, y - v, z - v, w - v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator-(RealF v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point4F& operator-=(const Point4F& v) {
        const Point4F result(x - v.x, y - v.y, z - v.z, w - v.w);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator-(const Point4F& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point4F& operator*=(RealF v) {
        const Point4F result(x * v, y * v, z * v, w * v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator*(RealF v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point4F& operator*=(const Point4F& v) {
        const Point4F result(x * v.x, y * v.y, z * v.z, w * v.w);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator*(const Point4F& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point4F& operator/=(RealF v) {
        const Point4F result(x / v, y / v, z / v, w / v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator/(RealF v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point4F& operator/=(const Point4F& v) {
        const Point4F result(x / v.x, y / v.y, z / v.z, w / v.w);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point4F operator/(const Point4F& v) const {
        auto p = *this;
        return p /= v;
    }

    Point4F& interpolate(const Point4F& a, const Point4F& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point4F result(
            a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t, a.w * u + b.w * t);
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point4F result;
        if (!(stream.read(result.x) && stream.read(result.y) && stream.read(result.z) &&
              stream.read(result.w))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y) && stream.write(z) && stream.write(w);
    }
};

static_assert(sizeof(Point4F) == 4 * sizeof(RealF));

class Point2D {
public:
    using Value = RealD;
    RealD x{};
    RealD y{};
    constexpr Point2D() noexcept = default;

    constexpr Point2D(RealD inX, RealD inY) noexcept : x(inX), y(inY) {}

    [[nodiscard]] constexpr RealD& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    [[nodiscard]] constexpr const RealD& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 2, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        return y;
    }

    constexpr Point2D& set(RealD inX = 0, RealD inY = 0) noexcept {
        x = inX;
        y = inY;
        return *this;
    }

    constexpr Point2D& operator()(RealD inX, RealD inY) noexcept {
        return set(inX, inY);
    }

    constexpr Point2D& operator()(const Point2D& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point2D& other) const noexcept {
        return isEqual(x, other.x) && isEqual(y, other.y);
    }

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return opent1::isZero(x) && opent1::isZero(y);
    }

    constexpr Point2D& neg() noexcept {
        x = 0 - x;
        y = 0 - y;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point2D& setMin(const Point2D& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2D& setMax(const Point2D& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        return *this;
    }

    constexpr Point2D& average(const Point2D& a, const Point2D& b) noexcept {
        x = static_cast<RealD>((a.x + b.x) * RealD(0.5));
        y = static_cast<RealD>((a.y + b.y) * RealD(0.5));
        return *this;
    }

    constexpr Point2D& operator+=(RealD v) {
        const Point2D result(x + v, y + v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator+(RealD v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2D& operator+=(const Point2D& v) {
        const Point2D result(x + v.x, y + v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator+(const Point2D& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point2D& operator-=(RealD v) {
        const Point2D result(x - v, y - v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator-(RealD v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2D& operator-=(const Point2D& v) {
        const Point2D result(x - v.x, y - v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator-(const Point2D& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point2D& operator*=(RealD v) {
        const Point2D result(x * v, y * v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator*(RealD v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2D& operator*=(const Point2D& v) {
        const Point2D result(x * v.x, y * v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator*(const Point2D& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point2D& operator/=(RealD v) {
        const Point2D result(x / v, y / v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator/(RealD v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point2D& operator/=(const Point2D& v) {
        const Point2D result(x / v.x, y / v.y);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point2D operator/(const Point2D& v) const {
        auto p = *this;
        return p /= v;
    }

    [[nodiscard]] constexpr RealD lenSq() const noexcept {
        return x * x + y * y;
    }

    [[nodiscard]] RealD len() const {
        return std::sqrt(lenSq());
    }

    [[nodiscard]] RealD lenf() const {
        return len();
    }

    Point2D& normalizef() {
        return normalizef(1);
    }

    [[nodiscard]] RealD lenvf() const {
        return lenf();
    }

    Point2D& normalize(RealD newLen, RealD curLen) {
        OPENT1_ASSERT_FATAL(std::isfinite(curLen) && curLen != 0 && std::isfinite(newLen),
                            "Invalid point normalization");
        return *this *= newLen / curLen;
    }

    Point2D& normalizef(RealD newLen, RealD curLen) {
        return normalize(newLen, curLen);
    }

    Point2D& normalize() {
        return normalize(1);
    }

    Point2D& normalize(RealD newLen) {
        return normalize(newLen, len());
    }

    Point2D& normalizef(RealD newLen) {
        return normalizef(newLen, lenf());
    }

    [[nodiscard]] RealD len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(RealD limit) const {
        return lenvf() <= limit;
    }

    Point2D& interpolate(const Point2D& a, const Point2D& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point2D result(a.x * u + b.x * t, a.y * u + b.y * t);
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point2D result;
        if (!(stream.read(result.x) && stream.read(result.y))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y);
    }
};

static_assert(sizeof(Point2D) == 2 * sizeof(RealD));

class Point3D {
public:
    using Value = RealD;
    RealD x{};
    RealD y{};
    RealD z{};
    constexpr Point3D() noexcept = default;

    constexpr Point3D(RealD inX, RealD inY, RealD inZ) noexcept : x(inX), y(inY), z(inZ) {}

    [[nodiscard]] constexpr RealD& operator[](std::size_t i) {
        OPENT1_ASSERT_FATAL(i < 3, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        return z;
    }

    [[nodiscard]] constexpr const RealD& operator[](std::size_t i) const {
        OPENT1_ASSERT_FATAL(i < 3, "Point index out of bounds");
        if (i == 0) {
            return x;
        }
        if (i == 1) {
            return y;
        }
        return z;
    }

    constexpr Point3D& set(RealD inX = 0, RealD inY = 0, RealD inZ = 0) noexcept {
        x = inX;
        y = inY;
        z = inZ;
        return *this;
    }

    constexpr Point3D& operator()(RealD inX, RealD inY, RealD inZ) noexcept {
        return set(inX, inY, inZ);
    }

    constexpr Point3D& operator()(const Point3D& p) noexcept {
        *this = p;
        return *this;
    }

    [[nodiscard]] constexpr bool operator==(const Point3D& other) const noexcept {
        return isEqual(x, other.x) && isEqual(y, other.y) && isEqual(z, other.z);
    }

    [[nodiscard]] constexpr bool isZero() const noexcept {
        return opent1::isZero(x) && opent1::isZero(y) && opent1::isZero(z);
    }

    constexpr Point3D& neg() noexcept {
        x = 0 - x;
        y = 0 - y;
        z = 0 - z;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator-() const noexcept {
        auto p = *this;
        return p.neg();
    }

    constexpr Point3D& setMin(const Point3D& p) noexcept {
        if (x > p.x) {
            x = p.x;
        }
        if (y > p.y) {
            y = p.y;
        }
        if (z > p.z) {
            z = p.z;
        }
        return *this;
    }

    constexpr Point3D& setMax(const Point3D& p) noexcept {
        if (x < p.x) {
            x = p.x;
        }
        if (y < p.y) {
            y = p.y;
        }
        if (z < p.z) {
            z = p.z;
        }
        return *this;
    }

    constexpr Point3D& average(const Point3D& a, const Point3D& b) noexcept {
        x = static_cast<RealD>((a.x + b.x) * RealD(0.5));
        y = static_cast<RealD>((a.y + b.y) * RealD(0.5));
        z = static_cast<RealD>((a.z + b.z) * RealD(0.5));
        return *this;
    }

    constexpr Point3D& operator+=(RealD v) {
        const Point3D result(x + v, y + v, z + v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator+(RealD v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point3D& operator+=(const Point3D& v) {
        const Point3D result(x + v.x, y + v.y, z + v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator+(const Point3D& v) const {
        auto p = *this;
        return p += v;
    }

    constexpr Point3D& operator-=(RealD v) {
        const Point3D result(x - v, y - v, z - v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator-(RealD v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point3D& operator-=(const Point3D& v) {
        const Point3D result(x - v.x, y - v.y, z - v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator-(const Point3D& v) const {
        auto p = *this;
        return p -= v;
    }

    constexpr Point3D& operator*=(RealD v) {
        const Point3D result(x * v, y * v, z * v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator*(RealD v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point3D& operator*=(const Point3D& v) {
        const Point3D result(x * v.x, y * v.y, z * v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator*(const Point3D& v) const {
        auto p = *this;
        return p *= v;
    }

    constexpr Point3D& operator/=(RealD v) {
        const Point3D result(x / v, y / v, z / v);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator/(RealD v) const {
        auto p = *this;
        return p /= v;
    }

    constexpr Point3D& operator/=(const Point3D& v) {
        const Point3D result(x / v.x, y / v.y, z / v.z);
        *this = result;
        return *this;
    }

    [[nodiscard]] constexpr Point3D operator/(const Point3D& v) const {
        auto p = *this;
        return p /= v;
    }

    [[nodiscard]] constexpr RealD lenSq() const noexcept {
        return x * x + y * y + z * z;
    }

    [[nodiscard]] RealD len() const {
        return std::sqrt(lenSq());
    }

    [[nodiscard]] RealD lenf() const {
        return len();
    }

    Point3D& normalizef() {
        return normalizef(1);
    }

    [[nodiscard]] RealD lenvf() const {
        return lenf();
    }

    Point3D& normalize(RealD newLen, RealD curLen) {
        OPENT1_ASSERT_FATAL(std::isfinite(curLen) && curLen != 0 && std::isfinite(newLen),
                            "Invalid point normalization");
        return *this *= newLen / curLen;
    }

    Point3D& normalizef(RealD newLen, RealD curLen) {
        return normalize(newLen, curLen);
    }

    Point3D& normalize() {
        return normalize(1);
    }

    Point3D& normalize(RealD newLen) {
        return normalize(newLen, len());
    }

    Point3D& normalizef(RealD newLen) {
        return normalizef(newLen, lenf());
    }

    [[nodiscard]] RealD len_f() const {
        return lenf();
    }

    [[nodiscard]] bool check_lenvf(RealD limit) const {
        return lenvf() <= limit;
    }

    Point3D& interpolate(const Point3D& a, const Point3D& b, RealF t) {
        const RealF u = 1.0f - t;
        const Point3D result(a.x * u + b.x * t, a.y * u + b.y * t, a.z * u + b.z * t);
        *this = result;
        return *this;
    }

    bool read(StreamIO& stream) {
        Point3D result;
        if (!(stream.read(result.x) && stream.read(result.y) && stream.read(result.z))) {
            return false;
        }
        *this = result;
        return true;
    }

    bool write(StreamIO& stream) const {
        return stream.write(x) && stream.write(y) && stream.write(z);
    }
};

static_assert(sizeof(Point3D) == 3 * sizeof(RealD));

[[nodiscard]] constexpr Int32 m_dot(const Point2I& a, const Point2I& b) {
    return detail::pointAdd(detail::pointMul(a.x, b.x), detail::pointMul(a.y, b.y));
}

[[nodiscard]] inline auto m_dist(const Point2I& a, const Point2I& b) {
    return (a - b).len();
}

[[nodiscard]] inline bool m_check_dist(const Point2I& a, const Point2I& b, Int32 d) {
    return m_dist(a, b) <= d;
}

[[nodiscard]] inline auto m_distf(const Point2I& a, const Point2I& b) {
    return (a - b).lenf();
}

[[nodiscard]] inline bool m_check_distf(const Point2I& a, const Point2I& b, Int32 d) {
    return m_distf(a, b) <= d;
}

[[nodiscard]] inline auto m_distvf(const Point2I& a, const Point2I& b) {
    return (a - b).lenvf();
}

[[nodiscard]] inline bool m_check_distvf(const Point2I& a, const Point2I& b, Int32 d) {
    return m_distvf(a, b) <= d;
}

[[nodiscard]] constexpr RealF m_dot(const Point2F& a, const Point2F& b) {
    return static_cast<RealF>(static_cast<RealD>(a.x) * b.x + static_cast<RealD>(a.y) * b.y);
}

[[nodiscard]] inline auto m_dist(const Point2F& a, const Point2F& b) {
    return (a - b).len();
}

[[nodiscard]] inline bool m_check_dist(const Point2F& a, const Point2F& b, RealF d) {
    return m_dist(a, b) <= d;
}

[[nodiscard]] inline auto m_distf(const Point2F& a, const Point2F& b) {
    return (a - b).lenf();
}

[[nodiscard]] inline bool m_check_distf(const Point2F& a, const Point2F& b, RealF d) {
    return m_distf(a, b) <= d;
}

[[nodiscard]] inline auto m_distvf(const Point2F& a, const Point2F& b) {
    return (a - b).lenvf();
}

[[nodiscard]] inline bool m_check_distvf(const Point2F& a, const Point2F& b, RealF d) {
    return m_distvf(a, b) <= d;
}

[[nodiscard]] constexpr RealD m_dot(const Point2D& a, const Point2D& b) {
    return static_cast<RealD>(static_cast<RealD>(a.x) * b.x + static_cast<RealD>(a.y) * b.y);
}

[[nodiscard]] inline auto m_dist(const Point2D& a, const Point2D& b) {
    return (a - b).len();
}

[[nodiscard]] inline bool m_check_dist(const Point2D& a, const Point2D& b, RealD d) {
    return m_dist(a, b) <= d;
}

[[nodiscard]] inline auto m_distf(const Point2D& a, const Point2D& b) {
    return (a - b).lenf();
}

[[nodiscard]] inline bool m_check_distf(const Point2D& a, const Point2D& b, RealD d) {
    return m_distf(a, b) <= d;
}

[[nodiscard]] inline auto m_distvf(const Point2D& a, const Point2D& b) {
    return (a - b).lenvf();
}

[[nodiscard]] inline bool m_check_distvf(const Point2D& a, const Point2D& b, RealD d) {
    return m_distvf(a, b) <= d;
}

[[nodiscard]] constexpr Int32 m_dot(const Point3I& a, const Point3I& b) {
    return detail::pointAdd(
        detail::pointAdd(detail::pointMul(a.x, b.x), detail::pointMul(a.y, b.y)),
        detail::pointMul(a.z, b.z));
}

[[nodiscard]] inline auto m_dist(const Point3I& a, const Point3I& b) {
    return (a - b).len();
}

[[nodiscard]] inline bool m_check_dist(const Point3I& a, const Point3I& b, Int32 d) {
    return m_dist(a, b) <= d;
}

[[nodiscard]] inline auto m_distf(const Point3I& a, const Point3I& b) {
    return (a - b).lenf();
}

[[nodiscard]] inline bool m_check_distf(const Point3I& a, const Point3I& b, Int32 d) {
    return m_distf(a, b) <= d;
}

[[nodiscard]] inline auto m_distvf(const Point3I& a, const Point3I& b) {
    return (a - b).lenvf();
}

[[nodiscard]] inline bool m_check_distvf(const Point3I& a, const Point3I& b, Int32 d) {
    return m_distvf(a, b) <= d;
}

[[nodiscard]] constexpr RealF m_dot(const Point3F& a, const Point3F& b) {
    return static_cast<RealF>(static_cast<RealD>(a.x) * b.x + static_cast<RealD>(a.y) * b.y +
                              static_cast<RealD>(a.z) * b.z);
}

[[nodiscard]] inline auto m_dist(const Point3F& a, const Point3F& b) {
    return (a - b).len();
}

[[nodiscard]] inline bool m_check_dist(const Point3F& a, const Point3F& b, RealF d) {
    return m_dist(a, b) <= d;
}

[[nodiscard]] inline auto m_distf(const Point3F& a, const Point3F& b) {
    return (a - b).lenf();
}

[[nodiscard]] inline bool m_check_distf(const Point3F& a, const Point3F& b, RealF d) {
    return m_distf(a, b) <= d;
}

[[nodiscard]] inline auto m_distvf(const Point3F& a, const Point3F& b) {
    return (a - b).lenvf();
}

[[nodiscard]] inline bool m_check_distvf(const Point3F& a, const Point3F& b, RealF d) {
    return m_distvf(a, b) <= d;
}

[[nodiscard]] constexpr RealD m_dot(const Point3D& a, const Point3D& b) {
    return static_cast<RealD>(static_cast<RealD>(a.x) * b.x + static_cast<RealD>(a.y) * b.y +
                              static_cast<RealD>(a.z) * b.z);
}

[[nodiscard]] inline auto m_dist(const Point3D& a, const Point3D& b) {
    return (a - b).len();
}

[[nodiscard]] inline bool m_check_dist(const Point3D& a, const Point3D& b, RealD d) {
    return m_dist(a, b) <= d;
}

[[nodiscard]] inline auto m_distf(const Point3D& a, const Point3D& b) {
    return (a - b).lenf();
}

[[nodiscard]] inline bool m_check_distf(const Point3D& a, const Point3D& b, RealD d) {
    return m_distf(a, b) <= d;
}

[[nodiscard]] inline auto m_distvf(const Point3D& a, const Point3D& b) {
    return (a - b).lenvf();
}

[[nodiscard]] inline bool m_check_distvf(const Point3D& a, const Point3D& b, RealD d) {
    return m_distvf(a, b) <= d;
}

[[nodiscard]] constexpr Int32 m_cross(const Point2I& a, const Point2I& b) {
    return detail::pointSub(detail::pointMul(a.x, b.y), detail::pointMul(a.y, b.x));
}

constexpr Point3I& m_cross(const Point3I& a, const Point3I& b, Point3I& result) {
    result = Point3I(detail::pointSub(detail::pointMul(a.y, b.z), detail::pointMul(a.z, b.y)),
                     detail::pointSub(detail::pointMul(a.z, b.x), detail::pointMul(a.x, b.z)),
                     detail::pointSub(detail::pointMul(a.x, b.y), detail::pointMul(a.y, b.x)));
    return result;
}

[[nodiscard]] constexpr RealF m_cross(const Point2F& a, const Point2F& b) {
    return static_cast<RealF>(static_cast<RealD>(a.x) * b.y - static_cast<RealD>(a.y) * b.x);
}

constexpr Point3F& m_cross(const Point3F& a, const Point3F& b, Point3F& result) {
    result =
        Point3F(static_cast<RealF>(static_cast<RealD>(a.y) * b.z - static_cast<RealD>(a.z) * b.y),
                static_cast<RealF>(static_cast<RealD>(a.z) * b.x - static_cast<RealD>(a.x) * b.z),
                static_cast<RealF>(static_cast<RealD>(a.x) * b.y - static_cast<RealD>(a.y) * b.x));
    return result;
}

[[nodiscard]] constexpr RealD m_cross(const Point2D& a, const Point2D& b) {
    return static_cast<RealD>(static_cast<RealD>(a.x) * b.y - static_cast<RealD>(a.y) * b.x);
}

constexpr Point3D& m_cross(const Point3D& a, const Point3D& b, Point3D& result) {
    result =
        Point3D(static_cast<RealD>(static_cast<RealD>(a.y) * b.z - static_cast<RealD>(a.z) * b.y),
                static_cast<RealD>(static_cast<RealD>(a.z) * b.x - static_cast<RealD>(a.x) * b.z),
                static_cast<RealD>(static_cast<RealD>(a.x) * b.y - static_cast<RealD>(a.y) * b.x));
    return result;
}

// Rounded component conversion follows the logical FPU rounding mode, including
// FISTP's INT_MIN result for nonfinite/out-of-range values.
inline Point3I& m_Point3F_Point3I(const Point3F& input, Point3I& output) noexcept {
    return output.set(detail::roundToInt32(input.x),
                      detail::roundToInt32(input.y),
                      detail::roundToInt32(input.z));
}

namespace detail {
template <class P> Int32 pointDotSign(const P& a, const P& b, UInt32 error = 0) {
    UInt64 positive = error, negative = 0;
    std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(P) / sizeof(Int32)), [&](auto i) {
        const Int64 product = static_cast<Int64>(a[i]) * b[i];
        if (product < 0) {
            negative += static_cast<UInt64>(-product);
        } else {
            positive += static_cast<UInt64>(product);
        }
    });
    return positive >= negative ? 1 : -1;
}
}

inline Int32 m_dotSign(const Point2I& a, const Point2I& b) {
    return detail::pointDotSign(a, b);
}

inline Int32 m_dotSign(const Point3I& a, const Point3I& b) {
    return detail::pointDotSign(a, b);
}

inline Int32 m_dotSign(const Point3I& a, const Point3I& b, Int32 error) {
    return detail::pointDotSign(a, b, static_cast<UInt32>(error));
}
} // namespace opent1
