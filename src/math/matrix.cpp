#include <opent1/math/matrix.hpp>
#include <ranges>

namespace opent1 {
#if defined(OPENT1_NATIVE_X87)
extern "C" void opent1_x87_matrix(const EulerF*, RMat3F*) noexcept;
static_assert(offsetof(RMat3F, m) == 4 && sizeof(RMat3F) == 40 && sizeof(EulerF) == 12);
#endif
namespace {
constexpr auto axes() {
    return std::views::iota(std::size_t{0}, std::size_t{3});
}

void rotateRows(RMat3F& m, std::size_t u, std::size_t v, RealF sine, RealF cosine) {
    std::ranges::for_each(axes(), [&](auto i) {
        const auto old = m.m[u][i];
        m.m[u][i] = old * cosine + m.m[v][i] * sine;
        m.m[v][i] = -old * sine + m.m[v][i] * cosine;
    });
    m.flags |= RMat3F::Matrix_HasRotation;
}

void rotateColumns(RMat3F& m, std::size_t u, std::size_t v, RealF sine, RealF cosine) {
    std::ranges::for_each(m.m, [&](auto& row) {
        const auto old = row[u];
        row[u] = old * cosine - row[v] * sine;
        row[v] = old * sine + row[v] * cosine;
    });
    m.flags |= RMat3F::Matrix_HasRotation;
}
}

RMat3F& RMat3F::identity() {
    m = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    flags &= ~(Matrix_HasRotation | Matrix_HasScale);
    return *this;
}

RMat3F& RMat3F::set(const EulerF& e) {
#if defined(OPENT1_NATIVE_X87)
    (void)m_fpuGetControlState();
    opent1_x87_matrix(&e, this);
    return *this;
#else
    if (e.x == 0 && e.y == 0 && e.z == 0) {
        return identity();
    }
    // Assembly keeps sincos and products in x87 registers. Double intermediates
    // approximate its default 53-bit precision, without host FPU state changes.
    const RealD cx = std::cos(static_cast<RealD>(e.x)), sx = std::sin(static_cast<RealD>(e.x));
    const RealD cy = std::cos(static_cast<RealD>(e.y)), sy = std::sin(static_cast<RealD>(e.y));
    const RealD cz = std::cos(static_cast<RealD>(e.z)), sz = std::sin(static_cast<RealD>(e.z));
    const RealD r1 = cy * cz, r2 = cy * sz, r3 = sy * cz, r4 = sy * sz;
    m = {{{static_cast<RealF>(r1 - r4 * sx),
           static_cast<RealF>(r2 + r3 * sx),
           static_cast<RealF>(-cx * sy)},
          {static_cast<RealF>(-cx * sz), static_cast<RealF>(cx * cz), static_cast<RealF>(sx)},
          {static_cast<RealF>(r3 + r2 * sx),
           static_cast<RealF>(r4 - r1 * sx),
           static_cast<RealF>(cx * cy)}}};
    flags = (flags & ~Matrix_HasScale) | Matrix_HasRotation;
    return *this;
#endif
}

RMat3F& RMat3F::transpose() {
    std::swap(m[0][1], m[1][0]);
    std::swap(m[0][2], m[2][0]);
    std::swap(m[1][2], m[2][1]);
    return *this;
}

void RMat3F::normalize() {
    Point3F a, b, c;
    getColumn(0, a);
    getColumn(1, b);
    m_cross(a, b, c);
    m_cross(c, a, b);
    a.normalize();
    b.normalize();
    c.normalize();
    setColumn(0, a);
    setColumn(1, b);
    setColumn(2, c);
    flags &= ~Matrix_HasScale;
}

RealF RMat3F::determinant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) +
           m[1][0] * (m[0][2] * m[2][1] - m[0][1] * m[2][2]) +
           m[2][0] * (m[0][1] * m[1][2] - m[0][2] * m[1][1]);
}

RMat3F& RMat3F::adjoint(RMat3F& out) const {
    RMat3F r;
    r.flags = flags;
    std::ranges::for_each(axes(), [&](auto i) {
        std::ranges::for_each(axes(), [&](auto j) {
            const auto a = (j + 1) % 3, b = (j + 2) % 3, c = (i + 1) % 3, d = (i + 2) % 3;
            r.m[i][j] = m[a][c] * m[b][d] - m[a][d] * m[b][c];
        });
    });
    out = r;
    return out;
}

