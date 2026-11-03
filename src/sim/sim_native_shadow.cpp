// Bounded port of TS::Shadow partitionTree, cachePolys and TPoly::split.
#include <opent1/sim/sim_environment_render.hpp>
#include <ranges>
#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace opent1::sim {
namespace {
using Poly = std::vector<ShadowVertex>;

struct Plane : Point3F {
    Flt32 d{};
    Plane() = default;

    Plane(Point3F p, Point3F n) {
        n.normalize();
        static_cast<Point3F&>(*this) = n;
        d = -m_dot(p, n);
    }

    int side(Point3F p, Flt32 precision = .05f) const {
        const auto distance = m_dot(*this, p);
        return distance - precision > -d ? 1 : distance + precision < -d ? -1 : 0;
    }

    bool intersect(Point3F a, Point3F b, Flt32& time) const {
        const auto dt = b - a;
        const auto den = m_dot(*this, dt), t = -(m_dot(*this, a) + d);
        if (den == 0 || (den > 0 ? t < 0 || t > den : t > 0 || t < den)) {
            return false;
        }
        time = t / den;
        return true;
    }
};

struct ReceiverPlane {
    Point3F normal;
    Flt32 constant{}, inverse{};
    std::size_t receiver{};
};

struct Receiver {
    std::vector<Point3F> vertices;
    Int32 plane{};
    Flt32 minimum{}, maximum{};
};

struct Node {
    Plane clip;
    Int32 inside{-1}, outside{-1}, outsidePlane{-1}, plane{-1};
};

struct Partition {
    const ShadowSourceWindow& window;
    std::vector<ReceiverPlane> planes;
    std::deque<Node> nodes; // stable references while recursive insertion allocates
    ShadowGeometry output;
    std::size_t work{4'194'304};

    void spend(std::size_t count = 1) {
        if (count > work) {
            throw std::length_error("native shadow work");
        }
        work -= count;
    }

    void depth(UInt32 value) {
        spend();
        if (value > 256) {
            throw std::length_error("native shadow depth");
        }
    }

    Int32 leaf(Int32 plane, Int32 outside) {
        spend();
        if (nodes.size() == 16384) {
            throw std::length_error("native shadow nodes");
        }
        nodes.push_back({{}, -1, -1, outside, plane});
        return Int32(nodes.size() - 1);
    }

    Int32 branch(const Plane* clip, Int32 plane, Int32 outside) {
        if (plane == outside || !clip) {
            return leaf(plane, outside);
        }
        const auto index = leaf(-1, outside);
        auto& node = nodes[std::size_t(index)];
        node.clip = *clip;
        node.inside = leaf(plane, outside);
        if (outside != -1) {
            node.outside = leaf(outside, outside);
        }
        return index;
    }

    void
    add(Int32 index, Point3F a, Point3F b, const Plane* clip, Int32 plane, UInt32 recursion = 0) {
        depth(recursion);
        auto& node = nodes[std::size_t(index)];
        if (node.plane != -1) {
            if (node.plane != plane) {
                const auto& old = planes[std::size_t(node.plane)];
                auto t = (old.constant - m_dot(old.normal, a)) * old.inverse;
                if (std::abs(t) < .1f) {
                    t = (old.constant - m_dot(old.normal, b)) * old.inverse;
                    if (std::abs(t) < .1f) {
                        auto c = a - (clip ? static_cast<const Point3F&>(*clip)
                                           : static_cast<const Point3F&>(node.clip));
                        const auto& next = planes[std::size_t(plane)];
                        t = (old.constant - m_dot(old.normal, c)) * old.inverse -
                            (next.constant - m_dot(next.normal, c)) * next.inverse;
                    }
                }
                if (t <= 0) {
                    return;
                }
                node.outsidePlane = node.plane;
                if (!clip) {
                    node.plane = plane;
                } else {
                    node.inside = leaf(plane, node.outsidePlane);
                    node.outside = leaf(node.plane, node.outsidePlane);
                    node.plane = -1;
                    node.clip = *clip;
                }
                return;
            }
            if (clip) {
                node.plane = -1;
                node.clip = *clip;
                node.inside = leaf(plane, node.outsidePlane);
                node.outside =
                    node.outsidePlane == -1 ? -1 : leaf(node.outsidePlane, node.outsidePlane);
            }
            return;
        }
        auto aSide = node.clip.side(a), bSide = node.clip.side(b);
        if (!aSide && !bSide) {
            if (!clip || std::abs(std::abs(clip->x) - std::abs(node.clip.x)) > .1f ||
                std::abs(std::abs(clip->y) - std::abs(node.clip.y)) > .1f ||
                std::abs(std::abs(clip->z) - std::abs(node.clip.z)) > .1f) {
                aSide = node.clip.side(a, .000001f);
                bSide = node.clip.side(b, .000001f);
            }
            if (clip) {
                aSide = m_dot(*clip, node.clip) > 0 ? 1 : -1;
                clip = nullptr;
            }
        }
        const auto outside = [&](Point3F from, Point3F to) {
            if (node.outside == -1) {
                node.outside = branch(clip, plane, node.outsidePlane);
            } else {
                add(node.outside, from, to, clip, plane, recursion + 1);
            }
        };
        Flt32 time{};
        Point3F c;
        switch (aSide * 3 + bSide) {
            case -4:
            case -1:
            case -3:
                outside(a, b);
                break;
            case 4:
            case 1:
            case 3:
                add(node.inside, a, b, clip, plane, recursion + 1);
                break;
            case -2:
                if (!node.clip.intersect(a, b, time)) {
                    throw std::invalid_argument("native shadow intersection");
                }
                c = a + (b - a) * time;
                outside(a, c);
                add(node.inside, c, b, clip, plane, recursion + 1);
                break;
            case 2:
                if (!node.clip.intersect(a, b, time)) {
                    throw std::invalid_argument("native shadow intersection");
                }
                c = a + (b - a) * time;
                add(node.inside, a, c, clip, plane, recursion + 1);
                outside(c, b);
                break;
            case 0:
                if (clip) {
                    throw std::invalid_argument("native shadow null clip");
                }
                add(node.inside, a, b, nullptr, plane, recursion + 1);
                outside(a, b);
                break;
            default:
                throw std::invalid_argument("native shadow side");
        }
    }

