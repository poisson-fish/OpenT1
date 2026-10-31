#include <opent1/math/plane.hpp>

namespace opent1 {
void PlaneF::set(const Point3F& point, const Point3F& normal) {
    auto n = normal;
    n.normalize();
    const auto distance = -(point.x * n.x + point.y * n.y + point.z * n.z);
    static_cast<Point3F&>(*this) = n;
    d = distance;
}

void PlaneF::set(const Point3F& a, const Point3F& b, const Point3F& c) {
    Point3F n;
    m_cross(a - b, c - b, n);
    set(b, n);
}

MPlaneSide PlaneF::whichSide(const Point3F& point) const {
    const auto value = m_dot(*this, point);
    return value > -d ? MPlaneInside : value < -d ? MPlaneOutside : MPlaneOn;
}

bool PlaneF::intersect(const LineSeg3F& line, Point3F& intersection) const {
    const auto delta = line.vector();
    const auto denominator = x * delta.x + y * delta.y + z * delta.z;
    if (denominator == 0) {
        return false;
    }
    const auto numerator = -(x * line.start.x + y * line.start.y + z * line.start.z + d);
    if (denominator > 0 ? numerator < 0 || numerator > denominator
                        : numerator > 0 || numerator < denominator) {
        return false;
    }
    intersection.set(line.start.x + m_muldiv(delta.x, numerator, denominator),
                     line.start.y + m_muldiv(delta.y, numerator, denominator),
                     line.start.z + m_muldiv(delta.z, numerator, denominator));
    return true;
}

PlaneSplit PlaneF::split(std::span<const Point3F> polygon) const {
    PlaneSplit result;
    if (polygon.size() < 3) {
        return result;
    }
    bool frontNondegenerate = false, backNondegenerate = false;
    auto previous = polygon.back();
    auto previousSide = whichSide(previous);
    std::ranges::for_each(polygon, [&](const auto& current) {
        const auto side = whichSide(current);
        if (previousSide * side == -1) {
            Point3F intersection;
            if (intersect({previous, current}, intersection)) {
                result.front.push_back(intersection);
                result.back.push_back(intersection);
            }
        }
        if (side >= 0) {
            result.front.push_back(current);
            frontNondegenerate |= side == MPlaneInside;
        }
        if (side <= 0) {
            result.back.push_back(current);
            backNondegenerate |= side == MPlaneOutside;
        }
        previous = current;
        previousSide = side;
    });
    if (result.front.size() < 3) {
        result.front.clear();
        frontNondegenerate = false;
    }
    if (result.back.size() < 3) {
        result.back.clear();
        backNondegenerate = false;
    }
    if (!frontNondegenerate && !backNondegenerate) {
        result.front.assign(polygon.begin(), polygon.end());
    }
    return result;
}
} // namespace opent1
