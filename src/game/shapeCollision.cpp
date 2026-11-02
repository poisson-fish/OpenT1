#include <opent1/game/shape_collision.hpp>
#include <cmath>

namespace opent1::game {

std::optional<sim::SimCollisionMaterial> ShapeCollisionImage::getMaterial(Int32 slot) const {
    const auto* instance = shapeInstance();
    if (!instance || !instance->shape()->materials || slot < 0) {
        return {};
    }
    const auto& materials = instance->shape()->materials->materials;
    if (std::size_t(slot) >= materials.size()) {
        return {};
    }
    const auto& material = materials[slot];
    return sim::SimCollisionMaterial{material.friction, material.elasticity, material.surfaceType};
}

std::optional<sim::SimCollisionMaterial>
ShapeCollisionImage::getContactMaterial(const SimCollisionImage& other, Int32 slot) const {
    // Terrain/interior and shape-vs-shape contacts are emitted by the peer.
    if (other.getType() == Terrain || other.getType() == Interior ||
        dynamic_cast<const ShapeCollisionImage*>(&other)) {
        return other.getMaterial(slot);
    }
    return getMaterial(slot);
}

ShapeCollisionImage::ShapeCollisionImage() noexcept
    : SimCollisionImage(ShapeCollisionImageType, ShapeCollisionImageTypeId) {
    moveable = true;
    collisionDetail = -1;
    collisionLevel = CollideFaces;
}

ShapeCollisionImage::ShapeCollisionImage(Type imageType, Int32 imagePriority) noexcept
    : SimCollisionImage(imageType, imagePriority) {
    moveable = true;
    collisionDetail = -1;
    collisionLevel = CollideFaces;
}

void ShapeCollisionImage::setShapeInstance(world::TsShapeInstance* instance,
                                           Int32 detail,
                                           std::shared_ptr<const world::TsShapeInstance> snapshot) {
    shapeInst_ = instance;
    retainedShape_ = std::move(snapshot);
    if (detail >= -1) {
        collisionDetail = detail;
    }
    syncCollisionSnapshots();
}

void ShapeCollisionImage::setCollisionSnapshots(
    std::shared_ptr<const world::TsShapeCollision> detailCol,
    std::shared_ptr<const world::TsShapeCollision> lineDetail0Col) {
    detailCollision_ = std::move(detailCol);
    lineCollision_ = lineDetail0Col ? std::move(lineDetail0Col) : detailCollision_;
}

void ShapeCollisionImage::syncCollisionSnapshots() const {
    if (!shapeInst_) {
        return;
    }
    const auto poseOpt = shapeInst_->pose();
    if (!poseOpt) {
        return;
    }
    if (collisionDetail >= 0) {
        if (auto built = world::TsShapeCollision::prepare(*poseOpt, collisionDetail)) {
            detailCollision_ = std::make_shared<const world::TsShapeCollision>(std::move(*built));
        }
    }
    if (collisionDetail == 0 && detailCollision_) {
        lineCollision_ = detailCollision_;
    } else if (auto built0 = world::TsShapeCollision::prepare(*poseOpt, 0)) {
        lineCollision_ = std::make_shared<const world::TsShapeCollision>(std::move(*built0));
    }
}

std::shared_ptr<const world::TsShapeCollision> ShapeCollisionImage::detailCollision() const {
    if (shapeInst_ && collisionDetail >= 0) {
        if (const auto poseOpt = shapeInst_->pose()) {
            if (auto built = world::TsShapeCollision::prepare(*poseOpt, collisionDetail)) {
                detailCollision_ =
                    std::make_shared<const world::TsShapeCollision>(std::move(*built));
            }
        }
    }
    return detailCollision_;
}

std::shared_ptr<const world::TsShapeCollision> ShapeCollisionImage::lineCollision() const {
    if (shapeInst_) {
        if (const auto poseOpt = shapeInst_->pose()) {
            if (auto built0 = world::TsShapeCollision::prepare(*poseOpt, 0)) {
                lineCollision_ =
                    std::make_shared<const world::TsShapeCollision>(std::move(*built0));
            }
        }
    }
    return lineCollision_ ? lineCollision_ : detailCollision_;
}

bool ShapeCollisionImage::checkShape(const ShapeCollisionImage& other,
                                     CollisionSurfaceList& list) const {
    const auto otherCol = other.detailCollision();
    if (!otherCol) {
        return false;
    }

    const TMat3F& otherRootDelta =
        other.shapeInst_ ? other.shapeInst_->rootDelta() : other.rootDeltaTransform;

    m_mul(otherRootDelta, other.transform, list.tWorld);
    TMat3F invWorld = list.tWorld;
    invWorld.inverse();
    m_mul(transform, invWorld, list.tLocal);

    Point3F center{};
    m_mul(sphere.center, list.tLocal, center);
    list.tLocal.inverse();

    return otherCol->sphere(center, sphere.radius, list, other.tsCollisionLevel());
}

TMat3F ShapeCollisionImage::buildShapeTransform(const SimCollisionImage& image,
                                                CollisionSurfaceList& list) const {
    const TMat3F& root = shapeInst_ ? shapeInst_->rootDelta() : rootDeltaTransform;
    TMat3F local;
    m_mul(root, list.tLocal, local);
    list.tLocal = local;
    m_mul(root, transform, list.tWorld);

    // tLocal maps contact surfaces back to the moving image. Collision inputs
    // need the inverse direction: image space into the animated shape space.
    // This is ShapeCollisionImage::buildImageTransform in the legacy engine.
    TMat3F inverse = list.tWorld;
    inverse.inverse();
    m_mul(image.transform, inverse, local);
    return local;
}

bool ShapeCollisionImage::collide(const SimCollisionImage& image,
                                  CollisionSurfaceList& list) const {
    TMat3F mat{};
    const auto imageType = image.getType();

    if (imageType == Sphere) {
        const auto* si = static_cast<const sim::SimCollisionSphereImage*>(&image);
        const auto col = detailCollision();
        if (!col) {
            return false;
        }
        mat = buildShapeTransform(image, list);

        Point3F center{};
        m_mul(si->center, mat, center);
        return col->sphere(center, si->radius, list, tsCollisionLevel());
    }

    if (imageType == Box) {
        const auto* bi = static_cast<const sim::SimCollisionBoxImage*>(&image);
        const auto col = detailCollision();
        if (!col) {
            return false;
        }
        mat = buildShapeTransform(image, list);
        return col->box(bi->box, mat, list);
    }

    if (imageType == Tube) {
        const auto* ti = static_cast<const sim::SimCollisionTubeImage*>(&image);
        const auto col = detailCollision();
        if (!col) {
            return false;
        }
        mat = buildShapeTransform(image, list);

        Point3F start{};
        Point3F end{};
        m_mul(ti->start, mat, start);
        m_mul(ti->end, mat, end);
        return col->tube(start, end, ti->radius, list, tsCollisionLevel());
    }

    if (imageType == Line) {
        const auto* li = static_cast<const sim::SimCollisionLineImage*>(&image);
        const auto col = lineCollision();
        if (!col) {
            return false;
        }
        mat = buildShapeTransform(image, list);

        Point3F start{};
        Point3F end{};
        m_mul(li->start, mat, start);
        m_mul(li->end, mat, end);
        // Line intersections always test against the highest detail level (0).
        return col->line(start, end, list, tsCollisionLevel());
    }

    if (imageType == Terrain) {
        const auto* ti = dynamic_cast<const world::SimTerrainCollisionImage*>(&image);
        if (!ti) {
            return false;
        }
        // Terrain tests consume the shape in terrain space, as in 1.11.
        mat = ti->buildImageTransform(*this);
        Point3F center{};
        m_mul(sphere.center, mat, center);
        const auto infoOpt = ti->nativeGeometry()
                                 ? ti->nativeGeometry()->surface(Point2F{center.x, center.y})
                                 : ti->geometry().surface(Point2F{center.x, center.y});
        if (infoOpt) {
            auto info = *infoOpt;
            const Flt32 dist = m_dot(center, info.normal) - m_dot(info.position, info.normal);
            if (std::fabs(dist) < sphere.radius) {
                info.distance = dist;
                list.surfaces.push_back(info);
                list.tLocal = mat;
                list.tLocal.inverse();
                list.tWorld = ti->transform;
                return true;
            }
        }
        return false;
    }

    if (imageType == Interior) {
        const auto* ii = dynamic_cast<const world::SimInteriorCollisionImage*>(&image);
        if (!ii) {
            return false;
        }
        mat = ii->buildImageTransform(*this);
        if (ii->geometry().box(bbox, mat, list)) {
            list.tLocal = mat;
            list.tLocal.inverse();
            list.tWorld = ii->transform;
            return true;
        }
        return false;
    }

    if (imageType == ShapeCollisionImageType) {
        const auto* si = dynamic_cast<const ShapeCollisionImage*>(&image);
        if (!si) {
            return false;
        }
        return checkShape(*si, list);
    }

    return false;
}

} // namespace opent1::game
