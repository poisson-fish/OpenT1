#pragma once
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/sim/sim_collision_movement.hpp>
#include <opent1/math/point.hpp>
#include <opent1/gfx/palette.hpp>
#include <opent1/gfx/directional_shadow.hpp>
#include <opent1/sim/sim_environment_render.hpp>

namespace opent1::sim {
inline constexpr SimObjectId CanvasSetId = 3, FrameEndNotifySetId = 6, TerrainId = 8,
                             CameraSetId = 9, TimerSetId = 12, RenderSetId = 13, LightSetId = 14,
                             CameraMountSetId = 23, GhostAlwaysSetId = 34, CSDelegateId = 35,
                             GhostManagerSetId = 36, PacketStreamSetId = 38,
                             ConsoleSchedulerId = 43, NamedGuiSetId = 44, TaggedGuiSetId = 45;

struct SimTimerEvent : SimEvent {
    SimTimerEvent() {
        type = EventTypeFirst + 10;
    }

    SimObjectId timerId{};
    SimTime timerInterval{};
};

struct SimMessageEvent : SimEvent {
    SimMessageEvent() {
        type = EventTypeFirst + 1;
    }

    Int32 message{};
};

class SimTimerSet : public SimSet {
public:
    explicit SimTimerSet(SimTime interval = .03f, SimObjectId id = TimerSetId);
    bool onAdd() override;

    void onRemove() override {
        running_ = false;
    }

    bool processEvent(const SimEvent&) override;

    [[nodiscard]] SimTime interval() const {
        return interval_;
    }

    [[nodiscard]] bool running() const {
        return running_;
    }

private:
    bool scheduleNext();
    SimTime interval_;
    bool running_{};
};

struct SimConsoleEvent : SimEvent {
    SimConsoleEvent() {
        type = EventTypeFirst + 8;
    }

    static constexpr std::size_t MaxArgs = 31, MaxCommandBytes = 512;
    // script present => evaluate; otherwise execute arguments[0] as command.
    std::optional<std::string> script;
    std::vector<std::string> arguments;
    bool echo{};
    [[nodiscard]] bool valid() const;
    bool execute(Console&) const;
    bool packCommand(BitStream&) const;
    // Decode the legacy remote prefix/source ID insertion atomically.
    bool unpackCommand(BitStream&, SimObjectId source);
    static bool schedule(SimObject&, std::string_view, SimTime delay, bool echo = false);
};

class SimConsoleScheduler : public SimObject {
public:
    SimConsoleScheduler() {
        setId(ConsoleSchedulerId);
    }
};

// Renderer-independent lighting snapshots; TS/GPU adapters consume these values
// when those layers migrate. Query results own data instead of borrowing lights.
struct SimLight {
    enum class Type { Directional, Point, DirectionalNonlinear, DirectionalWrap };
    Type type{Type::Point};
    Point3F position{}, direction{};
    ColorF intensity{};
    Flt32 range{1};
    Point3F attenuation{1, 0, 0};
    Flt32 shadowAttenuation{1};
    std::shared_ptr<const gfx::DirectionalShadowMap> shadowMap{};
};

struct SimLightQuery : SimQuery {
    static constexpr std::size_t MaxLights = 8; // one light per mounted player image
    SimLightQuery() {
        type = EventTypeFirst + 3;
    }

    ColorF ambient;
    std::vector<SimLight> lights;
};

struct SimSceneLighting {
    ColorF ambient;
    std::vector<SimLight> lights;
};

class SimLightSet : public SimSet {
public:
    SimLightSet() {
        setId(LightSetId);
    }

    [[nodiscard]] SimSceneLighting getSceneLighting(std::optional<Point3F> camera = {},
                                                    Flt32 ignoreDistance = 1e10f);

    bool setIgnoreDistance(Flt32 value) {
        if (!std::isfinite(value) || value < 0 || value > 1e12f) {
            return false;
        }
        ignoreDistance_ = value;
        return true;
    }

    [[nodiscard]] Flt32 ignoreDistance() const {
        return ignoreDistance_;
    }

private:
    Flt32 ignoreDistance_{1e10f};
};

struct SimDynamicLightUpdateEvent : SimEvent {
    SimDynamicLightUpdateEvent() {
        type = EventTypeFirst + 28;
    }

