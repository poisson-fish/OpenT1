#include <opent1/game/player_collision.hpp>
#include <cmath>

namespace opent1::game {

PlayerCollisionImage::PlayerCollisionImage() noexcept
    : ShapeCollisionImage(PlayerCollisionImageType, PlayerCollisionImageTypeId) {
    moveable = true;
    collisionDetail = -1;
    collisionLevel = CollideFaces;
}

bool PlayerCollisionImage::collide(const SimCollisionImage& image,
                                   CollisionSurfaceList& list) const {
    TMat3F mat{};
    list.stepVector.set(0.0f, 0.0f, -1.0f);

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
        const auto* line = static_cast<const sim::SimCollisionLineImage*>(&image);
        // Transform the line into our coordinate space (ignoring rootDelta).
        mat = SimCollisionImage::buildImageTransform(image);
        Point3F a{};
        Point3F b{};
        m_mul(line->start, mat, a);
        m_mul(line->end, mat, b);

        Flt32 hit = 0.0f;
        if (m_lineAABox(a, b, crouchBox.fMin, crouchBox.fMax, hit)) {
            CollisionSurface surface{};
            surface.time = hit;
            surface.position = line->start + (line->end - line->start) * hit;
            surface.distance = m_distf(surface.position, line->start);
            surface.normal.set(0.0f, 0.0f, 1.0f);
            surface.material = 0;
            surface.part = 0;
            surface.surface = 0;
            list.tWorld.identity();
            list.surfaces.push_back(surface);
            return true;
        }
        return false;
    }

    if (imageType == Terrain) {
        const auto* ti = dynamic_cast<const world::SimTerrainCollisionImage*>(&image);
        if (!ti) {
            return false;
        }
        // Ignore player's rotation when colliding with the terrain.
        TMat3F nmat{};
        nmat.p = transform.p;
        TMat3F invTerr = ti->transform;
        invTerr.inverse();
        m_mul(nmat, invTerr, mat);

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
        // Ignore player's rotation when colliding with interiors.
        TMat3F nmat{};
        nmat.p = transform.p;
        TMat3F invInt = ii->transform;
        invInt.inverse();
        m_mul(nmat, invInt, mat);

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

    if (imageType == PlayerCollisionImageType) {
        const auto* pi = dynamic_cast<const PlayerCollisionImage*>(&image);
        if (!pi) {
            return false;
        }
        mat = buildImageTransform(*pi);
        Point3F center2{};
        m_mul(pi->sphere.center, mat, center2);
        const Flt32 dist = m_distf(sphere.center, center2);
        if (dist < sphere.radius + pi->sphere.radius) {
            CollisionSurface surface{};
            surface.position = sphere.center;
            surface.normal = sphere.center - center2;
            surface.normal.normalize();
            surface.distance = dist;
            surface.material = 0;
            surface.part = 0;
            surface.surface = 0;
            surface.time = 0.0f;
            list.surfaces.push_back(surface);
            return true;
        }
        return false;
    }

    return false;
}

} // namespace opent1::game
