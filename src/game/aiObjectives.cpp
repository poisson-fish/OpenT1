#include <opent1/game/ai.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <queue>
#include <sstream>

namespace opent1::game {

// -----------------------------------------------------------------------------
// AIGraph
// -----------------------------------------------------------------------------

Int32 AIGraph::addNode(const Point3F& pos, Int32 maxDirections) {
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
        nodes_.size() >= 4096) {
        return -1;
    }
    if (std::ranges::any_of(nodes_, [&](const auto& node) {
            return (node.pos - pos).len() < 2.f;
        })) {
        return -1;
    }
    const Int32 idx = static_cast<Int32>(nodes_.size());
    GraphPoint pt{};
    pt.pos = pos;
    pt.maxDirections = std::clamp(maxDirections, 0, 4);
    nodes_.push_back(pt);
    return idx;
}

Int32 AIGraph::loadNode(std::string_view text) {
    std::istringstream input{std::string(text)};
    Point3F pos;
    Flt32 radius;
    char directions;
    if (!(input >> pos.x >> pos.y >> pos.z >> radius >> directions) || !std::isfinite(radius)) {
        return -1;
    }
    // The historical format uses %c, a byte, rather than a decimal digit.
    const auto count = static_cast<unsigned char>(directions);
    if (count > 4) {
        return -1;
    }
    const auto index = addNode(pos, count);
    if (index >= 0) {
        nodes_[index].radius = radius;
    }
    return index;
}

std::string AIGraph::printNode(Int32 index) const {
    if (index < 0 || std::size_t(index) >= nodes_.size()) {
        return {};
    }
    const auto& node = nodes_[index];
    return std::format("{:.6f} {:.6f} {:.6f} {:.6f} {}",
                       node.pos.x,
                       node.pos.y,
                       node.pos.z,
                       node.radius,
                       char(node.maxDirections));
}

bool AIGraph::connectNodes(Int32 a, Int32 b) {
    if (a < 0 || b < 0 || a == b || static_cast<std::size_t>(a) >= nodes_.size() ||
        static_cast<std::size_t>(b) >= nodes_.size()) {
        return false;
    }
    GraphPoint& nodeA = nodes_[static_cast<std::size_t>(a)];
    GraphPoint& nodeB = nodes_[static_cast<std::size_t>(b)];
    const Flt32 dist = (nodeB.pos - nodeA.pos).len();

    auto insertEdge = [](GraphPoint& node, Int32 peer, Flt32 d) {
        for (std::size_t i = 0; i < std::size_t(node.maxDirections); ++i) {
            if (node.connections[i] == peer) {
                node.distances[i] = d;
                return true;
            }
            if (node.connections[i] == -1) {
                node.connections[i] = peer;
                node.distances[i] = d;
                return true;
            }
        }
        return false;
    };

    auto nextA = nodeA, nextB = nodeB;
    if (!insertEdge(nextA, b, dist) || !insertEdge(nextB, a, dist)) {
        return false;
    }
    nodeA = nextA;
    nodeB = nextB;
    return true;
}

void AIGraph::buildGraph(std::span<const Point3F> points, Flt32 maxLinkDist) {
    nodes_.clear();
    for (const Point3F& p : points) {
        addNode(p);
    }
    const Int32 n = static_cast<Int32>(nodes_.size());
    for (Int32 i = 0; i < n; ++i) {
        struct Candidate {
            Int32 peer{-1};
            Flt32 dist{0.0f};
        };

        std::vector<Candidate> cands{};
        for (Int32 j = 0; j < n; ++j) {
            if (i == j) {
                continue;
            }
            const Flt32 d =
                (nodes_[static_cast<std::size_t>(j)].pos - nodes_[static_cast<std::size_t>(i)].pos)
                    .len();
            if (d <= maxLinkDist) {
                cands.push_back(Candidate{j, d});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
            return a.dist < b.dist;
        });
        for (std::size_t k = 0; k < cands.size() && k < MaxConnections; ++k) {
            connectNodes(i, cands[k].peer);
        }
    }
}