    SimAddress light;
    Point3F worldPosition;
    Flt32 radius{1};
    UInt32 packedColor{0xffffffffU};
};

struct SimDynamicLightDecRefCntEvent : SimEvent {
    SimDynamicLightDecRefCntEvent() {
        type = EventTypeFirst + 29;
    }

    SimAddress light;
};

// Sends updates to intersecting open containers, followed by decrements to the
// previous snapshot. Consumers retain values keyed by the light's generation,
// increment on update and remove on decrement to zero. No light pointers escape.
class SimDynamicLight : public SimObject {
public:
    SimDynamicLight() = default;
    bool onAdd() override;
    void onRemove() override;
    [[nodiscard]] UInt8 getIntensity(Flt32 distanceSquared) const;

    [[nodiscard]] const Point3F& getPosition() const {
        return position_;
    }

    [[nodiscard]] const TMat3F& getTransform() const {
        return transform_;
    }

    [[nodiscard]] Flt32 getRadius() const {
        return radius_;
    }

    [[nodiscard]] UInt32 getPackedColor() const {
        return packedColor_;
    }

    [[nodiscard]] const Box3F& getBoundingBox() const {
        return box_;
    }

    bool setPosition(const Point3F&);
    bool setTransform(const TMat3F&);
    bool setRadius(Flt32);
    bool setPackedColor(UInt32);
    bool setTolerances(Flt32 movement, Flt32 radius);
    void updateLight();
    void clearAffectedObjects();

private:
    std::vector<SimAddress> affected_;
    Point3F position_;
    TMat3F transform_;
    Box3F box_{{-1, -1, -1}, {1, 1, 1}};
    Flt32 radius_{1}, movementTolerance_{}, radiusTolerance_{};
    UInt32 packedColor_{0xffffffffU};
    bool updating_{}, clearRequested_{};
};

// Native windows and rendering stay in optional adapters. The simulation owns
// canvas membership and frame ordering without depending on SDL or GPU types.
class SimCanvas : public SimObject {
public:
    bool onAdd() override;

    void onRemove() override {
        palette_.reset();
    }

    bool setPalette(Resource<gfx::Palette>, Int32 transitionColor = 0);

    [[nodiscard]] Resource<gfx::Palette> getPalette() const {
        return palette_;
    }

    [[nodiscard]] Int32 paletteTransitionColor() const {
        return transition_;
    }

    virtual bool setTitle(std::string_view) = 0;
    virtual void renderFrame() = 0;

protected:
    // Renderer adapters apply the palette/optional indexed transition clear.
    virtual void paletteChanged() {}

private:
    Resource<gfx::Palette> palette_;
    Int32 transition_{-1};
};

class SimCanvasSet : public SimSet {
public:
    SimCanvasSet() {
        setId(CanvasSetId);
        setStable(true);
    }

    bool addObject(SimObject&) override;
    void render();
    bool setPalette(Resource<gfx::Palette>, Int32 transitionColor = 0);

    [[nodiscard]] Resource<gfx::Palette> getPalette() const {
        return palette_;
    }

private:
    Resource<gfx::Palette> palette_;
    UInt64 paletteRevision_{};
};

// Layer 5/6 consumers receive owned geometry and explicit animation inputs.
// Query providers append no borrowed material, object or renderer pointers.
struct SimVolumetricRenderQuery : SimQuery {
    SimVolumetricRenderQuery() {
        type = EventTypeFirst + 2;
    }

    VolumetricView view;
    std::optional<VolumetricGeometry> geometry;
    Flt32 animationScale{1};
};

struct SimVolumetricRenderImage {
    SimAddress source;
    VolumetricGeometry geometry;
    Flt32 time{}, animationScale{1};

    [[nodiscard]] std::size_t frame(std::size_t materialCount) const {
        return volumetricAnimationFrame(time, animationScale, materialCount);
    }
};

// Owns simulation services. Rendering hooks are explicit until the Layer 5
// surface/canvas and Layer 6 camera/GUI implementations exist. Spatial queries
// use the installed root container independently of those render integrations.
class SimWorld {
public:
    explicit SimWorld(Console* console = nullptr, std::shared_ptr<ResourceManager> resources = {});
    virtual ~SimWorld() = default;
    SimWorld(const SimWorld&) = delete;
    SimWorld& operator=(const SimWorld&) = delete;