    std::pair<Poly, Poly> split(const Poly& input, const Plane& plane) {
        spend(input.size());
        Poly front, back;
        bool frontDegenerate = true, backDegenerate = true;
        auto previous = input.back();
        auto side = plane.side(previous.position);
        for (const auto& current : input) {
            const auto nextSide = plane.side(current.position);
            Flt32 time{};
            const auto intersection = [&] {
                return ShadowVertex{previous.position +
                                        (current.position - previous.position) * time,
                                    previous.texture + (current.texture - previous.texture) * time};
            };
            switch (side * 3 + nextSide) {
                case -4:
                case -1:
                    back.push_back(current);
                    backDegenerate = false;
                    break;
                case 4:
                case 1:
                    front.push_back(current);
                    frontDegenerate = false;
                    break;
                case -3:
                case 0:
                case 3:
                    back.push_back(current);
                    front.push_back(current);
                    break;
                case -2:
                    if (plane.intersect(previous.position, current.position, time)) {
                        const auto v = intersection();
                        back.push_back(v);
                        front.push_back(v);
                        front.push_back(current);
                        frontDegenerate = false;
                    }
                    break;
                case 2:
                    if (plane.intersect(previous.position, current.position, time)) {
                        const auto v = intersection();
                        front.push_back(v);
                        back.push_back(v);
                        back.push_back(current);
                        backDegenerate = false;
                    }
                    break;
            }
            previous = current;
            side = nextSide;
        }
        frontDegenerate |= front.size() < 3;
        backDegenerate |= back.size() < 3;
        if (frontDegenerate && backDegenerate) {
            front = input;
            back.clear();
        } else {
            if (frontDegenerate) {
                front.clear();
            }
            if (backDegenerate) {
                back.clear();
            }
        }
        if (front.size() > 32 || back.size() > 32) {
            throw std::length_error("native shadow vertices");
        }
        return {std::move(front), std::move(back)};
    }

    void emit(const Poly& polygon, Int32 plane) {
        spend(polygon.size());
        if (output.polygons.size() == 100) {
            throw std::length_error("native shadow fragments");
        }
        const auto& receiver = planes[std::size_t(plane)];
        ShadowPolygon result;
        result.receiverIndex = receiver.receiver;
        for (const auto& v : polygon) {
            const auto t =
                (receiver.constant - m_dot(v.position, receiver.normal)) * receiver.inverse;
            result.vertices.push_back({window.direction * t + v.position, v.texture});
        }
        output.polygons.push_back(std::move(result));
    }

    void partition(const Poly& input, Int32 index = 0, UInt32 recursion = 0) {
        depth(recursion);
        const auto& node = nodes[std::size_t(index)];
        if (node.plane != -1) {
            emit(input, node.plane);
            return;
        }
        auto [front, back] = split(input, node.clip);
        if (node.inside != -1 && !front.empty()) {
            partition(front, node.inside, recursion + 1);
        }
        if (node.outside != -1 && !back.empty()) {
            partition(back, node.outside, recursion + 1);
        }
    }
};
}

