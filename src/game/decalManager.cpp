#include <opent1/game/decal_manager.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/console/console.hpp>

#include <algorithm>
#include <cmath>
#include <charconv>

namespace opent1::game {

DecalManager::DecalManager() {
    numDecals_ = 0;
    curFirstDecal_ = 0;
    curLastDecal_ = 0;
    activeCount_ = 0;
}

DecalManager::~DecalManager() = default;

void DecalManager::reset() {
    curFirstDecal_ = 0;
    curLastDecal_ = 0;
    activeCount_ = 0;
    numDecals_ = std::clamp(iPrefNumDecals, 0, static_cast<Int32>(MAX_DECALS));
    rgDecals_.fill(Decal{});
}

Int32 DecalManager::activeDecalCount() const noexcept {
    return activeCount_;
}

bool DecalManager::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        if (auto* console = mgr->console()) {
            const auto value = console->getVariable("pref::NumDecals");
            Int32 count{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
                iPrefNumDecals = count;
            }
        }
        mgr->assignId(*this, DecalManagerId);
        addToSet(SimRenderSetId);
    }
    for (std::size_t i = 0; i < DECALS; ++i) {
        sizes_[i] = kDecalTypes[i].size;
    }
    reset();
    return true;
}

std::optional<world::TsStaticShapeFrame>
DecalManager::prepareFrame(ResourceManager& resources, const sim::EnvironmentCamera& camera) {
    if (!materials_) {
        world::TsMaterialResources materials;
        materials.list.details = 1;
        materials.list.materialsPerDetail = DECALS;
        for (const auto& desc : kDecalTypes) {
            auto bitmap = resources.load<gfx::Bitmap>(desc.fileName);
            if (!bitmap) {
                return {};
            }
            world::TsMaterial material;
            material.flags =
                3U | ((bitmap->attributes & gfx::BitmapSubtractive) ? 0x2000U : 0x1000U);
            material.mapFile = desc.fileName;
            materials.list.materials.push_back(std::move(material));
            materials.textures.push_back(std::move(bitmap));
        }
        materials_ = std::move(materials);
    }
    std::vector<world::StaticSurfaceTriangle> triangles;
    triangles.reserve(std::size_t(activeCount_) * 2);
    constexpr std::array<Point2F, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (Int32 i = 0; i < activeCount_; ++i) {
        const auto& decal = rgDecals_[std::size_t((curFirstDecal_ + i) % numDecals_)];
        for (const auto indices : {std::array{0, 1, 2}, std::array{0, 2, 3}}) {
            world::StaticSurfaceTriangle triangle;
            triangle.material = UInt32(decal.type);
            for (std::size_t j = 0; j < 3; ++j) {
                triangle.vertices[j].position = decal.worldPts[indices[j]];
                triangle.vertices[j].texture = uv[indices[j]];
            }
            triangles.push_back(triangle);
        }
    }
    world::TsStaticView view;
    view.shapeToCamera = camera.worldToCamera;
    view.width = Int32(camera.viewport.width());
    view.height = Int32(camera.viewport.height());
    view.focal = camera.focal;
    view.center = camera.center;
    view.nearDistance = camera.nearDistance;
    view.farDistance = camera.farDistance;
    view.perspective = camera.perspective;
    view.texturePerspective = false;
    view.twoSided = true;
    return world::TsStaticShapeFrame::prepareSurfaces(*materials_, triangles, view);
}

bool DecalManager::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