Int32 AIGraph::findClosestNode(const Point3F& pos) const noexcept {
    Int32 best = -1;
    Flt32 bestDistSq = std::numeric_limits<Flt32>::max();
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const Flt32 dSq = (nodes_[i].pos - pos).lenSq();
        if (dSq < bestDistSq) {
            bestDistSq = dSq;
            best = static_cast<Int32>(i);
        }
    }
    return best;
}

bool AIGraph::getShortestPath(Int32 startNode, Int32 endNode, std::vector<Int32>& outPath) const {
    outPath.clear();
    const Int32 n = static_cast<Int32>(nodes_.size());
    if (startNode < 0 || endNode < 0 || startNode >= n || endNode >= n) {
        return false;
    }
    if (startNode == endNode) {
        outPath.push_back(startNode);
        return true;
    }

    std::vector<Flt32> dist(static_cast<std::size_t>(n), std::numeric_limits<Flt32>::max());
    std::vector<Int32> prev(static_cast<std::size_t>(n), -1);
    using QueueEntry = std::pair<Flt32, Int32>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> pq{};

    dist[static_cast<std::size_t>(startNode)] = 0.0f;
    pq.push(QueueEntry{0.0f, startNode});

    while (!pq.empty()) {
        const auto [curDist, u] = pq.top();
        pq.pop();
        if (u == endNode) {
            break;
        }
        if (curDist > dist[static_cast<std::size_t>(u)]) {
            continue;
        }
        const GraphPoint& node = nodes_[static_cast<std::size_t>(u)];
        for (std::size_t k = 0; k < MaxConnections; ++k) {
            const Int32 v = node.connections[k];
            if (v < 0 || v >= n) {
                continue;
            }
            const Flt32 nextDist = curDist + node.distances[k];
            if (nextDist < dist[static_cast<std::size_t>(v)]) {
                dist[static_cast<std::size_t>(v)] = nextDist;
                prev[static_cast<std::size_t>(v)] = u;
                pq.push(QueueEntry{nextDist, v});
            }
        }
    }

    if (prev[static_cast<std::size_t>(endNode)] == -1) {
        return false;
    }
    for (Int32 cur = endNode; cur != -1; cur = prev[static_cast<std::size_t>(cur)]) {
        outPath.push_back(cur);
    }
    std::reverse(outPath.begin(), outPath.end());
    return true;
}

// -----------------------------------------------------------------------------
// AIObj Directives & Wildcard Matching
// -----------------------------------------------------------------------------

