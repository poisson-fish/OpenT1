#include <opent1/world/ts_collision.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace opent1::world {
namespace {
constexpr std::size_t MaxWork = 4'194'304, MaxSurfaces = 4096;

bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) <= 1e12f;
}

bool rigid(const TMat3F& t, Flt32 tolerance = .0001f) {
    if (!finite(t.p)) {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const Point3F a{t.m[i][0], t.m[i][1], t.m[i][2]};
        if (!finite(a) || std::abs(m_dot(a, a) - 1) > tolerance) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (std::abs(m_dot(a, Point3F{t.m[j][0], t.m[j][1], t.m[j][2]})) > tolerance) {
                return false;
            }
        }
    }
    return std::abs(t.determinant() - 1) < 3 * tolerance;
}

bool valid(const Box3F& box) {
    return finite(box.fMin) && finite(box.fMax) && box.fMin.x <= box.fMax.x &&
           box.fMin.y <= box.fMax.y && box.fMin.z <= box.fMax.z;
}

void check(Point3F a, Point3F b, Flt32 radius = 0) {
    if (!finite(a) || !finite(b) || !std::isfinite(radius) || radius < 0 || radius > 1e12f) {
        throw std::invalid_argument("invalid TS collision primitive");
    }
}

void append(CollisionSurfaceList& list, CollisionSurface surface) {
    if (list.surfaces.size() == MaxSurfaces) {
        throw std::length_error("TS collision surface limit");
    }
    list.surfaces.push_back(surface);
}

bool publish(CollisionSurfaceList& output, CollisionSurfaceList result) {
    if (result.surfaces.empty()) {
        return false;
    }
    // Preserve the image dispatcher's coordinate metadata.
    output.surfaces = std::move(result.surfaces);
    return true;
}

Point3F point(Point3F p, const TMat3F& t) {
    Point3F result;
    m_mul(p, t, result);
    return result;
}

void toShape(CollisionSurface& s, const TMat3F& t) {
    s.position = point(s.position, t);
    m_mul(s.normal, static_cast<const RMat3F&>(t), s.normal);
}

// Original TS edge/cylinder policy, including its polynomial square-root bound.
bool edgeInTube(Point3F r1,
                Point3F r2,
                Flt32 t1,
                Flt32 t2,
                Flt32 radius,
                Flt32 invLength,
                Flt32& time,
                Point3F& hit) {
    if (t1 > t2) {
        std::swap(t1, t2);
        std::swap(r1, r2);
    }
    Flt32 lo = 0, hi = 1;
    if (t1 < 0) {
        if (t2 < 0) {
            return false;
        }
        lo = -t1 / (t2 - t1);
    }
    if (invLength * t2 > time) {
        if (invLength * t1 > time) {
            return false;
        }
        hi = (time - t1 * invLength) / (invLength * (t2 - t1));
    }
    const auto edge = r2 - r1;
    const auto length2 = m_dot(edge, edge);
    if (length2 == 0) {
        if (m_dot(r1, r1) >= radius * radius) {
            return false;
        }
    } else {
        auto projection = -m_dot(r1, edge);
        auto distance2 = length2 * m_dot(r1, r1) - projection * projection;
        if (distance2 >= length2 * radius * radius) {
            return false;
        }
        const auto inv = 1 / std::sqrt(length2);
        projection *= inv;
        distance2 *= inv;
        distance2 *= inv;
        const auto x = 1 - distance2 / (radius * radius);
        const auto bound = Flt32(-.699 * radius * (x + .07978539f) * (x - 2.277210289f));
        const auto low = inv * (projection - bound), high = inv * (projection + bound);
        if (high < lo || low > hi) {
            return false;
        }
        lo = std::max(lo, low);
    }
    time = (t1 + lo * (t2 - t1)) * invLength;
    hit = r1 + (r2 - r1) * lo;
    return true;
}
}

std::shared_ptr<const TsShapeCollision> TsShapeCollision::shared(const TsShapePose& pose,
                                                                 Int32 detail) {
    if (!pose.data_)
        return {};
    auto& cache = pose.data_->collisions;
    for (const auto& [cachedDetail, collision] : cache) {
        if (cachedDetail == detail)
            return collision;
    }
    auto built = prepare(pose, detail);
    if (!built)
        return {};
    auto result = std::make_shared<const TsShapeCollision>(std::move(*built));
    if (cache.size() == 2)
        cache.erase(cache.begin());
    cache.emplace_back(detail, result);
    return result;
}

