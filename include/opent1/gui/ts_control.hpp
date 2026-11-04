#pragma once
#include <opent1/gui/gui.hpp>
#include <opent1/world/world_scene.hpp>
#include <opent1/sim/sim_editor_events.hpp>

namespace opent1::gui {
struct MouseRay {
    Event event;
    Point3F origin, direction;
};

// A viewport retains the complete prepared scene and camera. A failed camera or
// resource query leaves the previous scene usable after world/resource removal.
class TSControl : public Control {
public:
    ~TSControl() override;
    bool produce3DMouseEvents{true};
    // The game host can submit the world separately (including GPU batches).
    bool sceneRenderedExternally{false};
    std::weak_ptr<sim::SimWorld> world;
    // Editors may render a server scene through a camera owned by the client.
    // Resolve the scene on every frame; never retain a replaced world's pointer.
    std::function<sim::SimWorld*()> sceneWorld;
    std::function<std::optional<sim::EnvironmentCamera>(Point2I)> camera;
    std::function<bool(const MouseRay&)> mouse;
    std::function<bool(const Event&)> input;
    std::function<bool(gfx::SoftwareSurface&, Point2I, RectI)> overlay;
    bool attach(sim::SimWorld&, sim::SimObject*, sim::SimObject* data = nullptr);
    bool nextCamera(sim::SimWorld&);
    bool queryCamera(sim::SimWorld&);

    [[nodiscard]] sim::SimAddress attached() const {
        return attached_;
    }

    bool prepare(sim::SimWorld&, const sim::EnvironmentCamera&, bool lighting = true);

    [[nodiscard]] std::shared_ptr<const world::WorldSceneFrame> frame() const {
        return frame_;
    }

    [[nodiscard]] const std::optional<sim::EnvironmentCamera>& cameraView() const {
        return renderedCamera_ ? renderedCamera_ : camera_;
    }

    [[nodiscard]] std::optional<MouseRay> ray(const Event&) const;
    [[nodiscard]] std::optional<sim::SimCollisionInfo>
    pick(sim::SimWorld&, const Event&, UInt32 mask = ~UInt32{}) const;
    void tick(double) override;
    bool event(const Event&) override;
    bool wantsTab() const override { return bool(input); }
    bool becomeFirstResponder() const override { return bool(input); }
    bool processEvent(const sim::SimEvent&) override;
    void close() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject&) override;
    bool draw(gfx::SoftwareSurface&, Point2I, RectI) const override;
    std::function<void()> checkpoint() override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    std::shared_ptr<const world::WorldSceneFrame> frame_;
    std::optional<sim::EnvironmentCamera> camera_;
    mutable std::optional<sim::EnvironmentCamera> renderedCamera_;
    std::optional<sim::SimCameraQuery> nativeCamera_;
    sim::SimAddress attached_;
    sim::SimObject* observed_{}; // guarded by SimObject delete notifications
    bool switching_{};
    bool selectCamera(sim::SimManager&, sim::SimObject*, sim::SimObject* data = nullptr);
    void detachCamera();
};

bool registerViewportClass(Persistent::ClassRegistry&);
} // namespace opent1::gui
