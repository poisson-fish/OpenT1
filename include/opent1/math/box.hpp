#pragma once
#include <opent1/math/point.hpp>

namespace opent1 {
// Containment excludes the maximum faces; touching boxes overlap.
class Box2I {
public:
    Point2I fMin{}, fMax{};
    constexpr Box2I() noexcept = default;

    constexpr Box2I(const Point2I& a, const Point2I& b) : fMin(a), fMax(b) {
        fMin.setMin(b);
        fMax.setMax(a);
    }

    constexpr Box2I(Int32 x0, Int32 y0, Int32 x1, Int32 y1) : fMin(x0, y0), fMax(x1, y1) {}

    constexpr Point2I& upperL() {
        return fMin;
    }

    constexpr Point2I& lowerR() {
        return fMax;
    }

    constexpr Int32& left() {
        return fMin.x;
    }

    constexpr Int32& right() {
        return fMax.x;
    }

    constexpr Int32& top() {
        return fMin.y;
    }

    constexpr Int32& bottom() {
        return fMax.y;
    }

    constexpr const Point2I& upperL() const {
        return fMin;
    }

    constexpr const Point2I& lowerR() const {
        return fMax;
    }

    constexpr const Int32& left() const {
        return fMin.x;
    }

    constexpr const Int32& right() const {
        return fMax.x;
    }

    constexpr const Int32& top() const {
        return fMin.y;
    }

    constexpr const Int32& bottom() const {
        return fMax.y;
    }

    [[nodiscard]] constexpr bool contains(const Point2I& p) const {
        return p.x >= fMin.x && p.x < fMax.x && p.y >= fMin.y && p.y < fMax.y;
    }

    constexpr void intersect(const Box2I& b) {
        fMin.setMax(b.fMin);
        fMax.setMin(b.fMax);
    }

    [[nodiscard]] constexpr bool overlaps(const Box2I& b) const {
        return !(b.fMin.x > fMax.x || fMin.x > b.fMax.x || b.fMin.y > fMax.y || fMin.y > b.fMax.y);
    }

    [[nodiscard]] constexpr bool isValid() const {
        return fMin.x <= fMax.x && fMin.y <= fMax.y;
    }