    [[nodiscard]] SimManager& manager() {
        return manager_;
    }

    [[nodiscard]] const SimManager& manager() const {
        return manager_;
    }

    virtual std::size_t advanceToTime(SimTime,
                                      std::size_t budget = std::numeric_limits<std::size_t>::max());
    void render();
    [[nodiscard]] std::vector<SimVolumetricRenderImage>
    gatherVolumetrics(const VolumetricView&,
                      std::optional<std::span<const SimAddress>> sources = std::nullopt);
    bool setPalette(std::string_view, Int32 transitionColor = 0);
    bool setPalette(Resource<gfx::Palette>, Int32 transitionColor = 0);
    [[nodiscard]] Resource<gfx::Palette> getPalette() const;
    void delPalette();

    void enableRender(bool value) {
        renderEnabled_ = value;
    }

    void setRenderHook(std::function<void()> hook) {
        render_ = std::move(hook);
    }

private:
    SimManager manager_;
    bool renderEnabled_{true}, advancing_{}, rendering_{};
    std::function<void()> render_;
};

class SimConsolePlugin {
public:
    virtual ~SimConsolePlugin() = default;

    virtual void init(Console&, SimManager*) {}

    virtual void shutdown() noexcept {}

    virtual void setManager(SimManager*) {}

    virtual void startFrame() {}

    virtual void endFrame() {}
};
enum class WorldType : std::size_t { Server, Client };

// Owned editor document. Its target is bound to one game/world incarnation;
// delayed UI edits cannot affect a recycled object ID or replacement world.
class SimInspection {
public:
    [[nodiscard]] std::string_view objectName() const {
        return name_;
    }

    [[nodiscard]] std::string_view className() const {
        return className_;
    }

    [[nodiscard]] std::span<const Inspect::Record> records() const {
        return fields_.records();
    }

    bool setValue(std::size_t index, Inspect::Value value) {
        return fields_.setValue(index, std::move(value));
    }

private:
    friend class SimGame;
    Inspect fields_;
    std::string name_, className_;
    std::shared_ptr<const UInt8> owner_;
    WorldType world_{};
    UInt64 revision_{};
    SimAddress address_;
    std::vector<std::pair<std::size_t, SimAddress>> actions_;
};

// Game-layer providers own their data-block namespace and typed fields. begin
// must leave no declaration on failure; abort rolls back a successful begin or
// failed end. contains/get/set include a provider's current pending declarations.
class SimDataBlockManager : public ConsoleDataManager {
public:
    [[nodiscard]] virtual bool contains(std::string_view name) const = 0;
    void declareDataBlock(std::string_view className, std::string_view name) override = 0;
    void endDataBlock() override = 0;
    void abortDataBlock() override = 0;
    std::string getDataField(std::string_view,
                             std::string_view,
                             std::optional<std::string_view>) override = 0;
    void setDataField(std::optional<std::string_view>,
                      std::string_view,
                      std::optional<std::string_view>,
                      std::span<const std::string>) override = 0;
};

class SimGame {
public:
    using Factory = std::function<std::unique_ptr<SimObject>()>;
    using Inspector = std::function<bool(SimInspection)>;
    explicit SimGame(std::shared_ptr<ResourceManager> resources = {});
    ~SimGame();
    SimGame(const SimGame&) = delete;
    SimGame& operator=(const SimGame&) = delete;

    [[nodiscard]] Console& console() {
        return console_;
    }

    [[nodiscard]] Persistent::ClassRegistry& classes() {
        return classes_;
    }

    [[nodiscard]] DataFieldManager& dataFields() {
        return dataFields_;
    }

    [[nodiscard]] SimWorld* world(WorldType) const;

    [[nodiscard]] UInt64 worldRevision(WorldType type) const {
        return worldRevisions_[static_cast<std::size_t>(type)];
    }

