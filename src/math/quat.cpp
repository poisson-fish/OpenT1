#include <opent1/math/matrix.hpp>
#include <algorithm>

namespace opent1 {
QuatF& QuatF::set(const EulerF& e) {
    const RealD cx = std::cos(-static_cast<RealD>(e.x) * 0.5),
                sx = std::sin(-static_cast<RealD>(e.x) * 0.5);
    const RealD cy = std::cos(-static_cast<RealD>(e.y) * 0.5),
                sy = std::sin(-static_cast<RealD>(e.y) * 0.5);
    const RealD cz = std::cos(-static_cast<RealD>(e.z) * 0.5),
                sz = std::sin(-static_cast<RealD>(e.z) * 0.5);
    const RealD cycz = cy * cz, sysz = sy * sz, sycz = sy * cz, cysz = cy * sz;
    return set(static_cast<RealF>(cycz * sx + sysz * cx),
               static_cast<RealF>(sycz * cx - cysz * sx),
               static_cast<RealF>(cysz * cx - sycz * sx),
               static_cast<RealF>(cycz * cx + sysz * sx));
}

QuatF& QuatF::set(const AngAxisF& a) {
    RealF s{}, c{};
    m_sincos(a.angle * 0.5f, s, c);
    return set(a.axis.x * s, a.axis.y * s, a.axis.z * s, c);
}

QuatF& QuatF::set(const RMat3F& m) {
    if (!(m.flags & RMat3F::Matrix_HasRotation)) {
        return identity();
    }
    const RealF trace = m.trace();
    if (trace + 1.0f > 0.00001f) {
        w = std::sqrt(trace + 1.0f) * 0.5f;
        const RealF c = 4.0f * w;
        x = (m.m[2][1] - m.m[1][2]) / c;
        y = (m.m[0][2] - m.m[2][0]) / c;
        z = (m.m[1][0] - m.m[0][1]) / c;
    } else {
        // The legacy trace-only formula divides by zero at a half turn.
        std::size_t i = 0;
        if (m.m[1][1] > m.m[0][0]) {
            i = 1;
        }
        if (m.m[2][2] > m.m[i][i]) {
            i = 2;
        }
        const auto j = (i + 1) % 3, k = (j + 1) % 3;
        const auto s = std::sqrt(std::max(0.0f, 1.0f + m.m[i][i] - m.m[j][j] - m.m[k][k]));
        OPENT1_ASSERT_FATAL(s > 0, "Invalid rotation matrix");
        std::array<RealF, 3> q{};
        q[i] = s * 0.5f;
        const auto inv = 0.5f / s;
        q[j] = (m.m[i][j] + m.m[j][i]) * inv;
        q[k] = (m.m[i][k] + m.m[k][i]) * inv;
        set(q[0], q[1], q[2], (m.m[k][j] - m.m[j][k]) * inv);
    }
    return *this;
}

AngAxisF& AngAxisF::set(const QuatF& q) {
    angle = m_acos(std::clamp(q.w, -1.0f, 1.0f)) * 2.0f;
    const auto s = m_sin(angle * 0.5f);
    if (isZero(s)) {
        axis.set(1, 0, 0);
    } else {
        axis.set(q.x / s, q.y / s, q.z / s);
    }
    return *this;
}

AngAxisF& AngAxisF::set(const RMat3F& m) {
    return set(QuatF(m));
}

RMat3F& AngAxisF::makeMatrix(RMat3F& m) const {
    return QuatF(*this).makeMatrix(m);
}

bool QuatF::operator==(const QuatF& q) const {
    auto a = *this, b = q;
    a.normalize();
    b.normalize().inverse();
    a *= b;
    return a.isIdentity();
}

QuatF& m_mul(const QuatF& a, const QuatF& b, QuatF& out) {
    const QuatF r(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                  a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                  a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
    out = r;
    return out;
}

QuatF& QuatF::operator*=(const QuatF& q) {
    return m_mul(*this, q, *this);
}

QuatF& QuatF::operator/=(const QuatF& q) {
    auto inverse = q;
    inverse.inverse();
    return *this *= inverse;
}

QuatF& QuatF::square() {
    const auto t = 2.0f * w;
    w = w * w - (x * x + y * y + z * z);
    x *= t;
    y *= t;
    z *= t;
    return *this;
}

QuatF& QuatF::inverse() {
    const auto length = w * w + x * x + y * y + z * z;
    const auto inv = length == 0 ? 1.0f : 1.0f / length;
    w *= inv;
    x *= -inv;
    y *= -inv;
    z *= -inv;
    return *this;
}

QuatF& QuatF::normalize() {
    const auto length = std::sqrt(x * x + y * y + z * z + w * w);
    return length == 0 ? identity() : *this /= length;
}

RMat3F& QuatF::makeMatrix(RMat3F& m) const {
    if (isIdentity()) {
        return m.identity();
    }
    const auto xs = 2 * x, ys = 2 * y, zs = 2 * z;
    const auto wx = w * xs, wy = w * ys, wz = w * zs, xx = x * xs, xy = x * ys, xz = x * zs,
               yy = y * ys, yz = y * zs, zz = z * zs;
    m.m = {{{1 - (yy + zz), xy - wz, xz + wy},
            {xy + wz, 1 - (xx + zz), yz - wx},
            {xz - wy, yz + wx, 1 - (xx + yy)}}};
    m.flags = (m.flags & ~RMat3F::Matrix_HasScale) | RMat3F::Matrix_HasRotation;
    return m;
}

QuatF& QuatF::interpolate(const QuatF& a, const QuatF& b, RealF t) {
    RealD cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const RealF sign = cosine < 0 ? -1.0f : 1.0f;
    cosine = std::clamp(std::abs(cosine), 0.0, 1.0);
    RealD s1 = 1.0 - t, s2 = sign * t;
    if (1.0 - cosine > 0.00001) {
        const auto omega = m_acos(cosine), sine = m_sin(omega);
        s1 = m_sin((1.0 - t) * omega) / sine;
        s2 = sign * m_sin(t * omega) / sine;
    }
    return set(static_cast<RealF>(s1 * a.x + s2 * b.x),
               static_cast<RealF>(s1 * a.y + s2 * b.y),
               static_cast<RealF>(s1 * a.z + s2 * b.z),
               static_cast<RealF>(s1 * a.w + s2 * b.w));
}

RealF QuatF::angleBetween(const QuatF& b) const {
    return m_acos(std::clamp(dot(b), -1.0f, 1.0f));
}

Point3F& m_mul(const Point3F& p, const QuatF& q, Point3F& out) {
    auto inv = q;
    inv.inverse();
    QuatF temp, value(p.x, p.y, p.z, 0);
    m_mul(inv, value, temp);
    m_mul(temp, q, value);
    return out.set(value.x, value.y, value.z);
}

Point3F& m_mul(const Point3F& p, const TQuatF& q, Point3F& out) {
    Point3F r;
    m_mul(p, static_cast<const QuatF&>(q), r);
    out = r + q.p;
    return out;
}

TQuatF& TQuatF::inverse() {
    QuatF::inverse();
    m_mul(-p, static_cast<const QuatF&>(*this), p);
    return *this;
}
} // namespace opent1
