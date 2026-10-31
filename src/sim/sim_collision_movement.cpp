#include <opent1/sim/sim_collision_movement.hpp>
#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace opent1::sim {
namespace {
constexpr auto axes() {
    return std::views::iota(std::size_t{0}, std::size_t{3});
}

bool finite(const Point3F& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool bounded(const Point3F& p) {
    return finite(p) && std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) <= 1e12f;
}

bool valid(const Box3F& b) {
    return finite(b.fMin) && finite(b.fMax) && b.isValid();
}

bool nonnegative(float f) {
    return std::isfinite(f) && f >= 0 && f <= 1e12f;
}

bool rigid(const RMat3F& m) {
    if ((m.flags & RMat3F::Matrix_HasScale) != 0) {
        return false;
    }
    if (!(m.flags & RMat3F::Matrix_HasRotation) && !std::ranges::all_of(axes(), [&](auto i) {
            return std::ranges::all_of(axes(), [&](auto j) {
                return std::abs(m.m[i][j] - (i == j ? 1.f : 0.f)) < 1e-6f;
            });
        })) {
        return false;
    }
    return std::ranges::all_of(axes(), [&](auto i) {
        Point3F a;
        m.getRow(i, a);
        return finite(a) && std::abs(m_dot(a, a) - 1.f) < .001f &&
               std::ranges::all_of(
                   axes(),
                   [&](auto j) {
                       Point3F b;
                       m.getRow(j, b);
                       return i == j || std::abs(m_dot(a, b)) < .001f;
                   });
    });
}

bool valid(const TMat3F& m) {
    return bounded(m.p) && rigid(m) && (m.p.isZero() || (m.flags & RMat3F::Matrix_HasTranslation));
}

Point3F unit(Point3F p, Point3F fallback = {0, 0, 1}) {
    const auto length = p.len();
    return length > 1e-12f ? p * (1.f / length) : fallback;
}

bool contains(const Box3F& a, const Box3F& b) {
    return std::ranges::all_of(axes(), [&](auto i) {
        return a.fMin[i] <= b.fMin[i] && a.fMax[i] >= b.fMax[i];
    });
}

template <class F> bool dispatched(SimObject& object, F&& f) {
    bool result{};
    if (auto* manager = object.getManager()) {
        manager->dispatch([&] {
            result = f();
        });
    } else {
        result = f();
    }
    return result;
}

struct FlagGuard {
    bool& flag;

    explicit FlagGuard(bool& f) : flag(f) {
        if (f) {
            throw std::logic_error("recursive spatial transition");
        }
        f = true;
    }

    ~FlagGuard() {
        flag = false;
    }
};

bool live(const SimObject& o) {
    return !o.isDeleted() && !o.isRemoved();
}

bool same(const SimAddress& a, const SimAddress& b) {
    return a.objectId == b.objectId && a.generation == b.generation;
}

std::vector<std::shared_ptr<SimCollisionImage>>
images(SimObject& object,
       SimCollisionImageQuery::Detail detail = SimCollisionImageQuery::Normal,
       std::optional<Box3F> bounds = {}) {
    SimCollisionImageQuery query;
    query.detail = detail;
    query.bounds = bounds;
    const auto accepted = object.getManager()
                              ? object.getManager()->sendQuery(object.address(), query)
                              : object.processQuery(query);
    if (!accepted || !live(object)) {
        return {};
    }
    if (query.images.size() > 20 || std::ranges::any_of(query.images, [](const auto& p) {
            return !p;
        })) {
        throw std::invalid_argument("invalid collision image query");
    }
    return query.images;
}

void checkSphere(const Point3F& center, float radius) {
    if (!bounded(center) || !nonnegative(radius)) {
        throw std::invalid_argument("invalid collision sphere");
    }
}

Point3F faceNormal(const Point3F& p, const Box3F& box) {
    float distance = std::numeric_limits<float>::max();
    Point3F normal;
    std::ranges::for_each(axes(), [&](auto i) {
        std::ranges::for_each(std::array{-1.f, 1.f}, [&](float side) {
            const auto d = std::abs(p[i] - (side < 0 ? box.fMin[i] : box.fMax[i]));
            if (d < distance) {
                distance = d;
                normal = {};
                normal[i] = side;
            }
        });
    });
    return normal;
}

// A finite swept sphere against a sphere. The primitive line variant retains
// legacy distance/normal fields for ordinary entry hits, but rejects past-end hits.
bool raySphere(const Point3F& start,
               const Point3F& end,
               const Point3F& center,
               float radius,
               CollisionSurface& s) {
    const auto delta = end - start;
    const auto lengthSq = m_dot(delta, delta);
    if (lengthSq < .01f) {
        return false;
    }
    const auto direction = delta * (1.f / std::sqrt(lengthSq));
    const auto offset = center - start;
    const auto v = m_dot(offset, direction);
    const auto discriminant = radius * radius - (m_dot(offset, offset) - v * v);
    if (discriminant < 0) {
        return false;
    }
    const auto distance = v - std::sqrt(discriminant);
    if (distance < 0 || distance > std::sqrt(lengthSq)) {
        return false;
    }
    s.position = direction * distance + start;
    s.distance = m_distf(s.position, start);
    s.normal = s.position - center;
    return true;
}
}

TMat3F SimCollisionImage::buildImageTransform(const SimCollisionImage& other) const {
    auto inverse = transform;
    inverse.inverse();
    TMat3F result;
    m_mul(other.transform, inverse, result);
    return result;
}

bool SimCollisionImage::test(const SimCollisionImage& moving,
                             const SimCollisionImage& other,
                             CollisionSurfaceList& output) {
    if (!valid(moving.transform) || !valid(other.transform)) {
        throw std::invalid_argument("collision needs rigid finite transforms");
    }
    CollisionSurfaceList result;
    const auto& primary = moving.getPriority() >= other.getPriority() ? moving : other;
    const auto& secondary = &primary == &moving ? other : moving;
    result.tWorld = primary.transform;
    if (&primary != &moving) {
        result.tLocal = moving.buildImageTransform(other);
    }
    if (!primary.collide(secondary, result)) {
        return false;
    }
    const auto segment = [](const auto& image) {
        return image.getType() == Line || image.getType() == Tube;
    };
    if ((result.surfaces.empty() && (!result.contained || segment(moving) || segment(other))) ||
        result.surfaces.size() > 4096 || !bounded(result.stepVector) || !valid(result.tWorld) ||
        !valid(result.tLocal) || std::ranges::any_of(result.surfaces, [](const auto& s) {
            return !bounded(s.position) || !bounded(s.normal) || !std::isfinite(s.time) ||
                   !std::isfinite(s.distance);
        })) {
        throw std::invalid_argument("invalid collision backend result");
    }
    output = std::move(result);
    return true;
}

bool SimCollisionSphereImage::collide(const SimCollisionImage& image,
                                      CollisionSurfaceList& list) const {
    checkSphere(center, radius);
    const auto* sphere = dynamic_cast<const SimCollisionSphereImage*>(&image);
    if (!sphere) {
        return false;
    }
    checkSphere(sphere->center, sphere->radius);
    Point3F other;
    m_mul(sphere->center, buildImageTransform(image), other);
    const auto offset = center - other;
    const auto distanceSq = m_dot(offset, offset);
    const auto sum = radius + sphere->radius;
    if (distanceSq > sum * sum) {
        return false;
    }
    CollisionSurface s;
    s.position = center;
    s.distance = std::sqrt(distanceSq);
    s.normal = s.distance < .001f ? Point3F{0, 0, 1} : offset * (1.f / s.distance);
    list.surfaces.push_back(s);
    return true;
}

