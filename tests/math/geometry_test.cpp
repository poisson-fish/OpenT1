#include "test_support.hpp"
#include "geometry_corpus.hpp"
#include <opent1/math/collision.hpp>
#include <opent1/math/plane.hpp>
#include <opent1/core/mem_stream.hpp>
#include <ranges>

namespace {
using namespace opent1;

void points() {
    constexpr auto min = std::numeric_limits<Int32>::min(), max = std::numeric_limits<Int32>::max();
    static_assert((Point2I(max, min) + Point2I(1, -1)) == Point2I(min, max));
    static_assert((-Point3I(min, 2, -3)) == Point3I(min, -2, 3));
    static_assert((Point2I(min, -1) << 1) == Point2I(0, -2));
    CHECK((Point3I(-3, -5, 7) >> 1) == Point3I(-2, -3, 3));
    CHECK(Point3I(max, min, 17).lenSq() == detail::low32(static_cast<Int64>(max) * max + 289));
    CHECK(m_dot(Point2I(max, max), Point2I(max, max)) == 2);
    CHECK(m_dotSign(Point3I(min, min, min), Point3I(min, min, min)) == 1);
    CHECK(m_dotSign(Point3I(min, min, min), Point3I(max, max, max)) == -1);
    CHECK(m_dotSign(Point3I(-1, 0, 0), Point3I(1, 0, 0), 1) == 1);
    Point3I integer(7, 8, 9);
    const auto saved = integer;
    test::rejects([&] {
        integer /= Point3I(1, 0, 2);
    });
    CHECK(integer == saved);
    test::rejects([&] {
        integer <<= -1;
    });
    CHECK(integer == saved);
    test::rejects([&] {
        integer >>= 32;
    });
    CHECK(integer == saved);
    test::rejects([&] {
        auto p = Point2I(min, 0) / -1;
        (void)p;
    });
    test::rejects([&] {
        integer.normalize(1, 0);
    });
    CHECK(integer == saved);
    Point2I interpolation;
    interpolation.interpolate({100, 200}, {20, 40}, 0.5f);
    CHECK(interpolation == Point2I(10, 20));
    Point3I interpolation3;
    interpolation3.interpolate({100, 200, 300}, {20, 40, 60}, 0.5f);
    CHECK(interpolation3 == Point3I(60, 120, 180));
    test::rejects([&] {
        interpolation.interpolate({0, 0}, {max, 0}, 2);
    });
    Point3F p(3, 4, 12);
    CHECK(p.len() == 13);
    CHECK(std::bit_cast<UInt32>(p.lenf()) == std::bit_cast<UInt32>(m_sqrtf(169.0f)));
    CHECK(p.check_lenvf(13));
    CHECK(!p.check_lenvf(12));
    auto normalized = p;
    normalized.normalize();
    CHECK(std::abs(normalized.len() - 1) < 0.000001f);
    normalized.set();
    test::rejects([&] {
        normalized.normalize();
    });
    CHECK(normalized == Point3F());
    normalized.normalizef();
    CHECK(normalized == Point3F());
    CHECK(Point3F(0.0000005f, 0, 0).isZero());
    CHECK(Point3F(1, 2, 3) == Point3F(1.0000005f, 2, 3));
    CHECK(!(Point3F(1, 2, 3) == Point3F(1.00001f, 2, 3)));
    Point3F a(1, 2, 3), b(4, 5, 6);
    m_cross(a, b, a);
    CHECK(a == Point3F(-3, 6, -3));
    const Point4F four(1, 2, 3, 4);
    CHECK(four[3] == 4);
    test::rejects([&] {
        (void)four[4];
    });
    test::rejects([&] {
        p[std::size_t(-1)] = 1;
    });
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{3}), [&](auto i) {
        p[i] = static_cast<float>(i + 1);
    });
    CHECK(p == Point3F(1, 2, 3));
    Point3I rounded;
    {
        ScopedFpuControl control(0x027f);
        m_Point3F_Point3I({-2.5f, 1.5f, 0.5f}, rounded);
        CHECK(rounded == Point3I(-2, 2, 0));
    }
    {
        ScopedFpuControl control(0x0e7f);
        m_Point3F_Point3I({-2.5f, 1.5f, 0.5f}, rounded);
        CHECK(rounded == Point3I(-2, 1, 0));
    }
    Point2S small(32767, -32768);
    small += Int16(1);
    CHECK(small == Point2S(-32768, -32767));
    Point2D wide(3, 4);
    CHECK(wide.len() == 5);
    // Scalar stream methods encode the wire shape without native object dumps.
    std::array<std::byte, 16> bytes{};
    MemRWStream output{std::span<std::byte>(bytes)};
    CHECK(four.write(output));
    const std::array<UInt32, 4> expected{0x3f800000, 0x40000000, 0x40400000, 0x40800000};
    std::ranges::for_each(std::views::iota(std::size_t{0}, bytes.size()), [&](auto i) {
        CHECK(std::to_integer<UInt8>(bytes[i]) == UInt8(expected[i / 4] >> ((i % 4) * 8)));
    });
    Point4F read;
    MemRWStream input{std::span<const std::byte>(bytes)};
    CHECK(read.read(input));
    CHECK(read == four);
    std::ranges::for_each(std::views::iota(std::size_t{0}, bytes.size()), [&](auto length) {
        MemRWStream shortInput{std::span<const std::byte>(bytes).first(length)};
        Point4F value(9, 8, 7, 6);
        CHECK(!value.read(shortInput));
        CHECK(value == Point4F(9, 8, 7, 6));
    });
}

