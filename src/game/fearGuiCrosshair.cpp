#include <opent1/game/status_combat_huds.hpp>
#include <ranges>
#include <opent1/game/sensor_manager.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace opent1::game {

namespace {
constexpr Flt32 kPi = 3.14159265358979323846f;

Flt32 targetDamage(const ShapeBase& target) {
    auto damage = target.getDamageLevel();
    const auto* data = dynamic_cast<const ShapeBaseData*>(target.getDatPtr());
    // Static/vehicle ghosts carry a fraction; player ghosts retain absolute
    // damage for movement and health HUD calculations.
    if (data && data->maxDamage > 0 &&
        (!target.isGhost() || dynamic_cast<const Player*>(&target))) {
        damage /= data->maxDamage;
    }
    return damage;
}
} // namespace

Crosshair::Crosshair() {
    fields_.extent = {1, 1};
}

Crosshair::ReticleMetrics Crosshair::computeReticleMetrics(Point2I parentExtent,
                                                           Flt32 currentFovDeg,
                                                           Flt32 targetFovDeg,
                                                           Flt32 sniperFovDeg,
                                                           bool sniperCrosshairPref,
                                                           Int32 observerTeam,
                                                           const ShapeBase* targetObj,
                                                           std::string_view targetLabel,
                                                           Flt32 projectileVelocity,
                                                           const Point3F& muzzlePos) {
    ReticleMetrics out{};
    out.lowRes = (parentExtent.x < 512 || parentExtent.y < 384);
    out.zoomReticle = sniperCrosshairPref && (currentFovDeg <= 30.0f || targetFovDeg <= 30.0f);

    const Flt32 clampedSniperFov = std::clamp(sniperFovDeg, 1.0f, 90.0f);
    const Int32 hWidth = static_cast<Int32>(std::tan(clampedSniperFov * kPi / 360.0f) *
                                            static_cast<Flt32>(parentExtent.x) * 0.5f);
    out.zoomHalfWidth = hWidth;
    const Point2I center{parentExtent.x / 2, parentExtent.y / 2};
    out.zoomRect = {center.x - hWidth, center.y - hWidth, center.x + hWidth, center.y + hWidth};

    if (targetObj != nullptr) {
        out.hasTarget = true;
        out.targetFriendly = (observerTeam >= 0 && targetObj->getTeam() == observerTeam);
        out.targetDamage = targetDamage(*targetObj);
        out.targetName = std::string(targetLabel);
        if (projectileVelocity > 0.0f) {
            const Point3F tgtPos = targetObj->getLinearPosition();
            const Point3F tgtVel = targetObj->getLinearVelocity();
            const Flt32 dx = tgtPos.x - muzzlePos.x;
            const Flt32 dy = tgtPos.y - muzzlePos.y;
            const Flt32 dz = tgtPos.z - muzzlePos.z;
            const Flt32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const Flt32 t = dist / projectileVelocity;
            out.hasLeadIndicator = true;
            out.leadPosition = {
                tgtPos.x + tgtVel.x * t, tgtPos.y + tgtVel.y * t, tgtPos.z + tgtVel.z * t};
        }
    }
    return out;
}

