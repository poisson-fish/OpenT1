#pragma once
#include <opent1/math/trig.hpp>

namespace opent1 {
class RMat3F;

// Radians, Darkstar Z-X-Y composition with row-vector matrices.
struct EulerF {
    RealF x{}, y{}, z{};
    constexpr EulerF() = default;

    constexpr EulerF(RealF xIn, RealF yIn, RealF zIn) : x(xIn), y(yIn), z(zIn) {}

    explicit EulerF(const RMat3F& matrix) {
        set(matrix);
    }

    constexpr EulerF& set(RealF xIn = 0, RealF yIn = 0, RealF zIn = 0) {
        x = xIn;
        y = yIn;
        z = zIn;
        return *this;
    }

    EulerF& set(const RMat3F& matrix);
    RMat3F& makeMatrix(RMat3F& matrix) const;

    constexpr EulerF& operator+=(const EulerF& b) {
        x += b.x;
        y += b.y;
        z += b.z;
        return *this;
    }

    constexpr EulerF& operator-=(const EulerF& b) {
        x -= b.x;
        y -= b.y;
        z -= b.z;
        return *this;
    }

    constexpr EulerF& operator*=(RealF s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    constexpr EulerF& operator/=(RealF s) {
        x /= s;
        y /= s;
        z /= s;
        return *this;
    }

    constexpr EulerF operator+(const EulerF& b) const {
        auto r = *this;
        return r += b;
    }

    constexpr EulerF operator-(const EulerF& b) const {
        auto r = *this;
        return r -= b;
    }

    constexpr EulerF operator*(RealF s) const {
        auto r = *this;
        return r *= s;
    }

    constexpr EulerF operator/(RealF s) const {
        auto r = *this;
        return r /= s;
    }

    bool operator==(const EulerF& b) const {
        return isEqual(x, b.x) && isEqual(y, b.y) && isEqual(z, b.z);
    }

    void reduce() {
        x = m_reduce(x);
        y = m_reduce(y);
        z = m_reduce(z);
    }
};

static_assert(sizeof(EulerF) == 12);
} // namespace opent1