std::optional<TsShapeCollision> TsShapeCollision::prepare(const TsShapePose& pose, Int32 detail) {
    const auto resource = pose.shape();
    if (!resource) {
        return {};
    }
    const auto& shape = *resource;
    if (detail < 0 || std::size_t(detail) >= shape.details.size() || shape.nodes.size() > 65536 ||
        shape.objects.size() > 65536 || pose.nodeTransforms().size() != shape.nodes.size() ||
        pose.nodeVisibility().size() != shape.nodes.size() ||
        pose.objects().size() != shape.objects.size()) {
        return {};
    }
    const auto root = shape.details[std::size_t(detail)].root;
    if (root < 0 || std::size_t(root) >= shape.nodes.size()) {
        return {};
    }
    std::vector<UInt8> selected(shape.nodes.size());
    for (std::size_t i = 0; i < shape.nodes.size(); ++i) {
        const auto parent = shape.nodes[i].parent;
        if (parent < -1 || (parent >= 0 && std::size_t(parent) >= i)) {
            return {};
        }
        selected[i] = UInt8(Int32(i) == root || (parent >= 0 && selected[std::size_t(parent)]));
    }
    TsShapeCollision result;
    result.center_ = shape.center;
    result.authoredBounds_ = shape.bounds;
    std::size_t remaining = MaxWork;
    for (std::size_t i = 0; i < shape.objects.size(); ++i) {
        const auto& object = shape.objects[i];
        const auto& state = pose.objects()[i];
        if (object.node < 0 || std::size_t(object.node) >= selected.size() || object.mesh < 0 ||
            std::size_t(object.mesh) >= shape.meshes.size()) {
            return {};
        }
        if (!selected[std::size_t(object.node)] ||
            !pose.nodeVisibility()[std::size_t(object.node)] || !state.visible) {
            continue;
        }
        const auto& mesh = shape.meshes[std::size_t(object.mesh)];
        if (mesh.faces.size() > remaining / 3 || mesh.verticesPerFrame < 2) {
            return {};
        }
        remaining -= mesh.faces.size() * 3;
        Part part;
        part.id = Int32(i);
        part.transform = pose.nodeTransforms()[std::size_t(object.node)];
        // Packed 16-bit authored quaternions have small length error. TS treats
        // these node bases as rotations and uses their transpose for inversion.
        if (!finite(object.offset) || !rigid(part.transform, .001f)) {
            return {};
        }
        part.transform.p = point(object.offset, part.transform);
        part.transform.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        part.inverse = part.transform;
        part.inverse.inverse();
        const auto min = mesh.point(0, state.frame), max = mesh.point(1, state.frame);
        if (!min || !max) {
            return {};
        }
        part.box.fMin = *min;
        part.box.fMax = *max;
        if (!valid(part.box)) {
            return {};
        }
        Box3F box;
        m_mul(part.box, part.transform, box);
        if (!valid(box)) {
            return {};
        }
        if (!result.bounds_) {
            result.bounds_ = box;
        } else {
            result.bounds_->fMin.setMin(box.fMin);
            result.bounds_->fMax.setMax(box.fMax);
        }
        for (std::size_t f = 0; f < mesh.faces.size(); ++f) {
            const auto& face = mesh.faces[f];
            Triangle triangle;
            triangle.material = face.material;
            triangle.surface = Int32(f);
            if (face.material < -1) {
                return {};
            }
            for (std::size_t k = 0; k < 3; ++k) {
                const auto p = mesh.point(face.corners[k].vertex, state.frame);
                if (!p || !finite(*p)) {
                    return {};
                }
                triangle.vertices[k] = *p;
            }
            part.triangles.push_back(triangle);
        }
        result.parts_.push_back(std::move(part));
    }
    return result;
}