void Crosshair::updateLive(sim::SimManager& manager,
                           const PlayerPSC& psc,
                           const PlayerManager* players,
                           const DataBlockManager* data,
                           const sim::EnvironmentCamera& camera,
                           UInt32 now,
                           bool sniperCrosshair,
                           bool sensors,
                           bool targets) {
    const auto* control = psc.getControlObject();
    const bool previousZoom = metrics_.zoomReticle;
    metrics_ = computeReticleMetrics(
        fields_.extent, psc.getFov(), psc.targetFov(), PlayerPSC::sniperFov, sniperCrosshair);
    if (now < psc.fovTargetTime() && psc.targetFov() > psc.startFov()) {
        metrics_.zoomReticle = sniperCrosshair && previousZoom;
    }
    metrics_.enabled = control && psc.isFirstPerson() && control->cameraZoomAndCrosshairsEnabled();
    if (!metrics_.enabled) {
        return;
    }
    const auto center = fields_.extent / 2;
    metrics_.showZoomRectangle =
        !metrics_.zoomReticle && psc.getFov() == psc.targetFov() && psc.zoomChangeTime() &&
        now - *psc.zoomChangeTime() < 2000;
    const auto halfHeight = Int32(
        std::tan(std::clamp(PlayerPSC::sniperFov, 1.f, 90.f) * kPi / 360) * fields_.extent.y * .5f);
    metrics_.zoomRect.top = center.y - halfHeight;
    metrics_.zoomRect.bottom = center.y + halfHeight;
    metrics_.zoomMagnification = fields_.extent.x / std::max(1, metrics_.zoomHalfWidth * 2);
    if (metrics_.zoomReticle) {
        Flt32 fraction = 1;
        if (now < psc.fovTargetTime() && psc.fovTargetTime() > psc.fovStartTime()) {
            fraction = Flt32(now >= psc.fovStartTime() ? now - psc.fovStartTime() : 0) /
                       Flt32(psc.fovTargetTime() - psc.fovStartTime());
            if (psc.targetFov() > psc.startFov()) {
                fraction = 1 - fraction;
            }
        }
        metrics_.zoomHalfWidth = Int32(center.x * fraction);
        metrics_.zoomHalfHeight = Int32(center.y * fraction);
    }
    const auto project = [&](Point3F point) -> std::optional<Point2I> {
        Point3F local;
        m_mul(point, camera.worldToCamera, local);
        if (local.y <= camera.nearDistance || local.y >= camera.farDistance) {
            return {};
        }
        const Point2F screen{camera.center.x + local.x * camera.focal.x / local.y,
                             camera.center.y - local.z * camera.focal.y / local.y};
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y) || screen.x < 0 ||
            screen.x >= fields_.extent.x || screen.y < 0 || screen.y >= fields_.extent.y) {
            return {};
        }
        return Point2I{Int32(screen.x), Int32(screen.y)};
    };
    auto* root = dynamic_cast<sim::SimContainer*>(manager.findObject(sim::RootContainerId));
    const auto eye = control->getEyeTransform().p;
    const auto unobstructed = [&](const ShapeBase* target, Point3F point) {
        if (!root) {
            return false;
        }
        sim::SimContainerQuery query;
        query.id = control->getId();
        query.mask = SimTerrainObjectType | StaticObjectType | ItemObjectType | VehicleObjectType |
                     SimInteriorObjectType | MoveableObjectType;
        query.box = {eye, point};
        sim::SimCollisionInfo collision;
        return !root->findLOS(query, collision) || collision.object.resolve(manager) == target;
    };
    auto distance = std::numeric_limits<Flt32>::max();
    if (sensors) {
        if (auto* visible = dynamic_cast<sim::SimSet*>(manager.findObject(SensorVisibleSetId))) {
            for (auto* object : visible->objects()) {
                const auto* target = dynamic_cast<ShapeBase*>(object);
                if (!target || target == control || (target->getType() & CorpseObjectType)) {
                    continue;
                }
                const auto* player = dynamic_cast<const Player*>(target);
                if (player) {
                    const auto index = player->getOwnerClient() - PlayerManager::BaseClientId;
                    if (player->isDead() || player->getMountObject() == control || index < 0 ||
                        std::size_t(index / 32) >= psc.visibleList().size() ||
                        !(psc.visibleList()[std::size_t(index / 32)] & (1U << (index % 32)))) {
                        continue;
                    }
                } else if (target->getTeam() != control->getTeam()) {
                    continue;
                }
                const auto& box = target->getBoundingBox();
                const auto size = box.fMax - box.fMin;
                auto position = target->getBoxCenter() + Point3F{0, 0, size.z * .5f};
                if (player) {
                    position = player->getTransform().p;
                    if (const auto pose = player->animationPose()) {
                        const auto& shape = *pose->shape();
                        for (auto i : std::views::iota(std::size_t{0}, shape.nodes.size())) {
                            const auto name = shape.nodes[i].name;
                            if (name >= 0 && std::size_t(name) < shape.names.size() &&
                                shape.names[name].starts_with("dummy midback")) {
                                TMat3F mount;
                                m_mul(pose->nodeTransforms()[i], player->getTransform(), mount);
                                position = mount.p;
                                break;
                            }
                        }
                    }
                    position.z += .65f;
                }
                const auto screen = project(position);
                if (!screen || !unobstructed(target, target->getBoxCenter())) {
                    continue;
                }
                const bool friendly = players && players->getNumTeams() > 1 &&
                                      target->getTeam() == control->getTeam();
                if (player) {
                    metrics_.markers.push_back({*screen, friendly});
                }
                Point3F cameraPoint;
                m_mul(position, camera.worldToCamera, cameraPoint);
                const auto width = std::max(size.x, size.y) * camera.focal.x / cameraPoint.y * .5f;
                const auto height = size.z * camera.focal.y / cameraPoint.y;
                const auto delta = target->getLinearPosition() - control->getLinearPosition();
                const auto range = m_dot(delta, delta);
                if (std::abs(center.x - screen->x) > std::max(Int32(width), 8) ||
                    center.y < screen->y - 4 ||
                    center.y > screen->y + std::max(Int32(height), 12) || range >= distance) {
                    continue;
                }
                distance = range;
                metrics_.hasTarget = true;
                metrics_.targetFriendly = friendly;
                metrics_.targetDamage = width >= 1 ? targetDamage(*target) : -1;
                if (const auto* rep = player && players
                                          ? players->findBaseRep(player->getOwnerClient())
                                          : nullptr) {
                    metrics_.targetName = rep->name;
                } else {
                    const auto* sensor =
                        dynamic_cast<const SensorManager*>(manager.findObject(SensorManagerId));
                    metrics_.targetName = sensor && target->sensorKey >= 0 &&
                                                  target->sensorKey < SensorManager::MaxTargets
                                              ? sensor->target(target->sensorKey).name
                                              : std::string{};
                    if (metrics_.targetName.empty()) {
                        metrics_.targetName = std::string(target->getName());
                    }
                    if (metrics_.targetName.empty() && target->getDatPtr()) {
                        metrics_.targetName = target->getDatPtr()->description;
                    }
                }
            }
        }
    }
    if (!targets) {
        return;
    }
    ProjectileDataType primary, secondary;
    (void)control->getWeaponAmmoType(primary, secondary);
    if (const auto* player = dynamic_cast<const Player*>(control); player && data) {
        const auto* image = dynamic_cast<const ItemImageData*>(data->lookupDataBlock(
            player->getItemImageEntry(0).imageId, DataBlockManager::ItemImageDataType));
        if (image) {
            primary = image->projectile;
        }
    }
    const auto* projectile = data && primary.type >= 0
                                 ? dynamic_cast<const ProjectileData*>(
                                       data->lookupDataBlock(primary.dataType, primary.type))
                                 : nullptr;
    const auto gravity = control->getForce(sim::MovementGravityForce);
    if (auto* targetSet = dynamic_cast<sim::SimSet*>(manager.findObject(TargetableSetId))) {
        for (auto* object : targetSet->objects()) {
            const auto* target = dynamic_cast<const GameBase*>(object);
            Point3F position;
            Int32 team{};
            if (!target || !target->getTarget(&position, &team) || team != control->getTeam()) {
                continue;
            }
            const auto firstMarker = metrics_.markers.size();
            TMat3F muzzle;
            if (!control->getMuzzleTransform(0, &muzzle)) {
                muzzle.identity();
            }
            Point3F muzzlePosition;
            m_mul(muzzle.p, control->getTransform(), muzzlePosition);
            for (bool second : {false, true}) {
                Point3F direction;
                bool inRange{};
                if (projectile) {
                    if (!projectile->getTargetVector(eye,
                                                     position,
                                                     gravity ? -gravity->len() : 0,
                                                     &direction,
                                                     &inRange,
                                                     second)) {
                        continue;
                    }
                } else {
                    if (second) {
                        continue;
                    }
                    direction = position - eye;
                    if (direction.len() < .001f) {
                        continue;
                    }
                    direction.normalize();
                }
                if (const auto screen = project(eye + direction * 2)) {
                    metrics_.markers.push_back(
                        {*screen, true, true, inRange, {}, (muzzlePosition - position).len()});
                }
            }
            if (metrics_.markers.size() > firstMarker) {
                metrics_.markers.back().targetPoint = project(position);
            }
        }
    }
}