void boxesPlanes() {
    const Box2I box({5, 7}, {-1, -3});
    CHECK(box.fMin == Point2I(-1, -3));
    CHECK(box.fMax == Point2I(5, 7));
    CHECK(box.contains({-1, -3}));
    CHECK(!box.contains({5, 0}));
    CHECK(box.overlaps(Box2I({5, 7}, {6, 8})));
    auto disjoint = box;
    disjoint.intersect(Box2I({10, 10}, {20, 20}));
    CHECK(!disjoint.isValid());
    const Box3F box3({-1, -2, -3}, {1, 2, 3});
    CHECK(box3.contains({0, 0, 0}));
    CHECK(!box3.contains({0, 2, 0}));
    PlaneF plane;
    plane.setYZ(1);
    CHECK(plane.isVertical());
    CHECK(plane.whichSide({2, 0, 0}) == MPlaneInside);
    CHECK(plane.whichSide({1, 0, 0}) == MPlaneOn);
    CHECK(plane.whichSide({0, 0, 0}) == MPlaneOutside);
    Point3F hit(8, 9, 10);
    CHECK(plane.intersect({{0, 1, 2}, {2, 3, 4}}, hit));
    CHECK(hit == Point3F(1, 2, 3));
    CHECK(plane.intersect({{1, 1, 1}, {2, 2, 2}}, hit));
    CHECK(hit == Point3F(1, 1, 1));
    CHECK(!plane.intersect({{2, 2, 2}, {3, 3, 3}}, hit));
    CHECK(hit == Point3F(1, 1, 1));
    CHECK(!plane.intersect({{1, 0, 0}, {1, 1, 1}}, hit));
    plane.shiftX(2);
    CHECK(plane.whichSide({3, 0, 0}) == MPlaneOn);
    test::rejects([&] {
        plane.set({0, 0, 0}, {0, 0, 0});
    });
    CHECK(plane.d == -3);
    const PlaneF fromPoints({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    CHECK(fromPoints.z == -1);
    const std::array<Point3F, 4> polygon{{{-2, -1, 0}, {2, -1, 0}, {2, 1, 0}, {-2, 1, 0}}};
    plane.setYZ(0);
    auto split = plane.split(polygon);
    CHECK(split.split());
    CHECK(split.front.size() == 4 && split.back.size() == 4);
    CHECK(split.front.front() == Point3F(0, -1, 0));
    CHECK(split.back.front() == polygon.front());
    CHECK(std::ranges::all_of(split.front, [](auto p) {
        return p.x >= 0;
    }));
    CHECK(std::ranges::all_of(split.back, [](auto p) {
        return p.x <= 0;
    }));
    plane.setYZ(3);
    split = plane.split(polygon);
    CHECK(split.front.empty() && split.back.size() == 4);
    plane.setXY(0);
    split = plane.split(polygon);
    CHECK(split.front.size() == 4 && split.back.size() == 4);
    CHECK(plane.split(std::span(polygon).first(2)).front.empty());
    const std::array<Point3F, 3> onEdge{{{-1, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
    plane.setYZ(0);
    split = plane.split(onEdge);
    CHECK(split.front.size() == 3 && split.back.size() == 3);
}

void collisions() {
    const Point3F min(-1, -1, -1), max(1, 1, 1);
    float time = 19;
    CHECK(m_lineAABox({-2, 0, 0}, {2, 0, 0}, min, max, time));
    CHECK(time == 0.25f);
    CHECK(m_lineAABox({-1, 1, 1}, {-2, 2, 2}, min, max, time));
    CHECK(time == 0);
    time = 19;
    CHECK(!m_lineAABox({-2, 1, 1}, {2, 1, 1}, min, max, time));
    CHECK(time == 19);
    CHECK(!m_lineAABox({-2, -2, 0}, {2, 2, 0}, min, max, time));
    CHECK(time == 19);
    CHECK(m_lineAABox({0, 0, 0}, {0, 0, 0}, min, max, time));
    CHECK(time == 0);
    CHECK(m_tubeAABox({-2, 1.25f, 0}, {2, 1.25f, 0}, 0.5f, min, max, time));
    float distance{};
    CHECK(!m_sphereAABox({2, 0, 0}, 1, min, max, distance));
    CHECK(distance == 1);
    CHECK(m_sphereAABox({2, 0, 0}, 1.01f, min, max, distance));
    test::rejects([&] {
        m_tubeAABox(min, max, -1, min, max, time);
    });
    test::rejects([&] {
        m_lineAABox(min, max, max, min, time);
    });
    const std::array<Point3F, 4> poly{{{-3, -3, 0}, {3, -3, 0}, {3, 3, 0}, {-3, 3, 0}}};
    CHECK(m_pointInPoly({0, 0, 0}, {0, 0, 1}, poly));
    CHECK(!m_pointInPoly({4, 0, 0}, {0, 0, 1}, poly));
    CHECK(!m_pointInPoly({0, 0, 0}, {0, 0, 1}, std::span<const Point3F>{}));
    Point3F normal;
    m_normal(poly[0], poly[1], poly[2], normal);
    CHECK(normal.z < 0);
    TMat3F matrix;
    Point3F ao, bo;
    CHECK(m_OBoxOBox(max, max, matrix, ao, bo));
    CHECK(ao == Point3F(2, 2, 2) && bo == ao);
    matrix.p = {2, 0, 0};
    CHECK(m_OBoxOBox(max, max, matrix, ao, bo));
    CHECK(ao.x == 0);
    matrix.p = {2.01f, 0, 0};
    ao = {9, 8, 7};
    bo = {6, 5, 4};
    CHECK(!m_OBoxOBox(max, max, matrix, ao, bo));
    CHECK(ao == Point3F(9, 8, 7) && bo == Point3F(6, 5, 4));
    matrix.identity();
    CHECK(m_rectOBox(max, max, matrix, ao, bo));
    CHECK(ao.z == 1);
    CHECK(m_lineOBox(max, max, matrix, ao, bo));
    CHECK(ao.x == 1 && ao.y == 2 && ao.z == 1);
    // Full polygon SAT: interior, translated world geometry, two-sided plane,
    // projected intervals crossing zero, and disjoint polygons on the same plane.
    float overlap = 99;
    CHECK(m_polyOBox(max, matrix, matrix, {0, 0, 1}, poly, overlap));
    CHECK(overlap == 1);
    auto moved = poly;
    std::ranges::for_each(moved, [](auto& p) {
        p += {10, 20, 30};
    });
    matrix.postTranslate({10, 20, 30});
    auto inverse = matrix;
    inverse.inverse();
    CHECK(m_polyOBox(max, matrix, inverse, {0, 0, 1}, moved, overlap));
    CHECK(overlap == 1);
    std::ranges::for_each(moved, [](auto& p) {
        p.x += 20;
    });
    overlap = 99;
    CHECK(!m_polyOBox(max, matrix, inverse, {0, 0, 1}, moved, overlap));
    CHECK(overlap == 99);
    CHECK(!m_polyOBox(max, matrix, inverse, {0, 0, 1}, std::span<const Point3F>{}, overlap));
    const std::array<Point3F, 3> cornerMiss{{{0.8f, 1.5f, 0}, {1.5f, 0.8f, 0}, {1.5f, 1.5f, 0}}};
    CHECK(!m_polyOBox(max, TMat3F{}, TMat3F{}, {0, 0, 1}, cornerMiss, overlap));
    TMat3F rotated(EulerF(0.3f, 0.7f, -0.2f), {11, -17, 23});
    auto rotatedInverse = rotated;
    rotatedInverse.inverse();
    std::array<Point3F, 4> worldPoly{};
    std::ranges::transform(poly, worldPoly.begin(), [&](auto v) {
        Point3F result;
        m_mul(v, rotated, result);
        return result;
    });
    Point3F worldNormal;
    m_mul({0, 0, 1}, static_cast<const RMat3F&>(rotated), worldNormal);
    CHECK(m_polyOBox(max, rotated, rotatedInverse, worldNormal, worldPoly, overlap));
    CHECK(std::abs(overlap - 1) < 0.00001f);
    // An independent vertex/projection SAT oracle checks oriented boxes,
    // including separators on cross axes and swapped coordinate frames.
    geometry_corpus::Inputs inputs;
    const auto corners = [](const Point3F& radii, const TMat3F& transform) {
        std::array<Point3F, 8> vertices{};
        std::ranges::for_each(std::views::iota(0U, 8U), [&](auto bits) {
            const Point3F p((bits & 1) ? radii.x : -radii.x,
                            (bits & 2) ? radii.y : -radii.y,
                            (bits & 4) ? radii.z : -radii.z);
            m_mul(p, transform, vertices[bits]);
        });
        return vertices;
    };
    int hits = 0, misses = 0;
    std::ranges::for_each(std::views::iota(0, 1000), [&](auto) {
        const EulerF angle{inputs.angle(), inputs.angle(), inputs.angle()};
        const Point3F translation{inputs.scalar() / 4, inputs.scalar() / 4, inputs.scalar() / 4};
        TMat3F t(angle, translation);
        const Point3F a(1, 0.5f, 2), b(0.25f, 1.5f, 1);
        const auto ac = corners(a, TMat3F{}), bc = corners(b, t);
        std::vector<Point3F> axes{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{3}), [&](auto i) {
            Point3F row;
            t.getRow(i, row);
            axes.push_back(row);
        });
        std::ranges::for_each(std::views::iota(0U, 3U), [&](auto i) {
            std::ranges::for_each(std::views::iota(3U, 6U), [&](auto j) {
                Point3F cross;
                m_cross(axes[i], axes[j], cross);
                axes.push_back(cross);
            });
        });
        const auto separated = std::ranges::any_of(axes, [&](const auto& axis) {
            const auto ar = std::ranges::minmax(ac | std::views::transform([&](const auto& v) {
                                                    return m_dot(v, axis);
                                                }));
            const auto br = std::ranges::minmax(bc | std::views::transform([&](const auto& v) {
                                                    return m_dot(v, axis);
                                                }));
            return ar.max < br.min || br.max < ar.min;
        });
        const auto hit = m_OBoxOBox(a, b, t, ao, bo);
        CHECK(hit == !separated);
        auto inv = t;
        inv.inverse();
        CHECK(m_OBoxOBox(b, a, inv, ao, bo) == hit);
        hit ? ++hits : ++misses;
    });
    CHECK(hits > 100 && misses > 100);
    CollisionSurface surface;
    surface.position = {1, 0, 0};
    surface.normal = {0, 1, 0};
    surface.time = 0.25f;
    surface.distance = 7;
    surface.material = 42;
    matrix.identity().postTranslate({2, 3, 4});
    surface.transform(matrix);
    CHECK(surface.position == Point3F(3, 3, 4) && surface.normal == Point3F(0, 1, 0));
    CHECK(surface.time == 0.25f && surface.distance == 7 && surface.material == 42);
    CollisionSurfaceList list;
    list.surfaces.push_back(surface);
    list.transform(matrix);
    CHECK(list.surfaces.front().position == Point3F(5, 6, 8));
    matrix.preScale({2, 1, 1});
    test::rejects([&] {
        surface.transform(matrix);
    });
}

void tests() {
    points();
    boxesPlanes();
    collisions();
}
}

int opent1_test_math_geometry() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("math", "math_geometry", opent1_test_math_geometry);
