#pragma once

#include <opent1/game/interior_shape.hpp>
#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/sim/sim_environment_render.hpp>

#include <span>
#include <vector>

namespace opent1::game {

class TribesShadowRenderImage {
public:
    static inline constexpr Int32 ShadowItrFeeler = 4;

    TMat3F transform{};
    Point3F shapeCenter{0.0f, 0.0f, 0.0f};
    Flt32 shapeRadius{1.0f};
    Box3F shapeBox{Point3F{-1.0f, -1.0f, -1.0f}, Point3F{1.0f, 1.0f, 1.0f}};
    Flt32 projectionRadius{1.0f};
    Point3F lightDirection{0.0f, 0.0f, -1.0f};
    bool castShadow{true};

    struct InteriorQueryVolume {
        Point3F itrCenter{};
        Point3F lightInItr{};
        Box3F feelerBox{};
        TMat3F transMat{};
    };

    TribesShadowRenderImage();
    ~TribesShadowRenderImage() = default;

    [[nodiscard]] InteriorQueryVolume
    buildInteriorQueryVolume(const InteriorShape& itr) const noexcept;
    [[nodiscard]] sim::ShadowSourceWindow buildSourceWindow() const;
    [[nodiscard]] sim::ShadowGeometry
    getUnknownPolys(const InteriorShape& itr,
                    std::span<const sim::ShadowReceiver> explicitReceivers = {},
                    const Point3F& cameraDirection = Point3F{0.0f, 1.0f, 0.0f}) const;

    [[nodiscard]] const std::vector<InteriorShape*>& currentInteriors() const noexcept {
        return currentInteriors_;
    }

    void addInterior(InteriorShape* itr) {
        if (itr) {
            currentInteriors_.push_back(itr);
        }
    }

    void clearInteriors() noexcept {
        currentInteriors_.clear();
    }

private:
    std::vector<InteriorShape*> currentInteriors_{};
};

} // namespace opent1::game