bool TsShapeCollision::line(Point3F start,
                            Point3F end,
                            CollisionSurfaceList& output,
                            TsCollisionLevel level) const {
    check(start, end);
    CollisionSurfaceList result;
    CollisionSurface best;
    best.time = 1.1f;
    bool found = false;
    for (const auto& part : parts_) {
        const auto a = point(start, part.inverse), b = point(end, part.inverse);
        Flt32 minTime{};
        if (!m_lineAABox(a, b, part.box.fMin, part.box.fMax, minTime) || minTime > best.time) {
            continue;
        }
        if (level == TsCollisionLevel::Parts) {
            if (!found || minTime < best.time) {
                best = {};
                best.part = part.id;
                best.time = minTime;
                best.position = start + (end - start) * minTime;
                found = true;
            }
            continue;
        }
        const auto direction = b - a;
        for (const auto& face : part.triangles) {
            const auto& v = face.vertices;
            Point3F normal;
            m_normal(v[0], v[1], v[2], normal);
            const auto denom = m_dot(normal, direction);
            if (denom >= 0) {
                continue;
            }
            const auto numerator = m_dot(normal, v[2]) - m_dot(normal, a);
            if (numerator >= .9f * minTime * denom || numerator < best.time * denom) {
                continue;
            }
            const auto time = numerator / denom;
            if (time < 0 || time > 1) {
                continue;
            }
            if (!m_pointInTriangle(a + direction * time, normal, v[0], v[1], v[2])) {
                continue;
            }
            const auto length = normal.len();
            if (!std::isfinite(length) || length <= 0) {
                continue;
            }
            best = {};
            best.time = time;
            best.normal = normal / length;
            best.part = part.id;
            best.surface = face.surface;
            best.material = face.material;
            m_mul(best.normal, static_cast<const RMat3F&>(part.transform), best.normal);
            best.normal *= 1 / best.normal.len(); // shape-level TS line normalization
            best.position = start + (end - start) * time;
            found = true;
        }
    }
    if (found) {
        append(result, best);
    }
    return publish(output, std::move(result));
}

bool TsShapeCollision::sphere(Point3F center,
                              Flt32 radius,
                              CollisionSurfaceList& output,
                              TsCollisionLevel level) const {
    check(center, center, radius);
    CollisionSurfaceList result;
    for (const auto& part : parts_) {
        const auto c = point(center, part.inverse);
        Flt32 unused{};
        if (!m_sphereAABox(c, radius * radius, part.box.fMin, part.box.fMax, unused)) {
            continue;
        }
        if (level == TsCollisionLevel::Parts) {
            const auto boxCenter = (part.box.fMin + part.box.fMax) * .5f,
                       radii = (part.box.fMax - part.box.fMin) * .5f, offset = c - boxCenter;
            for (std::size_t k = 0; k < 3; ++k) {
                if (std::abs(offset[k]) + radius < radii[k]) {
                    continue;
                }
                CollisionSurface s;
                s.part = part.id;
                s.normal[k] = offset[k] < 0 ? -1.f : 1.f;
                s.position = boxCenter + s.normal * radius;
                toShape(s, part.transform);
                s.distance = (s.position - center).len();
                append(result, s);
            }
            continue;
        }
        for (const auto& face : part.triangles) {
            const auto& v = face.vertices;
            Point3F normal;
            m_normal(v[0], v[1], v[2], normal);
            const auto length = normal.len();
            if (!std::isfinite(length) || length == 0) {
                continue;
            }
            normal *= 1 / length;
            const auto distance = m_dot(normal, c) - m_dot(normal, v[0]);
            if (distance < 0 || distance >= radius) {
                continue;
            }
            const auto projected = c - normal * distance;
            auto hit = projected;
            bool found = m_pointInTriangle(projected, normal, v[0], v[1], v[2]);
            if (!found) {
                const auto circle = radius * radius - distance * distance;
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto edge = v[(k + 1) % 3] - v[k];
                    const auto edgeLength = edge.len();
                    if (edgeLength == 0) {
                        continue;
                    }
                    const auto unit = edge / edgeLength;
                    const auto projection = m_dot(projected - v[k], unit);
                    const auto intersection = v[k] + unit * projection;
                    const auto distance2 =
                        m_dot(projected - intersection, projected - intersection);
                    if (distance2 > circle) {
                        continue;
                    }
                    if (projection < 0 && projection * projection > circle - distance2) {
                        continue;
                    }
                    if (projection > edgeLength &&
                        (projection - edgeLength) * (projection - edgeLength) >
                            circle - distance2) {
                        continue;
                    }
                    hit = v[k] + unit * std::clamp(projection, 0.f, edgeLength);
                    found = true;
                    break;
                }
            }
            if (found) {
                CollisionSurface s;
                s.position = hit;
                s.normal = normal;
                s.distance = distance;
                s.part = part.id;
                s.surface = face.surface;
                s.material = face.material;
                toShape(s, part.transform);
                append(result, s);
            }
        }
    }
    return publish(output, std::move(result));
}