RMat3F& RMat3F::inverse() {
    if (!(flags & Matrix_HasScale)) {
        return transpose();
    }
    const auto det = determinant();
    OPENT1_ASSERT_FATAL(std::isfinite(det) && det != 0, "Singular matrix has no inverse");
    RMat3F r;
    adjoint(r);
    const auto inv = 1.0f / det;
    std::ranges::for_each(r.m, [&](auto& row) {
        std::ranges::for_each(row, [&](auto& v) {
            v *= inv;
        });
    });
    *this = r;
    return *this;
}

Point3F& RMat3F::getRow(std::size_t i, Point3F& out) const {
    OPENT1_ASSERT_FATAL(i < 3, "Matrix row out of bounds");
    return out.set(m[i][0], m[i][1], m[i][2]);
}

Point3F& RMat3F::getColumn(std::size_t i, Point3F& out) const {
    OPENT1_ASSERT_FATAL(i < 3, "Matrix column out of bounds");
    return out.set(m[0][i], m[1][i], m[2][i]);
}

RMat3F& RMat3F::setRow(std::size_t i, const Point3F& v) {
    OPENT1_ASSERT_FATAL(i < 3, "Matrix row out of bounds");
    m[i] = {v.x, v.y, v.z};
    return *this;
}

RMat3F& RMat3F::setColumn(std::size_t i, const Point3F& v) {
    OPENT1_ASSERT_FATAL(i < 3, "Matrix column out of bounds");
    m[0][i] = v.x;
    m[1][i] = v.y;
    m[2][i] = v.z;
    return *this;
}

RMat3F& RMat3F::preScale(const Point3F& scale) {
    std::ranges::for_each(axes(), [&](auto i) {
        std::ranges::for_each(m[i], [&](auto& v) {
            v *= scale[i];
        });
    });
    flags |= Matrix_HasScale;
    return *this;
}

RMat3F& RMat3F::postScale(const Point3F& scale) {
    std::ranges::for_each(m, [&](auto& row) {
        std::ranges::for_each(axes(), [&](auto i) {
            row[i] *= scale[i];
        });
    });
    flags |= Matrix_HasScale;
    return *this;
}

RMat3F& RMat3F::preRotateX(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateRows(*this, 1, 2, s, c);
    return *this;
}

RMat3F& RMat3F::preRotateY(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateRows(*this, 0, 2, -s, c);
    return *this;
}

RMat3F& RMat3F::preRotateZ(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateRows(*this, 0, 1, s, c);
    return *this;
}

RMat3F& RMat3F::postRotateX(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateColumns(*this, 1, 2, s, c);
    return *this;
}

RMat3F& RMat3F::postRotateY(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateColumns(*this, 0, 2, -s, c);
    return *this;
}

RMat3F& RMat3F::postRotateZ(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateColumns(*this, 0, 1, s, c);
    return *this;
}

TMat3F& TMat3F::inverse() {
    auto r = *this;
    r.RMat3F::inverse();
    m_mul(-p, static_cast<const RMat3F&>(r), r.p);
    *this = r;
    return *this;
}

TMat3F& TMat3F::preTranslate(const Point3F& t) {
    Point3F v;
    m_mul(t, static_cast<const RMat3F&>(*this), v);
    return postTranslate(v);
}

TMat3F& TMat3F::postRotateX(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateColumns(*this, 1, 2, s, c);
    const auto old = p.y;
    p.y = old * c - (s)*p.z;
    p.z = old * (s) + p.z * c;
    return *this;
}

TMat3F& TMat3F::postRotateY(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateColumns(*this, 0, 2, -s, c);
    const auto old = p.x;
    p.x = old * c - (-s) * p.z;
    p.z = old * (-s) + p.z * c;
    return *this;
}

TMat3F& TMat3F::postRotateZ(RealF a) {
    RealF s, c;
    m_sincos(a, s, c);
    rotateColumns(*this, 0, 1, s, c);
    const auto old = p.x;
    p.x = old * c - (s)*p.y;
    p.y = old * (s) + p.y * c;
    return *this;
}

RMat3F& m_mul(const RMat3F& a, const RMat3F& b, RMat3F& out) {
    RMat3F r;
    std::ranges::for_each(axes(), [&](auto i) {
        std::ranges::for_each(axes(), [&](auto j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        });
    });
    r.flags = (a.flags | b.flags) & (RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasScale);
    out = r;
    return out;
}

TMat3F& m_mul(const TMat3F& a, const TMat3F& b, TMat3F& out) {
    TMat3F r;
    // The legacy TMat*TMat assembly evaluates dot products in x87 registers.
    std::ranges::for_each(axes(), [&](auto i) {
        std::ranges::for_each(axes(), [&](auto j) {
            r.m[i][j] = static_cast<RealF>(static_cast<RealD>(a.m[i][0]) * b.m[0][j] +
                                           static_cast<RealD>(a.m[i][1]) * b.m[1][j] +
                                           static_cast<RealD>(a.m[i][2]) * b.m[2][j]);
        });
    });
    m_mul(a.p, b, r.p);
    r.flags = a.flags | b.flags;
    out = r;
    return out;
}

