#include <opent1/gfx/render_limits.hpp>
#include <opent1/gfx/software_rasterizer.hpp>
#include "software_float.hpp"
#include <algorithm>
#include <cmath>
#include <ranges>

namespace opent1::gfx {
namespace {
using Float = detail::RasterFloat;
constexpr std::size_t none = std::size_t(-1);

constexpr Int64 edgeUnit = Int64{1} << 20;

struct Plane {
    double dx{}, dy{}, origin{};
    Flt32 row{};
};

struct Edge {
    std::size_t polygon{};
    Int32 first{}, last{};
    Int64 x{}, step{};
    bool leading{};
};

struct Fragment {
    std::size_t polygon{}, next{none};
    Int32 left{}, right{};
    bool behind{};
};

double areaOf(std::span<const Point3F> vertices) {
    double area = 0;
    auto previous = vertices.back();
    for (const auto& current : vertices) {
        area = ((Float(previous.x) - current.x) * (Float(previous.y) + current.y) + area).stored();
        previous = current;
    }
    return area;
}

bool convex(std::span<const Point3F> vertices, double area) {
    const double sign = area >= 0 ? 1 : -1;
    for (auto i : std::views::iota(std::size_t{0}, vertices.size())) {
        const auto a = vertices[i], b = vertices[(i + 1) % vertices.size()];
        // A repeated circuit of a convex boundary passes half-plane tests but
        // is not a simple polygon and would insert duplicate leading edges.
        if (area != 0 && std::ranges::any_of(vertices.subspan(i + 1), [&](auto p) {
                return p.x == a.x && p.y == a.y;
            })) {
            return false;
        }
        if (!std::ranges::all_of(vertices, [&](auto p) {
                return sign * ((double(b.x) - a.x) * (double(p.y) - a.y) -
                               (double(b.y) - a.y) * (double(p.x) - a.x)) >=
                       0;
            })) {
            return false;
        }
    }
    return true;
}

Plane depthPlane(std::span<const Point3F> vertices, double area) {
    double dx = 0, dy = 0;
    auto previous = vertices.back();
    for (const auto& current : vertices) {
        dx = ((Float(previous.z) + current.z) * (Float(previous.y) - current.y) + dx).stored();
        dy = ((Float(previous.z) - current.z) * (Float(previous.x) + current.x) + dy).stored();
        previous = current;
    }
    const auto factor = Float(-1) / area;
    Plane plane{(Float(dx) * factor).stored(), (Float(dy) * factor).stored()};
    const auto a = vertices.front();
    plane.origin = ((Float(a.z) - Float(plane.dx) * a.x) - Float(plane.dy) * a.y).stored();
    return plane;
}

Int32 edgePixel(Int64 value) {
    value += edgeUnit - 1;
    return Int32(value >= 0 ? value / edgeUnit : -((-value + edgeUnit - 1) / edgeUnit));
}

bool appendEdges(std::span<const Point3F> vertices,
                 std::size_t polygon,
                 RectI clip,
                 std::vector<Edge>& edges) {
    auto previous = vertices.back();
    for (const auto& current : vertices) {
        const Int32 a = Int32(std::ceil(previous.y)), b = Int32(std::ceil(current.y));
        const Int32 first = std::min(a, b), last = std::max(a, b);
        if (a != b && first <= clip.bottom && last > clip.top) {
            const auto slope = (Float(current.x) - previous.x) / (Float(current.y) - previous.y);
            const double rawStep = (slope * double(edgeUnit)).stored();
            const double rawStart =
                ((Float(previous.x) + (Float(first) - previous.y) * slope) * double(edgeUnit))
                    .stored();
            constexpr double limit = double(Int64{1} << 45);
            if (!std::isfinite(rawStart) || !std::isfinite(rawStep) ||
                std::abs(rawStart) >= limit || std::abs(rawStep) >= limit) {
                return false;
            }
            edges.push_back({polygon, first, last, Int64(rawStart), Int64(rawStep), a > b});
            if (edges.size() > renderLimits().edges) {
                return false;
            }
        }
        previous = current;
    }
    return true;
}

// Linked indices keep storage owned and stable through vector growth. Newly
// split fragments deliberately have no ALWAYS_BEHIND flag, as in p_scane.asm.
struct RowSorter {
    std::vector<Fragment> nodes;
    std::span<const Plane> planes;
    std::vector<VisibilitySpan>& output;
    RectI clip;
    DepthOrder order;
    Int32 y{};
    std::size_t operations{}, fragments{};
    bool ok{true};

    void emit(std::size_t node, Int32 left, Int32 right) {
        left = std::max(left, clip.left);
        right = std::min(right, clip.right + 1);
        if (right <= left) {
            return;
        }
        if (output.size() == renderLimits().spans) {
            ok = false;
            return;
        }
        output.push_back({nodes[node].polygon, y, left, right});
    }