bool SimCollisionBoxImage::collide(const SimCollisionImage& image,
                                   CollisionSurfaceList& list) const {
    if (!valid(box) || !bounded(box.fMin) || !bounded(box.fMax)) {
        throw std::invalid_argument("invalid collision box");
    }
    const auto mat = buildImageTransform(image);
    if (const auto* sphere = dynamic_cast<const SimCollisionSphereImage*>(&image)) {
        checkSphere(sphere->center, sphere->radius);
        Point3F center;
        m_mul(sphere->center, mat, center);
        Point3F closest;
        std::ranges::for_each(axes(), [&](auto i) {
            closest[i] = std::clamp(center[i], box.fMin[i], box.fMax[i]);
        });
        const auto offset = center - closest;
        const auto distanceSq = m_dot(offset, offset);
        if (distanceSq >= sphere->radius * sphere->radius) {
            return false;
        }
        CollisionSurface s;
        s.position = closest;
        s.distance = std::sqrt(distanceSq);
        s.normal = distanceSq > 1e-12f ? unit(offset) : faceNormal(center, box);
        list.surfaces.push_back(s);
        return true;
    }
    // The original Box/Box branch was unimplemented. SAT supplies a finite
    // contact normal along the least-penetrating face or edge axis.
    const auto* other = dynamic_cast<const SimCollisionBoxImage*>(&image);
    if (!other) {
        return false;
    }
    if (!valid(other->box) || !bounded(other->box.fMin) || !bounded(other->box.fMax)) {
        throw std::invalid_argument("invalid collision box");
    }
    const auto aCenter = (box.fMin + box.fMax) * .5f, aRadius = (box.fMax - box.fMin) * .5f;
    const auto bRadius = (other->box.fMax - other->box.fMin) * .5f;
    Point3F bCenter;
    m_mul((other->box.fMin + other->box.fMax) * .5f, mat, bCenter);
    std::array<Point3F, 3> basisA{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, basisB;
    std::ranges::for_each(axes(), [&](auto i) {
        mat.getRow(i, basisB[i]);
    });
    std::vector<Point3F> tests(basisA.begin(), basisA.end());
    tests.insert(tests.end(), basisB.begin(), basisB.end());
    std::ranges::for_each(basisA, [&](const auto& a) {
        std::ranges::for_each(basisB, [&](const auto& b) {
            Point3F cross;
            m_cross(a, b, cross);
            if (m_dot(cross, cross) > 1e-10f) {
                tests.push_back(unit(cross));
            }
        });
    });
    float best = std::numeric_limits<float>::max();
    Point3F normal;
    if (!std::ranges::all_of(tests, [&](Point3F n) {
            const auto ra =
                std::accumulate(axes().begin(), axes().end(), 0.f, [&](float sum, auto i) {
                    return sum + aRadius[i] * std::abs(n[i]);
                });
            const auto rb =
                std::accumulate(axes().begin(), axes().end(), 0.f, [&](float sum, auto i) {
                    return sum + bRadius[i] * std::abs(m_dot(n, basisB[i]));
                });
            const auto projection = m_dot(aCenter - bCenter, n);
            const auto overlap = ra + rb - std::abs(projection);
            if (overlap < 0) {
                return false;
            }
            if (overlap < best) {
                best = overlap;
                normal = projection < 0 ? -n : n;
            }
            return true;
        })) {
        return false;
    }
    CollisionSurface s;
    s.position = aCenter;
    s.normal = normal;
    s.distance = best;
    list.surfaces.push_back(s);
    return true;
}

bool SimCollisionLineImage::collide(const SimCollisionImage& image,
                                    CollisionSurfaceList& list) const {
    if (!bounded(start) || !bounded(end)) {
        throw std::invalid_argument("invalid collision line");
    }
    const auto mat = buildImageTransform(image);
    CollisionSurface s;
    if (const auto* sphere = dynamic_cast<const SimCollisionSphereImage*>(&image)) {
        checkSphere(sphere->center, sphere->radius);
        Point3F center;
        m_mul(sphere->center, mat, center);
        if (!raySphere(start, end, center, sphere->radius, s)) {
            return false;
        }
    } else if (const auto* box = dynamic_cast<const SimCollisionBoxImage*>(&image)) {
        if (!valid(box->box)) {
            throw std::invalid_argument("invalid collision box");
        }
        auto inverse = mat;
        inverse.inverse();
        Point3F a, b;
        m_mul(start, inverse, a);
        m_mul(end, inverse, b);
        if (!m_lineAABox(a, b, box->box.fMin, box->box.fMax, s.time)) {
            return false;
        }
        s.position = start + (end - start) * s.time;
        s.distance = m_distf(s.position, start);
        Point3F local = a + (b - a) * s.time;
        m_mul(faceNormal(local, box->box), static_cast<const RMat3F&>(mat), s.normal);
    } else {
        return false;
    }
    list.surfaces.push_back(s);
    return true;
}

bool SimCollisionTubeImage::collide(const SimCollisionImage& image,
                                    CollisionSurfaceList& list) const {
    checkSphere(start, radius);
    if (!bounded(end)) {
        throw std::invalid_argument("invalid collision tube");
    }
    const auto mat = buildImageTransform(image);
    CollisionSurface s;
    if (const auto* sphere = dynamic_cast<const SimCollisionSphereImage*>(&image)) {
        checkSphere(sphere->center, sphere->radius);
        Point3F center;
        m_mul(sphere->center, mat, center);
        if (!raySphere(start, end, center, radius + sphere->radius, s)) {
            return false;
        }
        s.normal = unit(s.normal);
        const auto length = (end - start).len();
        s.time = length ? s.distance / length : 0;
    } else if (const auto* box = dynamic_cast<const SimCollisionBoxImage*>(&image)) {
        if (!valid(box->box)) {
            throw std::invalid_argument("invalid collision box");
        }
        auto inverse = mat;
        inverse.inverse();
        Point3F a, b;
        m_mul(start, inverse, a);
        m_mul(end, inverse, b);
        if (!m_tubeAABox(a, b, radius, box->box.fMin, box->box.fMax, s.time)) {
            return false;
        }
        s.position = start + (end - start) * s.time;
        s.distance = (s.position - start).len();
        auto expanded = box->box;
        expanded.fMin -= radius;
        expanded.fMax += radius;
        m_mul(
            faceNormal(a + (b - a) * s.time, expanded), static_cast<const RMat3F&>(mat), s.normal);
    } else {
        return false;
    }
    list.surfaces.push_back(s);
    return true;
}

SimCollisionBackendImage::SimCollisionBackendImage(Type type,
                                                   std::shared_ptr<const Collider> collider)
    : SimCollisionImage(type, type), collider_(std::move(collider)) {
    if (type < Terrain || !collider_ || !*collider_) {
        throw std::invalid_argument("invalid collision backend");
    }
    moveable = type == TSShape;
}

bool SimCollisionBackendImage::collide(const SimCollisionImage& image,
                                       CollisionSurfaceList& list) const {
    auto callback = collider_;
    return (*callback)(image, buildImageTransform(image), list);
}

bool SimContainerQuery::isContained(const SimContainer& object) const {
    return id != object.getId() && (mask & object.getType()) &&
           contains(box, object.getBoundingBox());
}

bool SimContainerDatabase::add(SimContainer& object) {
    if (objects_.size() >= 65536 || std::ranges::find(objects_, &object) != objects_.end()) {
        return false;
    }
    objects_.push_back(&object);
    dirty_ = true;
    return true;
}

void SimContainerDatabase::remove(SimContainer& object) {
    std::erase(objects_, &object);
    dirty_ = true;
}

void SimContainerDatabase::findIntersections(const SimContainerQuery& query,
                                             SimContainerList& result) const {
    if (index_ != Index::Linear && dirty_) {
        sorted_ = objects_;
        std::ranges::stable_sort(sorted_, {}, [](const auto* o) {
            return o->getBoundingBox().fMin.x;
        });
        dirty_ = false;
    }
    const auto& list = index_ == Index::Linear ? objects_ : sorted_;
    const auto end = index_ == Index::Linear
                         ? list.end()
                         : std::ranges::upper_bound(list, query.box.fMax.x, {}, [](const auto* o) {
                               return o->getBoundingBox().fMin.x;
                           });
    std::ranges::for_each(std::ranges::subrange(list.begin(), end), [&](auto* object) {
        if (index_ == Index::XY && (object->getBoundingBox().fMax.y < query.box.fMin.y ||
                                    object->getBoundingBox().fMin.y > query.box.fMax.y)) {
            return;
        }
        if (object->isIntersecting(query.box) &&
            (object->isOpen() ||
             (query.id != object->getId() && (query.mask & object->getType())))) {
            result.push_back(object);
        }
    });
}

void SimContainerDatabase::findContained(const SimContainerQuery& query,
                                         SimContainerList& result) const {
    std::ranges::copy_if(objects_, std::back_inserter(result), [&](auto* o) {
        return query.isContained(*o);
    });
}

SimContainer* SimContainerDatabase::findOpenContainer(const SimContainerQuery& query) const {
    const auto found = std::ranges::find_if(objects_, [&](auto* o) {
        return o->isOpen() && o->isContained(query);
    });
    return found == objects_.end() ? nullptr : *found;
}

SimContainer::SimContainer() {
    setType(ContainerObjectType);
}

SimContainer::~SimContainer() {
    if (container_ && container_->database_) {
        container_->database_->remove(*this);
    }
    if (database_) {
        std::ranges::for_each(database_->objects_, [](auto* child) {
            child->container_ = nullptr;
        });
    }
}

bool SimContainer::installDatabase(std::unique_ptr<SimContainerDatabase> database) {
    if (database_ || !database || changing_ || !database->objects().empty()) {
        return false;
    }
    database_ = std::move(database);
    return true;
}

void SimContainer::detachContents(bool reparent) {
    if (!database_) {
        return;
    }
    const auto children = database_->objects_;
    std::ranges::for_each(children, [&](auto* child) {
        if (child->container_ != this) {
            return;
        }
        removeObject(*child);
        if (reparent && container_ && live(*child)) {
            container_->addObject(*child);
        }
    });
}

void SimContainer::uninstallDatabase() {
    if (changing_) {
        return;
    }
    dispatched(*this, [&] {
        const auto mask = containerMask_;
        containerMask_ = 0;
        try {
            detachContents(true);
        } catch (...) {
            containerMask_ = mask;
            throw;
        }
        database_.reset();
        containerMask_ = mask;
        return true;
    });
}

void SimContainer::onRemove() {
    // Publish a closed container before children are reparented, so they cannot
    // enter it again. Keep the database alive until all links are removed.
    containerMask_ = 0;
    detachContents(true);
    if (container_) {
        container_->removeObject(*this);
    }
    SimNetObject::onRemove();
}

void SimContainer::onDeleteNotify(SimObject& object) {
    if (auto* child = dynamic_cast<SimContainer*>(&object); child && child->container_ == this) {
        removeObject(*child);
    }
    SimObject::onDeleteNotify(object);
}

bool SimContainer::setContainerMask(UInt32 mask) {
    if (changing_) {
        return false;
    }
    return dispatched(*this, [&] {
        containerMask_ = mask;
        detachContents(true);
        return true;
    });
}

bool SimContainer::setBoundingBox(const Box3F& box) {
    if (!valid(box) || changing_) {
        return false;
    }
    // Reject escape from the root without publishing inconsistent coordinates.
    auto* ancestor = container_;
    while (ancestor && !ancestor->isContained(box)) {
        ancestor = ancestor->container_;
    }
    if (container_ && !ancestor) {
        return false;
    }
    return dispatched(*this, [&] {
        box_ = box;
        if (container_) {
            container_->updateObject(*this);
        }
        if (database_) {
            const auto children = database_->objects_;
            std::ranges::for_each(children, [&](auto* o) {
                if (o->container_ == this) {
                    updateObject(*o);
                }
            });
        }
        return live(*this);
    });
}

bool SimContainer::isIntersecting(const Box3F& box) const {
    return box_.overlaps(box);
}

bool SimContainer::isContained(const Box3F& box) const {
    return contains(box_, box);
}

bool SimContainer::isIntersecting(const SimContainerQuery& q) const {
    return q.id != getId() && (q.mask & getType()) && isIntersecting(q.box);
}

bool SimContainer::isContained(const SimContainerQuery& q) const {
    return q.id != getId() && (containerMask_ & q.type) && isContained(q.box);
}

bool SimContainer::addObject(SimContainer& object) {
    if (!database_ || changing_ || object.changing_ || &object == this || !live(*this) ||
        !live(object) || getManager() != object.getManager() || !isContained(object.box_) ||
        !(containerMask_ & object.getType())) {
        return false;
    }
    std::size_t depth = 0;
    for (auto* p = this; p; p = p->container_) {
        if (p == &object || ++depth >= 64) {
            return false;
        }
    }
    // Account for the incoming subtree, not just its root.
    std::vector<std::pair<const SimContainer*, std::size_t>> pending{{&object, depth}};
    while (!pending.empty()) {
        const auto [p, d] = pending.back();
        pending.pop_back();
        if (d >= 64) {
            return false;
        }
        if (p->database_) {
            std::ranges::for_each(p->database_->objects_, [&](auto* c) {
                pending.emplace_back(c, d + 1);
            });
        }
    }
    return dispatched(*this, [&] {
        SimContainerQuery q;
        q.id = object.getId();
        q.type = object.getType();
        q.mask = object.containerMask_;
        q.box = object.box_;
        if (auto* child = database_->findOpenContainer(q); child && child != &object) {
            return child->addObject(object);
        }
        if (object.container_ == this) {
            return true;
        }
        if (database_->objects_.size() >= 65536) {
            return false;
        }
        // Allocate before unlinking the old parent.
        if (database_->objects_.size() == database_->objects_.capacity()) {
            database_->objects_.reserve(std::min(
                std::size_t{65536}, std::max(std::size_t{8}, database_->objects_.size() * 2)));
        }
        if (object.container_) {
            object.container_->removeObject(object);
        }
        if (!live(object) || !live(*this) || object.container_) {
            return false;
        }
        if (!database_->add(object)) {
            return false;
        }
        object.container_ = this;
        deleteNotify(object);
        {
            FlagGuard guard(object.changing_);
            enterContainer(object);
        }
        if (!live(*this) || !live(object) || object.container_ != this) {
            return false;
        }
        if (!object.database_) {
            return true;
        }
        SimContainerList children;
        q.mask = object.containerMask_;
        database_->findContained(q, children);
        std::ranges::for_each(children, [&](auto* child) {
            if (live(object) && live(*child) && child->container_ == this) {
                object.addObject(*child);
            }
        });
        return live(object);
    });
}

bool SimContainer::removeObject(SimContainer& object) {
    if (object.container_ != this || !database_) {
        return false;
    }
    return dispatched(*this, [&] {
        database_->remove(object);
        object.container_ = nullptr;
        clearNotify(object);
        leaveContainer(object);
        return true;
    });
}

bool SimContainer::updateObject(SimContainer& object) {
    if (object.container_ != this || !database_) {
        return false;
    }
    database_->update();
    auto* parent = this;
    while (parent &&
           (!parent->isContained(object.box_) || !(parent->containerMask_ & object.getType()))) {
        parent = parent->container_;
    }
    if (!parent) {
        return removeObject(object);
    }
    if (parent != this) {
        return parent->addObject(object);
    }
    SimContainerQuery q;
    q.id = object.getId();
    q.type = object.getType();
    q.box = object.box_;
    if (auto* child = database_->findOpenContainer(q); child && child != &object) {
        return child->addObject(object);
    }
    return true;
}

void SimContainer::enterContainer(SimContainer& object) {
    if (container_) {
        container_->enterContainer(object);
    }
}

void SimContainer::leaveContainer(SimContainer& object) {
    if (container_) {
        container_->leaveContainer(object);
    }
}

SimContainer* SimContainer::findOpenContainer(const SimContainer& object) const {
    if (!database_ || !isContained(object.box_)) {
        return nullptr;
    }
    SimContainerQuery q;
    q.id = object.getId();
    q.type = object.getType();
    q.box = object.box_;
    auto* result = const_cast<SimContainer*>(this);
    while (auto* next = result->database_->findOpenContainer(q)) {
        result = next;
    }
    return result;
}

bool SimContainer::findIntersections(const SimContainerQuery& query,
                                     SimContainerList& result) const {
    if (!valid(query.box)) {
        throw std::invalid_argument("invalid spatial query");
    }
    if (!database_ || !(containerMask_ & query.mask)) {
        return false;
    }
    SimContainerList local;
    database_->findIntersections(query, local);
    std::ranges::for_each(local, [&](auto* child) {
        if (child->getId() != query.id && (child->getType() & query.mask)) {
            result.push_back(child);
        }
        if (child->isOpen()) {
            child->findIntersections(query, result);
        }
    });
    return !result.empty();
}

std::vector<SimAddress> SimContainer::scope(const SimContainerQuery& q) const {
    SimContainerList found;
    findIntersections(q, found);
    std::vector<SimAddress> result;
    std::ranges::transform(found, std::back_inserter(result), [](auto* o) {
        return o->address();
    });
    return result;
}

bool SimContainer::findLOS(const SimContainerQuery& query,
                           SimCollisionInfoList& result,
                           SimCollisionImageQuery::Detail detail) {
    if (!bounded(query.box.fMin) || !bounded(query.box.fMax)) {
        return false;
    }
    return dispatched(*this, [&] {
        SimContainerQuery q = query;
        q.box = Box3F(query.box.fMin, query.box.fMax);
        SimContainerList candidates;
        findIntersections(q, candidates);
        SimCollisionInfoList hits;
        SimCollisionLineImage line;
        line.start = query.box.fMin;
        line.end = query.box.fMax;
        std::ranges::for_each(candidates, [&](auto* object) {
            if (!live(*object)) {
                return;
            }
            const auto snapshot = images(*object, detail, q.box);
            std::ranges::for_each(std::views::iota(std::size_t{0}, snapshot.size()), [&](auto i) {
                if (!live(*object)) {
                    return;
                }
                CollisionSurfaceList surfaces;
                if (SimCollisionImage::test(line, *snapshot[i], surfaces)) {
                    hits.push_back({0, object->address(), snapshot[i], i, std::move(surfaces)});
                }
            });
        });
        result = std::move(hits);
        return !result.empty();
    });
}

bool SimContainer::findLOS(const SimContainerQuery& query,
                           SimCollisionInfo& result,
                           SimCollisionImageQuery::Detail detail) {
    SimCollisionInfoList hits;
    if (!findLOS(query, hits, detail)) {
        return false;
    }
    float best = std::numeric_limits<float>::max();
    std::optional<SimCollisionInfo> nearest;
    std::ranges::for_each(hits, [&](auto& hit) {
        if (hit.image->trigger && detail != SimCollisionImageQuery::Editor) {
            return;
        }
        Point3F point;
        m_mul(hit.surfaces.surfaces.front().position, hit.surfaces.tWorld, point);
        const auto distance = (point - query.box.fMin).len();
        if (distance < best) {
            best = distance;
            nearest = std::move(hit);
        }
    });
    if (!nearest) {
        return false;
    }
    result = std::move(*nearest);
    return true;
}

SimDefaultOpenContainer::SimDefaultOpenContainer(SimObjectId id) {
    if (id) {
        setId(id);
    }
    setBoundingBox({{-1e30f, -1e30f, -1e30f}, {1e30f, 1e30f, 1e30f}});
    setContainerMask(~UInt32{0});
    installDatabase(std::make_unique<SimContainerDatabase>());
}

SimMovement::SimMovement() {
    setBoundingBox(localBox_);
}

SimMovement::~SimMovement() {
    clearContacts();
}

void SimMovement::onRemove() {
    clearContacts();
    SimContainer::onRemove();
}

void SimMovement::setMovementFlags(UInt32 flags) {
    if (flags & ~UInt32{511}) {
        throw std::invalid_argument("invalid movement flags");
    }
    flags_ = flags;
}

void SimMovement::setForceSpeed() {
    const auto sum = forceSumExcept(0);
    forceSpeed_ = sum.len();
    flags_ &= ~AtRest;
}

bool SimMovement::setMass(Flt32 mass) {
    if (!nonnegative(mass) || (mass != 0 && mass < 1e-12f)) {
        return false;
    }
    auto forces = forces_;
    // Preserve the original scaling for nonzero masses. Transitions from zero
    // discard mass-dependent acceleration instead of dividing by zero.
    const float ratio = coefficient_.mass != 0 ? mass / coefficient_.mass : 0;
    std::ranges::for_each(forces, [&](auto& force) {
        if (!force.ignoreMass) {
            force.vector *= ratio;
        }
    });
    if (std::ranges::any_of(forces, [](const auto& f) {
            return !bounded(f.vector);
        })) {
        return false;
    }
    forces_ = std::move(forces);
    coefficient_.mass = mass;
    coefficient_.invMass = mass ? 1.f / mass : 0;
    setForceSpeed();
    return true;
}

bool SimMovement::setDensity(Flt32 v) {
    if (!nonnegative(v) || v < 1e-12f) {
        return false;
    }
    coefficient_.density = v;
    return true;
}

bool SimMovement::setDrag(Flt32 v) {
    if (!nonnegative(v)) {
        return false;
    }
    coefficient_.drag = v;
    return true;
}

bool SimMovement::setFriction(Flt32 v) {
    if (!nonnegative(v)) {
        return false;
    }
    coefficient_.friction = v;
    return true;
}

bool SimMovement::setElasticity(Flt32 v) {
    if (!nonnegative(v)) {
        return false;
    }
    coefficient_.elasticity = v;
    return true;
}

bool SimMovement::addForce(UInt32 type, const Point3F& vector, bool ignoreMass) {
    if (!type || !bounded(vector)) {
        return false;
    }
    Force f{type, ignoreMass, vector};
    if (!ignoreMass) {
        f.vector *= coefficient_.invMass;
    }
    if (!bounded(f.vector)) {
        return false;
    }
    auto found = std::ranges::find(forces_, type, &Force::type);
    if (found == forces_.end()) {
        if (forces_.size() >= 4096) {
            return false;
        }
        forces_.push_back(f);
    } else {
        *found = f;
    }
    setForceSpeed();
    return true;
}

bool SimMovement::removeForce(UInt32 type) {
    if (!std::erase_if(forces_, [&](const auto& f) {
            return f.type == type;
        })) {
        return false;
    }
    setForceSpeed();
    return true;
}

std::optional<Point3F> SimMovement::getForce(UInt32 type) const {
    const auto found = std::ranges::find(forces_, type, &Force::type);
    return found == forces_.end() ? std::nullopt : std::optional{found->vector};
}

Point3F SimMovement::forceSumExcept(UInt32 except) const {
    return std::accumulate(forces_.begin(), forces_.end(), Point3F{}, [&](auto sum, const auto& f) {
        return f.type != except ? sum + f.vector : sum;
    });
}

bool SimMovement::setContainerDrag(Flt32 drag) {
    if (!nonnegative(drag)) {
        return false;
    }
    containerDrag_ = drag;
    return true;
}

bool SimMovement::setContainerCurrent(const Point3F& velocity, Flt32 drag) {
    if (!bounded(velocity) || !nonnegative(drag)) {
        return false;
    }
    currentSpeed_ = velocity.isZero() ? 0 : velocity.lenf();
    currentDrag_ = currentSpeed_ ? drag : 0;
    currentDirection_ = currentSpeed_ ? velocity * (1.f / currentSpeed_) : Point3F{};
    return true;
}

bool SimMovement::setTimeSlice(SimTime dt) {
    if (!std::isfinite(dt) || dt < 0 || dt > 3600) {
        return false;
    }
    timeSlice_ = dt;
    return true;
}

bool SimMovement::setLinearVelocity(const Point3F& velocity) {
    if (!bounded(velocity)) {
        return false;
    }
    velocity_ = velocity;
    flags_ &= ~AtRest;
    return true;
}

bool SimMovement::setAngularVelocity(const Point3F& velocity) {
    if (!bounded(velocity)) {
        return false;
    }
    angularVelocity_ = velocity;
    flags_ &= ~AtRest;
    return true;
}

bool SimMovement::setAcceleration(const Point3F& linear, const Point3F& angular) {
    if (!bounded(linear) || !bounded(angular)) {
        return false;
    }
    acceleration_ = linear;
    angularAcceleration_ = angular;
    flags_ &= ~AtRest;
    return true;
}

bool SimMovement::setDisplacementVelocity(const Point3F& velocity) {
    if (!bounded(velocity)) {
        return false;
    }
    const auto lengthSq = m_dot(velocity, velocity);
    if (lengthSq != 0) {
        const auto dot = m_dot(velocity, velocity_);
        if (dot < lengthSq) {
            const auto candidate = velocity_ + (velocity - velocity * (dot / lengthSq));
            if (!bounded(candidate)) {
                return false;
            }
            velocity_ = candidate;
            flags_ &= ~AtRest;
        }
    }
    return true;
}

bool SimMovement::applyImpulse(const Point3F& force) {
    if (coefficient_.mass == 0 || !bounded(force)) {
        return false;
    }
    return setLinearVelocity(velocity_ + force * coefficient_.invMass);
}

bool SimMovement::applyImpulse(const Point3F& offset, const Point3F& force) {
    if (coefficient_.mass == 0 || !bounded(force) || !bounded(offset)) {
        return false;
    }
    const auto lengthSq = m_dot(offset, offset);
    if (!(flags_ & UseAngularVelocity) || lengthSq < .1f) {
        return applyImpulse(force);
    }
    const auto inverseLength = 1.f / lengthSq;
    const auto linear =
        velocity_ + offset * (coefficient_.invMass * m_dot(offset, force) * inverseLength);
    Point3F cross;
    m_cross(offset, force, cross);
    const auto angular = angularVelocity_ + cross * (coefficient_.invMass * 2.5f * inverseLength);
    if (!bounded(linear) || !bounded(angular)) {
        return false;
    }
    velocity_ = linear;
    angularVelocity_ = angular;
    flags_ &= ~AtRest;
    return true;
}

Point3F SimMovement::getVelocity(const Point3F& offset) const {
    Point3F v;
    m_cross(angularVelocity_, offset, v);
    return v + velocity_;
}

bool SimMovement::collide(const Point3F& point, const Point3F& normal) {
    if (!bounded(point) || !bounded(normal)) {
        return false;
    }
    const auto offset = point - transform_.p;
    const auto length = offset.len();
    const auto speed = m_dot(getVelocity(offset), normal);
    if (speed > 0) {
        return true;
    }
    const auto delta = normal * ((coefficient_.elasticity + 1.f) * -speed);
    // A collision at the center has no angular lever arm. The old normalize
    // followed by division by cp.len() generated NaNs in this case.
    if (length < 1e-12f) {
        return setLinearVelocity(velocity_ + delta);
    }
    auto linear = offset * (1.f / length);
    linear *= m_dot(delta, linear);
    const auto rotational = delta - linear;
    Point3F cross;
    m_cross(offset, rotational, cross);
    const auto angular = angularVelocity_ + unit(cross, {}) * (rotational.len() / length);
    if (!bounded(velocity_ + linear) || !bounded(angular)) {
        return false;
    }
    velocity_ += linear;
    angularVelocity_ = angular;
    flags_ &= ~AtRest;
    return true;
}

void SimMovement::updateLinearVelocity() {
    auto v = velocity_;
    std::ranges::for_each(forces_, [&](const auto& force) {
        if (force.type & forceMask_) {
            v += force.vector * timeSlice_;
        }
    });
    v += acceleration_ * timeSlice_;
    if ((flags_ & UseDrag) && containerDrag_ != 0) {
        v -= v * (coefficient_.drag * containerDrag_ * timeSlice_);
    }
    if ((flags_ & UseCurrent) && currentSpeed_ != 0) {
        const auto factor = (currentSpeed_ - m_dot(v, currentDirection_)) * coefficient_.drag *
                            currentDrag_ * timeSlice_;
        v += currentDirection_ * factor;
    }
    if (!bounded(v)) {
        throw std::overflow_error("movement velocity overflow");
    }
    velocity_ = v;
}

TMat3F SimMovement::updateMovement() {
    const auto oldVelocity = velocity_;
    updateLinearVelocity();
    TMat3F result = transform_;
    result.p = transform_.p + (oldVelocity + velocity_) * (timeSlice_ * .5f);
    if (flags_ & UseAngularVelocity) {
        const RMat3F rotation(EulerF(angularVelocity_.x * timeSlice_,
                                     angularVelocity_.y * timeSlice_,
                                     angularVelocity_.z * timeSlice_));
        m_mul(static_cast<const RMat3F&>(transform_), rotation, static_cast<RMat3F&>(result));
    }
    result.flags |= RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    if (!valid(result)) {
        velocity_ = oldVelocity;
        throw std::overflow_error("movement transform overflow");
    }
    return result;
}

Box3F SimMovement::buildContainerBox(const TMat3F& mat) const {
    Box3F box;
    if (flags_ & RotateBoundingBox) {
        m_mul(localBox_, mat, box);
    } else {
        box = localBox_;
        box += mat.p;
    }
    return box;
}

bool SimMovement::setTransform(const TMat3F& mat) {
    if (!valid(mat)) {
        return false;
    }
    auto inverse = mat;
    inverse.inverse();
    const auto oldTransform = transform_, oldInverse = inverse_;
    // Queries made by spatial callbacks must see matching transform and bounds.
    transform_ = mat;
    inverse_ = inverse;
    if (!setBoundingBox(buildContainerBox(mat))) {
        transform_ = oldTransform;
        inverse_ = oldInverse;
        return false;
    }
    return true;
}

bool SimMovement::setLocalBoundingBox(const Box3F& box) {
    if (!valid(box) || !bounded(box.fMin) || !bounded(box.fMax) || moving_) {
        return false;
    }
    const auto previous = localBox_;
    localBox_ = box;
    if (!setBoundingBox(buildContainerBox(transform_))) {
        localBox_ = previous;
        return false;
    }
    return true;
}

bool SimMovement::setCollisionRetryCount(Int32 count) {
    if (count < 1 || count > 64) {
        return false;
    }
    retryCount_ = count;
    return true;
}

std::shared_ptr<SimCollisionImage> SimMovement::getSelfImage() {
    auto list = images(*this);
    return list.empty() ? nullptr : list.front();
}

SimMovement::ImageList SimMovement::getImageList(const Box3F& box) {
    ImageList result;
    auto* manager = getManager();
    if (!manager || !collisionMask_) {
        return result;
    }
    auto* root = dynamic_cast<SimContainer*>(manager->findObject(RootContainerId));
    if (!root) {
        return result;
    }
    SimContainerQuery query;
    query.id = excludedId_ ? excludedId_ : getId();
    query.type = getType();
    query.mask = collisionMask_;
    query.box = box;
    SimContainerList candidates;
    root->findIntersections(query, candidates);
    std::ranges::for_each(candidates, [&](auto* object) {
        if (object == this || !live(*object)) {
            return;
        }
        const auto snapshot = images(*object, SimCollisionImageQuery::Normal, box);
        std::ranges::for_each(snapshot, [&](const auto& image) {
            if (result.size() >= 65536) {
                throw std::length_error("collision candidate limit");
            }
            result.push_back({object->address(), image});
        });
    });
    return result;
}

bool SimMovement::testImage(MovementInfo& info) {
    info.collisionList.clear();
    std::ranges::for_each(info.imageList, [&](const auto& candidate) {
        if (!live(*this) || (getManager() && !candidate.object.resolve(*getManager()))) {
            return;
        }
        CollisionSurfaceList surfaces;
        if (!SimCollisionImage::test(*info.image, *candidate.image, surfaces)) {
            return;
        }
        if (candidate.image->trigger) {
            if (std::ranges::none_of(info.triggeredList, [&](const auto& a) {
                    return same(a, candidate.object);
                })) {
                info.triggeredList.push_back(candidate.object);
                notify(candidate.object, address());
            }
        } else {
            info.collisionList.push_back(
                {1, candidate.object, candidate.image, 0, std::move(surfaces)});
        }
    });
    return info.collisionList.empty();
}

bool SimMovement::testPosition(const TMat3F& mat, MovementInfo& info) {
    if (!valid(mat)) {
        return false;
    }
    return dispatched(*this, [&] {
        if (!collisionMask_) {
            info.collisionList.clear();
            return true;
        }
        if (!info.image) {
            info.image = getSelfImage();
        }
        if (!info.image || !live(*this)) {
            return live(*this);
        }
        const auto original = info.image->transform;
        struct Restore {
            SimCollisionImage& image;
            TMat3F transform;
            ~Restore() {
                image.transform = transform;
            }
        } restore{*info.image, original};
        info.image->transform = mat;
        if (info.imageList.empty()) {
            info.imageList = getImageList(buildContainerBox(mat));
        }
        return testImage(info);
    });
}

SimMovement::Material SimMovement::collisionMaterial(const MovementInfo& info,
                                                     const SimCollisionInfo& collision,
                                                     const CollisionSurface& surface) const {
    const auto primary = info.image && (!collision.image ||
                                        info.image->getPriority() >= collision.image->getPriority())
                             ? info.image
                             : collision.image;
    const auto secondary = primary == info.image ? collision.image : info.image;
    if (primary && secondary) {
        if (const auto material = primary->getContactMaterial(*secondary, surface.material)) {
            return *material;
        }
    }
    return surface.material && materials_ ? (*materials_)(surface.material) : Material{};
}

bool SimMovement::processSurface(Point3F normal, const CollisionSurface& surface, UInt32 flags) {
    const auto dot = m_dot(normal, velocity_);
    if (dot >= 0) {
        return false;
    }
    Material material;
    if (surfaceMaterial_) {
        material = *surfaceMaterial_;
    } else if (surface.material && materials_) {
        const auto lookup = materials_;
        material = (*lookup)(surface.material);
    }
    if (!nonnegative(material.friction) || !nonnegative(material.elasticity)) {
        throw std::invalid_argument("invalid collision material");
    }
    auto velocity = velocity_;
    normal *= -dot + .0001f;
    velocity += normal;
    if (flags & UseFriction) {
        auto friction = velocity;
        const auto f = coefficient_.friction * -dot * material.friction;
        const auto length = friction.len();
        if (length > f) {
            friction *= f / length;
        }
        velocity -= friction;
    }
    if (flags & UseElasticity) {
        normal *= coefficient_.elasticity * material.elasticity;
        velocity += normal;
    }
    if (!bounded(velocity)) {
        throw std::overflow_error("collision response overflow");
    }
    velocity_ = velocity;
    contact_ = true;
    return true;
}

bool SimMovement::processCollision(MovementInfo& info) {
    bool hit = false;
    const auto flags = info.retryCount ? flags_ & ~(UseFriction | TrackContacts) : flags_;
    std::ranges::for_each(info.collisionList, [&](const auto& collision) {
        if (!live(*this)) {
            return;
        }
        if (collision.surfaces.contained) {
            hit = true;
            velocity_ = {};
        }
        if (std::ranges::none_of(info.collidedList, [&](const auto& a) {
                return same(a, collision.object);
            })) {
            info.collidedList.push_back(collision.object);
        }
        std::ranges::for_each(collision.surfaces.surfaces, [&](const auto& surface) {
            if (!live(*this) || surface.distance < 0) {
                return;
            }
            Point3F normal;
            m_mul(surface.normal, static_cast<const RMat3F&>(collision.surfaces.tWorld), normal);
            const auto previous = surfaceMaterial_;
            struct Restore {
                std::optional<Material>& value;
                std::optional<Material> previous;
                ~Restore() {
                    value = previous;
                }
            } restore{surfaceMaterial_, previous};
            const auto primary =
                info.image && (!collision.image ||
                               info.image->getPriority() >= collision.image->getPriority())
                    ? info.image
                    : collision.image;
            const auto secondary = primary == info.image ? collision.image : info.image;
            surfaceMaterial_ = primary && secondary
                                   ? primary->getContactMaterial(*secondary, surface.material)
                                   : std::nullopt;
            if (live(*this) && processSurface(normal, surface, flags)) {
                hit = true;
            }
        });
    });
    if (hit) {
        info.target.p = velocity_ * (timeSlice_ - info.startTime) + info.transform.p;
        info.target.flags |= RMat3F::Matrix_HasTranslation;
    }
    return hit;
}

void SimMovement::notify(SimAddress to, SimAddress object) {
    if (auto* manager = getManager(); manager && to.resolve(*manager)) {
        auto event = std::make_unique<SimMovementCollisionEvent>();
        event->address = to;
        event->object = object;
        manager->postEvent(std::move(event), true);
    }
}

void SimMovement::notifyCollision(const MovementInfo& info) {
    std::ranges::for_each(info.collidedList, [&](const auto& other) {
        notify(other, address());
        notify(address(), other);
    });
}

void SimMovement::testRestCondition() {
    if (!contact_ || (flags_ & AtRest)) {
        return;
    }
    const auto tolerance = forceSpeed_ * timeSlice_ * 2.f;
    if (std::ranges::all_of(axes(),
                            [&](auto i) {
                                return velocity_[i] > -tolerance && velocity_[i] < tolerance &&
                                       std::abs(angularVelocity_[i]) < .02f;
                            }) &&
        acceleration_.x + acceleration_.y + acceleration_.z == 0 &&
        angularAcceleration_.x + angularAcceleration_.y + angularAcceleration_.z == 0) {
        flags_ |= AtRest;
    }
}

bool SimMovement::setRotation(const RMat3F& mat, bool force) {
    auto target = transform_;
    static_cast<RMat3F&>(target) = mat;
    target.flags |= RMat3F::Matrix_HasTranslation;
    return setPosition(target, force);
}

bool SimMovement::setPosition(TMat3F target, bool force) {
    if (!valid(target) || moving_) {
        return false;
    }
    if (force || !collisionMask_) {
        return dispatched(*this, [&] {
            FlagGuard guard(moving_);
            contact_ = false;
            flags_ &= ~AtRest;
            return setTransform(target);
        });
    }
    return position(target, false, 1, 1);
}

bool SimMovement::stepPosition(TMat3F target, Flt32 minDistance, Flt32 maxDistance) {
    if (!valid(target) || moving_ || !std::isfinite(minDistance) || !std::isfinite(maxDistance) ||
        minDistance <= 0 || maxDistance < minDistance) {
        return false;
    }
    return position(target, true, minDistance, maxDistance);
}

bool SimMovement::position(TMat3F target, bool stepped, Flt32 minDistance, Flt32 maxDistance) {
    return dispatched(*this, [&] {
        FlagGuard guard(moving_);
        contact_ = false;
        flags_ &= ~AtRest;
        MovementInfo info;
        info.transform = transform_;
        info.target = target;
        info.image = collisionMask_ ? getSelfImage() : nullptr;
        if (!live(*this)) {
            return false;
        }
        // 1.11: movement without a collision image still applies its transform.
        if (!info.image) {
            return setTransform(target);
        }
        struct Restore {
            SimCollisionImage& image;
            TMat3F transform;
            ~Restore() {
                image.transform = transform;
            }
        } restore{*info.image, info.image->transform};
        auto box = buildContainerBox(target);
        const auto startBox = buildContainerBox(transform_);
        box.fMin.setMin(startBox.fMin);
        box.fMax.setMax(startBox.fMax);
        const auto velocityDelta = velocity_ * timeSlice_;
        box.fMin.setMin(box.fMin + velocityDelta);
        box.fMax.setMax(box.fMax + velocityDelta);
        info.imageList = getImageList(box);
        float distance = maxDistance;
        const auto oldVelocity = velocity_, oldAngular = angularVelocity_;
        const auto oldFlags = flags_;
        try {
            for (info.retryCount = 0; info.retryCount < retryCount_ && live(*this);
                 ++info.retryCount) {
                info.image->transform = info.target;
                bool blocked = false;
                if (stepped) {
                    info.image->transform.p = info.transform.p;
                    const auto delta = info.target.p - info.transform.p;
                    const auto countFloat = std::ceil(delta.len() / distance);
                    if (!std::isfinite(countFloat) || countFloat > 65536) {
                        return false;
                    }
                    const auto count = static_cast<Int32>(countFloat);
                    const auto step = count ? delta * (1.f / static_cast<float>(count)) : Point3F{};
                    // Test rotation in place too; legacy skipped it for zero steps.
                    const auto steps = std::max(count, 1);
                    for (Int32 s = 0; s < steps && live(*this); ++s) {
                        info.image->transform.p += step;
                        if (!testImage(info)) {
                            const auto dt =
                                (timeSlice_ - info.startTime) / static_cast<float>(steps);
                            info.collisionTime = info.startTime + dt * static_cast<float>(s + 1);
                            info.startTime += dt * static_cast<float>(s);
                            if (processCollision(info)) {
                                blocked = true;
                                distance = minDistance;
                                break;
                            }
                        }
                        info.transform.p = info.image->transform.p;
                    }
                } else if (!testImage(info)) {
                    info.startTime = 0;
                    info.collisionTime = 1;
                    blocked = processCollision(info);
                }
                if (!live(*this)) {
                    return false;
                }
                if (!blocked) {
                    const auto ok = setTransform(stepped ? info.image->transform : info.target);
                    notifyCollision(info);
                    testRestCondition();
                    return ok;
                }
            }
            if (!live(*this)) {
                return false;
            }
            if (stepped) {
                setTransform(info.transform);
            }
            flags_ |= AtRest;
            velocity_ = {};
            angularVelocity_ = {};
            notifyCollision(info);
            return false;
        } catch (...) {
            velocity_ = oldVelocity;
            angularVelocity_ = oldAngular;
            flags_ = oldFlags;
            throw;
        }
    });
}

bool SimMovement::sweep(TMat3F target, std::shared_ptr<SimCollisionImage> image) {
    if (!valid(target) || moving_) {
        return false;
    }
    if (!collisionMask_) {
        return setPosition(target, true);
    }
    return dispatched(*this, [&] {
        FlagGuard guard(moving_);
        contact_ = false;
        flags_ &= ~AtRest;
        MovementInfo info;
        info.transform = transform_;
        info.target = target;
        info.image = std::move(image);
        for (info.retryCount = 0; info.retryCount < retryCount_ && live(*this); ++info.retryCount) {
            Box3F box(info.transform.p, info.target.p);
            if (auto* tube = dynamic_cast<SimCollisionTubeImage*>(info.image.get())) {
                tube->start = info.transform.p;
                tube->end = info.target.p;
                box.fMin -= tube->radius;
                box.fMax += tube->radius;
            } else {
                auto& line = static_cast<SimCollisionLineImage&>(*info.image);
                line.start = info.transform.p;
                line.end = info.target.p;
            }
            info.imageList = getImageList(box);
            if (testImage(info)) {
                const auto ok = setTransform(info.target);
                notifyCollision(info);
                testRestCondition();
                return ok;
            }
            // Choose the winning surface together with its OWN world matrix.
            const auto nearest =
                std::ranges::min_element(info.collisionList, {}, [&](const auto& collision) {
                    Point3F p;
                    m_mul(
                        collision.surfaces.surfaces.front().position, collision.surfaces.tWorld, p);
                    return (p - info.transform.p).len();
                });
            Point3F point;
            m_mul(nearest->surfaces.surfaces.front().position, nearest->surfaces.tWorld, point);
            info.transform.p = point;
            info.collisionTime = 1;
            if (!processCollision(info)) {
                info.target.p = point;
                const auto ok = setTransform(info.target);
                notifyCollision(info);
                return ok;
            }
        }
        if (!live(*this)) {
            return false;
        }
        setTransform(info.transform);
        velocity_ = {};
        angularVelocity_ = {};
        flags_ |= AtRest;
        notifyCollision(info);
        return false;
    });
}

bool SimMovement::movePoint(TMat3F target) {
    return sweep(target, std::make_shared<SimCollisionLineImage>());
}

bool SimMovement::moveSphere(TMat3F target, Flt32 radius) {
    if (!nonnegative(radius)) {
        return false;
    }
    auto image = std::make_shared<SimCollisionTubeImage>();
    image->radius = radius;
    return sweep(target, std::move(image));
}

bool SimMovement::addContact(SimMovement& other) {
    if (&other == this || getManager() != other.getManager() || contacts_.size() >= 4096 ||
        other.contacts_.size() >= 4096 || std::ranges::find(contacts_, &other) != contacts_.end()) {
        return false;
    }
    contacts_.reserve(contacts_.size() + 1);
    other.contacts_.reserve(other.contacts_.size() + 1);
    contacts_.push_back(&other);
    other.contacts_.push_back(this);
    return true;
}

bool SimMovement::removeContact(SimMovement& other) {
    const auto count = std::erase(contacts_, &other);
    std::erase(other.contacts_, this);
    return count != 0;
}

void SimMovement::clearContacts() {
    std::ranges::for_each(contacts_, [&](auto* other) {
        std::erase(other->contacts_, this);
    });
    contacts_.clear();
}

bool SimMovement::displaceObjects(const TMat3F& oldTransform) {
    if (!valid(oldTransform) || displacing_ || moving_) {
        return false;
    }
    const auto displacement = transform_.p - oldTransform.p;
    if (displacement.isZero()) {
        return true;
    }
    return dispatched(*this, [&] {
        FlagGuard guard(displacing_);
        struct State {
            SimMovement* object;
            TMat3F transform;
            Point3F velocity, angular;
            UInt32 flags, mask;
            bool contact;
        };
        std::vector<State> states;
        std::unordered_set<SimMovement*> seen{this};
        std::vector<SimMovement*> pending;
        const auto enqueue = [&](SimMovement* object) {
            if (object && live(*object) && seen.insert(object).second) {
                pending.push_back(object);
            }
        };
        std::ranges::for_each(contacts_, enqueue);
        const auto intersections = [&](SimMovement& mover) {
            auto self = mover.getSelfImage();
            if (!self) {
                return;
            }
            self->transform = mover.transform_;
            const auto candidates = mover.getImageList(mover.getBoundingBox());
            std::ranges::for_each(candidates, [&](const auto& candidate) {
                auto* obj =
                    getManager()
                        ? dynamic_cast<SimMovement*>(candidate.object.resolve(*getManager()))
                        : nullptr;
                if (!obj || candidate.image->trigger ||
                    !(obj->getType() &
                      ~(TerrainObjectType | InteriorObjectType | ContainerObjectType))) {
                    return;
                }
                CollisionSurfaceList list;
                if (SimCollisionImage::test(*self, *candidate.image, list)) {
                    enqueue(obj);
                }
            });
        };
        const auto rollback = [&] {
            std::ranges::for_each(states | std::views::reverse, [&](auto& s) {
                if (!live(*s.object)) {
                    return;
                }
                s.object->setTransform(s.transform);
                s.object->velocity_ = s.velocity;
                s.object->angularVelocity_ = s.angular;
                s.object->flags_ = s.flags;
                s.object->collisionMask_ = s.mask;
                s.object->contact_ = s.contact;
            });
        };
        try {
            intersections(*this);
            // Worklist traversal is stateful: pushing contacts discovers chains.
            while (!pending.empty()) {
                auto* object = pending.back();
                pending.pop_back();
                if (!live(*object)) {
                    continue;
                }
                if (states.size() >= 4096) {
                    rollback();
                    return false;
                }
                states.push_back({object,
                                  object->transform_,
                                  object->velocity_,
                                  object->angularVelocity_,
                                  object->flags_,
                                  object->collisionMask_,
                                  object->contact_});
                std::ranges::for_each(object->contacts_, enqueue);
                const auto originalMask = object->collisionMask_, originalFlags = object->flags_;
                // 1.11: ASSIGN the static mask; intersecting with the old mask
                // could silently disable terrain/interior collision entirely.
                object->collisionMask_ = TerrainObjectType | InteriorObjectType;
                object->flags_ &= ~(UseFriction | UseElasticity);
                object->setDisplacementVelocity(velocity_);
                auto target = object->transform_;
                target.p += displacement;
                target.flags |= RMat3F::Matrix_HasTranslation;
                const auto& box = object->getBoundingBox();
                const auto maxStep = (box.len_x() + box.len_y() + box.len_z()) / 3.f;
                const auto step = std::max(maxStep, .001f);
                const auto moved = object->stepPosition(target, step * .5f, step);
                object->collisionMask_ = originalMask;
                object->flags_ = originalFlags;
                if (!live(*this)) {
                    rollback();
                    return false;
                }
                if (!moved && live(*object) && processBlocker(*object)) {
                    rollback();
                    return false;
                }
                if (live(*object)) {
                    intersections(*object);
                }
            }
            // All physical changes succeeded before notification callbacks.
            std::ranges::for_each(states, [&](auto& state) {
                if (live(*this) && live(*state.object)) {
                    const auto delta = state.object->transform_.p - state.transform.p;
                    onDisplacedObject(*state.object, delta);
                    if (live(*this) && live(*state.object)) {
                        state.object->onDisplacement(*this, delta);
                    }
                }
                if (live(*state.object)) {
                    state.object->velocity_ = state.velocity;
                }
            });
            return live(*this);
        } catch (...) {
            const auto error = std::current_exception();
            rollback();
            std::rethrow_exception(error);
        }
    });
}
} // namespace opent1::sim