bool AIObj::wildCardMatch(std::string_view key, std::string_view pattern) noexcept {
    std::size_t k = 0;
    std::size_t p = 0;
    std::size_t starIdx = std::string_view::npos;
    std::size_t matchIdx = 0;

    while (k < key.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || std::tolower(static_cast<unsigned char>(pattern[p])) ==
                                      std::tolower(static_cast<unsigned char>(key[k])))) {
            ++k;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starIdx = p++;
            matchIdx = k;
        } else if (starIdx != std::string_view::npos) {
            p = starIdx + 1;
            k = ++matchIdx;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool AIObj::orderMatch(Int32 order, std::string_view expr) noexcept {
    if (expr.empty() || expr == "*") {
        return true;
    }
    const std::string orderStr = std::format("{}", order);
    return wildCardMatch(orderStr, expr);
}

void AIObj::addDirective(const Directive& input, bool overwrite) {
    auto directive = input;
    directive.callbacks = {};
    directive.nativeCallbacks = {};
    if (directive.orderMask & TargetDirective) {
        directive.nativeCallbacks = {"AI::onTargetLOSAcquired",
                                     "AI::onTargetDied",
                                     "AI::onTargetLOSLost",
                                     "AI::onTargetLOSRegained"};
    }
    if (directive.orderMask & (WaypointDirective | FollowDirective)) {
        pathDone_ = false;
    }
    if (directive.order > 0) {
        for (auto& existing : directives_) {
            if (existing.order == directive.order) {
                if (overwrite) {
                    existing = directive;
                }
                return;
            }
        }
    } else {
        // aiHelp.cpp keeps the existing hundred bucket here (it does not add 100).
        constexpr Int32 maxUserOrder = (0x0Bacb000 << 3) >> 1;
        Int32 order = directives_.empty() ? 100 : directives_.back().order;
        if (order >= maxUserOrder) {
            order = 100;
            for (auto entry = directives_.rbegin(); entry != directives_.rend(); ++entry) {
                if (entry->order < maxUserOrder - 150) {
                    order = entry->order + 150;
                    break;
                }
            }
        }
        directive.order = order - order % 100;
    }
    directives_.push_back(std::move(directive));
    std::stable_sort(
        directives_.begin(), directives_.end(), [](const auto& left, const auto& right) {
            return left.order < right.order;
        });
}

Int32 AIObj::removeDirectives(std::string_view ordExpr) {
    Int32 removed = 0;
    for (std::size_t i = 0; i < directives_.size();) {
        if (orderMatch(directives_[i].order, ordExpr)) {
            directives_.erase(directives_.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        } else {
            ++i;
        }
    }
    return removed;
}

std::vector<AIObj::Directive> AIObj::listDirectives(UInt32 mask) const {
    std::vector<Directive> out{};
    for (const auto& d : directives_) {
        if ((d.orderMask & mask) != 0) {
            out.push_back(d);
        }
    }
    return out;
}

bool AIObj::setCallback(Int32 callbackType, std::string_view funcName, Flt32 period) {
    if (callbackType < 0 || callbackType >= NumCallbackTypes) {
        return false;
    }
    callbacks_[static_cast<std::size_t>(callbackType)].assign(funcName.data(), funcName.size());
    if (callbackType == Periodic) {
        setPeriodicDelay(period);
    }
    return true;
}

bool AIObj::installDirectiveCallback(Int32 number,
                                     std::string_view function,
                                     std::string_view orderExpression,
                                     UInt32 typeMask) {
    if (number < 1 || number > 4) {
        return false;
    }
    for (auto& directive : directives_) {
        if ((directive.orderMask & typeMask) &&
            (orderExpression.empty() || orderMatch(directive.order, orderExpression))) {
            directive.nativeCallbacks[std::size_t(number - 1)] = function;
        }
    }
    return true;
}

void AIObj::invokeCallback(Int32 callbackType, std::string_view extraArg) {
    if (callbackType < 0 || callbackType >= NumCallbackTypes) {
        return;
    }
    const std::string& fn = callbacks_[static_cast<std::size_t>(callbackType)];
    if (fn.empty()) {
        return;
    }
    if (aiManager_) {
        aiManager_->recordCallback(fn, name, extraArg);
    }
}

void AIObj::findTargets() {
    currentTarget_ = {};
    auto* manager = aiManager_ ? aiManager_->getManager() : nullptr;
    auto* players = PlayerManager::get(manager);
    if (!players || !player_) {
        return;
    }
    const auto resolve = [&](Int32 targetId) -> Player* {
        if (auto* ai = aiManager_->findByPlayerId(targetId); ai && ai->player()) {
            return ai->player();
        }
        auto* rep = players->findBaseRep(targetId);
        if (auto* client = dynamic_cast<PlayerManager::ClientRep*>(rep)) {
            return dynamic_cast<Player*>(client->ownedObject ? client->ownedObject
                                         : client->playerPSC ? client->playerPSC->getControlObject()
                                                             : nullptr);
        }
        if (auto* ai = dynamic_cast<AIObj*>(rep)) {
            return ai->player();
        }
        return nullptr;
    };
    std::vector<Int32> ids;
    if (automaticTargets_) {
        for (auto* rep = players->getBaseRepList(); rep; rep = rep->nextClient) {
            if (rep->id != id && (rep->team != team || players->getNumTeams() <= 1) &&
                resolve(rep->id)) {
                ids.push_back(rep->id);
            }
        }
    }
    for (auto& directive : directives_) {
        if ((directive.orderMask & TargetDirective) && directive.targetMode == PlayerTarget) {
            const auto targetId =
                directive.targetPlayerId >= 0 ? directive.targetPlayerId : directive.playerId;
            if (std::ranges::find(ids, targetId) == ids.end()) {
                ids.push_back(targetId);
            }
            directive.activeTarget = {};
        }
    }
    for (const auto targetId : ids) {
        if (std::ranges::none_of(targetVisibility_, [targetId](const auto& state) {
                return state.id == targetId;
            })) {
            targetVisibility_.push_back({targetId});
        }
    }
    const auto callback = [&](Int32 type, Int32 targetId) {
        bool scripted = false;
        for (const auto& directive : directives_) {
            if ((directive.orderMask & TargetDirective) &&
                (directive.targetPlayerId == targetId || directive.playerId == targetId)) {
                const std::size_t slot = type == TargetAcquired  ? 0
                                         : type == TargetKilled  ? 1
                                         : type == TargetLOSLost ? 2
                                                                 : 3;
                const auto& function = directive.nativeCallbacks[slot].empty()
                                           ? directive.callbacks[type]
                                           : directive.nativeCallbacks[slot];
                if (!function.empty()) {
                    aiManager_->recordCallback(function, name, std::to_string(targetId));
                    scripted = true;
                }
            }
        }
        if (!scripted) {
            invokeCallback(type, std::to_string(targetId));
        }
    };
    std::vector<TargetVisibility*> eligible;
    for (auto& state : targetVisibility_) {
        auto* candidate = resolve(state.id);
        if (!candidate || candidate->isDead()) {
            if (!state.dead && std::ranges::find(ids, state.id) != ids.end()) {
                callback(TargetKilled, state.id);
                state.dead = true;
            }
            state.visible = state.seen = false;
            continue;
        }
        const bool wanted = std::ranges::find(ids, state.id) != ids.end();
        const bool inRange =
            (candidate->getBoxCenter() - player_->getBoxCenter()).len() < iqVars_.spotDistance();
        if (!wanted || !inRange) {
            state.visible = state.seen = false;
            continue;
        }
        eligible.push_back(&state);
    }
    // Native LOS work cycles through one eligible target every five ticks.
    if (++losThinkCounter_ >= 5 && !eligible.empty()) {
        losThinkCounter_ = 0;
        auto next = std::ranges::find_if(eligible, [&](const auto* state) {
            return state->id == lastLosCheckId_;
        });
        if (next == eligible.end() || ++next == eligible.end()) {
            next = eligible.begin();
        }
        auto& state = **next;
        auto* candidate = resolve(state.id);
        auto* root = dynamic_cast<sim::SimContainer*>(manager->findObject(sim::RootContainerId));
        bool visible = root != nullptr;
        if (root) {
            sim::SimContainerQuery query;
            query.id = player_->getId();
            query.mask = ~UInt32(SimProjectileObjectType | SimCameraObjectType | ItemObjectType);
            query.box.fMin = player_->getEyeTransform().p;
            query.box.fMax = candidate->getEyeTransform().p;
            sim::SimCollisionInfo hit;
            visible = !root->findLOS(query, hit) || hit.object.resolve(*manager) == candidate;
        }
        if (visible && !state.visible) {
            callback(state.seen ? TargetLOSRegained : TargetAcquired, state.id);
        } else if (!visible && state.visible) {
            callback(TargetLOSLost, state.id);
        }
        state.visible = visible;
        state.seen = state.seen || visible;
        lastLosCheckId_ = state.id;
    }
    Flt32 distance = std::numeric_limits<Flt32>::max();
    for (const auto* state : eligible) {
        if (!state->visible) {
            continue;
        }
        auto* candidate = resolve(state->id);
        const Flt32 range = (candidate->getBoxCenter() - player_->getBoxCenter()).len();
        DynamicTarget target{
            state->id, candidate->getBoxCenter(), candidate->getLeadVelocity(), range, candidate};
        if (range < distance) {
            currentTarget_ = target;
            distance = range;
        }
        for (auto& directive : directives_) {
            if ((directive.orderMask & TargetDirective) &&
                (directive.targetPlayerId == state->id || directive.playerId == state->id)) {
                directive.activeTarget = target;
            }
        }
    }
}

void AIObj::updateDirectives(SimTime dt, PlayerMove& pm) {
    const Point3F myPos = player_ ? player_->getBoxCenter() : Point3F{};
    bool hasMoveTarget = false;

    struct RoutePoint {
        Directive* directive;
        Int32 point;
        Point3F position;
    };

    std::vector<RoutePoint> route;
    for (auto& directive : directives_) {
        if (directive.orderMask & WaypointDirective) {
            if (directive.pts.empty()) {
                route.push_back({&directive, 0, directive.pos});
            } else {
                for (std::size_t n = 0; n < directive.pts.size(); ++n) {
                    route.push_back({&directive, Int32(n), directive.pts[n]});
                }
            }
        } else if (directive.orderMask & FollowDirective) {
            Player* follow = nullptr;
            if (aiManager_) {
                if (auto* ai = aiManager_->findByPlayerId(directive.playerId)) {
                    follow = ai->player();
                }
                if (!follow) {
                    if (auto* players = PlayerManager::get(aiManager_->getManager())) {
                        if (auto* client = players->findClient(directive.playerId)) {
                            follow = dynamic_cast<Player*>(client->ownedObject);
                        }
                    }
                }
            }
            route.push_back({&directive, 0, follow ? follow->getBoxCenter() + seekOff_ : myPos});
        }
    }
    std::vector<std::pair<std::string, Int32>> reached;
    if (!route.empty() && !pathDone_) {
        auto current = std::ranges::find_if(route, [&](const auto& point) {
            return point.directive->order == pathOrder_ && point.point == pathPoint_;
        });
        Int32 index = current != route.end() && pathActive_ ? Int32(current - route.begin()) : -1;
        const auto begin = [&](Int32 next, const Point3F& from) {
            index = next;
            const auto& point = route[std::size_t(index)];
            pathOrder_ = point.directive->order;
            pathPoint_ = point.point;
            pathActive_ = true;
            lastWaypoint_ = from;
            jetSkill_.stage = JetSkill::NoNav;
            if (jetNavigation_ != 0) {
                jetSkill_.init(*this, point.position);
                jetSkill_.startPos = from;
                jetSkill_.midpoint = (from + point.position) * .5f;
                jetSkill_.midpoint.z += 40.f;
            }
        };
        if (index < 0) {
            auto next = std::ranges::find_if(route, [&](const auto& point) {
                return point.directive->order > pathOrder_;
            });
            if (pathType_ == TwoWay && pathDirection_ < 0) {
                next = route.end();
                for (auto point = route.begin();
                     point != route.end() && point->directive->order < pathOrder_;
                     ++point) {
                    next = point;
                }
            }
            if (next == route.end() && pathType_ == TwoWay) {
                next = pathDirection_ > 0 ? std::prev(route.end()) : route.begin();
                pathDirection_ = -pathDirection_;
            }
            if (next != route.end()) {
                begin(Int32(next - route.begin()), myPos);
            } else {
                pathDone_ = true;
                pathActive_ = false;
            }
        }
        for (std::size_t attempts = 0; index >= 0 && attempts < route.size(); ++attempts) {
            const auto point = route[std::size_t(index)];
            const auto delta = point.position - myPos;
            const bool within = jetSkill_.stage != JetSkill::NoNav
                                    ? delta.len() < .2f
                                    : std::hypot(delta.x, delta.y) < 1.2f;
            if ((point.directive->orderMask & FollowDirective) || !within) {
                curDest_ = point.position;
                hasMoveTarget = true;
                break;
            }
            const auto& callback = point.directive->nativeCallbacks[0].empty()
                                       ? point.directive->callbacks[WaypointReached]
                                       : point.directive->nativeCallbacks[0];
            reached.emplace_back(callback.empty() ? callbacks_[WaypointReached] : callback,
                                 point.directive->order);
            Int32 next = index + pathDirection_;
            if (route.size() < 2) {
                next = -1;
            } else if (pathType_ == TwoWay) {
                if (index == 0) {
                    pathDirection_ = 1;
                }
                if (std::size_t(index + 1) == route.size()) {
                    pathDirection_ = -1;
                }
                next = index + pathDirection_;
            } else {
                next = index + 1;
                if (std::size_t(next) == route.size()) {
                    next = pathType_ == Circular ? 0 : -1;
                }
            }
            if (next < 0) {
                pathDone_ = true;
                pathActive_ = false;
                jetSkill_.stage = JetSkill::NoNav;
                break;
            }
            begin(next, point.position);
        }
    }
    // Select in directive order. Native player targets compete by distance;
    // point/laser targets replace the current selection without resetting that distance.
    DynamicTarget chosenTarget{};
    Int32 targetMode = NoTarget;
    Point3F targetPoint{};
    Int32 laserOwner = -1;
    Flt32 bestDistance = std::numeric_limits<Flt32>::max();
    for (const auto& directive : directives_) {
        if (!(directive.orderMask & TargetDirective)) {
            continue;
        }
        if (directive.targetMode == PlayerTarget) {
            if (directive.activeTarget.id >= 0 &&
                (targetMode == NoTarget || directive.activeTarget.dist < bestDistance)) {
                chosenTarget = directive.activeTarget;
                bestDistance = chosenTarget.dist;
                targetMode = PlayerTarget;
            }
        } else {
            targetMode = directive.targetMode;
            targetPoint = directive.pos;
            laserOwner = directive.targetPlayerId;
        }
    }
    if (automaticTargets_ && currentTarget_.id >= 0 &&
        (targetMode == NoTarget || currentTarget_.dist < bestDistance)) {
        chosenTarget = currentTarget_;
        targetMode = PlayerTarget;
    }
    if (player_ && player_->isDead()) {
        targetMode = NoTarget;
    }
    const auto timeToThink = [&](bool force) {
        if (force || ++thinkCounter_ >= iqVars_.thinkWait) {
            thinkCounter_ = 0;
        }
        return thinkCounter_ == 0;
    };
    const auto oldTarget = [&](bool force) -> DynamicTarget {
        if (seekFlag_ <= 2048 || !timeToThink(force) || !aiManager_ || !player_) {
            return {};
        }
        auto* manager = aiManager_->getManager();
        auto* players = PlayerManager::get(manager);
        auto* rep = players ? players->findClient(seekFlag_) : nullptr;
        auto* target = rep ? dynamic_cast<Player*>(rep->ownedObject) : nullptr;
        if (auto* ai = aiManager_->findByPlayerId(seekFlag_)) {
            target = ai->player();
        }
        if (!target || target->isDead()) {
            return {};
        }
        const Flt32 distance = m_distf(target->getBoxCenter(), myPos);
        if (distance >= iqVars_.spotDistance()) {
            return {};
        }
        auto* root =
            manager ? dynamic_cast<sim::SimContainer*>(manager->findObject(sim::RootContainerId))
                    : nullptr;
        if (!root) {
            return {};
        }
        sim::SimContainerQuery query;
        query.id = player_->getId();
        query.mask = ~UInt32(SimProjectileObjectType | SimCameraObjectType | ItemObjectType);
        query.box.fMin = player_->getEyeTransform().p;
        query.box.fMax = target->getEyeTransform().p;
        sim::SimCollisionInfo hit;
        if (root->findLOS(query, hit) && hit.object.resolve(*manager) != target) {
            return {};
        }
        return {seekFlag_, target->getBoxCenter(), target->getLeadVelocity(), distance, target};
    };
    const auto attack = [&](const DynamicTarget& target, Int32 mode) {
        if (mode == PlayerTarget) {
            fireAtPlayerTarget(&pm, target);
        } else if (mode == LaserTarget) {
            fireAtLaserOwner(&pm, laserOwner);
        } else if (mode == PointTarget) {
            fireAtPointTarget(&pm, targetPoint);
        }
        if (mode == PointTarget) {
            return;
        }
        if (attackMode_ == 0 && hasMoveTarget) {
            if (jetSkill_.stage != JetSkill::NoNav) {
                (void)jetSkill_.jetToPoint(*this, pm, curDest_);
            } else {
                setMovementAction(pm,
                                  getAngleSeekZ(curDest_, myPos, player_->getRot().z),
                                  getSpeed(m_distf(curDest_, myPos)));
            }
        } else if (attackMode_ == 1 && mode == PlayerTarget) {
            Point3F destination = target.obj ? target.obj->getBoxCenter() : target.pos;
            if (m_distf(Point2F(destination.x, destination.y), Point2F(myPos.x, myPos.y)) < 17.f) {
                auto away = destination - myPos;
                away.normalize();
                destination -= away * 17.f;
            }
            setMovementAction(pm,
                              getAngleSeekZ(destination, myPos, player_->getRot().z),
                              getSpeed(m_distf(destination, myPos)));
        }
    };
    switch (state_) {
        case State::NeedToThink:
        case State::DoNothing:
            if (timeToThink(state_ == State::NeedToThink)) {
                if (!pathDone_ && !route.empty()) {
                    state_ = State::FollowingPath;
                } else if (seekFlag_) {
                    state_ = State::AttackStill;
                } else if (targetMode != NoTarget) {
                    state_ = State::AttackTarget;
                } else {
                    state_ = State::DoNothing;
                }
            }
            break;
        case State::FollowingPath:
            if (hasMoveTarget) {
                seekLocation(&pm);
            }
            if (oldTarget(false).id >= 0) {
                state_ = State::AttackStill;
            } else if (targetMode != NoTarget) {
                state_ = State::AttackTarget;
            }
            break;
        case State::AttackStill: {
            const auto target = oldTarget(true);
            if (target.id >= 0) {
                attack(target, PlayerTarget);
            } else {
                state_ = State::NeedToThink;
            }
            break;
        }
        case State::AttackTarget:
            if (targetMode != NoTarget) {
                attack(chosenTarget, targetMode);
            } else {
                state_ = State::NeedToThink;
            }
            break;
    }
    (void)dt;
    for (const auto& entry : reached) {
        if (aiManager_ && !entry.first.empty()) {
            aiManager_->recordCallback(entry.first, name);
        }
    }
}

void AIObj::doServerProcesses(SimTime curTime) {
    const SimTime dt = (lastProcessTime_ > 0.0f && curTime > lastProcessTime_)
                           ? (curTime - lastProcessTime_)
                           : 0.032f;
    lastProcessTime_ = curTime;

    currentTargetId_ = -1;
    trigger_.preLoop();
    if (auto* players = PlayerManager::get(aiManager_ ? aiManager_->getManager() : nullptr)) {
        if (const auto* rep = players->findBaseRep(id)) {
            team = rep->team;
        }
    }
    if (player_ && player_->isDead()) {
        seekFlag_ = 0;
        currentTarget_ = {};
    } else if (!seekFlag_) {
        findTargets();
    } else {
        currentTarget_ = {};
    }

    PlayerMove pm{};
    pm.reset();
    updateDirectives(dt, pm);
    lastMove_ = pm;

    if (player_) {
        const std::array<PlayerMove, 1> moves{pm};
        player_->serverUpdateMove(moves);
    }
}

} // namespace opent1::game