TMat3F& m_mul(const RMat3F& a, const TMat3F& b, TMat3F& out) {
    TMat3F r;
    m_mul(a, static_cast<const RMat3F&>(b), r);
    r.p = b.p;
    r.flags = a.flags | b.flags;
    out = r;
    return out;
}

TMat3F& m_mul(const TMat3F& a, const RMat3F& b, TMat3F& out) {
    TMat3F r;
    m_mul(static_cast<const RMat3F&>(a), b, r);
    m_mul(a.p, b, r.p);
    r.flags = a.flags | b.flags;
    out = r;
    return out;
}

TMat3F& m_mul(const Point3F& a, const TMat3F& b, TMat3F& out) {
    auto r = b;
    m_mul(a, b, r.p);
    r.flags |= RMat3F::Matrix_HasTranslation;
    out = r;
    return out;
}

Point3F& m_mul(const Point3F& p, const RMat3F& m, Point3F& out) {
    Point3F r;
    std::ranges::for_each(axes(), [&](auto j) {
        r[j] = static_cast<RealF>(static_cast<RealD>(p.x) * m.m[0][j] +
                                  static_cast<RealD>(p.y) * m.m[1][j] +
                                  static_cast<RealD>(p.z) * m.m[2][j]);
    });
    out = r;
    return out;
}

Point3F& m_mul(const Point3F& p, const TMat3F& m, Point3F& out) {
    Point3F r;
    std::ranges::for_each(axes(), [&](auto j) {
        const auto first =
            static_cast<RealD>(p.x) * m.m[0][j] + static_cast<RealD>(p.y) * m.m[1][j];
        const auto last = static_cast<RealD>(p.z) * m.m[2][j];
        // The x component adds translation last; y/z add it to the last product.
        r[j] = static_cast<RealF>(j == 0 ? (first + last) + m.p[j] : first + (last + m.p[j]));
    });
    out = r;
    return out;
}

Point3F& m_mulInverse(const Point3F& p, const RMat3F& m, Point3F& out) {
    OPENT1_ASSERT_FATAL(!(m.flags & RMat3F::Matrix_HasScale),
                        "Inverse multiply requires orthonormal matrix");
    Point3F r;
    std::ranges::for_each(axes(), [&](auto j) {
        r[j] = static_cast<RealF>(static_cast<RealD>(p.x) * m.m[j][0] +
                                  static_cast<RealD>(p.y) * m.m[j][1] +
                                  static_cast<RealD>(p.z) * m.m[j][2]);
    });
    out = r;
    return out;
}

Point3F& m_mulInverse(const Point3F& p, const TMat3F& m, Point3F& out) {
    OPENT1_ASSERT_FATAL(!(m.flags & RMat3F::Matrix_HasScale),
                        "Inverse multiply requires orthonormal matrix");
    // Keep translation differences in double, as the x87 registers did. Rounding
    // these to Point3F first loses small offsets before cancellation in rotation.
    const RealD x = static_cast<RealD>(p.x) - m.p.x, y = static_cast<RealD>(p.y) - m.p.y,
                z = static_cast<RealD>(p.z) - m.p.z;
    out.set(static_cast<RealF>((x * m.m[0][0] + y * m.m[0][1]) + z * m.m[0][2]),
            static_cast<RealF>(x * m.m[1][0] + (y * m.m[1][1] + z * m.m[1][2])),
            static_cast<RealF>(x * m.m[2][0] + (y * m.m[2][1] + z * m.m[2][2])));
    return out;
}

Box3F& m_mul(const Box3F& box, const RMat3F& m, Box3F& out) {
    const auto center = (box.fMin + box.fMax) * 0.5f, radii = (box.fMax - box.fMin) * 0.5f;
    Point3F c, r;
    m_mul(center, m, c);
    std::ranges::for_each(axes(), [&](auto j) {
        r[j] = std::abs(m.m[0][j]) * radii.x + std::abs(m.m[1][j]) * radii.y +
               std::abs(m.m[2][j]) * radii.z;
    });
    out.fMin = c - r;
    out.fMax = c + r;
    return out;
}

Box3F& m_mul(const Box3F& box, const TMat3F& m, Box3F& out) {
    m_mul(box, static_cast<const RMat3F&>(m), out);
    out += m.p;
    return out;
}
} // namespace opent1