bool TsShapeCollision::tube(Point3F start,
                            Point3F end,
                            Flt32 radius,
                            CollisionSurfaceList& output,
                            TsCollisionLevel level) const {
    check(start, end, radius);
    if (radius == 0) {
        return line(start, end, output, level);
    }
    if ((end - start).len() == 0) {
        return sphere(start, radius, output, level);
    }
    CollisionSurfaceList result;
    CollisionSurface best;
    best.time = 1.1f;
    bool found = false;
    for (const auto& part : parts_) {
        const auto a = point(start, part.inverse), b = point(end, part.inverse);
        Flt32 minTime{};
        if (!m_tubeAABox(a, b, radius, part.box.fMin, part.box.fMax, minTime) ||
            minTime > best.time) {
            continue;
        }
        if (level == TsCollisionLevel::Parts) {
            if (!found || minTime < best.time) {
                best = {};
                best.part = part.id;
                best.time = minTime;
                best.position = start + (end - start) * minTime;
                found = true;
            }
            continue;
        }
        const auto length = (b - a).len();
        const auto direction = (b - a) / length;
        for (const auto& face : part.triangles) {
            const auto& v = face.vertices;
            std::array<Point3F, 3> radial;
            std::array<Flt32, 3> along;
            for (std::size_t k = 0; k < 3; ++k) {
                along[k] = m_dot(v[k], direction) - m_dot(a, direction);
                radial[k] = v[k] - a - direction * along[k];
            }
            auto time = std::min(best.time, 1.f);
            Point3F hit;
            bool edgeHit = false;
            for (std::size_t k = 0; k < 3; ++k) {
                const auto n = (k + 1) % 3;
                edgeHit |= edgeInTube(
                    radial[k], radial[n], along[k], along[n], radius, 1 / length, time, hit);
            }
            Point3F normal;
            m_normal(v[0], v[1], v[2], normal);
            bool faceHit = false;
            if (m_pointInTriangle({}, direction, radial[0], radial[1], radial[2])) {
                const auto denom = m_dot(normal, direction) * length;
                const auto numerator = m_dot(normal, v[2]) - m_dot(normal, a);
                if (denom < 0 && numerator > time * denom) {
                    const auto candidate = numerator / denom;
                    if (candidate >= 0 && candidate <= 1) {
                        time = candidate;
                        hit = a + (b - a) * time;
                        faceHit = true;
                    }
                }
            }
            if (!edgeHit && !faceHit) {
                continue;
            }
            if (!faceHit) {
                hit += a + direction * (length * time);
            }
            const auto norm = normal.len();
            if (!std::isfinite(norm) || norm == 0) {
                continue;
            }
            best = {};
            best.time = time;
            best.position = hit;
            best.normal = normal;
            best.part = part.id;
            best.surface = face.surface;
            best.material = face.material;
            toShape(best, part.transform);
            found = true;
        }
    }
    if (found) {
        append(result, best);
    }
    return publish(output, std::move(result));
}

bool TsShapeCollision::box(const Box3F& box,
                           const TMat3F& transform,
                           CollisionSurfaceList& output) const {
    if (!valid(box) || !rigid(transform)) {
        throw std::invalid_argument("invalid TS collision box");
    }
    const auto center = (box.fMin + box.fMax) * .5f, radii = (box.fMax - box.fMin) * .5f;
    CollisionSurfaceList result;
    auto centered = transform;
    centered.p = point(center, transform);
    for (const auto& part : parts_) {
        TMat3F local;
        m_mul(centered, part.inverse, local);
        local.p -= (part.box.fMin + part.box.fMax) * .5f;
        Point3F a, b;
        if (m_OBoxOBox((part.box.fMax - part.box.fMin) * .5f, radii, local, a, b)) {
            CollisionSurface s;
            s.part = part.id;
            append(result, s);
        }
    }
    return publish(output, std::move(result));
}

bool TsShapeCollision::shape(const TsShapeCollision& other,
                             const TMat3F& transform,
                             CollisionSurfaceList& output) const {
    if (!rigid(transform)) {
        throw std::invalid_argument("invalid TS pair transform");
    }
    if (parts_.size() && other.parts_.size() > MaxWork / parts_.size()) {
        throw std::length_error("TS collision pair limit");
    }
    CollisionSurfaceList result;
    for (const auto& part : parts_) {
        for (const auto& peer : other.parts_) {
            TMat3F toThis, local;
            m_mul(peer.transform, transform, toThis);
            toThis.p = point((peer.box.fMin + peer.box.fMax) * .5f, toThis);
            m_mul(toThis, part.inverse, local);
            local.p -= (part.box.fMin + part.box.fMax) * .5f;
            Point3F a, b;
            if (m_OBoxOBox((part.box.fMax - part.box.fMin) * .5f,
                           (peer.box.fMax - peer.box.fMin) * .5f,
                           local,
                           a,
                           b)) {
                CollisionSurface s;
                s.part = part.id;
                s.surface = peer.id;
                append(result, s);
            }
        }
    }
    return publish(output, std::move(result));
}
} // namespace opent1::world