    constexpr Box2I& operator+=(const Point2I& v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator+=(Int32 v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator-=(const Point2I& v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator-=(Int32 v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator*=(const Point2I& v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator*=(Int32 v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator/=(const Point2I& v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2I& operator/=(Int32 v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    [[nodiscard]] constexpr Int32 len_x() const {
        return detail::pointSub(fMax.x, fMin.x);
    }

    [[nodiscard]] constexpr Int32 len_y() const {
        return detail::pointSub(fMax.y, fMin.y);
    }

    [[nodiscard]] constexpr bool operator==(const Box2I&) const = default;
};

static_assert(sizeof(Box2I) == 4 * sizeof(Int32));

// Containment excludes the maximum faces; touching boxes overlap.
class Box3I {
public:
    Point3I fMin{}, fMax{};
    constexpr Box3I() noexcept = default;

    constexpr Box3I(const Point3I& a, const Point3I& b) : fMin(a), fMax(b) {
        fMin.setMin(b);
        fMax.setMax(a);
    }

    [[nodiscard]] constexpr bool contains(const Point3I& p) const {
        return p.x >= fMin.x && p.x < fMax.x && p.y >= fMin.y && p.y < fMax.y && p.z >= fMin.z &&
               p.z < fMax.z;
    }

    constexpr void intersect(const Box3I& b) {
        fMin.setMax(b.fMin);
        fMax.setMin(b.fMax);
    }

    [[nodiscard]] constexpr bool overlaps(const Box3I& b) const {
        return !(b.fMin.x > fMax.x || fMin.x > b.fMax.x || b.fMin.y > fMax.y || fMin.y > b.fMax.y ||
                 b.fMin.z > fMax.z || fMin.z > b.fMax.z);
    }

    [[nodiscard]] constexpr bool isValid() const {
        return fMin.x <= fMax.x && fMin.y <= fMax.y && fMin.z <= fMax.z;
    }

    constexpr Box3I& operator+=(const Point3I& v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator+=(Int32 v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator-=(const Point3I& v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator-=(Int32 v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator*=(const Point3I& v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator*=(Int32 v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator/=(const Point3I& v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3I& operator/=(Int32 v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    [[nodiscard]] constexpr Int32 len_x() const {
        return detail::pointSub(fMax.x, fMin.x);
    }

    [[nodiscard]] constexpr Int32 len_y() const {
        return detail::pointSub(fMax.y, fMin.y);
    }

    [[nodiscard]] constexpr Int32 len_z() const {
        return detail::pointSub(fMax.z, fMin.z);
    }

    [[nodiscard]] constexpr bool operator==(const Box3I&) const = default;
};

static_assert(sizeof(Box3I) == 6 * sizeof(Int32));

// Containment excludes the maximum faces; touching boxes overlap.
class Box2F {
public:
    Point2F fMin{}, fMax{};
    constexpr Box2F() noexcept = default;

    constexpr Box2F(const Point2F& a, const Point2F& b) : fMin(a), fMax(b) {
        fMin.setMin(b);
        fMax.setMax(a);
    }

    constexpr Box2F(RealF x0, RealF y0, RealF x1, RealF y1) : fMin(x0, y0), fMax(x1, y1) {}

    constexpr Point2F& upperL() {
        return fMin;
    }

    constexpr Point2F& lowerR() {
        return fMax;
    }

    constexpr RealF& left() {
        return fMin.x;
    }

    constexpr RealF& right() {
        return fMax.x;
    }

    constexpr RealF& top() {
        return fMin.y;
    }

    constexpr RealF& bottom() {
        return fMax.y;
    }

    constexpr const Point2F& upperL() const {
        return fMin;
    }

    constexpr const Point2F& lowerR() const {
        return fMax;
    }

    constexpr const RealF& left() const {
        return fMin.x;
    }

    constexpr const RealF& right() const {
        return fMax.x;
    }

    constexpr const RealF& top() const {
        return fMin.y;
    }

    constexpr const RealF& bottom() const {
        return fMax.y;
    }

    [[nodiscard]] constexpr bool contains(const Point2F& p) const {
        return p.x >= fMin.x && p.x < fMax.x && p.y >= fMin.y && p.y < fMax.y;
    }

    constexpr void intersect(const Box2F& b) {
        fMin.setMax(b.fMin);
        fMax.setMin(b.fMax);
    }

    [[nodiscard]] constexpr bool overlaps(const Box2F& b) const {
        return !(b.fMin.x > fMax.x || fMin.x > b.fMax.x || b.fMin.y > fMax.y || fMin.y > b.fMax.y);
    }

    [[nodiscard]] constexpr bool isValid() const {
        return fMin.x <= fMax.x && fMin.y <= fMax.y;
    }

    constexpr Box2F& operator+=(const Point2F& v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator+=(RealF v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator-=(const Point2F& v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator-=(RealF v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator*=(const Point2F& v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator*=(RealF v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator/=(const Point2F& v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box2F& operator/=(RealF v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    [[nodiscard]] constexpr RealF len_x() const {
        return fMax.x - fMin.x;
    }

    [[nodiscard]] constexpr RealF len_y() const {
        return fMax.y - fMin.y;
    }

    [[nodiscard]] constexpr bool operator==(const Box2F&) const = default;
};

static_assert(sizeof(Box2F) == 4 * sizeof(RealF));

// Containment excludes the maximum faces; touching boxes overlap.
class Box3F {
public:
    Point3F fMin{}, fMax{};
    constexpr Box3F() noexcept = default;

    constexpr Box3F(const Point3F& a, const Point3F& b) : fMin(a), fMax(b) {
        fMin.setMin(b);
        fMax.setMax(a);
    }

    [[nodiscard]] constexpr bool contains(const Point3F& p) const {
        return p.x >= fMin.x && p.x < fMax.x && p.y >= fMin.y && p.y < fMax.y && p.z >= fMin.z &&
               p.z < fMax.z;
    }

    constexpr void intersect(const Box3F& b) {
        fMin.setMax(b.fMin);
        fMax.setMin(b.fMax);
    }

    [[nodiscard]] constexpr bool overlaps(const Box3F& b) const {
        return !(b.fMin.x > fMax.x || fMin.x > b.fMax.x || b.fMin.y > fMax.y || fMin.y > b.fMax.y ||
                 b.fMin.z > fMax.z || fMin.z > b.fMax.z);
    }

    [[nodiscard]] constexpr bool isValid() const {
        return fMin.x <= fMax.x && fMin.y <= fMax.y && fMin.z <= fMax.z;
    }

    constexpr Box3F& operator+=(const Point3F& v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator+=(RealF v) {
        auto lo = fMin + v;
        auto hi = fMax + v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator-=(const Point3F& v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator-=(RealF v) {
        auto lo = fMin - v;
        auto hi = fMax - v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator*=(const Point3F& v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator*=(RealF v) {
        auto lo = fMin * v;
        auto hi = fMax * v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator/=(const Point3F& v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    constexpr Box3F& operator/=(RealF v) {
        auto lo = fMin / v;
        auto hi = fMax / v;
        fMin = lo;
        fMax = hi;
        return *this;
    }

    [[nodiscard]] constexpr RealF len_x() const {
        return fMax.x - fMin.x;
    }

    [[nodiscard]] constexpr RealF len_y() const {
        return fMax.y - fMin.y;
    }

    [[nodiscard]] constexpr RealF len_z() const {
        return fMax.z - fMin.z;
    }

    [[nodiscard]] constexpr bool operator==(const Box3F&) const = default;
};

static_assert(sizeof(Box3F) == 6 * sizeof(RealF));

struct LineSeg3F {
    Point3F start{}, end{};

    [[nodiscard]] constexpr Point3F vector() const {
        return end - start;
    }

    [[nodiscard]] RealF len() const {
        return vector().len();
    }

    void reverse() {
        std::swap(start, end);
    }
};
} // namespace opent1
