#include <opent1/world/shadow_receivers.hpp>
#include <opent1/gfx/render_stats.hpp>
#include <opent1/core/reusable_scratch.hpp>
#include "collision_geometry.hpp"
#include <numeric>
#include <bit>
#include <unordered_map>
#include <limits>
#include <memory_resource>

namespace opent1::world {
namespace {
using namespace collision_detail;
constexpr std::size_t MaxCandidates = 4096, MaxFragments = 4096;

struct XY {
    double x{}, y{};
};

struct Half {
    double x{}, y{}, c{};

    double at(XY p) const {
        return x * p.x + y * p.y + c;
    }
};

// Clipping a triangle to the source window/depth interval needs at most nine
// vertices. Differences can grow further; the public limit remains 32.
struct Poly {
    std::array<XY, 34> vertices;
    std::size_t count{};
    Poly() = default;

    Poly(const Poly& other) : count(other.count) {
        std::copy_n(other.begin(), count, vertices.begin());
    }

    Poly& operator=(const Poly& other) {
        count = other.count;
        std::copy_n(other.begin(), count, vertices.begin());
        return *this;
    }

    const XY* begin() const {
        return vertices.data();
    }

    const XY* end() const {
        return begin() + count;
    }

    XY& operator[](std::size_t i) {
        return vertices[i];
    }

    const XY& operator[](std::size_t i) const {
        return vertices[i];
    }

    bool empty() const {
        return count == 0;
    }

