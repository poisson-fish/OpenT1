#pragma once
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/sim/sim_environment_render.hpp>

namespace opent1::sim {
// Renderer-independent state of the original volumetric object. Layer 5/6 can
// consume these owned parameters to draw faces/load DML without owning physics.
struct VolumetricState {
    Point3F position, dimensions{60, 60, 30}, currentVelocity;
    Flt32 textureScale{.01f}, transparency{.2f}, animationScale{1}, damagePerSecond{},
        currentDrag{}, containerDrag{}, density{1}, boxExtra{};
    Int32 subdivisions{1}, volumeType{1010520};
    std::string materialName;
    UInt16 faces{63}; // top, down, left, right, front, back
    [[nodiscard]] bool valid() const;
    bool operator==(const VolumetricState&) const = default;
};

class SimVolumetric : public SimDefaultOpenContainer {
public:
    enum UpdateMask : UInt32 { Moved = 1, Edited = 2 };

    SimVolumetric();
    bool onAdd() override;
    bool processQuery(SimQuery&) override;
    bool inspectWrite(Inspect&) const override;
    bool inspectRead(Inspect&) override;

    [[nodiscard]] const VolumetricState& state() const {
        return state_;
    }

    bool setState(VolumetricState);

    [[nodiscard]] Box3F displayBox() const {
        return {state_.position, state_.position + state_.dimensions};
    }

    [[nodiscard]] VolumetricGeometry renderGeometry(const VolumetricView& view) const {
        return buildVolumetricGeometry(state_, view);
    }

    // Geometric overlap, including a box spanning the entire volume. Degenerate
    // axes use point containment, so thin/point objects never divide by zero.
    [[nodiscard]] Flt32 submergedFraction(const Box3F&) const;
    bool applyInfluence(SimMovement&);
    bool updateObject(SimContainer&) override;
    bool packUpdate(BitStream&, UInt32 mask) const;
    bool unpackUpdate(BitStream&);

    UInt16 getGhostTag() const override {
        return 134;
    }

    UInt32 packUpdate(network::GhostManager&, UInt32 mask, BitStream& b) const override {
        packUpdate(b, mask & 3);
        return 0;
    }

    bool unpackUpdate(network::GhostManager&, BitStream& b) override {
        return unpackUpdate(b);
    }

    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

protected:
    void enterContainer(SimContainer&) override;
    void leaveContainer(SimContainer&) override;

private:
    VolumetricState state_;
};

bool registerCommonClasses(Persistent::ClassRegistry&);

struct ShadowDetailSettings {
    Int32 bitmapDimension{64}, blurMethod{3};
    Flt32 updateDelta{};
    bool visible{};
};

// Original BaseShadowRenderImage detail policy, without TS/GPU/global state.
// CPU source windows/projection/filtering are in sim_environment_render.hpp.
// TS silhouette rasterization and receiver collection/drawing need render layers.
[[nodiscard]] ShadowDetailSettings
chooseShadowDetails(Flt32 projectedSize, Flt32 maxPlaneDot, Flt32 detailScale = 1);
// IRC's independent socket/session/client/plugin API is in sim_irc.hpp.
} // namespace opent1::sim
