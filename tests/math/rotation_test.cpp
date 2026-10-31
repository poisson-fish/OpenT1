#include "test_support.hpp"
#include "geometry_corpus.hpp"
#include <opent1/math/matrix.hpp>
#include <numbers>

namespace {
using namespace opent1;

bool close(float a, float b, float tolerance = 0.00001f) {
    return std::abs(a - b) <= tolerance * std::max(1.0f, std::abs(b));
}

void same(const Point3F& a,
          const Point3F& b,
          std::source_location at = std::source_location::current()) {
    test::check(close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z),
                std::format("({},{},{}) != ({},{},{})", a.x, a.y, a.z, b.x, b.y, b.z),
                at);
}

void same(const RMat3F& a, const RMat3F& b) {
    std::ranges::for_each(std::views::iota(0, 3), [&](auto i) {
        std::ranges::for_each(std::views::iota(0, 3), [&](auto j) {
            CHECK(close(a.m[i][j], b.m[i][j]));
        });
    });
}

void tests() {
    constexpr auto quarter = std::numbers::pi_v<float> / 2;
    const RMat3F identity;
    Point3F out;
    m_mul({0, 1, 0}, RMat3F(EulerF(quarter, 0, 0)), out);
    same(out, {0, 0, 1});
    m_mul({1, 0, 0}, RMat3F(EulerF(0, quarter, 0)), out);
    same(out, {0, 0, -1});
    m_mul({1, 0, 0}, RMat3F(EulerF(0, 0, quarter)), out);
    same(out, {0, 1, 0});
    // Quaternion and Euler matrices share the row-vector convention, including
    // exact half turns that made the legacy trace-only conversion divide by zero.
    std::ranges::for_each(std::views::iota(0U, 3U), [&](auto axis) {
        Point3F direction;
        direction[axis] = 1;
        const QuatF q(AngAxisF(direction, std::numbers::pi_v<float>));
        RMat3F matrix;
        q.makeMatrix(matrix);
        const QuatF roundtrip(matrix);
        CHECK(std::isfinite(roundtrip.w));
        RMat3F rebuilt;
        roundtrip.makeMatrix(rebuilt);
        same(matrix, rebuilt);
        CHECK(close(std::abs(q.dot(roundtrip)), 1));
    });
    const AngAxisF noRotation(QuatF{});
    CHECK(noRotation.axis == Point3F(1, 0, 0) && noRotation.angle == 0);
    QuatF zero(0, 0, 0, 0);
    zero.normalize();
    CHECK(zero == QuatF());
    QuatF q(EulerF(0.3f, -0.4f, 0.7f)), negative = q;
    negative.neg();
    CHECK(q == negative);
    QuatF between;
    between.interpolate(q, negative, 0.5f);
    CHECK(between == q);
    between.interpolate(QuatF{}, QuatF(AngAxisF({0, 0, 1}, quarter)), 0.5f);
    m_mul({2, 0, 0}, between, out);
    same(out, {std::sqrt(2.0f), -std::sqrt(2.0f), 0});
    auto squared = q;
    squared.square();
    auto product = q;
    product *= q;
    CHECK(close(squared.x, product.x) && close(squared.y, product.y) &&
          close(squared.z, product.z) && close(squared.w, product.w));
    auto inverse = q;
    inverse.inverse();
    product = q;
    product *= inverse;
    CHECK(product.isIdentity());
    TMat3F cancellation(EulerF(0, 0, -quarter / 2), {1, -1, 0});
    m_mulInverse({100000000, 100000000, 0}, cancellation, out);
    CHECK(close(out.x, -std::sqrt(2.0f)));
    // General inverse handles a basis with both shear and scale.
    RMat3F shear;
    shear.m = {{{2, 1, 0}, {0, 3, 1}, {1, 0, 4}}};
    shear.flags = RMat3F::Matrix_HasScale;
    auto inv = shear;
    inv.inverse();
    RMat3F result;
    m_mul(shear, inv, result);
    same(result, identity);
    auto alias = shear;
    m_mul(alias, inv, alias);
    same(alias, identity);
    alias = inv;
    m_mul(shear, alias, alias);
    same(alias, identity);
    auto adj = shear;
    shear.adjoint(adj);
    alias = shear;
    alias.adjoint(alias);
    same(adj, alias);
    RMat3F singular;
    singular.preScale({0, 1, 1});
    const auto before = singular;
    test::rejects([&] {
        singular.inverse();
    });
    CHECK(singular.m == before.m && singular.flags == before.flags);
    TMat3F singularTransform;
    singularTransform.preScale({0, 1, 1}).postTranslate({1, 2, 3});
    const auto saved = singularTransform;
    test::rejects([&] {
        singularTransform.inverse();
    });
    CHECK(singularTransform.p == saved.p && singularTransform.m == saved.m);
    test::rejects([&] {
        m_mulInverse({1, 2, 3}, shear, out);
    });
    test::rejects([&] {
        shear.getRow(3, out);
    });
    test::rejects([&] {
        shear.setColumn(3, {1, 2, 3});
    });
    auto normalized = RMat3F(EulerF(0.2f, 0.3f, 0.4f));
    normalized.preScale({2, 3, 4});
    normalized.normalize();
    CHECK(!(normalized.flags & RMat3F::Matrix_HasScale));
    CHECK(close(normalized.determinant(), 1));
    normalized = singular;
    test::rejects([&] {
        normalized.normalize();
    });
    CHECK(normalized.m == singular.m);
    TMat3F transform;
    transform.postTranslate({1, 2, 3});
    const auto translation = transform.p;
    transform.preRotateX(0).preRotateY(0).preRotateZ(0);
    CHECK(transform.flags == 3 && transform.p == translation);
    transform.postRotateX(0).postRotateY(0).postRotateZ(0);
    CHECK(transform.flags == 3);
    transform.identity();
    CHECK(transform.flags == 0 && transform.p.isZero());
    // Extracted Euler angles handle both gimbal poles and approximate matrix input.
    std::ranges::for_each(std::array{-1.0f, 1.0f}, [&](auto sign) {
        RMat3F matrix(EulerF(sign * quarter, 0.3f, 0.4f));
        EulerF angles(matrix);
        RMat3F rebuilt(angles);
        same(matrix, rebuilt);
    });
    RMat3F drift;
    drift.flags = 1;
    drift.m[1][2] = 1.000001f;
    CHECK(std::isfinite(EulerF(drift).x));
    geometry_corpus::Inputs input;
    std::ranges::for_each(std::views::iota(0, 256), [&](auto) {
        const EulerF e{input.angle(), input.angle(), input.angle()},
            f{input.angle(), input.angle(), input.angle()};
        const Point3F a{input.scalar(), input.scalar(), input.scalar()},
            b{input.scalar(), input.scalar(), input.scalar()};
        const Point3F point{input.scalar(), input.scalar(), input.scalar()};
        const TMat3F first(e, a), second(f, b);
        TMat3F combined;
        m_mul(first, second, combined);
        Point3F sequential;
        m_mul(point, first, sequential);
        m_mul(sequential, second, sequential);
        m_mul(point, combined, out);
        same(out, sequential);
        auto left = first;
        m_mul(left, second, left);
        same(left, combined);
        same(left.p, combined.p);
        auto right = second;
        m_mul(first, right, right);
        same(right, combined);
        same(right.p, combined.p);
        auto inv = first;
        inv.inverse();
        m_mul(point, first, out);
        m_mul(out, inv, out);
        same(out, point);
        m_mul(point, first, out);
        m_mulInverse(out, first, out);
        same(out, point);
        RMat3F matrix(e), fromQ;
        QuatF rotation(e);
        rotation.makeMatrix(fromQ);
        same(matrix, fromQ);
        Point3F mq;
        m_mul(point, rotation, mq);
        m_mul(point, matrix, out);
        same(mq, out);
        const TQuatF tq(rotation, a);
        m_mul(point, tq, out);
        m_mul(point, first, mq);
        same(out, mq);
        auto inverseQ = tq;
        inverseQ.inverse();
        m_mul(out, inverseQ, out);
        same(out, point);
        const Box3F box({-1, -2, -3}, {2, 3, 4});
        Box3F bounds;
        m_mul(box, first, bounds);
        auto boxAlias = box;
        m_mul(boxAlias, first, boxAlias);
        same(bounds.fMin, boxAlias.fMin);
        same(bounds.fMax, boxAlias.fMax);
        std::ranges::for_each(std::views::iota(0U, 8U), [&](auto bits) {
            const Point3F p((bits & 1) ? box.fMax.x : box.fMin.x,
                            (bits & 2) ? box.fMax.y : box.fMin.y,
                            (bits & 4) ? box.fMax.z : box.fMin.z);
            Point3F vertex;
            m_mul(p, first, vertex);
            CHECK(vertex.x >= bounds.fMin.x - 0.00001f && vertex.x <= bounds.fMax.x + 0.00001f);
            CHECK(vertex.y >= bounds.fMin.y - 0.00001f && vertex.y <= bounds.fMax.y + 0.00001f);
            CHECK(vertex.z >= bounds.fMin.z - 0.00001f && vertex.z <= bounds.fMax.z + 0.00001f);
        });
    });
}
}

int opent1_test_math_rotation() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("math", "math_rotation", opent1_test_math_rotation);
