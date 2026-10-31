#pragma once
#include <opent1/math/euler.hpp>
#include <opent1/math/point.hpp>

namespace opent1 {
class RMat3F;
class QuatF;

struct AngAxisF {
    Point3F axis{1, 0, 0};
    RealF angle{};
    constexpr AngAxisF() = default;

    constexpr AngAxisF(const Point3F& a, RealF radians) : axis(a), angle(radians) {}

    explicit AngAxisF(const QuatF& q) {
        set(q);
    }

    explicit AngAxisF(const RMat3F& m) {
        set(m);
    }

    AngAxisF& set(const QuatF& q);
    AngAxisF& set(const RMat3F& m);

    constexpr AngAxisF& set(const Point3F& a, RealF radians) {
        axis = a;
        angle = radians;
        return *this;
    }

    bool operator==(const AngAxisF& b) const {
        return isEqual(angle, b.angle) && axis == b.axis;
    }

    RMat3F& makeMatrix(RMat3F& m) const;
};

class QuatF {
public:
    RealF x{}, y{}, z{}, w{1};
    constexpr QuatF() = default;

    constexpr QuatF(RealF a, RealF b, RealF c, RealF d) : x(a), y(b), z(c), w(d) {}

    explicit QuatF(const EulerF& e) {
        set(e);
    }

    explicit QuatF(const AngAxisF& a) {
        set(a);
    }

    explicit QuatF(const RMat3F& m) {
        set(m);
    }

    constexpr QuatF& set(RealF a, RealF b, RealF c, RealF d) {
        x = a;
        y = b;
        z = c;
        w = d;
        return *this;
    }

    QuatF& set(const EulerF& e);
    QuatF& set(const AngAxisF& a);
    QuatF& set(const RMat3F& m);
    bool operator==(const QuatF& b) const;
    QuatF& operator*=(const QuatF& b);
    QuatF& operator/=(const QuatF& b);

    constexpr QuatF& operator+=(const QuatF& b) {
        x += b.x;
        y += b.y;
        z += b.z;
        w += b.w;
        return *this;
    }

    constexpr QuatF& operator-=(const QuatF& b) {
        x -= b.x;
        y -= b.y;
        z -= b.z;
        w -= b.w;
        return *this;
    }

    constexpr QuatF& operator*=(RealF s) {
        x *= s;
        y *= s;
        z *= s;
        w *= s;
        return *this;
    }

    constexpr QuatF& operator/=(RealF s) {
        x /= s;
        y /= s;
        z /= s;
        w /= s;
        return *this;
    }

    constexpr RealF dot(const QuatF& b) const {
        return w * b.w + x * b.x + y * b.y + z * b.z;
    }

    constexpr QuatF& identity() {
        return set(0, 0, 0, 1);
    }

    constexpr QuatF& neg() {
        x = -x;
        y = -y;
        z = -z;
        w = -w;
        return *this;
    }

    bool isIdentity() const {
        return isZero(x) && isZero(y) && isZero(z);
    }

    bool IsIdentity() const {
        return isIdentity();
    }

    QuatF& square();
    QuatF& inverse();
    QuatF& normalize();
    RMat3F& makeMatrix(RMat3F& m) const;
    QuatF& interpolate(const QuatF& a, const QuatF& b, RealF t);

    QuatF& slerp(const QuatF& b, RealF t) {
        return interpolate(*this, b, t);
    }

    RealF angleBetween(const QuatF& b) const;
};

QuatF& m_mul(const QuatF& a, const QuatF& b, QuatF& out);
Point3F& m_mul(const Point3F& p, const QuatF& q, Point3F& out);

// Transform quaternions preserve point magnitude during rotation.
struct TQuatF : QuatF {
    Point3F p{};
    Int32 flags{};
    TQuatF() = default;

    TQuatF(const QuatF& rotation, const Point3F& translation) {
        set(rotation, translation);
    }

    TQuatF(const EulerF& rotation, const Point3F& translation) {
        set(QuatF(rotation), translation);
    }

    TQuatF(const AngAxisF& rotation, const Point3F& translation) {
        set(QuatF(rotation), translation);
    }

    TQuatF& set(const QuatF& rotation, const Point3F& translation) {
        static_cast<QuatF&>(*this) = rotation;
        p = translation;
        flags = 3;
        return *this;
    }

    TQuatF& identity() {
        QuatF::identity();
        p.set();
        flags = 0;
        return *this;
    }

    TQuatF& inverse();
};

Point3F& m_mul(const Point3F& p, const TQuatF& q, Point3F& out);
static_assert(sizeof(AngAxisF) == 16 && sizeof(QuatF) == 16 && sizeof(TQuatF) == 32);
} // namespace opent1
