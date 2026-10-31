#pragma once
#include <opent1/math/box.hpp>
#include <opent1/math/quat.hpp>
#include <array>

namespace opent1 {
class RMat3F {
public:
    enum : Int32 { Matrix_HasRotation = 1, Matrix_HasTranslation = 2, Matrix_HasScale = 4 };

    Int32 flags{};
    std::array<std::array<RealF, 3>, 3> m{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    constexpr RMat3F() = default;

    explicit RMat3F(const EulerF& e) {
        set(e);
    }

    explicit RMat3F(const QuatF& q) {
        set(q);
    }

    explicit RMat3F(const AngAxisF& a) {
        set(a);
    }

    RMat3F& set(const EulerF& e);

    RMat3F& set(const QuatF& q) {
        return q.makeMatrix(*this);
    }

    RMat3F& set(const AngAxisF& a) {
        return a.makeMatrix(*this);
    }

    RMat3F& identity();
    RMat3F& transpose();
    void normalize();
    RealF determinant() const;
    RMat3F& adjoint(RMat3F& out) const;
    // Singular matrices raise AssertException and leave this matrix intact.
    RMat3F& inverse();

    RealF trace() const {
        return m[0][0] + m[1][1] + m[2][2];
    }

    Point3F& getRow(std::size_t i, Point3F& out) const;
    Point3F& getColumn(std::size_t i, Point3F& out) const;
    RMat3F& setRow(std::size_t i, const Point3F& row);
    RMat3F& setColumn(std::size_t i, const Point3F& column);

    EulerF& angles(EulerF& e) const {
        return e.set(*this);
    }

    RMat3F& preScale(const Point3F& scale);
    RMat3F& postScale(const Point3F& scale);
    RMat3F& preRotateX(RealF angle);
    RMat3F& preRotateY(RealF angle);
    RMat3F& preRotateZ(RealF angle);
    RMat3F& postRotateX(RealF angle);
    RMat3F& postRotateY(RealF angle);
    RMat3F& postRotateZ(RealF angle);

    void pitch(RealF angle) {
        postRotateX(angle);
    }

    void roll(RealF angle) {
        postRotateY(angle);
    }

    void yaw(RealF angle) {
        postRotateZ(angle);
    }
};

class TMat3F : public RMat3F {
public:
    Point3F p{};
    TMat3F() = default;

    TMat3F(const EulerF& e, const Point3F& t) {
        set(e, t);
    }

    TMat3F(const QuatF& q, const Point3F& t) {
        set(q, t);
    }

    TMat3F(const AngAxisF& a, const Point3F& t) {
        set(a, t);
    }

    template <class Rotation> TMat3F& set(const Rotation& r, const Point3F& t) {
        RMat3F::set(r);
        p = t;
        flags |= Matrix_HasTranslation;
        return *this;
    }

    TMat3F& identity() {
        RMat3F::identity();
        p.set();
        flags &= ~Matrix_HasTranslation;
        return *this;
    }

    TMat3F& inverse();

    TMat3F& preScale(const Point3F& scale) {
        RMat3F::preScale(scale);
        return *this;
    }

    TMat3F& postScale(const Point3F& scale) {
        RMat3F::postScale(scale);
        p *= scale;
        return *this;
    }

    TMat3F& preTranslate(const Point3F& t);

    TMat3F& postTranslate(const Point3F& t) {
        p += t;
        flags |= Matrix_HasTranslation;
        return *this;
    }

    TMat3F& preRotateX(RealF a) {
        RMat3F::preRotateX(a);
        return *this;
    }

    TMat3F& preRotateY(RealF a) {
        RMat3F::preRotateY(a);
        return *this;
    }

    TMat3F& preRotateZ(RealF a) {
        RMat3F::preRotateZ(a);
        return *this;
    }

    TMat3F& postRotateX(RealF a);
    TMat3F& postRotateY(RealF a);
    TMat3F& postRotateZ(RealF a);
};

// Row vectors: m_mul(a,b,out) composes a then b. Every output may alias an input.
RMat3F& m_mul(const RMat3F& a, const RMat3F& b, RMat3F& out);
TMat3F& m_mul(const TMat3F& a, const TMat3F& b, TMat3F& out);
TMat3F& m_mul(const RMat3F& a, const TMat3F& b, TMat3F& out);
TMat3F& m_mul(const TMat3F& a, const RMat3F& b, TMat3F& out);
TMat3F& m_mul(const Point3F& a, const TMat3F& b, TMat3F& out);
Point3F& m_mul(const Point3F& p, const RMat3F& m, Point3F& out);
Point3F& m_mul(const Point3F& p, const TMat3F& m, Point3F& out);
// These inverse helpers require an orthonormal matrix (no scale/shear).
Point3F& m_mulInverse(const Point3F& p, const RMat3F& m, Point3F& out);
Point3F& m_mulInverse(const Point3F& p, const TMat3F& m, Point3F& out);
Box3F& m_mul(const Box3F& box, const RMat3F& m, Box3F& out);
Box3F& m_mul(const Box3F& box, const TMat3F& m, Box3F& out);
static_assert(sizeof(RMat3F) == 40 && sizeof(TMat3F) == 52);
} // namespace opent1