void DecalManager::addFootprint(const TMat3F& inMat,
                                DECALTYPE type,
                                Flt32 footspan,
                                std::optional<Point3F> explicitHitPos,
                                std::optional<Point3F> explicitHitNormal) {
    if (numDecals_ <= 0) {
        return;
    }

    TMat3F tmat = inMat;
    EulerF angles{};
    tmat.angles(angles);

    const Point3F temp(footspan / 2.0f, 0.0f, 0.0f);
    Point3F offset{};
    m_mul(temp, RMat3F(angles), offset);
    tmat.p += offset;

    Point3F hitPos{};
    Point3F hitNormal{};

    if (explicitHitPos.has_value() && explicitHitNormal.has_value()) {
        hitPos = *explicitHitPos;
        hitNormal = *explicitHitNormal;
    } else if (auto* mgr = getManager()) {
        auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId));
        if (!root) {
            return;
        }
        sim::SimContainerQuery cq{};
        cq.id = static_cast<sim::SimObjectId>(-1);
        cq.type = static_cast<UInt32>(-1);
        cq.mask = SimTerrainObjectType;
        cq.detail = sim::SimCollisionImageQuery::High;
        cq.box.fMin = tmat.p + Point3F(0.0f, 0.0f, 0.25f);
        cq.box.fMax = tmat.p - Point3F(0.0f, 0.0f, 0.75f);

        sim::SimCollisionInfo info{};
        if (!root->findLOS(cq, info) || info.surfaces.surfaces.empty()) {
            return;
        }
        m_mul(info.surfaces.surfaces[0].position, info.surfaces.tWorld, hitPos);
        m_mul(info.surfaces.surfaces[0].normal,
              static_cast<const RMat3F&>(info.surfaces.tWorld),
              hitNormal);
    } else {
        return;
    }

    Point3F perp1{};
    Point3F perp2(0.0f, 1.0f, 0.0f);
    m_cross(perp2, hitNormal, perp1);
    m_cross(perp1, hitNormal, perp2);
    perp1.normalize();
    perp2.normalize();

    const Flt32 sinZ = static_cast<Flt32>(std::sin(static_cast<RealD>(angles.z)));
    const Flt32 cosZ = static_cast<Flt32>(std::cos(static_cast<RealD>(angles.z)));
    const Flt32 sinPiZ = static_cast<Flt32>(std::sin(kRealDPi - static_cast<RealD>(angles.z)));
    const Flt32 cosPiZ = static_cast<Flt32>(std::cos(kRealDPi - static_cast<RealD>(angles.z)));

    const Point3F temp1 = perp1 * sinZ + perp2 * cosZ;
    const Point3F temp2 = perp2 * sinPiZ + perp1 * cosPiZ;

    const std::size_t typeIdx = static_cast<std::size_t>(
        std::clamp(static_cast<Int32>(type), 0, static_cast<Int32>(DECALS - 1)));
    perp1 = temp1 * sizes_[typeIdx];
    perp2 = temp2 * sizes_[typeIdx];
    hitNormal *= 0.05f;
    tmat.p.z = hitPos.z;

    addDecal(tmat.p, hitNormal, perp1, perp2, type);
}

void DecalManager::addBulletHole(Point3F pos, Point3F normal, DECALTYPE type) {
    if (numDecals_ <= 0) {
        return;
    }

    Point3F perp1{};
    Point3F perp2{};
    if (normal.z == 1.0f || normal.z == -1.0f) {
        perp2.set(1.0f, 0.0f, 0.0f);
    } else {
        perp2.set(0.0f, 0.0f, 1.0f);
    }

    m_cross(perp2, normal, perp1);
    m_cross(perp1, normal, perp2);

    const Flt32 rRand = random_.getFloat() * static_cast<Flt32>(kRealDPi);
    const Flt32 sinR = static_cast<Flt32>(std::sin(static_cast<RealD>(rRand)));
    const Flt32 cosR = static_cast<Flt32>(std::cos(static_cast<RealD>(rRand)));
    const Flt32 sinPiR = static_cast<Flt32>(std::sin(kRealDPi - static_cast<RealD>(rRand)));
    const Flt32 cosPiR = static_cast<Flt32>(std::cos(kRealDPi - static_cast<RealD>(rRand)));

    const Point3F temp1 = perp1 * sinR + perp2 * cosR;
    const Point3F temp2 = perp2 * sinPiR + perp1 * cosPiR;

    const std::size_t typeIdx = static_cast<std::size_t>(
        std::clamp(static_cast<Int32>(type), 0, static_cast<Int32>(DECALS - 1)));
    perp1 = temp1 * sizes_[typeIdx];
    perp2 = temp2 * sizes_[typeIdx];
    normal *= 0.05f;

    addDecal(pos, normal, perp1, perp2, type);
}

void DecalManager::addDecal(const Point3F& pos,
                            const Point3F& normal,
                            const Point3F& perp1,
                            const Point3F& perp2,
                            DECALTYPE type) {
    if (numDecals_ <= 0) {
        return;
    }

    Decal& decal = rgDecals_[static_cast<std::size_t>(curLastDecal_)];
    decal.type = type;
    decal.center = pos;
    decal.worldPts[0] = pos + normal + perp1 - perp2;
    decal.worldPts[1] = pos + normal + perp1 + perp2;
    decal.worldPts[2] = pos + normal - perp1 + perp2;
    decal.worldPts[3] = pos + normal - perp1 - perp2;

    curLastDecal_ = (curLastDecal_ + 1) % numDecals_;
    if (activeCount_ < numDecals_) {
        ++activeCount_;
    } else {
        curFirstDecal_ = curLastDecal_;
    }
}

} // namespace opent1::game