    void add(XY p) {
        if (count == vertices.size())
            throw std::length_error("shadow fragment vertices");
        vertices[count++] = p;
    }
};

double area(const Poly& p) {
    if (p.count < 3)
        return 0;
    const auto origin = p[0];
    double value{};
    for (std::size_t i = 1; i + 1 < p.count; ++i)
        value += (p[i].x - origin.x) * (p[i + 1].y - origin.y) -
                 (p[i].y - origin.y) * (p[i + 1].x - origin.x);
    return value;
}

Poly clip(const Poly& p, Half h, std::size_t& work) {
    if (gfx::currentRenderWorkStats)
        ++gfx::currentRenderWorkStats->shadowClipCalls;
    if (p.empty())
        return {};
    spend(work, p.count);
    Poly result;
    auto a = p[p.count - 1];
    auto da = h.at(a);
    for (auto b : p) {
        const auto db = h.at(b);
        if ((da >= 0) != (db >= 0)) {
            const auto t = da / (da - db);
            result.add({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        }
        if (db >= 0)
            result.add(b);
        a = b;
        da = db;
    }
    std::size_t kept{};
    for (std::size_t i = 0; i < result.count; ++i)
        if (!kept ||
            std::hypot(result[i].x - result[kept - 1].x, result[i].y - result[kept - 1].y) >= 1e-9)
            result.vertices[kept++] = result[i];
    result.count = kept;
    if (kept > 1 &&
        std::hypot(result[0].x - result[kept - 1].x, result[0].y - result[kept - 1].y) < 1e-9)
        --result.count;
    if (std::abs(area(result)) < 1e-10)
        result.count = 0;
    if (result.count > 32)
        throw std::length_error("shadow fragment vertices");
    return result;
}

struct Edges {
    std::array<Half, 33> values{};
    std::size_t count{};

    const Half* begin() const {
        return values.data();
    }

    const Half* end() const {
        return begin() + count;
    }
};

Edges edges(const Poly& p) {
    Edges result;
    const auto sign = area(p) > 0 ? 1. : -1.;
    for (std::size_t i = 0; i < p.count; ++i) {
        const auto a = p[i], b = p[(i + 1) % p.count];
        const auto x = -(b.y - a.y) * sign, y = (b.x - a.x) * sign;
        result.values[result.count++] = {x, y, -x * a.x - y * a.y};
    }
    return result;
}

struct Bounds {
    double minX{}, minY{}, maxX{}, maxY{};

    explicit Bounds(const Poly& p) : minX(p[0].x), minY(p[0].y), maxX(minX), maxY(minY) {
        for (auto v : p) {
            minX = std::min(minX, v.x);
            minY = std::min(minY, v.y);
            maxX = std::max(maxX, v.x);
            maxY = std::max(maxY, v.y);
        }
    }

    bool overlaps(const Bounds& b) const {
        return maxX > b.minX && b.maxX > minX && maxY > b.minY && b.maxY > minY;
    }
};

struct Candidate {
    Poly polygon;
    Bounds bounds;
    Half depth;
    Point3F normal;
    float constant{};
};

struct EdgeKey {
    std::array<UInt64, 4> points;
    bool operator==(const EdgeKey&) const = default;
};

struct EdgeHash {
    std::size_t operator()(const EdgeKey& key) const {
        std::size_t h{};
        for (auto v : key.points)
            h ^= std::hash<UInt64>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

EdgeKey edgeKey(XY a, XY b) {
    // Canonicalize signed zero, but never round geometry across a ledge/gap.
    return {{std::bit_cast<UInt64>(a.x == 0 ? 0. : a.x),
             std::bit_cast<UInt64>(a.y == 0 ? 0. : a.y),
             std::bit_cast<UInt64>(b.x == 0 ? 0. : b.x),
             std::bit_cast<UInt64>(b.y == 0 ? 0. : b.y)}};
}

std::optional<Poly> convexUnion(const Poly& a, std::size_t edge, const Poly& b, std::size_t other) {
    std::array<XY, 64> points;
    std::size_t count{};
    for (std::size_t i = 0; i < a.count; ++i)
        points[count++] = a[(edge + 1 + i) % a.count];
    for (std::size_t i = 2; i < b.count; ++i)
        points[count++] = b[(other + i) % b.count];
    const auto turn = [](XY a0, XY b0, XY c0) {
        return (b0.x - a0.x) * (c0.y - b0.y) - (b0.y - a0.y) * (c0.x - b0.x);
    };
    // Double-precision light space keeps this tolerance far below the existing
    // 1e-9 vertex and 1e-10 polygon-area rejection thresholds.
    bool changed = true;
    while (changed && count > 3) {
        changed = false;
        for (std::size_t i = 0; i < count; ++i)
            if (std::abs(
                    turn(points[(i + count - 1) % count], points[i], points[(i + 1) % count])) <=
                1e-12) {
                std::move(points.begin() + i + 1, points.begin() + count, points.begin() + i);
                --count;
                changed = true;
                break;
            }
    }
    if (count > 32)
        return {};
    const auto sign = area(a) > 0 ? 1. : -1.;
    for (std::size_t i = 0; i < count; ++i)
        if (turn(points[i], points[(i + 1) % count], points[(i + 2) % count]) * sign < -1e-12)
            return {};
    Poly result;
    for (std::size_t i = 0; i < count; ++i)
        result.add(points[i]);
    return result;
}

// Eliminate triangulation diagonals before partitioning. Only convex unions
// sharing an exact edge and the same depth plane qualify; holes/concave outlines
// remain separate. The edge table avoids a second all-pairs merging algorithm.
void coalesce(std::vector<Candidate>& candidates,
              std::size_t& work,
              std::pmr::memory_resource* memory) {
    std::pmr::unordered_multimap<EdgeKey, std::size_t, EdgeHash> owners(memory);
    owners.reserve(candidates.size() * 3);
    std::vector<bool> active(candidates.size(), true);
    const auto remove = [&](std::size_t id) {
        const auto& p = candidates[id].polygon;
        for (std::size_t i = 0; i < p.count; ++i) {
            auto [first, last] = owners.equal_range(edgeKey(p[i], p[(i + 1) % p.count]));
            for (auto it = first; it != last;) {
                if (it->second == id)
                    it = owners.erase(it);
                else
                    ++it;
            }
        }
    };
    for (std::size_t input = 0; input < candidates.size(); ++input) {
        auto current = input;
        bool merged = true;
        while (merged) {
            merged = false;
            const auto& a = candidates[current];
            for (std::size_t edge = 0; edge < a.polygon.count && !merged; ++edge) {
                const auto key = edgeKey(a.polygon[(edge + 1) % a.polygon.count], a.polygon[edge]);
                const auto [first, last] = owners.equal_range(key);
                for (auto it = first; it != last; ++it) {
                    spend(work);
                    const auto other = it->second;
                    if (other == current || !active[other])
                        continue;
                    const auto& b = candidates[other];
                    if (std::abs(a.depth.x - b.depth.x) + std::abs(a.depth.y - b.depth.y) +
                            std::abs(a.depth.c - b.depth.c) >=
                        1e-8)
                        continue;
                    for (std::size_t e = 0; e < b.polygon.count; ++e) {
                        if (!(edgeKey(b.polygon[e], b.polygon[(e + 1) % b.polygon.count]) == key))
                            continue;
                        const auto joined = convexUnion(a.polygon, edge, b.polygon, e);
                        if (!joined)
                            break;
                        remove(other);
                        const auto keep = std::min(current, other),
                                   discard = std::max(current, other);
                        candidates[keep].polygon = *joined;
                        candidates[keep].bounds = Bounds(*joined);
                        active[discard] = false;
                        current = keep;
                        merged = true;
                        break;
                    }
                    if (merged)
                        break;
                }
            }
        }
        const auto& p = candidates[current].polygon;
        for (std::size_t i = 0; i < p.count; ++i)
            owners.emplace(edgeKey(p[i], p[(i + 1) % p.count]), current);
    }
    std::size_t next{};
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (active[i])
            candidates[next++] = std::move(candidates[i]);
    candidates.erase(candidates.begin() + next, candidates.end());
}

// Indexed units are authored interior surfaces, retained terrain patches, or
// explicit triangles. No decoded texture/lightmap data is copied into an index.
struct Index {
    struct Item {
        Box3F box;
        UInt32 id{};
    };

    struct Node {
        Box3F box;
        UInt32 first{}, count{}, left{}, right{};
    };

    std::vector<Item> items;
    std::vector<Node> nodes;

    std::size_t bytes() const {
        return items.capacity() * sizeof(Item) + nodes.capacity() * sizeof(Node);
    }

    UInt32 build(UInt32 first, UInt32 count) {
        const auto id = UInt32(nodes.size());
        auto box = items[first].box;
        for (UInt32 i = first + 1; i < first + count; ++i) {
            box.fMin.setMin(items[i].box.fMin);
            box.fMax.setMax(items[i].box.fMax);
        }
        nodes.push_back({box, first, count, 0, 0});
        if (count > 8) {
            const auto extent = box.fMax - box.fMin;
            std::size_t axis = extent.y > extent.x ? 1 : 0;
            if (extent.z > extent[axis])
                axis = 2;
            const auto middle = first + count / 2;
            std::nth_element(items.begin() + first,
                             items.begin() + middle,
                             items.begin() + first + count,
                             [axis](const auto& a, const auto& b) {
                                 return a.box.fMin[axis] + a.box.fMax[axis] <
                                        b.box.fMin[axis] + b.box.fMax[axis];
                             });
            const auto left = build(first, count / 2), right = build(middle, count - count / 2);
            nodes[id].left = left;
            nodes[id].right = right;
            nodes[id].count = 0;
        }
        return id;
    }

    void query(UInt32 node, const Box3F& box, std::vector<UInt32>& out, std::size_t& work) const {
        spend(work);
        const auto& n = nodes[node];
        if (!n.box.overlaps(box))
            return;
        if (!n.count) {
            query(n.left, box, out, work);
            query(n.right, box, out, work);
            return;
        }
        for (UInt32 i = n.first; i < n.first + n.count; ++i) {
            spend(work);
            if (items[i].box.overlaps(box))
                out.push_back(items[i].id);
        }
    }
};

std::shared_ptr<const Index> makeIndex(const WorldSurfaceMesh& mesh) {
    auto result = std::make_shared<Index>();
    const auto add = [&](Box3F box, UInt32 id) {
        if (!valid(box))
            throw std::invalid_argument("invalid shadow receiver bounds");
        result->items.push_back({box, id});
    };
    if (mesh.interiorMesh) {
        const auto& m = *mesh.interiorMesh;
        if (m.ranges.size() != m.surfaceBounds.size() || !m.source ||
            m.ranges.size() != m.source->surfaces.size())
            throw std::invalid_argument("invalid shadow interior");
        for (UInt32 i = 0; i < m.ranges.size(); ++i)
            if (m.ranges[i][1])
                add(m.surfaceBounds[i], i);
    } else if (mesh.terrainPatches) {
        for (UInt32 i = 0; i < mesh.terrainPatches->patches.size(); ++i) {
            const auto& p = mesh.terrainPatches->patches[i];
            // All endpoint heights, including an in-progress geomorph, are
            // conservative. Unused vertices only enlarge the query bounds.
            Box3F box{p.origin, p.origin + Point3F{p.extent, p.extent, 0}};
            box.fMin.z = std::numeric_limits<float>::max();
            box.fMax.z = std::numeric_limits<float>::lowest();
            for (const auto& h : p.heights) {
                box.fMin.z = std::min(box.fMin.z, p.origin.z + std::min(h[0], h[1]));
                box.fMax.z = std::max(box.fMax.z, p.origin.z + std::max(h[0], h[1]));
            }
            add(box, i);
        }
    } else {
        for (UInt32 i = 0; i < mesh.triangles.size(); ++i) {
            const auto& t = mesh.triangles[i];
            Box3F box{t.vertices[0].position, t.vertices[0].position};
            for (const auto& v : t.vertices) {
                box.fMin.setMin(v.position);
                box.fMax.setMax(v.position);
            }
            add(box, i);
        }
    }
    if (!result->items.empty()) {
        result->nodes.reserve(result->items.size() / 2 + 1);
        result->build(0, UInt32(result->items.size()));
    }
    if (gfx::currentRenderWorkStats)
        ++gfx::currentRenderWorkStats->shadowIndexBuilds;
    return result;
}

// Polygon bounds occupy a fixed grid in source light space. Flat references
// avoid per-cell allocations; stamps deduplicate pairs before exact clipping.
struct CandidateGrid {
    static constexpr int Width = 8;
    std::array<std::size_t, Width * Width + 1> offsets{};
    std::vector<UInt16>& refs;
    Bounds bounds;

    std::array<int, 4> cells(const Bounds& b) const {
        const auto cell = [](double value, double lo, double hi) {
            return std::clamp(int((value - lo) / std::max(hi - lo, 1e-12) * Width), 0, Width - 1);
        };
        return {cell(b.minX, bounds.minX, bounds.maxX),
                cell(b.minY, bounds.minY, bounds.maxY),
                cell(b.maxX, bounds.minX, bounds.maxX),
                cell(b.maxY, bounds.minY, bounds.maxY)};
    }

    explicit CandidateGrid(const std::vector<Candidate>& candidates,
                           const Poly& window,
                           std::vector<UInt16>& storage)
        : refs(storage), bounds(window) {
        for (const auto& c : candidates) {
            const auto range = cells(c.bounds);
            for (int y = range[1]; y <= range[3]; ++y)
                for (int x = range[0]; x <= range[2]; ++x)
                    ++offsets[std::size_t(y * Width + x) + 1];
        }
        std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());
        refs.resize(offsets.back());
        auto cursor = offsets;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto range = cells(candidates[i].bounds);
            for (int y = range[1]; y <= range[3]; ++y)
                for (int x = range[0]; x <= range[2]; ++x)
                    refs[cursor[std::size_t(y * Width + x)]++] = UInt16(i);
        }
    }
};

struct QueryScratch {
    std::vector<Candidate> candidates;
    std::vector<UInt32> selected, stamps;
    std::vector<UInt16> neighbours, gridRefs;
    std::vector<Poly> fragments, remaining;
    std::array<std::byte, 128 * 1024> buffer;
    std::pmr::monotonic_buffer_resource memory{buffer.data(), buffer.size()};

    void clear() {
        opent1::detail::clearScratch(candidates);
        opent1::detail::clearScratch(selected);
        opent1::detail::clearScratch(stamps);
        opent1::detail::clearScratch(neighbours);
        opent1::detail::clearScratch(gridRefs);
        opent1::detail::clearScratch(fragments);
        opent1::detail::clearScratch(remaining);
        memory.release();
    }
};
}

struct ShadowReceiverCache::State {
    struct Entry {
        std::weak_ptr<const void> source;
        std::shared_ptr<const Index> index;
    };

    std::vector<Entry> entries;
    std::size_t bytes{};
};

ShadowReceiverCache::ShadowReceiverCache() : state_(std::make_unique<State>()) {}

ShadowReceiverCache::~ShadowReceiverCache() = default;

void ShadowReceiverCache::clear() {
    state_->entries.clear();
    state_->bytes = 0;
}

struct ShadowReceiverScene::State {
    struct Instance {
        WorldSurfaceInstance source;
        std::shared_ptr<const Index> index;
    };

    std::vector<Instance> instances;
    bool valid{true};
};

ShadowReceiverScene::ShadowReceiverScene(std::span<const WorldSurfaceInstance> sources,
                                         ShadowReceiverCache* cache)
    : state_(std::make_unique<State>()) {
    gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::shadowIndexMs);
    if (sources.size() > 512) {
        state_->valid = false;
        return;
    }
    try {
        for (const auto& source : sources) {
            const auto count = !source.mesh ? 0
                               : source.mesh->interiorMesh
                                   ? source.mesh->interiorMesh->surfaceIds.size()
                                   : source.mesh->triangleCount();
            if (!source.mesh || !rigid(source.placement) || count > 1'048'576)
                throw std::invalid_argument("invalid shadow instance");
            std::shared_ptr<const void> identity;
            if (source.mesh->interiorMesh)
                identity = source.mesh->interiorMesh;
            else if (source.mesh->terrainPatches)
                identity = source.mesh->terrainPatches;
            else
                identity = source.mesh;
            std::shared_ptr<const Index> index;
            if (cache && identity) {
                for (auto& entry : cache->state_->entries)
                    if (entry.source.lock() == identity) {
                        index = entry.index;
                        break;
                    }
            }
            if (index) {
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowIndexHits;
            } else {
                index = makeIndex(*source.mesh);
                if (cache && identity && index->bytes() <= 16 * 1024 * 1024) {
                    auto& storage = *cache->state_;
                    std::erase_if(storage.entries, [&](const auto& entry) {
                        if (!entry.source.expired())
                            return false;
                        storage.bytes -= entry.index->bytes();
                        return true;
                    });
                    while (!storage.entries.empty() &&
                           (storage.entries.size() >= 64 ||
                            storage.bytes + index->bytes() > 16 * 1024 * 1024)) {
                        storage.bytes -= storage.entries.front().index->bytes();
                        storage.entries.erase(storage.entries.begin());
                    }
                    storage.entries.push_back({identity, index});
                    storage.bytes += index->bytes();
                }
            }
            state_->instances.push_back({source, std::move(index)});
        }
    } catch (const std::invalid_argument&) {
        state_->valid = false;
    }
}

ShadowReceiverScene::~ShadowReceiverScene() = default;

std::optional<std::vector<sim::ShadowReceiver>> ShadowReceiverScene::collect(
    const sim::ShadowSourceWindow& w, float length, Point3F origin, std::size_t* sharedWork) const {
    gfx::ScopedRenderWorkTimer timer(&gfx::RenderWorkStats::shadowReceiversMs);
    if (!state_->valid || !std::isfinite(length) || length <= 0 || length > 65536 ||
        !finite(origin) || !finite(w.direction) || !finite(w.right) || !finite(w.up) ||
        std::abs(w.direction.len() - 1) > .0001f || std::abs(w.right.len() - 1) > .0001f ||
        std::abs(w.up.len() - 1) > .0001f || std::abs(m_dot(w.right, w.up)) > .0001f ||
        std::abs(m_dot(w.right, w.direction)) > .0001f ||
        std::abs(m_dot(w.up, w.direction)) > .0001f || !std::ranges::all_of(w.points, [](auto p) {
            return finite(p);
        }))
        return {};
    std::size_t work = sharedWork ? std::min(*sharedWork, MaxWork) : MaxWork;

    struct Accounting {
        std::size_t& remaining;
        std::size_t initial;
        std::size_t* shared;

        ~Accounting() {
            const auto used = initial - remaining;
            if (shared)
                *shared -= used;
            if (gfx::currentRenderWorkStats)
                gfx::currentRenderWorkStats->shadowWork += used;
        }
    } accounting{work, work, sharedWork};

    try {
        const auto xy = [&](Point3F p) {
            const auto x = double(p.x) - w.points[0].x, y = double(p.y) - w.points[0].y,
                       z = double(p.z) - w.points[0].z;
            return XY{x * w.right.x + y * w.right.y + z * w.right.z,
                      x * w.up.x + y * w.up.y + z * w.up.z};
        };
        Poly window;
        for (auto p : w.points)
            window.add(xy(p));
        if (std::abs(area(window)) < 1e-10 || std::ranges::any_of(w.points, [&](auto p) {
                return std::abs(m_dot(p - w.points[0], w.direction)) > .0001f;
            }))
            return {};
        const auto windowEdges = edges(window);
        const Bounds windowBounds(window);
        for (const auto& edge : windowEdges)
            for (auto p : window)
                if (edge.at(p) < -1e-8)
                    return {};
        opent1::detail::ScratchLease<QueryScratch> lease;
        auto& scratch = lease.get();
        auto& candidates = scratch.candidates;
        auto& selected = scratch.selected;
        candidates.reserve(32);
        selected.reserve(64);
        for (const auto& instance : state_->instances) {
            auto placement = instance.source.placement;
            placement.p -= origin;
            placement.flags |= RMat3F::Matrix_HasTranslation;
            auto inverse = placement;
            inverse.inverse();
            const auto first = point(w.points[0], inverse);
            Box3F query{first, first};
            for (auto p : w.points)
                for (auto end : {p, p + w.direction * length}) {
                    const auto local = point(end, inverse);
                    query.fMin.setMin(local);
                    query.fMax.setMax(local);
                }
            query.fMin -= .01f;
            query.fMax += .01f;
            selected.clear();
            if (!instance.index->nodes.empty())
                instance.index->query(0, query, selected, work);
            // BVH traversal cannot change first-input coplanar tie ownership.
            std::ranges::sort(selected);
            const auto& mesh = *instance.source.mesh;
            const auto collect = [&](const std::array<Point3F, 3>& local, UInt32 material) {
                spend(work);
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowTriangleTests;
                if (material >= mesh.materials.list().materials.size())
                    throw std::invalid_argument("invalid shadow material");
                const auto& m = mesh.materials.list().materials[material];
                if (!m.type() || (m.flags & 0x2000U))
                    return;
                // Keep the affine transform and plane in double precision.
                // Casting each transformed vertex to float first gives adjacent
                // triangles different planes on rotated flat receivers.
                std::array<std::array<double, 3>, 3> points;
                for (std::size_t i = 0; i < 3; ++i) {
                    if (!finite(local[i]))
                        throw std::invalid_argument("invalid shadow point");
                    for (std::size_t j = 0; j < 3; ++j) {
                        points[i][j] = double(local[i].x) * placement.m[0][j] +
                                       double(local[i].y) * placement.m[1][j] +
                                       double(local[i].z) * placement.m[2][j] + placement.p[j];
                        if (!std::isfinite(points[i][j]) || std::abs(points[i][j])>1e12)
                            throw std::invalid_argument("invalid shadow point");
                    }
                }
                std::array<double, 3> u, v, n;
                for (std::size_t j = 0; j < 3; ++j) {
                    u[j] = points[1][j] - points[2][j];
                    v[j] = points[0][j] - points[2][j];
                }
                n = {u[1] * v[2] - u[2] * v[1],
                     u[2] * v[0] - u[0] * v[2],
                     u[0] * v[1] - u[1] * v[0]};
                const auto normalLength = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (!std::isfinite(normalLength))
                    throw std::invalid_argument("invalid shadow plane");
                if (normalLength < 1e-10)
                    return;
                for (auto& value : n)
                    value /= normalLength;
                const auto dot = [&](Point3F axis) {
                    return n[0] * axis.x + n[1] * axis.y + n[2] * axis.z;
                };
                const auto planeDot = dot(w.direction);
                if (planeDot >= -.01)
                    return;
                const auto constant =
                    n[0] * points[0][0] + n[1] * points[0][1] + n[2] * points[0][2];
                const Half depth{-dot(w.right) / planeDot,
                                 -dot(w.up) / planeDot,
                                 (n[0] * (points[0][0] - w.points[0].x) +
                                  n[1] * (points[0][1] - w.points[0].y) +
                                  n[2] * (points[0][2] - w.points[0].z)) /
                                     planeDot};
                const Point3F normal{float(n[0]), float(n[1]), float(n[2])};
                Poly polygon;
                for (const auto& p : points) {
                    const auto x = p[0] - w.points[0].x, y = p[1] - w.points[0].y,
                               z = p[2] - w.points[0].z;
                    polygon.add({x * w.right.x + y * w.right.y + z * w.right.z,
                                 x * w.up.x + y * w.up.y + z * w.up.z});
                }
                if (!Bounds(polygon).overlaps(windowBounds))
                    return;
                for (auto edge : windowEdges) {
                    polygon = clip(polygon, edge, work);
                    if (polygon.empty())
                        return;
                }
                polygon = clip(polygon, depth, work);
                polygon = clip(polygon, {-depth.x, -depth.y, length - depth.c}, work);
                if (polygon.empty())
                    return;
                if (candidates.size() == MaxCandidates)
                    throw std::length_error("shadow candidates");
                candidates.push_back({polygon, Bounds(polygon), depth, normal, float(constant)});
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowCandidates;
            };
            const auto explicitTriangle = [&](const StaticSurfaceTriangle& t) {
                collect({t.vertices[0].position, t.vertices[1].position, t.vertices[2].position},
                        t.material);
            };
            for (auto id : selected) {
                if (mesh.interiorMesh) {
                    if (mesh.interiorSelection &&
                        !std::ranges::binary_search(*mesh.interiorSelection, id))
                        continue;
                    const auto& m = *mesh.interiorMesh;
                    const auto range = m.ranges[id];
                    if (!m.hardware || range[0] > m.hardware->indices().size() ||
                        range[1] > m.hardware->indices().size() - range[0] || range[1] % 3)
                        throw std::invalid_argument("invalid shadow interior indices");
                    for (UInt32 t = range[0]; t < range[0] + range[1]; t += 3) {
                        std::array<Point3F, 3> points;
                        for (std::size_t j = 0; j < 3; ++j) {
                            const auto index = m.hardware->indices()[t + j];
                            if (index >= m.hardware->vertices().size())
                                throw std::invalid_argument("invalid shadow vertex");
                            const auto& v = m.hardware->vertices()[index];
                            points[j] = {v.position.x, v.position.y, v.inverseDepth};
                        }
                        collect(points, m.source->surfaces[id].material);
                    }
                } else if (mesh.terrainPatches) {
                    const auto& patches = mesh.terrainPatches->patches;
                    const auto end = id + 1 < patches.size() ? patches[id + 1].firstTriangle
                                                             : mesh.terrainPatches->triangles;
                    for (auto t = patches[id].firstTriangle; t < end; ++t)
                        explicitTriangle(mesh.terrainTriangle(t));
                } else
                    explicitTriangle(mesh.triangles[id]);
            }
        }
        coalesce(candidates, work, &scratch.memory);
        CandidateGrid grid(candidates, window, scratch.gridRefs);
        auto& neighbours = scratch.neighbours;
        auto& stamps = scratch.stamps;
        stamps.assign(candidates.size(), 0);
        auto& fragments = scratch.fragments;
        auto& remaining = scratch.remaining;
        fragments.reserve(16);
        remaining.reserve(16);
        neighbours.reserve(candidates.size());
        std::vector<sim::ShadowReceiver> result;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& c = candidates[i];
            neighbours.clear();
            const auto cells = grid.cells(c.bounds);
            for (int y = cells[1]; y <= cells[3]; ++y)
                for (int x = cells[0]; x <= cells[2]; ++x) {
                    const auto cell = std::size_t(y * CandidateGrid::Width + x);
                    for (auto at = grid.offsets[cell]; at < grid.offsets[cell + 1]; ++at) {
                        spend(work);
                        const auto j = grid.refs[at];
                        if (j == i || stamps[j] == i + 1)
                            continue;
                        stamps[j] = UInt32(i + 1);
                        if (c.bounds.overlaps(candidates[j].bounds))
                            neighbours.push_back(j);
                    }
                }
            std::ranges::sort(neighbours);
            fragments.clear();
            fragments.push_back(c.polygon);
            const auto cEdges = edges(c.polygon);
            for (auto j : neighbours) {
                if (fragments.empty())
                    break;
                spend(work);
                if (gfx::currentRenderWorkStats)
                    ++gfx::currentRenderWorkStats->shadowPairTests;
                const auto& occluder = candidates[j];
                auto cuts = edges(occluder.polygon);
                const Half depth{c.depth.x - occluder.depth.x,
                                 c.depth.y - occluder.depth.y,
                                 c.depth.c - occluder.depth.c};
                const bool coplanar =
                    std::abs(depth.x) + std::abs(depth.y) + std::abs(depth.c) < 1e-8;
                if (coplanar) {
                    if (j > i)
                        continue;
                } else {
                    if (std::ranges::all_of(c.polygon, [&](auto p) {
                            return depth.at(p) <= 0;
                        }))
                        continue;
                    cuts.values[cuts.count++] = depth;
                }
                // Shared edges and disjoint triangles often have overlapping
                // boxes. Reject them before creating any clipped fragments.
                bool separate = false;
                for (auto edge : cEdges)
                    if (std::ranges::all_of(occluder.polygon, [&](auto p) {
                            return edge.at(p) <= 0;
                        })) {
                        separate = true;
                        break;
                    }
                if (separate)
                    continue;
                remaining.clear();
                for (const auto& input : fragments) {
                    auto hidden = input;
                    for (auto cut : cuts) {
                        hidden = clip(hidden, cut, work);
                        if (hidden.empty())
                            break;
                    }
                    if (hidden.empty()) {
                        remaining.push_back(input);
                        continue;
                    }
                    auto fragment = input;
                    for (auto cut : cuts) {
                        auto outside = clip(fragment, {-cut.x, -cut.y, -cut.c}, work);
                        if (!outside.empty())
                            remaining.push_back(std::move(outside));
                        fragment = clip(fragment, cut, work);
                        if (fragment.empty())
                            break;
                    }
                    if (remaining.size() > MaxFragments)
                        throw std::length_error("shadow fragments");
                }
                fragments.swap(remaining);
            }
            for (const auto& fragment : fragments) {
                if (result.size() == 100)
                    throw std::length_error("shadow receivers");
                sim::ShadowReceiver receiver{c.normal, c.constant, {}};
                receiver.vertices.reserve(fragment.count);
                for (auto p : fragment)
                    receiver.vertices.push_back(w.points[0] + w.right * float(p.x) +
                                                w.up * float(p.y) +
                                                w.direction * float(c.depth.at(p)));
                result.push_back(std::move(receiver));
            }
        }
        return result;
    } catch (const std::invalid_argument&) {
        return {};
    } catch (const std::length_error&) {
        if (gfx::currentRenderWorkStats)
            ++gfx::currentRenderWorkStats->shadowBudgetExhaustions;
        return {};
    }
}

std::optional<sim::ShadowGeometry>
ShadowReceiverScene::geometry(const sim::ShadowSourceWindow& window,
                              Point3F direction,
                              float length,
                              float lift,
                              Point3F origin,
                              std::size_t* work) const {
    const auto receivers = collect(window, length, origin, work);
    if (!receivers)
        return {};
    try {
        const auto geometry = sim::buildShadowGeometry(window, *receivers, direction, lift);
        sim::ShadowGeometry result;
        result.maxPlaneDot = geometry.maxPlaneDot;
        for (const auto& p : geometry.polygons)
            for (std::size_t i = 1; i + 1 < p.vertices.size(); ++i) {
                Point3F normal;
                m_normal(p.vertices[0].position,
                         p.vertices[i].position,
                         p.vertices[i + 1].position,
                         normal);
                if (normal.len() < 1e-8f)
                    continue;
                if (result.polygons.size() == 100)
                    return {};
                result.polygons.push_back(
                    {p.receiverIndex, {p.vertices[0], p.vertices[i], p.vertices[i + 1]}});
            }
        return result;
    } catch (const std::invalid_argument&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
}
} // namespace opent1::world
