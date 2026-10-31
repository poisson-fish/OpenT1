#include <opent1/world/scene_objects.hpp>
#include <cmath>
#include <opent1/sim/sim_resource_volume.hpp>

namespace opent1::world {
namespace {
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool finite(ColorF c) {
    return std::isfinite(c.red) && std::isfinite(c.green) && std::isfinite(c.blue) && c.red >= 0 &&
           c.green >= 0 && c.blue >= 0;
}

bool point(StreamIO& s, const Point3F& p) {
    return s.write(p.x) && s.write(p.y) && s.write(p.z);
}

bool readPoint(StreamIO& s, Point3F& p) {
    return s.read(p.x) && s.read(p.y) && s.read(p.z);
}

bool color(StreamIO& s, ColorF c) {
    return s.write(c.red) && s.write(c.green) && s.write(c.blue);
}

bool readColor(StreamIO& s, ColorF& c) {
    return s.read(c.red) && s.read(c.green) && s.read(c.blue);
}
}

bool LinearMotion::update(float dt) {
    if (!std::isfinite(dt) || !finite(position) || !finite(velocity)) {
        return false;
    }
    auto next = position;
    next.x += dt * velocity.x;
    next.y += dt * velocity.y;
    next.z += dt * velocity.z;
    if (!finite(next)) {
        return false;
    }
    position = next;
    return true;
}

bool AcceleratedMotion::update(float dt) {
    if (!std::isfinite(dt) || !finite(position) || !finite(velocity) || !finite(target) ||
        !finite(acceleration)) {
        return false;
    }
    const auto amount = acceleration * dt;
    auto next = velocity;
    const auto axis = [](float current, float goal, float step) -> std::optional<float> {
        if (!std::isfinite(step)) {
            return {};
        }
        if (goal < current) {
            if ((current -= step) < goal) {
                current = goal;
            }
        } else if (goal > current) {
            if ((current += step) > goal) {
                current = goal;
            }
        }
        // The original helper returns int, intentionally quantizing velocity.
        if (!std::isfinite(current) || double(current) < std::numeric_limits<Int32>::min() ||
            double(current) > std::numeric_limits<Int32>::max()) {
            return {};
        }
        return float(Int32(current));
    };
    const auto x = axis(next.x, target.x, amount.x), y = axis(next.y, target.y, amount.y),
               z = axis(next.z, target.z, amount.z);
    if (!x || !y || !z) {
        return false;
    }
    next = {*x, *y, *z};
    auto average = velocity;
    average += next;
    average *= float(.5 * dt);
    auto p = position;
    p += average;
    if (!finite(p)) {
        return false;
    }
    position = p;
    velocity = next;
    return true;
}

bool PathState::valid() const {
    return id >= -1 && id < 4096 && waypoints.size() <= 4096 &&
           std::ranges::all_of(
               waypoints,
               [](const auto& p) {
                   return finite(p.position) && finite(p.rotation) && finite(p.linear) &&
                          finite(p.angular);
               });
}

bool PathState::write(StreamIO& s) const {
    if (!valid() || !s.write(Int32((looping ? 2 : 0) | (compressed ? 4 : 0))) ||
        !s.write(static_cast<Int16>(waypoints.size()))) {
        return false;
    }
    for (const auto& w : waypoints) {
        if (!point(s, w.position) || !point(s, w.rotation) || !s.write(w.persistTag)) {
            return false;
        }
    }
    return s.write(id);
}

bool PathState::read(StreamIO& s) {
    PathState p;
    Int32 flags{};
    Int16 n{};
    if (!s.read(flags) || (flags & ~6) || !s.read(n) || n < 0 || n > 4096) {
        return false;
    }
    p.looping = (flags & 2) != 0;
    p.compressed = true;
    p.waypoints.resize(n);
    for (auto& w : p.waypoints) {
        if (!readPoint(s, w.position) || !readPoint(s, w.rotation) || !s.read(w.persistTag)) {
            return false;
        }
    }
    // Consume the ID written by the native writer; its reader accidentally
    // leaves these four bytes in the stream. Never carry that under-read forward.
    if (!s.read(p.id) || !p.valid()) {
        return false;
    }
    *this = std::move(p);
    return true;
}

std::optional<PathSample> samplePath(const PathState& p, Int32 index) {
    if (!p.valid() || p.waypoints.empty()) {
        return {};
    }
    const auto n = static_cast<Int32>(p.waypoints.size());
    if (p.looping) {
        index %= n;
    }
    if (index < 0 || index >= n) {
        return {};
    }
    const auto& a = p.waypoints[index];
    const auto& b = p.waypoints[(index + 1) % n];
    const auto linear = b.position - a.position, angular = b.rotation - a.rotation;
    if (!finite(linear) || !finite(angular)) {
        return {};
    }
    return PathSample{a.position, a.rotation, linear, angular, index, n};
}

bool PathEvent::pack(BitStream& b) const {
    if (id < 0 || id >= 4096 || (path && (!path->valid() || path->id != id))) {
        return false;
    }
    const auto bits = 33U + (path ? 33U + 384U * path->waypoints.size() : 0U);
    if (b.remainingWriteBits() < bits) {
        return false;
    }
    b.write(id);
    b.writeFlag(path.has_value());
    if (path) {
        b.writeFlag(path->looping);
        b.write(static_cast<Int32>(path->waypoints.size()));
        // Native SimPathManager::Waypoint declares angular/linear caches first,
        // followed by rotation and position. Preserve that order explicitly.
        for (const auto& w : path->waypoints) {
            point(b, w.angular);
            point(b, w.linear);
            point(b, w.rotation);
            point(b, w.position);
        }
    }
    return b.isValid();
}

bool PathEvent::unpack(BitStream& b) {
    PathEvent e = *this;
    if (b.remainingReadBits() < 33 || !b.read(e.id) || e.id < 0 || e.id >= 4096) {
        return false;
    }
    if (b.readFlag()) {
        if (b.remainingReadBits() < 33) {
            return false;
        }
        PathState p;
        p.id = e.id;
        p.looping = b.readFlag();
        Int32 n{};
        if (!b.read(n) || n < 0 || n > 4096 ||
            b.remainingReadBits() < static_cast<std::size_t>(n) * 384) {
            return false;
        }
        p.waypoints.resize(n);
        for (auto& w : p.waypoints) {
            if (!readPoint(b, w.angular) || !readPoint(b, w.linear) || !readPoint(b, w.rotation) ||
                !readPoint(b, w.position)) {
                return false;
            }
        }
        if (!p.valid()) {
            return false;
        }
        e.path = std::move(p);
    } else {
        e.path.reset();
    }
    if (!b.isValid()) {
        return false;
    }
    *this = std::move(e);
    return true;
}

Int32 SimPathManager::add(PathState p) {
    if (!p.valid()) {
        return -1;
    }
    if (p.id == -1) {
        if (paths_.size() >= 4096) {
            return -1;
        }
        p.id = static_cast<Int32>(paths_.size());
    }
    const auto id = p.id;
    if (!replace(id, std::move(p))) {
        return -1;
    }
    return id;
}

bool SimPathManager::replace(Int32 id, std::optional<PathState> p) {
    if (id < 0 || id >= 4096 || (p && !p->valid())) {
        return false;
    }
    std::shared_ptr<const PathState> snapshot;
    if (p) {
        p->id = id;
        snapshot = std::make_shared<const PathState>(std::move(*p));
    }
    auto staged = paths_;
    if (staged.size() <= static_cast<std::size_t>(id)) {
        staged.resize(static_cast<std::size_t>(id) + 1);
    }
    staged[id] = snapshot;
    PathEvent event;
    event.id = id;
    if (snapshot) {
        event.path = *snapshot;
    }
    try {
        if (!receiving_ && broadcast && !broadcast(event)) {
            return false;
        }
    } catch (...) {
        return false;
    }
    paths_ = std::move(staged);
    return true;
}

std::vector<PathEvent> SimPathManager::fullSync() const {
    std::vector<PathEvent> result;
    for (std::size_t i = 0; i < paths_.size(); ++i) {
        if (paths_[i]) {
            PathEvent e;
            e.id = Int32(i);
            e.path = *paths_[i];
            result.push_back(std::move(e));
        }
    }
    return result;
}

bool SimPathManager::sync(sim::network::PacketStream& stream) const {
    if (!getManager() || stream.getManager() != getManager() || stream.isRemoved() ||
        !stream.eventManager()) {
        return false;
    }
    std::vector<sim::network::EventManager::Delivery> deliveries;
    for (const auto& event : fullSync()) {
        deliveries.push_back({stream.eventManager(), std::make_unique<PathEvent>(event)});
    }
    return sim::network::EventManager::postRemoteBatch(std::move(deliveries));
}

void SimPathManager::useNetworkBroadcast() {
    broadcast = [this](const PathEvent& event) {
        if (!getManager() || isRemoved()) {
            return false;
        }
        auto* set = dynamic_cast<sim::SimSet*>(getManager()->findObject(sim::PacketStreamSetId));
        if (!set) {
            return false;
        }
        std::vector<sim::network::EventManager::Delivery> deliveries;
        for (auto* object : set->objects()) {
            auto* stream = dynamic_cast<sim::network::PacketStream*>(object);
            if (!stream || stream->isRemoved() || !stream->ghostManager() ||
                stream->ghostManager()->mode() != sim::network::GhostManager::Mode::Normal) {
                continue;
            }
            if (!stream->eventManager()) {
                return false;
            }
            deliveries.push_back({stream->eventManager(), std::make_unique<PathEvent>(event)});
        }
        return sim::network::EventManager::postRemoteBatch(std::move(deliveries));
    };
}

std::shared_ptr<const PathState> SimPathManager::path(Int32 id) const {
    return id >= 0 && static_cast<std::size_t>(id) < paths_.size() ? paths_[id] : nullptr;
}

std::optional<PathSample> SimPathManager::waypoint(Int32 id, Int32 index) {
    const auto current = path(id);
    if (!current) {
        return {};
    }
    auto sample = samplePath(*current, index);
    if (!sample) {
        return {};
    }
    auto next = std::make_shared<PathState>(*current);
    auto& cache = next->waypoints[std::size_t(sample->index)];
    cache.linear = sample->linear;
    cache.angular = sample->angular;
    paths_[std::size_t(id)] = std::move(next);
    return sample;
}

void SimPathManager::retire(Int32 id) {
    if (id < 0 || std::size_t(id) >= paths_.size()) {
        return;
    }
    paths_[id].reset();
    PathEvent event;
    event.id = id;
    try {
        if (broadcast && !broadcast(event)) {
            deliveryHealthy_ = false;
        }
    } catch (...) {
        deliveryHealthy_ = false;
    }
}

bool SimPathManager::processEvent(const sim::SimEvent& event) {
    if (const auto* e = dynamic_cast<const PathEvent*>(&event)) {
        if (receiving_) {
            return false;
        }
        receiving_ = true;
        bool result{};
        try {
            result = replace(e->id, e->path);
        } catch (...) {
            receiving_ = false;
            throw;
        }
        receiving_ = false;
        return result;
    }
    return SimObject::processEvent(event);
}

bool SimPath::onAdd() {
    if (!getManager() || !state_.valid()) {
        return false;
    }
    auto* service = dynamic_cast<SimPathManager*>(getManager()->findObject(PathManagerId));
    if (!service) {
        return false;
    }
    auto next = snapshot();
    if (!next) {
        return false;
    }
    const auto id = service->add(*next);
    if (id < 0) {
        return false;
    }
    state_ = std::move(*next);
    state_.id = id;
    service_ = service->address();
    return SimGroup::onAdd();
}

void SimPath::onRemove() {
    if (getManager()) {
        if (auto* service = dynamic_cast<SimPathManager*>(service_.resolve(*getManager()))) {
            service->retire(state_.id);
        }
    }
    SimGroup::onRemove();
}

bool SimPath::setState(PathState next) {
    if (!next.valid()) {
        return false;
    }
    if (getManager()) {
        auto* service = dynamic_cast<SimPathManager*>(service_.resolve(*getManager()));
        if (!service) {
            return false;
        }
        next.id = state_.id;
        if (!service->replace(next.id, next)) {
            return false;
        }
    }
    state_ = std::move(next);
    return true;
}

std::optional<PathState> SimPath::snapshot() const {
    if (state_.compressed) {
        return state_;
    }
    auto next = state_;
    next.waypoints.clear();
    for (auto* child : objects()) {
        sim::SimObjectTransformQuery query;
        if (!child->processQuery(query)) {
            continue;
        }
        EulerF e;
        query.transform.angles(e);
        PathWaypoint w;
        w.position = query.transform.p;
        w.rotation = {e.x, e.y, e.z};
        w.persistTag = waypointTag ? waypointTag(*child) : 0;
        next.waypoints.push_back(w);
    }
    return next.valid() ? std::optional(std::move(next)) : std::nullopt;
}

bool SimPath::compress() {
    auto next = snapshot();
    return next && setState(std::move(*next));
}

bool SimPath::expand() {
    if (!state_.compressed) {
        return true;
    }
    if (!getManager() || (!state_.waypoints.empty() && !createWaypoint) || !objects().empty()) {
        return false;
    }
    std::vector<std::unique_ptr<sim::SimObject>> staged;
    std::vector<sim::SimAddress> adopted;
    try {
        for (const auto& w : state_.waypoints) {
            auto object = createWaypoint(w.persistTag);
            if (!object) {
                return false;
            }
            sim::SimObjectTransformEvent event;
            event.transform.set(EulerF{w.rotation.x, w.rotation.y, w.rotation.z}, w.position);
            if (!object->processEvent(event)) {
                return false;
            }
            staged.push_back(std::move(object));
        }
        for (auto& object : staged) {
            auto* added = getManager()->addObject(std::move(object), this);
            if (!added) {
                for (const auto& a : adopted) {
                    if (auto* c = a.resolve(*getManager())) {
                        getManager()->deleteObject(*c);
                    }
                }
                return false;
            }
            adopted.push_back(added->address());
        }
    } catch (...) {
        for (const auto& a : adopted) {
            if (auto* c = a.resolve(*getManager())) {
                getManager()->deleteObject(*c);
            }
        }
        return false;
    }
    state_.compressed = false;
    return true;
}

bool SimPath::publish() {
    return state_.compressed ? setState(state_) : compress();
}

bool SimPath::processQuery(sim::SimQuery& query) {
    if (auto* q = dynamic_cast<PathQuery*>(&query)) {
        q->id = state_.id;
        q->count = Int32(state_.compressed ? state_.waypoints.size() : objects().size());
        return true;
    }
    if (auto* q = dynamic_cast<WaypointQuery*>(&query)) {
        auto path = state_;
        if (!path.compressed) {
            path.waypoints.clear();
            for (auto* c : objects()) {
                sim::SimObjectTransformQuery transform;
                if (!c->processQuery(transform)) {
                    return false;
                }
                EulerF e;
                transform.transform.angles(e);
                path.waypoints.push_back({0, transform.transform.p, {e.x, e.y, e.z}});
            }
        }
        auto sample = samplePath(path, q->index);
        if (!sample) {
            return false;
        }
        q->sample = sample;
        return true;
    }
    return SimGroup::processQuery(query);
}

bool SimPath::processEvent(const sim::SimEvent& event) {
    if (const auto* edit = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        if (!getManager()) {
            return false;
        }
        const auto before = state_;
        const bool expanded = state_.compressed;
        if (!expand()) {
            return false;
        }
        const auto rollback = [&] {
            if (expanded) {
                while (!objects().empty()) {
                    getManager()->deleteObject(*objects().back());
                }
                state_ = before;
            }
        };
        try {
            std::vector<std::unique_ptr<sim::SimEvent>> pending;
            pending.reserve(objects().size() + 1);
            // Original SimEditEvent::post delivers marker edits asynchronously.
            // Prepare the entire fanout and first poll as one local queue batch.
            for (auto* child : objects()) {
                auto copy = std::make_unique<sim::SimEditEvent>(*edit);
                copy->address = child->address();
                copy->time = getManager()->getCurrentTime();
                pending.push_back(std::move(copy));
            }
            if (!editing_) {
                auto poll = std::make_unique<sim::SimMessageEvent>();
                poll->address = address();
                poll->time = getManager()->getCurrentTime() + 5.f;
                pending.push_back(std::move(poll));
            }
            if (!getManager()->postEvents(std::move(pending))) {
                rollback();
                return false;
            }
        } catch (...) {
            rollback();
            return false;
        }
        editing_ = true;
        return true;
    }
    if (dynamic_cast<const sim::SimMessageEvent*>(&event) && editing_) {
        if (!publish()) {
            return false;
        }
        auto poll = std::make_unique<sim::SimMessageEvent>();
        poll->address = address();
        poll->time = getManager()->getCurrentTime() + 5.f;
        return getManager()->postEvent(std::move(poll));
    }
    return SimGroup::processEvent(event);
}

Persistent::Error SimPath::write(StreamIO& s, Int32, Int32, Persistent::StoreContext&) const {
    auto next = snapshot();
    return next && next->write(s) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

Persistent::Error SimPath::read(StreamIO& s, Int32, Int32, Persistent::LoadContext&) {
    PathState next;
    if (!next.read(s)) {
        return Persistent::Error::ReadError;
    }
    next.id = -1;
    return setState(std::move(next)) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

bool LightState::valid() const {
    if (!(type == 1 || type == 2 || type == 5 || type == 6) || !finite(intensity) ||
        !finite(ambient)) {
        return false;
    }
    // Native constructors leave the other light type's fields uninitialized,
    // but packUpdate still sends them in a full ghost update.
    return type == 2 ? std::isfinite(range) && range >= 0 && finite(position)
                     : finite(direction) && std::isfinite(direction.len()) && direction.len() > 0;
}

sim::SimLight LightState::light() const {
    sim::SimLight light;
    light.type = type == 1   ? sim::SimLight::Type::Directional
                 : type == 2 ? sim::SimLight::Type::Point
                 : type == 5 ? sim::SimLight::Type::DirectionalNonlinear
                             : sim::SimLight::Type::DirectionalWrap;
    light.range = range;
    light.intensity = intensity;
    light.position = position;
    light.direction = direction;
    return light;
}

bool LightState::write(StreamIO& s) const {
    return valid() && s.write(type) && s.write(range) && color(s, intensity) && color(s, ambient) &&
           point(s, position) && point(s, direction) && s.write(isStatic);
}

bool LightState::read(StreamIO& s, Int32 version) {
    LightState next;
    if ((version != 1 && version != 2) || !s.read(next.type) || !s.read(next.range) ||
        !readColor(s, next.intensity) || !readColor(s, next.ambient) ||
        !readPoint(s, next.position) || !readPoint(s, next.direction) ||
        (version == 2 && !s.read(next.isStatic)) || !next.valid()) {
        return false;
    }
    *this = next;
    return true;
}

bool LightState::pack(BitStream& b, UInt32 mask) const {
    const auto bits = 7U + ((mask & Static) ? 1U : 0U) + ((mask & Type) ? 3U : 0U) +
                      ((mask & Range) ? 32U : 0U) + ((mask & Intensity) ? 96U : 0U) +
                      ((mask & Ambient) ? 96U : 0U) + ((mask & Position) ? 96U : 0U) +
                      ((mask & Direction) ? 96U : 0U);
    if (!valid() || b.remainingWriteBits() < bits) {
        return false;
    }
    if (b.writeFlag((mask & Static) != 0)) {
        b.writeFlag(isStatic);
    }
    if (b.writeFlag((mask & Type) != 0)) {
        b.writeInt(type, 3);
    }
    if (b.writeFlag((mask & Range) != 0)) {
        b.write(range);
    }
    if (b.writeFlag((mask & Intensity) != 0)) {
        color(b, intensity);
    }
    if (b.writeFlag((mask & Ambient) != 0)) {
        color(b, ambient);
    }
    if (b.writeFlag((mask & Position) != 0)) {
        point(b, position);
    }
    if (b.writeFlag((mask & Direction) != 0)) {
        point(b, direction);
    }
    return b.isValid();
}

bool LightState::unpack(BitStream& b) {
    auto next = *this;
    if (b.readFlag()) {
        next.isStatic = b.readFlag();
    }
    if (b.readFlag()) {
        next.type = b.readInt(3);
    }
    if (b.readFlag()) {
        b.read(next.range);
    }
    if (b.readFlag()) {
        readColor(b, next.intensity);
    }
    if (b.readFlag()) {
        readColor(b, next.ambient);
    }
    if (b.readFlag()) {
        readPoint(b, next.position);
    }
    if (b.readFlag()) {
        readPoint(b, next.direction);
    }
    if (!b.isValid() || !next.valid()) {
        return false;
    }
    *this = next;
    return true;
}

SimNativeLight::SimNativeLight() {
    setType(sim::MissionObjectType);
    setNetFlags(Ghostable | ScopeAlways);
}

bool SimNativeLight::setState(const LightState& next) {
    if (!next.valid()) {
        return false;
    }
    auto normalized = next;
    if (next.type == 2) {
        normalized.direction = {0, 1, 0};
    } else {
        normalized.direction /= normalized.direction.len();
        if (!std::isfinite(normalized.range) || normalized.range <= 0) {
            normalized.range = 1;
        }
        if (!finite(normalized.position)) {
            normalized.position = {};
        }
    }
    const auto local =
        marker_ ? marker_->getLocalBoundingBox() : Box3F{{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
    const Box3F box{local.fMin + normalized.position, local.fMax + normalized.position};
    if (!setBoundingBox(box)) {
        return false;
    }
    state_ = normalized;
    setMaskBits(127);
    return true;
}

bool SimNativeLight::onAdd() {
    return setState(state_) && SimContainer::onAdd() && addToSet(sim::LightSetId);
}

bool SimNativeLight::setMarker(TsShapeResources resources) {
    auto next = std::make_unique<SimTsShape>();
    if (!next->setResources(std::move(resources))) {
        return false;
    }
    const auto local = next->getLocalBoundingBox();
    if (!setBoundingBox({local.fMin + state_.position, local.fMax + state_.position})) {
        return false;
    }
    marker_ = std::move(next);
    return true;
}

bool SimNativeLight::edit(bool enabled) {
    if (getManager()) {
        if (enabled) {
            auto* root =
                dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
            auto* set = dynamic_cast<sim::SimSet*>(getManager()->findObject(sim::RenderSetId));
            if (!root || !set) {
                return false;
            }
            const bool member = std::ranges::find(set->objects(), this) != set->objects().end();
            if (!addToSet(sim::RenderSetId)) {
                return false;
            }
            if (!root->addObject(*this)) {
                if (!member) {
                    removeFromSet(sim::RenderSetId);
                }
                return false;
            }
        } else {
            removeFromSet(sim::RenderSetId);
            if (getContainer()) {
                getContainer()->removeObject(*this);
            }
        }
    }
    editing_ = enabled;
    return true;
}

bool SimNativeLight::processEvent(const sim::SimEvent& event) {
    if (const auto* e = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
        auto next = state_;
        next.position = e->transform.p;
        return setState(next);
    }
    if (const auto* e = dynamic_cast<const sim::SimTriggerEvent*>(&event)) {
        if (!e->activate || !std::isfinite(e->value) || e->value < 0) {
            return false;
        }
        auto next = state_;
        next.intensity = {e->value, e->value, e->value};
        return setState(next);
    }
    if (const auto* e = dynamic_cast<const sim::SimEditEvent*>(&event)) {
        return e->editor != sim::MissionEditorId || edit(e->state);
    }
    return SimContainer::processEvent(event);
}

bool SimNativeLight::processQuery(sim::SimQuery& query) {
    if (auto* q = dynamic_cast<sim::SimLightQuery*>(&query)) {
        q->ambient = state_.ambient;
        q->lights.clear();
        if (state_.type != 2 || state_.range > 0) {
            q->lights.push_back(state_.light());
        }
        return true;
    }
    if (auto* q = dynamic_cast<sim::SimImageTransformQuery*>(&query)) {
        q->transform.identity();
        q->transform.p = state_.position;
        q->transform.flags = RMat3F::Matrix_HasTranslation;
        return true;
    }
    if (editing_ && state_.type == 2) {
        if (auto* q = dynamic_cast<WorldShapeQuery*>(&query); q && marker_) {
            WorldShapeQuery prepared;
            if (!marker_->processQuery(prepared) || !prepared.image) {
                return false;
            }
            prepared.image->shapeToWorld.p = state_.position;
            prepared.image->shapeToWorld.flags |= RMat3F::Matrix_HasTranslation;
            q->image = std::move(prepared.image);
            return true;
        }
        if (auto* q = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
            auto image = std::make_shared<sim::SimCollisionBoxImage>();
            image->box = marker_ ? marker_->getLocalBoundingBox()
                                 : Box3F{{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
            image->transform.p = state_.position;
            image->transform.flags = RMat3F::Matrix_HasTranslation;
            q->images = {std::move(image)};
            return true;
        }
    }
    return SimContainer::processQuery(query);
}

UInt32 SimNativeLight::packUpdate(sim::network::GhostManager&, UInt32 mask, BitStream& b) const {
    return state_.pack(b, mask) ? 0 : mask;
}

bool SimNativeLight::unpackUpdate(sim::network::GhostManager&, BitStream& b) {
    auto next = state_;
    return next.unpack(b) && setState(next);
}

Persistent::Error
SimNativeLight::write(StreamIO& s, Int32, Int32 u, Persistent::StoreContext& c) const {
    return SimNetObject::write(s, 0, u, c) == Persistent::Error::Ok && state_.write(s)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimNativeLight::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    if (getManager()) {
        return Persistent::Error::ReadError;
    }
    SimNativeLight staged;
    LightState next;
    if (staged.SimNetObject::read(s, 0, u, c) != Persistent::Error::Ok || !next.read(s, v) ||
        !setState(next)) {
        return Persistent::Error::ReadError;
    }
    setId(staged.getId());
    setNetFlags(netFlags() | staged.netFlags());
    return Persistent::Error::Ok;
}

SimPlace::SimPlace() {
    installDatabase(std::make_unique<sim::SimXYDB>());
    setContainerMask(~0U);
}

bool SimPlace::onAdd() {
    if (!SimContainer::onAdd() || !getManager()) {
        return false;
    }
    auto* root = dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId));
    return root && root->addObject(*this);
}

SimMissionTerrain* SimPlace::terrain() const {
    return getManager() ? dynamic_cast<SimMissionTerrain*>(getManager()->findObject(sim::TerrainId))
                        : nullptr;
}

float SimPlace::farDistance() const {
    const auto* t = terrain();
    return t ? t->policy().visibleDistance : 10E10f;
}

bool SimPlace::renderChild(const sim::EnvironmentCamera& camera, const Box3F& box) const {
    auto inverse = camera.worldToCamera;
    inverse.inverse();
    return TerrainMissionPolicy::scopeChild(inverse.p, farDistance(), box);
}

float SimPlace::haze(Point3F camera, Point3F position) const {
    const auto* t = terrain();
    if (!finite(camera) || !finite(position)) {
        return 0;
    }
    if (!t) {
        if (const auto* parent = dynamic_cast<const SimPlace*>(getContainer())) {
            return parent->haze(camera, position);
        }
        return 0;
    }
    return t->policy().haze(camera, position);
}

void SimPlace::enterContainer(sim::SimContainer& object) {
    SimContainer::enterContainer(object);
    if (const auto* t = terrain()) {
        if (auto* mover = dynamic_cast<sim::SimMovement*>(&object)) {
            mover->addForce(sim::MovementGravityForce, t->state().gravity, true);
            mover->setContainerDrag(t->state().drag);
            mover->setContainerCurrent({}, 0);
        }
    }
}

void SimPlace::leaveContainer(sim::SimContainer& object) {
    if (auto* mover = dynamic_cast<sim::SimMovement*>(&object)) {
        mover->removeForce(sim::MovementGravityForce);
    }
    SimContainer::leaveContainer(object);
}

std::array<BoxFace, 6> boxFaces(const Box3F& box) {
    const auto& a = box.fMin;
    const auto& g = box.fMax;
    const Point3F b{a.x, a.y, g.z}, c{g.x, a.y, g.z}, d{g.x, a.y, a.z}, e{a.x, g.y, a.z},
        f{a.x, g.y, g.z}, h{g.x, g.y, a.z};
    return {{{{a, b, c, d}, 253},
             {{e, h, g, f}, 253},
             {{a, e, f, b}, 252},
             {{d, c, g, h}, 252},
             {{b, f, g, c}, 251},
             {{a, d, h, e}, 251}}};
}

std::shared_ptr<const WorldSurfaceMesh> boxSurface(const Box3F& box) {
    if (!finite(box.fMin) || !finite(box.fMax) || box.fMin.x >= box.fMax.x ||
        box.fMin.y >= box.fMax.y || box.fMin.z >= box.fMax.z) {
        return {};
    }
    auto mesh = std::make_shared<WorldSurfaceMesh>();
    mesh->bounds = box;
    auto& materials = mesh->materials;
    materials.list.details = 1;
    materials.list.materialsPerDetail = 3;
    materials.textures.resize(3);
    for (Int32 i = 0; i < 3; ++i) {
        TsMaterial m;
        m.flags = 1;
        m.index = 253 - i;
        materials.list.materials.push_back(m);
    }
    const auto faces = boxFaces(box);
    for (const auto& face : faces) {
        for (const auto& indices :
             std::array<std::array<std::size_t, 3>, 2>{{{0, 1, 2}, {0, 2, 3}}}) {
            StaticSurfaceTriangle triangle;
            triangle.material = 253 - face.color;
            for (std::size_t i = 0; i < 3; ++i) {
                triangle.vertices[i].position = face.points[indices[i]];
            }
            mesh->triangles.push_back(std::move(triangle));
        }
    }
    return mesh;
}

std::optional<TMat3F>
faceDirection(Point3F y, std::optional<Point3F> axis, std::optional<float> spin) {
    if (!finite(y) || std::abs(y.len() - 1.f) > .0002f ||
        (axis && (!finite(*axis) || std::abs(axis->len() - 1.f) > .0002f)) ||
        (spin && !std::isfinite(*spin))) {
        return {};
    }
    Point3F x, z;
    if (axis) {
        z = *axis;
        m_cross(y, z, x);
        if (m_dot(x, x) < .00001f) {
            m_cross(y, std::abs(y.z) < .95f ? Point3F{0, 0, 1} : Point3F{1, 0, 0}, x);
        }
        x.normalize();
        m_cross(z, x, y);
    } else if (std::abs(y.z) < .95f) {
        m_cross(y, Point3F{0, 0, 1}, x);
        x.normalize();
        m_cross(x, y, z);
    } else {
        m_cross(Point3F{1, 0, 0}, y, z);
        z.normalize();
        m_cross(y, z, x);
    }
    TMat3F result;
    result.setRow(0, x);
    result.setRow(1, y);
    result.setRow(2, z);
    result.flags = RMat3F::Matrix_HasRotation;
    if (spin && !axis) {
        RMat3F rotation(EulerF{0, *spin, 0});
        m_mul(rotation, result, result);
    }
    return result;
}

float effectSortValue(bool translucent, float distance, float nearDistance) {
    if (!translucent) {
        return 0;
    }
    const float d = distance - .1f;
    return d <= nearDistance ? .99f : nearDistance / d;
}

bool SimEffectShape::resources(TsShapeResources opaque, TsShapeResources translucent) {
    if (!opaque.shape || !translucent.shape) {
        return false;
    }
    TsShapeInstance a(opaque.shape), b(translucent.shape);
    if (!opaque.shape->sequences.empty() && !a.addThread(0)) {
        return false;
    }
    if (!translucent.shape->sequences.empty() && !b.addThread(0)) {
        return false;
    }
    if (!a.pose() || !b.pose()) {
        return false;
    }
    opaque_ = std::move(opaque);
    translucent_ = std::move(translucent);
    opaqueInstance_ = std::move(a);
    translucentInstance_ = std::move(b);
    return true;
}

bool SimEffectShape::translucent(bool enabled) {
    auto& old = translucentMode_ ? translucentInstance_ : opaqueInstance_;
    auto& next = enabled ? translucentInstance_ : opaqueInstance_;
    if (!old || !next) {
        return false;
    }
    auto staged = *next;
    if (!old->threads().empty() && !staged.threads().empty() &&
        !staged.setSequence(0, staged.threads()[0].sequence, old->threads()[0].position)) {
        return false;
    }
    next = std::move(staged);
    translucentMode_ = enabled;
    return true;
}

bool SimEffectShape::advance(double dt) {
    auto& instance = translucentMode_ ? translucentInstance_ : opaqueInstance_;
    return instance && instance->advance(dt);
}

bool SimEffectShape::faceCamera(Point3F camera,
                                bool away,
                                std::optional<Point3F> axis,
                                std::optional<float> spin) {
    auto direction = transform.p - camera;
    if (!finite(direction)) {
        return false;
    }
    if (direction.len() == 0) {
        direction = {0, 1, 0};
    } else {
        direction.normalize();
    }
    if (away) {
        direction *= -1;
    }
    auto rotation = faceDirection(direction, axis, spin);
    if (!rotation) {
        return false;
    }
    rotation->p = transform.p;
    rotation->flags |= transform.flags & RMat3F::Matrix_HasTranslation;
    transform = *rotation;
    return true;
}

bool SimEffectShape::onAdd() {
    return SimObject::onAdd() && addToSet(sim::RenderSetId);
}

bool SimEffectShape::processQuery(sim::SimQuery& query) {
    if (auto* q = dynamic_cast<WorldShapeQuery*>(&query)) {
        const auto& instance = translucentMode_ ? translucentInstance_ : opaqueInstance_;
        if (!instance) {
            return false;
        }
        auto pose = instance->pose();
        if (!pose) {
            return false;
        }
        q->image = WorldShapeImage{
            translucentMode_ ? translucent_ : opaque_, std::move(*pose), transform, 0, true, 0};
        return true;
    }
    return SimObject::processQuery(query);
}

bool registerSceneObjectClasses(Persistent::ClassRegistry& r) {
    return r.registerClass<SimPath>("SimPath", makeFourCC('S', 'P', 'T', 'H')) &&
           r.registerClass<SimPathManager>("SimPathManager") &&
           r.registerClass<SimNativeLight>("SimLight", makeFourCC('S', 'l', 'i', 't'), 6) &&
           r.registerClass<SimPlace>("SimPlaceContainer");
}

std::shared_ptr<void> installLightFactory(sim::SimGame& game, std::string marker) {
    if (marker.empty() || marker.size() > 255 || marker.find('\0') != marker.npos) {
        throw std::invalid_argument("light marker name");
    }
    auto leases = std::make_shared<std::vector<std::shared_ptr<void>>>();
    leases->push_back(game.scopedLoadPreparation(
        "SimLight", [marker](sim::SimObject& object, ResourceManager& resources) {
            auto* light = dynamic_cast<SimNativeLight*>(&object);
            if (!light) {
                return true;
            }
            auto shape = TsShapeResources::load(resources, marker);
            return shape && light->setMarker(std::move(*shape));
        }));
    leases->push_back(game.scopedFactory(
        "SimLight", [&game, marker = std::move(marker)]() -> std::unique_ptr<sim::SimObject> {
            auto* manager = game.manager();
            auto* resources = manager ? sim::SimResource::find(*manager) : nullptr;
            if (!resources) {
                return {};
            }
            auto shape = TsShapeResources::load(resources->get(), marker);
            auto light = std::make_unique<SimNativeLight>();
            if (!shape || !light->setMarker(std::move(*shape))) {
                return {};
            }
            return light;
        }));
    return leases;
}

std::shared_ptr<void>
installEffectFactory(sim::SimGame& game, std::string opaque, std::string translucent) {
    if (opaque.empty() || translucent.empty() || opaque.size() > 255 || translucent.size() > 255 ||
        opaque.find('\0') != opaque.npos || translucent.find('\0') != translucent.npos) {
        throw std::invalid_argument("effect shape names");
    }
    return game.scopedFactory(
        "SimEffectShape",
        [&game,
         opaque = std::move(opaque),
         translucent = std::move(translucent)]() -> std::unique_ptr<sim::SimObject> {
            auto* manager = game.manager();
            auto* resources = manager ? sim::SimResource::find(*manager) : nullptr;
            if (!resources) {
                return {};
            }
            auto a = TsShapeResources::load(resources->get(), opaque),
                 b = TsShapeResources::load(resources->get(), translucent);
            auto effect = std::make_unique<SimEffectShape>();
            if (!a || !b || !effect->resources(std::move(*a), std::move(*b))) {
                return {};
            }
            return effect;
        });
}

bool registerSceneObjectNetwork(sim::network::Registry& r) {
    return r.registerGhost<SimNativeLight>(6) && r.registerEvent<PathEvent>(
                                                     sim::EventTypeFirst + 37,
                                                     [](const auto& e, auto& b, auto&) {
                                                         return e.packAddress(b) && e.pack(b);
                                                     },
                                                     [](auto& e, auto& b, auto&) {
                                                         return e.unpackAddress(b) && e.unpack(b);
                                                     },
                                                     sim::network::ReceivePolicy::Client);
}
} // namespace opent1::world
