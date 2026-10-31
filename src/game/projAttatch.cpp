#include <opent1/game/proj_attach.hpp>

#include <algorithm>
#include <cmath>

namespace opent1::game {

// -----------------------------------------------------------------------------
// SwooshTrail
// -----------------------------------------------------------------------------

SwooshTrail::SwooshTrail(Flt32 in_totalLength, UInt32 in_numSections, Flt32 in_initialTrailWidth)
    : m_totalLength_(in_totalLength), m_numSections_(std::max<UInt32>(1u, in_numSections)),
      m_lengthTrigger_(m_totalLength_ / static_cast<Flt32>(m_numSections_)),
      m_initialWidth_(in_initialTrailWidth), m_numActiveSections_(0), m_rendering_(false),
      m_pointChain_(static_cast<std::size_t>(m_numSections_) + 1u) {
    itype = AttachedTrail::Translucent;
}

void SwooshTrail::updateTransform(const TMat3F& in_newTransform, Flt32 /*in_newTime*/) {
    if (m_numActiveSections_ != 0) {
        m_rendering_ = true;

        const Point3F firstSectionVec = in_newTransform.p - m_pointChain_[1].centerPoint;
        const Flt32 firstSectionLen = firstSectionVec.len();

        if (firstSectionLen <= m_lengthTrigger_) {
            PointBar& rBarFront = m_pointChain_[0];
            rBarFront.centerPoint = in_newTransform.p;
            const auto& rMat = static_cast<const RMat3F&>(in_newTransform);
            m_mul(Point3F{1.0f, 0.0f, 0.0f}, rMat, rBarFront.unitLeft);
            rBarFront.leftPoint = rBarFront.centerPoint + (rBarFront.unitLeft * m_initialWidth_);
            rBarFront.rightPoint = rBarFront.centerPoint - (rBarFront.unitLeft * m_initialWidth_);

            const Point3F* pBeginPoint = &rBarFront.centerPoint;
            Flt32 totalLength = 0.0f;
            for (UInt32 i = 1; i <= m_numActiveSections_ - 1; ++i) {
                PointBar& rCurrent = m_pointChain_[i];
                const Point3F& rEndPoint = rCurrent.centerPoint;
                totalLength += (rEndPoint - *pBeginPoint).len();

                const Flt32 width = (1.0f - (totalLength / m_totalLength_)) * m_initialWidth_;
                rCurrent.leftPoint = rCurrent.centerPoint + (rCurrent.unitLeft * width);
                rCurrent.rightPoint = rCurrent.centerPoint - (rCurrent.unitLeft * width);

                pBeginPoint = &rEndPoint;
            }

            if (m_numActiveSections_ > 1) {
                PointBar& rBarPenultimate = m_pointChain_[m_numActiveSections_ - 1];
                PointBar& rBarBack = m_pointChain_[m_numActiveSections_];

                Point3F diffVec = rBarBack.centerPoint - rBarPenultimate.centerPoint;
                const Flt32 diffLen = diffVec.len();
                const Flt32 targetLen =
                    (m_lengthTrigger_ * static_cast<Flt32>(m_numActiveSections_ - 1)) - totalLength;
                if (diffLen > 0.0f) {
                    diffVec *= targetLen / diffLen;
                }
                rBarBack.centerPoint = rBarPenultimate.centerPoint + diffVec;
            }
        } else {
            if (m_numActiveSections_ < m_numSections_) {
                for (UInt32 i = m_numActiveSections_ + 1; i > 0; --i) {
                    m_pointChain_[i] = m_pointChain_[i - 1];
                }
                m_numActiveSections_ += 1;
            } else {
                for (UInt32 i = m_numActiveSections_; i > 0; --i) {
                    m_pointChain_[i] = m_pointChain_[i - 1];
                }
            }

            PointBar& rBarFront = m_pointChain_[0];
            rBarFront.centerPoint = in_newTransform.p;
            const auto& rMat = static_cast<const RMat3F&>(in_newTransform);
            m_mul(Point3F{1.0f, 0.0f, 0.0f}, rMat, rBarFront.unitLeft);
            rBarFront.leftPoint = rBarFront.centerPoint + (rBarFront.unitLeft * m_initialWidth_);
            rBarFront.rightPoint = rBarFront.centerPoint - (rBarFront.unitLeft * m_initialWidth_);

            m_pointChain_[m_numActiveSections_].leftPoint =
                m_pointChain_[m_numActiveSections_].centerPoint;
            m_pointChain_[m_numActiveSections_].rightPoint =
                m_pointChain_[m_numActiveSections_].centerPoint;

            const Point3F* pBeginPoint = &rBarFront.centerPoint;
            Flt32 totalLength = 0.0f;
            for (UInt32 i = 1; i <= m_numActiveSections_ - 1; ++i) {
                PointBar& rCurrent = m_pointChain_[i];
                const Point3F& rEndPoint = rCurrent.centerPoint;
                totalLength += (rEndPoint - *pBeginPoint).len();

                const Flt32 width = (1.0f - (totalLength / m_totalLength_)) * m_initialWidth_;
                rCurrent.leftPoint = rCurrent.centerPoint + (rCurrent.unitLeft * width);
                rCurrent.rightPoint = rCurrent.centerPoint - (rCurrent.unitLeft * width);

                pBeginPoint = &rEndPoint;
            }
        }
    } else {
        PointBar& rBarFront = m_pointChain_[0];
        PointBar& rBarBack = m_pointChain_[1];

        rBarFront.centerPoint = in_newTransform.p;
        const auto& rMat = static_cast<const RMat3F&>(in_newTransform);
        m_mul(Point3F{1.0f, 0.0f, 0.0f}, rMat, rBarFront.unitLeft);

        rBarFront.leftPoint = rBarFront.centerPoint + (rBarFront.unitLeft * m_initialWidth_);
        rBarFront.rightPoint = rBarFront.centerPoint - (rBarFront.unitLeft * m_initialWidth_);

        rBarBack.centerPoint = rBarFront.centerPoint;
        rBarBack.leftPoint = rBarFront.centerPoint;
        rBarBack.rightPoint = rBarFront.centerPoint;
        rBarBack.unitLeft = rBarFront.unitLeft;

        m_numActiveSections_ = 1;
    }
}

void SwooshTrail::setSortValueFromCamera(const TMat3F& worldToCamera, Flt32 nearDist) {
    if (itype == AttachedTrail::Translucent && m_numActiveSections_ != 0 && m_rendering_) {
        const PointBar& rBar = m_pointChain_[m_numActiveSections_];
        Point3F pCam{};
        m_mul(rBar.centerPoint, worldToCamera, pCam);
        sortValue = (pCam.y <= nearDist) ? 0.99f : (nearDist / pCam.y);
    } else {
        sortValue = 0.0f;
    }
}

bool SwooshTrail::getBoundingBox(Box3F& /*out_rBox*/) const noexcept {
    return false;
}

// -----------------------------------------------------------------------------
// SimpleSwooshTrail
// -----------------------------------------------------------------------------

SimpleSwooshTrail::SimpleSwooshTrail(Flt32 in_length, Flt32 in_trailWidth)
    : m_totalLength_(in_length), m_initialWidth_(in_trailWidth), m_set_(false),
      m_rendering_(false) {
    itype = AttachedTrail::Translucent;
}

void SimpleSwooshTrail::setResources(const Resource<gfx::Bitmap>& rBitmap) {
    m_hBitmap_ = rBitmap;
}

void SimpleSwooshTrail::loadResources(std::string_view in_textureName,
                                      sim::SimManager* io_pManager) {
    m_textureName_.assign(in_textureName);
    if (!io_pManager || m_textureName_.empty()) {
        return;
    }
    if (auto* sr = sim::SimResource::find(*io_pManager)) {
        m_hBitmap_ = sr->get().load<gfx::Bitmap>(m_textureName_);
    }
}

void SimpleSwooshTrail::updateTransform(const TMat3F& in_newTransform, Flt32 in_newTime) {
    m_lastTime_ = in_newTime;

    if (m_set_) {
        m_rendering_ = true;

        m_centerPoint_ = in_newTransform.p;
        const auto& rMat = static_cast<const RMat3F&>(in_newTransform);
        m_mul(Point3F{1.0f, 0.0f, 0.0f}, rMat, m_unitLeft_);

        Point3F diffVec = m_rearPoint_ - m_centerPoint_;
        const Flt32 newLen = diffVec.len();

        if (newLen > m_totalLength_ && newLen > 0.0f) {
            diffVec *= m_totalLength_ / newLen;
            m_rearPoint_ = m_centerPoint_ + diffVec;
        }
    } else {
        m_centerPoint_ = in_newTransform.p;
        m_rearPoint_ = m_centerPoint_;

        const auto& rMat = static_cast<const RMat3F&>(in_newTransform);
        m_mul(Point3F{1.0f, 0.0f, 0.0f}, rMat, m_unitLeft_);

        m_set_ = true;
    }
}

void SimpleSwooshTrail::setSortValueFromCamera(const TMat3F& worldToCamera, Flt32 nearDist) {
    if (itype == AttachedTrail::Translucent && m_rendering_) {
        Point3F pCam{};
        m_mul(m_rearPoint_, worldToCamera, pCam);
        sortValue = (pCam.y <= nearDist) ? 0.99f : (nearDist / pCam.y);
    } else {
        sortValue = 0.0f;
    }
}

bool SimpleSwooshTrail::getBoundingBox(Box3F& /*out_rBox*/) const noexcept {
    return false;
}

std::array<Point3F, 4> SimpleSwooshTrail::quadVertices() const noexcept {
    return {m_rearPoint_ + (m_unitLeft_ * (m_initialWidth_ / 6.0f)),
            m_rearPoint_ - (m_unitLeft_ * (m_initialWidth_ / 6.0f)),
            m_centerPoint_ - (m_unitLeft_ * (m_initialWidth_ / 2.0f)),
            m_centerPoint_ + (m_unitLeft_ * (m_initialWidth_ / 2.0f))};
}

std::array<Point2F, 4> SimpleSwooshTrail::textureCoords() const noexcept {
    const Flt32 yVal =
        (m_lastTime_ * 5.0f) - static_cast<Flt32>(static_cast<Int32>(m_lastTime_ * 5.0f));
    return {Point2F{1.0f, (0.0f + yVal) / 2.0f},
            Point2F{0.0f, (0.0f + yVal) / 2.0f},
            Point2F{0.0f, (1.0f + yVal) / 2.0f},
            Point2F{1.0f, (1.0f + yVal) / 2.0f}};
}

} // namespace opent1::game