    [[nodiscard]] SimManager* manager() const;
    bool setWorld(WorldType, std::unique_ptr<SimWorld>);
    bool focus(std::optional<WorldType>);
    // Execute simulation/script callbacks in a world without changing input
    // focus or releasing held actions on the interactive client.
    void withWorld(WorldType, const std::function<void()>&);
    bool registerFactory(std::string_view name, Factory);
    [[nodiscard]] std::shared_ptr<void> scopedFactory(std::string_view name, Factory);
    // Prepare decoded objects before any identity/onAdd publication. Callbacks
    // may bind resources but must leave registered simulation objects alone.
    // A rejection/exception discards the entire new graph. Leases may outlive us.
    using LoadPreparation = std::function<bool(SimObject&, ResourceManager&)>;
    [[nodiscard]] std::shared_ptr<void> scopedLoadPreparation(std::string_view, LoadPreparation);
    bool registerPlugin(std::shared_ptr<SimConsolePlugin>);
    bool removePlugin(const std::shared_ptr<SimConsolePlugin>&);
    // Explicit monotonic wall-clock time (seconds), suitable for SDL callbacks
    // and deterministic replay. Paused scales (zero) retain event delays.
    bool setTimeScale(double);
    // Hosts which render after simulation/audio can skip the automatic canvas
    // pass. Explicit script redraws still execute while advancing the world.
    void frame(double wallTime, bool renderCanvases = true);

    [[nodiscard]] double simulationTime() const {
        return simTime_;
    }

    // Script mission time can restart while event queues and frame clocks stay
    // monotonic, including when a mission changes inside a scheduled callback.
    void resetScriptTime(SimTime base) noexcept {
        scriptTimeBase_ = base;
    }

    [[nodiscard]] SimObject* findObject(std::string_view, SimManager* preferred = nullptr) const;
    SimObject* createObject(std::string_view className,
                            std::string_view name = {},
                            SimGroup* parent = nullptr,
                            std::span<const std::string> arguments = {});
    SimObject* loadObject(std::string_view name, std::string_view filename);
    // Clipboard/mission hosts share file loading's pre-adoption resource phase.
    // The complete byte record must be consumed; target failure rolls back all
    // newly adopted objects. No temporary files or borrowed decoded graph.
    SimObject* loadObject(std::string_view name,
                          std::span<const std::byte>,
                          SimSet* destination = nullptr,
                          const std::function<bool(SimObject&)>& accept = {});
    [[nodiscard]] std::optional<std::vector<std::byte>> encodeObject(const SimObject&,
                                                                     Int32 user = 0);

    [[nodiscard]] std::weak_ptr<const void> lifetime() const {
        return factoryLifetime_;
    }

    bool storeObject(const SimObject&, const std::filesystem::path&, Int32 user = 0);
    [[nodiscard]] std::optional<SimInspection> inspectObject(std::string_view);
    bool applyInspection(const SimInspection&);
    bool activateInspection(const SimInspection&, std::size_t record);

    void setInspector(Inspector presenter) {
        inspector_ = std::move(presenter);
    }

    // Provider replacement is rejected during game/field/declaration callbacks.
    bool setDataBlockManager(std::shared_ptr<SimDataBlockManager>);

private:
    friend class SimConsoleDataManager;
    std::unique_ptr<SimObject> makeObject(std::string_view);
    SimObject* loadObjectStream(std::string_view,
                                StreamIO&,
                                SimSet*,
                                Int32 exactBytes,
                                const std::function<bool(SimObject&)>&);
    void withWorlds(const std::function<void()>&);
    std::optional<std::string> command(Console&, Int32, std::span<const std::string>);
    std::optional<std::string> invokeCommand(Console&, Int32, std::span<const std::string>);
    void setPluginManagers(SimManager*);
    SimManager* executionManager_{};
    std::shared_ptr<ResourceManager> resources_;
    Console console_;
    Persistent::ClassRegistry classes_;
    DataFieldManager dataFields_;
    std::array<std::unique_ptr<SimWorld>, 2> worlds_;
    std::optional<WorldType> focused_;
    std::vector<std::shared_ptr<SimConsolePlugin>> plugins_;
    std::unordered_map<std::string, Factory> factories_;
    std::map<std::string, LoadPreparation> loadPreparations_;
    std::shared_ptr<const void> factoryLifetime_{std::make_shared<int>(0)};
    std::shared_ptr<SimDataBlockManager> dataBlocks_;
    Inspector inspector_;
    std::shared_ptr<const UInt8> inspectionOwner_{std::make_shared<const UInt8>(0)};
    std::array<UInt64, 2> worldRevisions_{};
    double wallTime_{}, simTime_{}, timeScale_{1};
    SimTime scriptTimeBase_{};
    bool started_{}, driving_{}, switching_{};
    std::size_t declarationDepth_{};
};
} // namespace opent1::sim
