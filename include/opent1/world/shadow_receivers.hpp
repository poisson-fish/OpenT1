#pragma once
#include <opent1/world/world_surfaces.hpp>

namespace opent1::world {
// Session-owned, bounded derived data. Entries weakly identify immutable source
// geometry, so mission changes cannot keep old assets alive or reuse stale IDs.
class ShadowReceiverCache {
public:
    ShadowReceiverCache();
    ~ShadowReceiverCache();
    void clear();
    ShadowReceiverCache(const ShadowReceiverCache&) = delete;
    ShadowReceiverCache& operator=(const ShadowReceiverCache&) = delete;

private:
    friend class ShadowReceiverScene;
    struct State;
    std::unique_ptr<State> state_;
};

// Prepared once per shadow pass. Local-space indexes are shared by all casters;
// origin rebases the query and output without modifying the retained geometry.
class ShadowReceiverScene {
public:
    explicit ShadowReceiverScene(std::span<const WorldSurfaceInstance>,
                                 ShadowReceiverCache* = nullptr);
    ~ShadowReceiverScene();
    [[nodiscard]] std::optional<std::vector<sim::ShadowReceiver>>
    collect(const sim::ShadowSourceWindow&,
            Flt32 length = 100,
            Point3F origin = {},
            std::size_t* sharedWork = nullptr) const;
    [[nodiscard]] std::optional<sim::ShadowGeometry>
    geometry(const sim::ShadowSourceWindow&,
             Point3F cameraDirection,
             Flt32 length = 100,
             Flt32 lift = .1f,
             Point3F origin = {},
             std::size_t* sharedWork = nullptr) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
