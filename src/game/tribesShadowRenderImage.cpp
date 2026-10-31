#include <opent1/game/tribes_shadow_render_image.hpp>

#include <cmath>

namespace opent1::game {

TribesShadowRenderImage::TribesShadowRenderImage() {
    transform.identity();
}

TribesShadowRenderImage::InteriorQueryVolume
TribesShadowRenderImage::buildInteriorQueryVolume(const InteriorShape& itr) const noexcept {
    InteriorQueryVolume out{};

    Point3F worldCenter{};
    m_mul(shapeCenter, transform, worldCenter);
    m_mul(worldCenter, itr.getInvTransform(), out.itrCenter);

    m_mul(lightDirection, static_cast<const RMat3F&>(itr.getInvTransform()), out.lightInItr);

    const Flt32 rad = projectionRadius;
    const Flt32 srad = 1.42f * rad;
    out.feelerBox.fMin.set(-srad, rad * 0.5f, -srad);
    out.feelerBox.fMax.set(srad, rad + static_cast<Flt32>(ShadowItrFeeler) * rad, srad);

    Point3F yAxis = out.lightInItr;
    if (yAxis.lenSq() > 1e-12f) {
        yAxis.normalize();
    } else {
        yAxis.set(0.0f, 0.0f, -1.0f);
    }
    const Point3F upHint =
        (std::abs(yAxis.z) < 0.9f) ? Point3F{0.0f, 0.0f, 1.0f} : Point3F{1.0f, 0.0f, 0.0f};
    Point3F xAxis{};
    m_cross(yAxis, upHint, xAxis);
    if (xAxis.lenSq() > 1e-12f) {
        xAxis.normalize();
    } else {
        xAxis.set(1.0f, 0.0f, 0.0f);
    }
    Point3F zAxis{};
    m_cross(xAxis, yAxis, zAxis);

    RMat3F setLight{};
    setLight.setRow(0, xAxis);
    setLight.setRow(1, yAxis);
    setLight.setRow(2, zAxis);
    setLight.flags = RMat3F::Matrix_HasRotation;
    static_cast<RMat3F&>(out.transMat) = setLight;
    out.transMat.p = out.itrCenter;
    out.transMat.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
    return out;
}

sim::ShadowSourceWindow TribesShadowRenderImage::buildSourceWindow() const {
    Point3F unitLight = lightDirection;
    if (unitLight.lenSq() > 1e-12f) {
        unitLight.normalize();
    } else {
        unitLight.set(0.0f, 0.0f, -1.0f);
    }
    return sim::makeShadowSourceWindow(unitLight,
                                       shapeCenter,
                                       shapeRadius,
                                       shapeBox,
                                       static_cast<const RMat3F&>(transform),
                                       transform.p);
}

sim::ShadowGeometry
TribesShadowRenderImage::getUnknownPolys(const InteriorShape& itr,
                                         std::span<const sim::ShadowReceiver> explicitReceivers,
                                         const Point3F& cameraDirection) const {
    (void)buildInteriorQueryVolume(itr);
    const auto window = buildSourceWindow();
    if (!explicitReceivers.empty()) {
        return sim::nativeShadowGeometry(window, explicitReceivers, cameraDirection);
    }
    return sim::ShadowGeometry{};
}

} // namespace opent1::game