bool Crosshair::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    fields_.extent = {1, 1};
    return true;
}

bool Crosshair::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!metrics_.enabled) {
        return true;
    }
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    const Point2I center{offset.x + fields_.extent.x / 2, offset.y + fields_.extent.y / 2};
    const auto font = metrics_.lowRes && lowResFont ? lowResFont : hudFont;
    if (metrics_.zoomReticle) {
        const auto half = metrics_.zoomHalfWidth,
                   height = metrics_.zoomHalfHeight ? metrics_.zoomHalfHeight : half;
        s.line({center.x - half, center.y}, {center.x + half, center.y}, GREEN);
        s.line({center.x, center.y - height}, {center.x, center.y + height}, GREEN);
        for (const auto delta : {-1, 1}) {
            s.line({center.x + delta, center.y - height},
                   {center.x + delta, center.y - height / 3},
                   GREEN);
            s.line({center.x + delta, center.y + height / 3},
                   {center.x + delta, center.y + height},
                   GREEN);
            s.line({center.x - half, center.y + delta},
                   {center.x - half / 3, center.y + delta},
                   GREEN);
            s.line({center.x + half / 3, center.y + delta},
                   {center.x + half, center.y + delta},
                   GREEN);
        }
    } else {
        const auto* bitmap = sprite(metrics_.lowRes ? "lr_H_Reticle.bmp" : "H_Reticle.bmp");
        if (bitmap && !s.blit(*bitmap, center - Point2I{bitmap->width / 2, bitmap->height / 2})) {
            return false;
        }
        if (metrics_.showZoomRectangle) {
            const auto& r = metrics_.zoomRect;
            const auto length = metrics_.lowRes ? 4 : 7;
            for (const auto x : {r.left, r.right}) {
                for (const auto y : {r.top, r.bottom}) {
                    const Point2I corner = offset + Point2I{x, y};
                    s.line(corner, corner + Point2I{x == r.left ? length : -length, 0}, GREEN);
                    s.line(corner, corner + Point2I{0, y == r.top ? length : -length}, GREEN);
                }
            }
            const auto label = std::format("ZOOM: {}x", metrics_.zoomMagnification);
            if (font && !s.text(*font,
                                {center.x - Int32(font->textWidth(label)) / 2,
                                 center.y + (metrics_.lowRes ? 18 : 30)},
                                label)) {
                return false;
            }
        }
    }
    if (metrics_.hasTarget) {
        auto point = center + Point2I{metrics_.lowRes ? 10 : 16, metrics_.lowRes ? 10 : 16};
        if (font && !metrics_.targetName.empty()) {
            if (!s.text(*font, point, metrics_.targetName)) {
                return false;
            }
            point.y += font->info.height + (metrics_.lowRes ? 3 : 4);
        }
        const Point2I size = metrics_.lowRes ? Point2I{20, 4} : Point2I{27, 6};
        if (metrics_.targetDamage >= 0 && metrics_.targetDamage <= 1) {
            s.rectangle({point.x, point.y, point.x + size.x, point.y + size.y}, BLUE_HILITE);
            const auto width = metrics_.targetDamage == 1
                                   ? 0
                                   : std::max(1, Int32((1 - metrics_.targetDamage) * (size.x - 2)));
            if (width) {
                s.fill({point.x + 1, point.y + 1, point.x + width + 1, point.y + size.y - 1},
                       width >= (metrics_.lowRes ? 5 : 8) ? GREEN : RED);
            }
        }
    }
    for (const auto& marker : metrics_.markers) {
        const auto point = offset + marker.point;
        if (marker.lead) {
            const auto size = metrics_.lowRes ? 3 : 4;
            const auto color = marker.inRange ? BLUE_HILITE : GREEN_78;
            if (marker.targetPoint) {
                s.line(point, offset + *marker.targetPoint, WHITE);
            }
            const std::array<gfx::PolygonVertex, 3> triangle{
                {{{Flt32(point.x), Flt32(point.y)}},
                 {{Flt32(point.x - size), Flt32(point.y - size * 2)}},
                 {{Flt32(point.x + size), Flt32(point.y - size * 2)}}}};
            gfx::PolygonMaterial material;
            material.color = UInt8(color);
            if (!s.polygon(triangle, material)) {
                return false;
            }
            const auto distance = std::format("{:.1f} m", marker.distance);
            if (font && !s.text(*font,
                                {point.x - Int32(font->textWidth(distance)) / 2,
                                 point.y + (metrics_.lowRes ? 3 : 4)},
                                distance)) {
                return false;
            }
        } else {
            const auto name = std::string(metrics_.lowRes ? "lr_" : "") +
                              (marker.friendly ? "H_Friend.bmp" : "H_Foe.bmp");
            if (const auto* bitmap = sprite(name);
                bitmap && !s.blit(*bitmap, point - Point2I{bitmap->width / 2, bitmap->height})) {
                return false;
            }
        }
    }
    return true;
}

} // namespace opent1::game
