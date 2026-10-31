#pragma once
#include <opent1/math/matrix.hpp>
#include <span>
#include <vector>

namespace opent1 {
// Geometric queries require finite coordinates. Matrix flags must describe the
// actual basis; rigid inputs have orthonormal axes and no scale or shear.
void m_normal(const Point3F& a, const Point3F& b, const Point3F& c, Point3F& out);
bool m_pointInTriangle(
    const Point3F& p, const Point3F& normal, const Point3F& a, const Point3F& b, const Point3F& c);
bool m_pointInPoly(const Point3F& p, const Point3F& normal, std::span<const Point3F> vertices);
// Legacy line/AABox policy: a start on a face is inside; entering exactly on
// an edge/corner from outside is excluded. Misses leave hitTime unchanged.
bool m_lineAABox(
    const Point3F& a, const Point3F& b, const Point3F& min, const Point3F& max, RealF& hitTime);
bool m_tubeAABox(const Point3F& a,
                 const Point3F& b,
                 RealF radius,
                 const Point3F& min,
                 const Point3F& max,
                 RealF& hitTime);
// Sphere tangency is excluded. minDistanceSquared is always written.
bool m_sphereAABox(const Point3F& center,
                   RealF radiusSquared,
                   const Point3F& min,
                   const Point3F& max,
                   RealF& minDistanceSquared);
// Rigid transforms map B into A. Touching boxes collide. Overlap outputs are
// published only on success; radii must be nonnegative and transforms unscaled.
bool m_OBoxOBox(const Point3F& aRadii,
                const Point3F& bRadii,
                const TMat3F& transform,
                Point3F& aOverlap,
                Point3F& bOverlap);
bool m_rectOBox(const Point3F& aRadii,
                const Point3F& bRadii,
                const TMat3F& transform,
                Point3F& aOverlap,
                Point3F& bOverlap);
bool m_lineOBox(const Point3F& aRadii,
                const Point3F& bRadii,
                const TMat3F& transform,
                Point3F& aOverlap,
                Point3F& bOverlap);
bool m_planeOBox(const Point3F& radii,
                 const TMat3F& transform,
                 const Point3F& normal,
                 const Point3F& point,
                 RealF& overlap);
// Convex planar polygon in A space; inverse maps A to the box's local space.
// Uses complete separating axes, replacing the legacy unfinished polygon test.
// Two-sided plane penetration must be positive; a coplanar box face is excluded.
// Caller-owned scratch buffers and unchecked vertex counts are retired.
bool m_polyOBox(const Point3F& radii,
                const TMat3F& transform,
                const TMat3F& inverse,
                const Point3F& normal,
                std::span<const Point3F> vertices,
                RealF& overlap);

struct CollisionSurface {
    RealF time{};
    Point3F position{}, normal{};
    RealF distance{};
    Int32 material{}, surface{}, part{}; // IDs, never truncated pointers.
    // Legacy rigid-space transform; time/distance/IDs retain their values.
    void transform(const TMat3F& matrix);
};

struct CollisionSurfaceList {
    std::vector<CollisionSurface> surfaces;
    TMat3F tWorld{}, tLocal{};
    Point3F stepVector{};
    bool contained{};
    void transform(const TMat3F& matrix);
};
} // namespace opent1