    void resort(std::size_t start, std::size_t node) {
        while (nodes[start].next != none && nodes[nodes[start].next].left < nodes[node].left) {
            start = nodes[start].next;
            if (++operations > renderLimits().operations) {
                ok = false;
                return;
            }
        }
        nodes[node].next = nodes[start].next;
        nodes[start].next = node;
    }

    void reverseInsert(std::size_t& head, std::size_t node) {
        std::size_t previous = none, current = head;
        while (current != none && nodes[current].left > nodes[node].left) {
            previous = current;
            current = nodes[current].next;
            if (++operations > renderLimits().operations) {
                ok = false;
                return;
            }
        }
        nodes[node].next = current;
        if (previous == none) {
            head = node;
        } else {
            nodes[previous].next = node;
        }
    }

    std::size_t split(std::size_t original, Int32 left, Int32 right) {
        if (++fragments > renderLimits().fragments) {
            ok = false;
            return none;
        }
        const auto index = nodes.size();
        nodes.push_back({nodes[original].polygon, none, left, right, false});
        return index;
    }

    // Positive means the current fragment is in front. The original stores each
    // comparison to float and branches on its sign bit, including signed zero.
    Flt32 status(std::size_t current, std::size_t next, Int32 x) const {
        const auto& a = planes[nodes[current].polygon];
        const auto& b = planes[nodes[next].polygon];
        const auto difference = (Float(a.row) + Float(x) * a.dx) - (Float(b.row) + Float(x) * b.dx);
        return (order == DepthOrder::LargerInFront ? difference : Float(0) - difference)
            .storedFloat();
    }

    Int32 intersection(std::size_t current, std::size_t next) {
        const auto& a = planes[nodes[current].polygon];
        const auto& b = planes[nodes[next].polygon];
        const double value = ((Float(b.row) - a.row) / (Float(a.dx) - b.dx)).stored();
        if (!std::isfinite(value) || value < INT32_MIN || value > INT32_MAX) {
            ok = false;
            return 0;
        }
        return Int32(value);
    }

    // Clip all transparent row fragments against this opaque candidate before
    // resolving the next opaque overlap. This preserves original crossing/tie
    // behavior and leaves transparent-vs-transparent order to the draw pass.
    void clipTransparent(std::size_t current, std::size_t& transparent) {
        std::size_t reversed = none, trans = transparent;
        transparent = none;
        while (trans != none && ok) {
            if (++operations > renderLimits().operations) {
                ok = false;
                return;
            }
            const auto next = nodes[trans].next;
            const auto cp = nodes[current], tp = nodes[trans];
            if (tp.left >= cp.right) {
                break;
            }
            if (tp.right <= cp.left) {
                emit(trans, tp.left, tp.right);
            } else if (cp.behind) {
                reverseInsert(reversed, trans);
            } else {
                const auto start = status(current, trans, std::max(cp.left, tp.left));
                const auto end = status(current, trans, std::min(cp.right, tp.right) - 1);
                if (!std::signbit(start) && !std::signbit(end)) {
                    emit(trans, tp.left, cp.left);
                    if (cp.right < tp.right) {
                        nodes[trans].left = cp.right;
                        reverseInsert(reversed, trans);
                    }
                } else if (std::signbit(start) && std::signbit(end)) {
                    reverseInsert(reversed, trans);
                } else {
                    const auto x = intersection(current, trans);
                    if (!ok) {
                        return;
                    }
                    if (!std::signbit(start)) {
                        emit(trans, tp.left, cp.left);
                        nodes[trans].left = x;
                        reverseInsert(reversed, trans);
                    } else {
                        if (tp.right > cp.right) {
                            const auto extra = split(trans, cp.right, tp.right);
                            if (!ok) {
                                return;
                            }
                            reverseInsert(reversed, extra);
                        }
                        nodes[trans].right = x;
                        reverseInsert(reversed, trans);
                    }
                }
            }
            trans = next;
        }
        // Merge the reversed processed list into the untouched forward tail.
        while (reversed != none && ok) {
            if (++operations > renderLimits().operations) {
                ok = false;
                return;
            }
            const auto next = nodes[reversed].next;
            if (trans == none || nodes[reversed].left <= nodes[trans].left) {
                nodes[reversed].next = trans;
                trans = reversed;
            } else {
                resort(trans, reversed);
            }
            reversed = next;
        }
        transparent = trans;
    }

