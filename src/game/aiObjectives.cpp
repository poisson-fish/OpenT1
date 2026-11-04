#include <opent1/game/ai.hpp>
#include <ranges>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <queue>
#include <sstream>

namespace opent1::game {

namespace {
// aiHelp.cpp reserves these orders for automatically enrolled enemy BaseReps.
constexpr Int32 autoOrderReserve = 0x0Bacb000 << 3;
constexpr Int32 autoOrderMask = 0x0FFFF000 << 3;

bool isAutomaticTarget(const AIObj::Directive& directive) {
    return (directive.orderMask & AIObj::TargetDirective) &&
           (directive.order & autoOrderMask) == autoOrderReserve;
}
}

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
        for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                          return i < std::size_t(node.maxDirections);
                      })) {
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
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < n;
                  })) {
        struct Candidate {
            Int32 peer{-1};
            Flt32 dist{0.0f};
        };

        std::vector<Candidate> cands{};
        for (auto j : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 j) {
                          return j < n;
                      })) {
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
        for (auto k : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t k) {
                          return k < cands.size() && k < MaxConnections;
                      })) {
            connectNodes(i, cands[k].peer);
        }
    }
}

Int32 AIGraph::findClosestNode(const Point3F& pos) const noexcept {
    Int32 best = -1;
    Flt32 bestDistSq = std::numeric_limits<Flt32>::max();
    for (auto i : std::views::iota(std::size_t{0}, nodes_.size())) {
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
        for (auto k : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t k) {
                          return k < MaxConnections;
                      })) {
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
    directive.identity = ++nextDirectiveIdentity_;
    directive.haveLOS = directive.hadLOS = directive.validLOS = directive.targetWasDead = false;
    directive.activeTarget = {};
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
            for (const auto& entry : directives_ | std::views::reverse) {
                if (entry.order < maxUserOrder - 150) {
                    order = entry.order + 150;
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

void AIObj::setAutomaticTargets(bool autoTarg) noexcept {
    automaticTargets_ = autoTarg;
    if (!autoTarg) {
        std::erase_if(directives_, isAutomaticTarget);
    }
}

AIObj::Directive* AIObj::findDirectiveIdentity(UInt64 identity) noexcept {
    const auto found = std::ranges::find(directives_, identity, &Directive::identity);
    return found == directives_.end() ? nullptr : &*found;
}

void AIObj::refreshAutomaticTargets() {
    auto* players = PlayerManager::get(aiManager_ ? aiManager_->getManager() : nullptr);
    if (!automaticTargets_ || !players || !player_)
        return;
    const auto isEnemy = [&](const PlayerManager::BaseRep* rep) {
        return rep && rep->id != id && (rep->team != team || players->getNumTeams() <= 1);
    };
    std::erase_if(directives_, [&](const auto& directive) {
        return isAutomaticTarget(directive) &&
               !isEnemy(players->findBaseRep(directive.targetPlayerId));
    });
    for (auto* rep = players->getBaseRepList(); rep; rep = rep->nextClient) {
        const auto order = autoOrderReserve | rep->id;
        if (isEnemy(rep) && std::ranges::none_of(directives_, [&](const auto& directive) {
                return directive.order == order;
            })) {
            Directive directive;
            directive.orderMask = TargetDirective;
            directive.targetMode = PlayerTarget;
            directive.targetPlayerId = rep->id;
            directive.order = order;
            addDirective(directive);
        }
    }
}

void AIObj::findTargets() {
    currentTarget_ = {};
    auto* manager = aiManager_ ? aiManager_->getManager() : nullptr;
    auto* players = PlayerManager::get(manager);
    if (!players || !player_)
        return;
    const auto targetId = [](const Directive& directive) {
        return directive.targetPlayerId >= 0 ? directive.targetPlayerId : directive.playerId;
    };
    const auto resolve = [&](Int32 representativeId) -> Player* {
        const auto* rep = players->findBaseRep(representativeId);
        return rep ? dynamic_cast<Player*>(rep->ownedObject) : nullptr;
    };
    // Callback scripts may erase, replace or append directives. Snapshot identities,
    // then reacquire references after each synchronous callback.
    std::vector<UInt64> targets;
    for (auto& directive : directives_) {
        if (!(directive.orderMask & TargetDirective))
            continue;
        targets.push_back(directive.identity);
        directive.activeTarget = {};
        auto* candidate =
            directive.targetMode == PlayerTarget ? resolve(targetId(directive)) : nullptr;
        directive.validLOS = directive.targetMode != PlayerTarget ||
                             (candidate && !candidate->isDead() &&
                              m_distf(player_->getLeadCenter(), candidate->getLeadCenter()) <
                                  iqVars_.spotDistance());
        if (!directive.validLOS)
            directive.haveLOS = directive.hadLOS = false;
    }
    const auto callback = [&](const Directive& directive, Int32 type) {
        const std::size_t slot = type == TargetAcquired  ? 0
                                 : type == TargetKilled  ? 1
                                 : type == TargetLOSLost ? 2
                                                         : 3;
        const std::string function = directive.nativeCallbacks[slot].empty()
                                         ? directive.callbacks[type]
                                         : directive.nativeCallbacks[slot];
        const auto representativeId = targetId(directive);
        if (!function.empty())
            aiManager_->recordCallback(function, name, std::to_string(representativeId));
    };
    if (!targets.empty() && ++losThinkCounter_ >= 5) {
        losThinkCounter_ = 0;
        std::size_t next = 0;
        if (lastLosCheckId_) {
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const auto* directive = findDirectiveIdentity(targets[i]);
                if (directive && targetId(*directive) == lastLosCheckId_) {
                    next = (i + 1) % targets.size();
                    break;
                }
            }
        }
        lastLosCheckId_ = 0;
        for (std::size_t count = 0; count < targets.size(); ++count) {
            auto* directive = findDirectiveIdentity(targets[(next + count) % targets.size()]);
            if (!directive || !directive->validLOS)
                continue;
            const auto identity = directive->identity;
            const auto representativeId = targetId(*directive);
            if (directive->targetMode == PlayerTarget) {
                auto* candidate = resolve(representativeId);
                auto* root =
                    dynamic_cast<sim::SimContainer*>(manager->findObject(sim::RootContainerId));
                bool visible = false;
                if (candidate && root) {
                    sim::SimContainerQuery query;
                    query.id = player_->getId();
                    query.mask =
                        ~UInt32(SimProjectileObjectType | SimCameraObjectType | ItemObjectType);
                    query.box.fMin = player_->getEyeTransform().p;
                    query.box.fMax = candidate->getEyeTransform().p;
                    sim::SimCollisionInfo hit;
                    visible =
                        !root->findLOS(query, hit) || hit.object.resolve(*manager) == candidate;
                }
                if (visible && !directive->haveLOS)
                    callback(*directive, directive->hadLOS ? TargetLOSRegained : TargetAcquired);
                else if (!visible && directive->haveLOS)
                    callback(*directive, TargetLOSLost);
                if (!player_)
                    return;
                directive = findDirectiveIdentity(identity);
                if (directive) {
                    directive->haveLOS = visible;
                    directive->hadLOS = directive->hadLOS || visible;
                }
            } else {
                directive->haveLOS = directive->hadLOS = true;
            }
            lastLosCheckId_ = representativeId;
            break;
        }
    }
    Flt32 bestDistance = std::numeric_limits<Flt32>::max();
    for (const auto identity : targets) {
        auto* directive = findDirectiveIdentity(identity);
        if (!directive || directive->targetMode != PlayerTarget)
            continue;
        const auto representativeId = targetId(*directive);
        auto* candidate = resolve(representativeId);
        if (!candidate || candidate->isDead()) {
            if (!directive->targetWasDead) {
                directive->targetWasDead = true;
                callback(*directive, TargetKilled);
                if (!player_)
                    return;
            }
            continue;
        }
        if (!directive->haveLOS)
            continue;
        const auto range = m_distf(candidate->getLeadCenter(), player_->getLeadCenter());
        directive->activeTarget = {representativeId,
                                   candidate->getLeadCenter(),
                                   candidate->getLeadVelocity(),
                                   range,
                                   candidate};
        if (range < bestDistance) {
            currentTarget_ = directive->activeTarget;
            bestDistance = range;
        }
    }
}

void AIObj::updateDirectives(SimTime dt, PlayerMove& pm) {
    if (!player_)
        return;
    const Point3F myPos = player_->getLeadCenter();
    bool hasMoveTarget = false;

    struct RoutePoint {
        Int32 order, point;
        UInt32 mask;
        Point3F position;
        std::string callback;
    };

    const auto buildRoute = [&] {
        std::vector<RoutePoint> route;
        for (const auto& directive : directives_) {
            const auto callback = directive.nativeCallbacks[0].empty()
                                      ? directive.callbacks[WaypointReached]
                                      : directive.nativeCallbacks[0];
            if (directive.orderMask & WaypointDirective) {
                if (directive.pts.empty()) {
                    route.push_back(
                        {directive.order, 0, directive.orderMask, directive.pos, callback});
                } else {
                    for (std::size_t i = 0; i < directive.pts.size(); ++i)
                        route.push_back({directive.order,
                                         Int32(i),
                                         directive.orderMask,
                                         directive.pts[i],
                                         callback});
                }
            } else if (directive.orderMask & FollowDirective) {
                auto* players = PlayerManager::get(aiManager_ ? aiManager_->getManager() : nullptr);
                const auto* rep = players ? players->findBaseRep(directive.playerId) : nullptr;
                const auto* follow = rep ? dynamic_cast<Player*>(rep->ownedObject) : nullptr;
                route.push_back({directive.order,
                                 0,
                                 directive.orderMask,
                                 follow ? follow->getLeadCenter() + seekOff_ : myPos,
                                 callback});
            }
        }
        return route;
    };
    auto route = buildRoute();
    const auto currentIndex = [&]() -> Int32 {
        const auto found = std::ranges::find_if(route, [&](const auto& point) {
            return point.order == pathOrder_ && point.point == pathPoint_;
        });
        return found == route.end() ? -1 : Int32(found - route.begin());
    };
    const auto nextIndex = [&](bool advance) -> Int32 {
        if (route.empty())
            return -1;
        const auto index = currentIndex();
        if (index >= 0) {
            if (!advance)
                return index;
            if (route.size() < 2)
                return -1;
            if (pathType_ == TwoWay) {
                if (index == 0)
                    pathDirection_ = 1;
                if (std::size_t(index + 1) == route.size())
                    pathDirection_ = -1;
                return index + pathDirection_;
            }
            const auto next = index + 1;
            return std::size_t(next) < route.size() ? next : pathType_ == Circular ? 0 : -1;
        }
        auto next = std::ranges::find_if(route, [&](const auto& point) {
            return point.order > pathOrder_;
        });
        if (pathType_ == TwoWay) {
            if (route.front().order > pathOrder_) {
                pathDirection_ = 1;
                return 0;
            }
            if (route.back().order < pathOrder_) {
                pathDirection_ = -1;
                return Int32(route.size() - 1);
            }
            if (pathDirection_ < 0) {
                next = std::ranges::find_if(route, [&](const auto& point) {
                    return point.order >= pathOrder_;
                });
                if (next != route.begin())
                    --next;
            }
        }
        return next == route.end() ? -1 : Int32(next - route.begin());
    };
    const auto begin = [&](Int32 index, const Point3F& from) {
        if (index < 0) {
            pathDone_ = true;
            pathActive_ = false;
            jetSkill_.stage = JetSkill::NoNav;
            return;
        }
        const auto& point = route[std::size_t(index)];
        pathOrder_ = point.order;
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
    if (!pathDone_) {
        auto index = currentIndex();
        if (index < 0 || !pathActive_) {
            index = nextIndex(false);
            begin(index, myPos);
        }
        const auto budget = directives_.size();
        for (std::size_t attempts = 0; index >= 0 && attempts < budget; ++attempts) {
            const auto point = route[std::size_t(index)];
            const bool within =
                jetSkill_.stage != JetSkill::NoNav
                    ? distance3(point.position, myPos) < .2f
                    : (frameAccurate_ ? m_dist(Point2F{point.position.x, point.position.y},
                                               Point2F{myPos.x, myPos.y})
                                      : m_distf(Point2F{point.position.x, point.position.y},
                                                Point2F{myPos.x, myPos.y})) < 1.2f;
            if ((point.mask & FollowDirective) || !within) {
                curDest_ = point.position;
                hasMoveTarget = true;
                break;
            }
            if (!point.callback.empty() && aiManager_)
                aiManager_->recordCallback(point.callback, name);
            if (!player_)
                return;
            // A waypoint callback can remove the next waypoint or replace the
            // whole route. Rebuild before choosing the next destination.
            route = buildRoute();
            const auto previous = currentIndex();
            const auto from = previous >= 0 ? route[std::size_t(previous)].position : myPos;
            index = nextIndex(true);
            begin(index, from);
        }
        if (!hasMoveTarget)
            pathDone_ = true;
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
            auto* players = PlayerManager::get(aiManager_ ? aiManager_->getManager() : nullptr);
            const auto* rep = players ? players->findBaseRep(directive.activeTarget.id) : nullptr;
            auto* target = rep ? dynamic_cast<Player*>(rep->ownedObject) : nullptr;
            if (!target || target->isDead())
                continue;
            const DynamicTarget freshTarget{directive.activeTarget.id,
                                            target->getLeadCenter(),
                                            target->getLeadVelocity(),
                                            m_distf(target->getLeadCenter(), myPos),
                                            target};
            if (directive.activeTarget.id >= 0 &&
                (targetMode == NoTarget || freshTarget.dist < bestDistance)) {
                chosenTarget = freshTarget;
                bestDistance = chosenTarget.dist;
                targetMode = PlayerTarget;
            }
        } else {
            targetMode = directive.targetMode;
            targetPoint = directive.pos;
            laserOwner = directive.targetPlayerId;
        }
    }
    if (player_ && player_->isDead()) {
        targetMode = NoTarget;
    }
    currentTarget_ = targetMode == PlayerTarget ? chosenTarget : DynamicTarget{};
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
        auto* rep = players ? players->findBaseRep(seekFlag_) : nullptr;
        auto* target = rep ? dynamic_cast<Player*>(rep->ownedObject) : nullptr;
        if (!target || target->isDead()) {
            return {};
        }
        const Flt32 distance = m_distf(target->getLeadCenter(), myPos);
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
        return {seekFlag_, target->getLeadCenter(), target->getLeadVelocity(), distance, target};
    };
    const auto attack = [&](const DynamicTarget& target, Int32 mode) {
        if (mode == PlayerTarget) {
            fireAtPlayerTarget(&pm, target);
        } else if (mode == LaserTarget) {
            const bool aiming = fireAtLaserOwner(&pm, laserOwner);
            if (!aiming && attackMode_ == 0 && hasMoveTarget)
                seekLocation(&pm);
            return;
        } else if (mode == PointTarget) {
            fireAtPointTarget(&pm, targetPoint);
        }
        if (mode == PointTarget) {
            return;
        }
        const auto moveToward = [&](const Point3F& destination) {
            auto heading = getAngleSeekZ(destination, myPos, player_->getRot().z);
            // Native moveTowardLoc includes the turn just generated by aiming.
            heading.current += pm.turnRot;
            setMovementAction(pm, heading, getSpeed(distance3(destination, myPos)));
        };
        if (attackMode_ == 0 && hasMoveTarget) {
            if (jetSkill_.stage != JetSkill::NoNav) {
                (void)jetSkill_.jetToPoint(*this, pm, curDest_);
            } else {
                moveToward(curDest_);
            }
        } else if (attackMode_ == 1 && mode == PlayerTarget) {
            Point3F destination = target.pos;
            if (m_distf(Point2F(destination.x, destination.y), Point2F(myPos.x, myPos.y)) < 17.f) {
                auto away = destination - myPos;
                away.normalize();
                destination -= away * 17.f;
            }
            moveToward(destination);
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
}

void AIObj::preProcess() {
    currentTargetId_ = -1;
    currentTarget_ = {};
    trigger_.preLoop();
    frameAccurate_ = false;
    if (auto* players = PlayerManager::get(aiManager_ ? aiManager_->getManager() : nullptr))
        if (const auto* rep = players->findBaseRep(id))
            team = rep->team;
}

void AIObj::doServerProcesses(SimTime curTime) {
    const SimTime dt = (lastProcessTime_ > 0.0f && curTime > lastProcessTime_)
                           ? (curTime - lastProcessTime_)
                           : 0.032f;
    lastProcessTime_ = curTime;

    if (player_ && player_->isDead()) {
        seekFlag_ = 0;
        currentTarget_ = {};
    } else if (!seekFlag_) {
        findTargets();
    } else {
        currentTarget_ = {};
    }

    if (!player_)
        return;
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