ShadowGeometry nativeShadowGeometry(const ShadowSourceWindow& window,
                                    std::span<const ShadowReceiver> receivers,
                                    const Point3F& cameraDirection,
                                    Flt32 lift) {
    // Shared validation includes finite/unit/coplanar/convex checks. Its exact
    // clipped result is discarded; native admission/partition follows below.
    if (receivers.size() > 4096) {
        throw std::length_error("native shadow receivers");
    }
    (void)buildShadowGeometry(window, {}, cameraDirection, lift);
    if (receivers.empty()) {
        return {};
    }
    for (const auto& receiver : receivers) {
        (void)buildShadowGeometry(window, std::span(&receiver, 1), cameraDirection, lift);
    }
    Partition state{window, {}, {}, {}};
    std::vector<Receiver> sorted;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < receivers.size();
                  })) {
        const auto& receiver = receivers[i];
        Receiver poly;
        poly.vertices = receiver.vertices;
        state.spend(receiver.vertices.size());
        const auto constant = receiver.constant - m_dot(receiver.normal, window.direction) * lift;
        const auto same = [&](const ReceiverPlane& p) {
            state.spend();
            return std::abs(p.constant - constant) < .01f &&
                   std::abs(p.normal.x - receiver.normal.x) < .01f &&
                   std::abs(p.normal.y - receiver.normal.y) < .01f &&
                   std::abs(p.normal.z - receiver.normal.z) < .01f;
        };
        const auto existing = std::find_if(state.planes.begin(), state.planes.end(), same);
        poly.plane = Int32(existing - state.planes.begin());
        if (existing == state.planes.end()) {
            const auto dot = m_dot(window.direction, receiver.normal);
            state.planes.push_back(
                {receiver.normal, constant, dot * dot > .0001f ? 1 / dot : 100.f, i});
            state.output.maxPlaneDot =
                std::max(state.output.maxPlaneDot, -m_dot(cameraDirection, receiver.normal));
        }
        // cachePolys computes plane/lift metadata first, then resets lift for
        // clip-tree construction. getPlanes offsets only the first minT sample.
        poly.minimum = poly.maximum =
            m_dot(poly.vertices[0] - window.direction * lift, window.direction);
        for (auto n : std::views::iota(std::size_t{1}) | std::views::take_while([&](std::size_t n) {
                          return n < poly.vertices.size();
                      })) {
            const auto d = m_dot(poly.vertices[n], window.direction);
            poly.minimum = std::min(poly.minimum, d);
            poly.maximum = std::max(poly.maximum, d);
        }
        sorted.push_back(std::move(poly));
    }
    // The native comparator's <=/>= tie behavior is not a strict ordering. Use
    // bounded insertion, preserving source order for truly equal extents.
    for (auto i : std::views::iota(std::size_t{1}) | std::views::take_while([&](std::size_t i) {
                      return i < sorted.size();
                  })) {
        for (std::size_t j = i; j > 0; --j) {
            state.spend();
            const auto& a = sorted[j];
            const auto& b = sorted[j - 1];
            const bool before = std::abs(a.minimum - b.minimum) > .05f ? a.minimum < b.minimum
                                                                       : a.maximum > b.maximum;
            if (!before) {
                break;
            }
            std::swap(sorted[j], sorted[j - 1]);
        }
    }
    Poly source;
    constexpr std::array<Point2F, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{4})) {
        source.push_back({window.points[i], uv[i]});
    }
    if (state.planes.size() == 1) {
        if (state.planes[0].inverse < 0) {
            state.emit(source, 0);
        }
        return state.output;
    }
    for (const auto& poly : sorted) {
        if (state.planes[std::size_t(poly.plane)].inverse >= 0) {
            continue;
        }
        auto a = poly.vertices.back();
        for (auto b : poly.vertices) {
            const auto edge = b - a;
            Point3F normal;
            m_cross(window.direction, edge, normal);
            if (normal.len() < 1e-8f) {
                throw std::invalid_argument("native shadow edge");
            }
            const Plane clip(a, normal);
            if (state.nodes.empty()) {
                state.branch(&clip, poly.plane, -1);
            } else {
                state.add(0, a, b, &clip, poly.plane);
            }
            a = b;
        }
    }
    if (!state.nodes.empty()) {
        state.partition(source);
    }
    return state.output;
}
} // namespace opent1::sim
