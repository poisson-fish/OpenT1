#pragma once
#include <opent1/math/box.hpp>
#include <span>
#include <vector>

namespace opent1 {
enum MPlaneSide : Int32 { MPlaneOn = 0, MPlaneInside = 1, MPlaneOutside = -1 };

struct PlaneSplit {
    std::vector<Point3F> front, back;

    [[nodiscard]] bool split() const noexcept {
        return !front.empty() && !back.empty();
    }
};

// Normalized point/normal and three-point constructors use the legacy winding.
// Coefficient construction accepts an arbitrary normal, including an unnormalized one.
struct PlaneF : Point3F {
    RealF d{};
    PlaneF() = default;

    PlaneF(RealF a, RealF b, RealF c, RealF distance) : Point3F(a, b, c), d(distance) {}

    PlaneF(const Point3F& p, const Point3F& n) {
        set(p, n);
    }

    PlaneF(const Point3F& a, const Point3F& b, const Point3F& c) {
        set(a, b, c);
    }

    void set(const Point3F& point, const Point3F& normal);
    void set(const Point3F& a, const Point3F& b, const Point3F& c);

    void setXY(RealF zIn) {
        Point3F::set(0, 0, 1);
        d = -zIn;
    }

    void setYZ(RealF xIn) {
        Point3F::set(1, 0, 0);
        d = -xIn;
    }

    void setXZ(RealF yIn) {
        Point3F::set(0, 1, 0);
        d = -yIn;
    }

    void shiftX(RealF value) {
        d -= value * x;
    }

    void shiftY(RealF value) {
        d -= value * y;
    }

    void shiftZ(RealF value) {
        d -= value * z;
    }

    [[nodiscard]] bool isHorizontal() const {
        return x == 0 && y == 0;
    }

    [[nodiscard]] bool isVertical() const {
        return (x != 0 || y != 0) && z == 0;
    }

    [[nodiscard]] MPlaneSide whichSide(const Point3F& point) const;
    // Parallel/coplanar segments return false. Failure preserves the output.
    [[nodiscard]] bool intersect(const LineSeg3F& line, Point3F& intersection) const;
    // Bounded input; results own their vertices. Coplanar polygons appear in both halves, matching legacy output.
    // Concave polygons keep legacy bridging edges rather than generating components.
    [[nodiscard]] PlaneSplit split(std::span<const Point3F> polygon) const;
};

static_assert(sizeof(PlaneF) == 16);
} // namespace opent1