    void scan(std::size_t opaque, std::size_t transparent) {
        auto current = opaque;
        bool clipTrans = true;
        while (current != none && ok) {
            if (++operations > renderLimits().operations) {
                ok = false;
                return;
            }
            if (clipTrans) {
                clipTransparent(current, transparent);
                if (!ok) {
                    return;
                }
            }
            const auto next = nodes[current].next;
            if (next == none) {
                emit(current, nodes[current].left, nodes[current].right);
                break;
            }
            const auto cp = nodes[current], np = nodes[next];
            if (np.left >= cp.right) {
                emit(current, cp.left, cp.right);
                current = next;
                clipTrans = true;
                continue;
            }
            const auto start = status(current, next, np.left),
                       end = status(current, next, std::min(cp.right, np.right) - 1);
            const bool inFront =
                !cp.behind && (np.behind || (!std::signbit(start) && !std::signbit(end)));
            const bool behind =
                cp.behind || (!np.behind && std::signbit(start) && std::signbit(end));
            if (inFront) {
                nodes[current].next = np.next;
                if (cp.right < np.right) {
                    nodes[next].left = cp.right;
                    resort(current, next);
                }
                clipTrans = false;
            } else if (behind) {
                emit(current, cp.left, np.left);
                if (cp.right > np.right) {
                    nodes[current].left = np.right;
                    resort(next, current);
                }
                current = next;
                clipTrans = true;
            } else {
                const auto x = intersection(current, next);
                if (!ok) {
                    return;
                }
                if (!std::signbit(start)) {
                    if (cp.right > np.right) {
                        const auto extra = split(current, np.right, cp.right);
                        if (!ok) {
                            return;
                        }
                        resort(next, extra);
                    }
                    nodes[current].right = x;
                    nodes[next].left = x;
                    nodes[current].next = nodes[next].next;
                    resort(current, next);
                    clipTrans = false;
                } else {
                    if (np.right > cp.right) {
                        const auto extra = split(next, cp.right, np.right);
                        if (!ok) {
                            return;
                        }
                        resort(next, extra);
                    }
                    emit(current, cp.left, np.left);
                    nodes[current].left = x;
                    nodes[next].right = x;
                    resort(next, current);
                    current = next;
                    clipTrans = true;
                }
            }
        }
        while (transparent != none && ok) {
            if (++operations > renderLimits().operations) {
                ok = false;
                return;
            }
            emit(transparent, nodes[transparent].left, nodes[transparent].right);
            transparent = nodes[transparent].next;
        }
    }
};
} // namespace

std::optional<std::vector<VisibilitySpan>>
sortVisibility(std::span<const VisibilityPolygon> polygons, RectI clip, DepthOrder order) {
    if (polygons.size() > renderLimits().polygons ||
        UInt8(order) > UInt8(DepthOrder::SmallerInFront) ||
        (clip.valid() &&
         (clip.left < -Bitmap::MaxDimension || clip.top < -Bitmap::MaxDimension ||
          clip.right >= Bitmap::MaxDimension || clip.bottom >= Bitmap::MaxDimension))) {
        return {};
    }
    std::vector<Plane> planes(polygons.size());
    std::vector<Edge> edges;
    for (auto i : std::views::iota(std::size_t{0}, polygons.size())) {
        const auto vertices = polygons[i].vertices;
        if (vertices.size() < 3 || vertices.size() > 100 ||
            !std::ranges::all_of(vertices, [](const auto& v) {
                return std::isfinite(v.x) && std::isfinite(v.y) &&
                       std::abs(v.x) <= Bitmap::MaxDimension &&
                       std::abs(v.y) <= Bitmap::MaxDimension && std::isfinite(v.z) &&
                       v.z >= 1.f / 65536 && v.z <= 65536;
            })) {
            return {};
        }
        const auto area = areaOf(vertices);
        if (!convex(vertices, area)) {
            return {};
        }
        if (area <= 0 || !clip.valid()) {
            continue;
        }
        planes[i] = depthPlane(vertices, area);
        if (!appendEdges(vertices, i, clip, edges)) {
            return {};
        }
    }
    std::vector<VisibilitySpan> output;
    if (edges.empty()) {
        return output;
    }
    // New edges precede existing new edges at equal fixed X. On later rows,
    // already-active edges precede new ones at equal X. Stable sorting retains
    // edge crossing order and the original coplanar polygon tie policy.
    std::vector<std::size_t> active, pending(edges.size());
    std::ranges::copy(std::views::iota(std::size_t{0}, edges.size()), pending.begin());
    std::ranges::sort(pending, [&](auto a, auto b) {
        return edges[a].first != edges[b].first ? edges[a].first < edges[b].first : a > b;
    });
    std::size_t cursor = 0, operations = 0;
    RowSorter sorter{{}, planes, output, clip, order};
    const auto firstRow = std::min(clip.top, std::ranges::min(edges, {}, &Edge::first).first);
    for (auto y :
         std::views::iota(static_cast<Int32>(firstRow)) | std::views::take_while([&](Int32 y) {
             return y <= clip.bottom;
         })) {
        std::erase_if(active, [&](auto i) {
            return edges[i].last <= y;
        });
        while (cursor < pending.size() && edges[pending[cursor]].first <= y) {
            active.push_back(pending[cursor++]);
        }
        operations += active.size();
        if (operations > renderLimits().operations) {
            return {};
        }
        const auto at = [&](auto i) {
            const auto& e = edges[i];
            return e.x + e.step * (Int64(y) - e.first);
        };
        std::stable_sort(active.begin(), active.end(), [&](auto a, auto b) {
            return at(a) < at(b);
        });
        // Reconstruct edge tie history above the clip without evaluating depth
        // or emitting spans there. Clipping cannot change a coplanar tie.
        if (y < clip.top) {
            continue;
        }
        std::vector<Int32> left(polygons.size()), right(polygons.size());
        for (const auto i : active) {
            const auto& e = edges[i];
            (e.leading ? left[e.polygon] : right[e.polygon]) = edgePixel(at(i));
        }
        sorter.nodes.clear();
        sorter.fragments = 0;
        sorter.y = y;
        std::size_t opaque = none, transparent = none, opaqueTail = none, transparentTail = none;
        for (const auto i : active) {
            const auto& e = edges[i];
            if (!e.leading || right[e.polygon] <= left[e.polygon]) {
                continue;
            }
            const auto node = sorter.nodes.size();
            const auto& p = polygons[e.polygon];
            sorter.nodes.push_back(
                {e.polygon, none, left[e.polygon], right[e.polygon], p.alwaysBehind});
            auto& head = p.transparent ? transparent : opaque;
            auto& tail = p.transparent ? transparentTail : opaqueTail;
            if (tail == none) {
                head = node;
            } else {
                sorter.nodes[tail].next = node;
            }
            tail = node;
            auto& plane = planes[e.polygon];
            plane.row = (Float(y) * plane.dy + plane.origin).storedFloat();
        }
        sorter.scan(opaque, transparent);
        if (!sorter.ok) {
            return {};
        }
    }
    // Polygon IDs are a bounded dense key. Stable bucket placement preserves
    // native span order within each primitive without sorting every scanline
    // fragment (often tens of thousands of spans per frame).
    std::vector<std::size_t> counts(polygons.size()), offsets(polygons.size());
    for (const auto& span : output) {
        ++counts[span.polygon];
    }
    std::size_t next = 0;
    for (const bool transparent : {false, true}) {
        for (auto i : std::views::iota(std::size_t{0}, polygons.size())) {
            if (polygons[i].transparent == transparent) {
                offsets[i] = next;
                next += counts[i];
            }
        }
    }
    std::vector<VisibilitySpan> ordered(output.size());
    for (const auto& span : output) {
        ordered[offsets[span.polygon]++] = span;
    }
    return ordered;
}

bool SoftwareSurface::sortedFills(std::span<const SortedFill> fills, DepthOrder order) {
    if (fills.size() > renderLimits().polygons) {
        return false;
    }
    std::vector<VisibilityPolygon> polygons;
    polygons.reserve(fills.size());

    struct Ink {
        const SubPalette* palette{};
        UInt8 color{};
        bool blend{};
    };

    std::vector<Ink> inks;
    inks.reserve(fills.size());
    for (const auto& fill : fills) {
        const auto* p = palette_ ? &palette_->find(fill.paletteIndex) : nullptr;
        if (fill.blend &&
            (!p || p->remap.size() != 65536 ||
             (p->type != PaletteType::Translucent && p->type != PaletteType::Additive &&
              p->type != PaletteType::Subtractive))) {
            return false;
        }
        auto color = fill.color;
        if (!fill.blend && fill.paletteIndex != 0xffffffffU && p &&
            p->type == PaletteType::ShadeHaze) {
            color = std::to_integer<UInt8>(
                p->remap[std::size_t((1U << palette_->shadeShift) - 1) * 256 + color]);
        }
        inks.push_back({p, color, fill.blend});
        polygons.push_back({fill.vertices, fill.blend, fill.alwaysBehind});
    }
    const auto spans = sortVisibility(polygons, clip_, order);
    if (!spans) {
        return false;
    }
    std::ranges::for_each(*spans, [&](const auto& span) {
        const auto& ink = inks[span.polygon];
        auto output = bitmap_.row(span.y).subspan(std::size_t(span.left),
                                                  std::size_t(span.right - span.left));
        if (ink.blend) {
            std::ranges::transform(output, output.begin(), [&](auto value) {
                return ink.palette->remap[UInt32(ink.color) * 256 + std::to_integer<UInt8>(value)];
            });
        } else {
            std::ranges::fill(output, std::byte(ink.color));
        }
    });
    return true;
}
} // namespace opent1::gfx
