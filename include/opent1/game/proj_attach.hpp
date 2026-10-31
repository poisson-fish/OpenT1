#pragma once

#include <opent1/core/res_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/math/box.hpp>
#include <opent1/math/matrix.hpp>
#include <opent1/math/point.hpp>
#include <opent1/sim/sim_base.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opent1::game {

class AttachedTrail : public FearRenderImage {
public:
    enum ImageType : Int32 {
        Normal = 0,
        Translucent = 1,
    };

    Int32 itype{Translucent};
    Flt32 sortValue{0.0f};

    virtual ~AttachedTrail() = default;

    virtual void setResources(const Resource<gfx::Bitmap>& rBitmap) = 0;

    virtual void loadResources(std::string_view textureName, sim::SimManager* manager) {
        (void)textureName;
        (void)manager;
    }

    virtual void updateTransform(const TMat3F& newTransform, Flt32 newTime) = 0;
    virtual void setSortValueFromCamera(const TMat3F& worldToCamera, Flt32 nearDist) = 0;
    virtual bool getBoundingBox(Box3F& outBox) const noexcept = 0;
};

class SwooshTrail : public AttachedTrail {
public:
    struct PointBar {
        Point3F centerPoint{0.0f, 0.0f, 0.0f};
        Point3F leftPoint{0.0f, 0.0f, 0.0f};
        Point3F rightPoint{0.0f, 0.0f, 0.0f};
        Point3F unitLeft{1.0f, 0.0f, 0.0f};
    };

    SwooshTrail(Flt32 in_totalLength, UInt32 in_numSections, Flt32 in_initialTrailWidth);
    ~SwooshTrail() override = default;

    void setResources(const Resource<gfx::Bitmap>&) override {}

    void updateTransform(const TMat3F& in_newTransform, Flt32 in_newTime) override;
    void setSortValueFromCamera(const TMat3F& worldToCamera, Flt32 nearDist) override;
    bool getBoundingBox(Box3F& out_rBox) const noexcept override;

    [[nodiscard]] Flt32 totalLength() const noexcept {
        return m_totalLength_;
    }

    [[nodiscard]] UInt32 numSections() const noexcept {
        return m_numSections_;
    }

    [[nodiscard]] Flt32 lengthTrigger() const noexcept {
        return m_lengthTrigger_;
    }

    [[nodiscard]] Flt32 initialWidth() const noexcept {
        return m_initialWidth_;
    }

    [[nodiscard]] UInt32 numActiveSections() const noexcept {
        return m_numActiveSections_;
    }

    [[nodiscard]] bool isRendering() const noexcept {
        return m_rendering_;
    }

    [[nodiscard]] std::span<const PointBar> pointChain() const noexcept {
        return m_pointChain_;
    }

private:
    Flt32 m_totalLength_{0.0f};
    UInt32 m_numSections_{1};
    Flt32 m_lengthTrigger_{0.0f};
    Flt32 m_initialWidth_{0.0f};
    UInt32 m_numActiveSections_{0};
    bool m_rendering_{false};
    std::vector<PointBar> m_pointChain_{};
};

class SimpleSwooshTrail : public AttachedTrail {
public:
    SimpleSwooshTrail(Flt32 in_length, Flt32 in_trailWidth);
    ~SimpleSwooshTrail() override = default;

    void setResources(const Resource<gfx::Bitmap>& rBitmap) override;
    void loadResources(std::string_view in_textureName, sim::SimManager* io_pManager) override;
    void updateTransform(const TMat3F& in_newTransform, Flt32 in_newTime) override;
    void setSortValueFromCamera(const TMat3F& worldToCamera, Flt32 nearDist) override;
    bool getBoundingBox(Box3F& out_rBox) const noexcept override;

    [[nodiscard]] const Point3F& centerPoint() const noexcept {
        return m_centerPoint_;
    }

    [[nodiscard]] const Point3F& unitLeft() const noexcept {
        return m_unitLeft_;
    }

    [[nodiscard]] const Point3F& rearPoint() const noexcept {
        return m_rearPoint_;
    }

    [[nodiscard]] Flt32 totalLength() const noexcept {
        return m_totalLength_;
    }

    [[nodiscard]] Flt32 initialWidth() const noexcept {
        return m_initialWidth_;
    }

    [[nodiscard]] bool isSet() const noexcept {
        return m_set_;
    }

    [[nodiscard]] bool isRendering() const noexcept {
        return m_rendering_;
    }

    [[nodiscard]] Flt32 lastTime() const noexcept {
        return m_lastTime_;
    }

    [[nodiscard]] const std::string& textureName() const noexcept {
        return m_textureName_;
    }

    [[nodiscard]] const Resource<gfx::Bitmap>& bitmap() const noexcept {
        return m_hBitmap_;
    }

    [[nodiscard]] std::array<Point3F, 4> quadVertices() const noexcept;
    [[nodiscard]] std::array<Point2F, 4> textureCoords() const noexcept;

private:
    Point3F m_centerPoint_{0.0f, 0.0f, 0.0f};
    Point3F m_unitLeft_{1.0f, 0.0f, 0.0f};
    Point3F m_rearPoint_{0.0f, 0.0f, 0.0f};
    Flt32 m_totalLength_{0.0f};
    Flt32 m_initialWidth_{0.0f};
    bool m_set_{false};
    bool m_rendering_{false};
    Flt32 m_lastTime_{0.0f};
    std::string m_textureName_{};
    Resource<gfx::Bitmap> m_hBitmap_{};
};

} // namespace opent1::game
