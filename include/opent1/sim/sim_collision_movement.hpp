#pragma once
#include <opent1/sim/sim_net.hpp>
#include <opent1/core/bitset.hpp>
#include <opent1/math/collision.hpp>
#include <functional>

namespace opent1::sim {
inline constexpr SimObjectId RootContainerId = 7;
inline constexpr UInt32 TerrainObjectType = 1U << 1, InteriorObjectType = 1U << 2,
                        MissionObjectType = 1U << 4, ContainerObjectType = 1U << 6,
                        MarkerObjectType = 1U << 10;
inline constexpr UInt32 MovementGravityForce = 1, MovementBuoyancyForce = 2;

struct SimCollisionMaterial {
    Flt32 friction{1}, elasticity{1};
    Int32 surfaceType{-1};
};

// Images are shared snapshots, retained across provider removal during a query.
// Shape parameters and transforms require external synchronization.
class SimCollisionImage {
public:
    enum Type : Int32 { Sphere, Box, Tube, Line, Terrain, Interior, TSShape, ApplicationType };

    explicit SimCollisionImage(Type type, Int32 priority) : type_(type), priority_(priority) {}

    virtual ~SimCollisionImage() = default;

    [[nodiscard]] Type getType() const {
        return type_;
    }

    [[nodiscard]] Int32 getPriority() const {
        return priority_;
    }

    bool moveable{}, trigger{};
    TMat3F transform;
    [[nodiscard]] TMat3F buildImageTransform(const SimCollisionImage&) const;
    // Result surfaces are in the higher-priority image's space, as in Darkstar.
    // Misses leave output intact. Both transforms must be rigid and finite.
    // Contained volume contacts may omit surfaces; lines/tubes require a hit position.
    static bool
    test(const SimCollisionImage& moving, const SimCollisionImage& other, CollisionSurfaceList&);
    virtual bool collide(const SimCollisionImage&, CollisionSurfaceList&) const = 0;

    // Optional owned material policy. Surface IDs may include slot zero; an
    // absent value retains the mover's existing application lookup/defaults.
    [[nodiscard]] virtual std::optional<SimCollisionMaterial> getMaterial(Int32) const {
        return {};
    }

    // The dispatch owner can produce contacts belonging to its peer (TS/world).
    [[nodiscard]] virtual std::optional<SimCollisionMaterial>
    getContactMaterial(const SimCollisionImage&, Int32 slot) const {
        return getMaterial(slot);
    }

private:
    Type type_;
    Int32 priority_;
};

class SimCollisionSphereImage : public SimCollisionImage {
public:
    SimCollisionSphereImage() : SimCollisionImage(Sphere, Sphere) {}

    Point3F center;
    Flt32 radius{1};
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
};

class SimCollisionBoxImage : public SimCollisionImage {
public:
    SimCollisionBoxImage() : SimCollisionImage(Box, Box) {}

    Box3F box{{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
};

class SimCollisionLineImage : public SimCollisionImage {
public:
    SimCollisionLineImage() : SimCollisionImage(Line, Line) {}

    Point3F start, end;
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
};

class SimCollisionTubeImage : public SimCollisionImage {
public:
    SimCollisionTubeImage() : SimCollisionImage(Tube, Tube) {}

    Point3F start, end;
    Flt32 radius{1};
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;
};

// Terrain/interior/TS collision engines implement this seam in Layer 6. The
// transform maps the argument into this image's space. No asset is approximated.
class SimCollisionBackendImage : public SimCollisionImage {
public:
    using Collider =
        std::function<bool(const SimCollisionImage&, const TMat3F&, CollisionSurfaceList&)>;
    SimCollisionBackendImage(Type, std::shared_ptr<const Collider>);
    bool collide(const SimCollisionImage&, CollisionSurfaceList&) const override;

private:
    std::shared_ptr<const Collider> collider_;
};

struct SimCollisionImageQuery : SimQuery {
    enum Detail : Int32 { Bounding, Low, Normal, High, Editor };

    SimCollisionImageQuery() {
        type = EventTypeFirst + 7;
    }

    Detail detail{Normal};
    std::optional<Box3F> bounds; // world-space sweep/query bounds for streamed providers
    std::vector<std::shared_ptr<SimCollisionImage>> images; // max 20
};

struct SimImageTransformQuery : SimQuery {
    SimImageTransformQuery() {
        type = EventTypeFirst + 6;
    }

    TMat3F transform;
    Int32 userData{};
};

struct SimCollisionInfo {
    Flt32 time{};
    SimAddress object;
    std::shared_ptr<SimCollisionImage> image;
    std::size_t imageIndex{};
    CollisionSurfaceList surfaces;

    void transform(const TMat3F& mat) {
        surfaces.transform(mat);
    }
};

using SimCollisionInfoList = std::vector<SimCollisionInfo>;
class SimContainer;
using SimContainerList = std::vector<SimContainer*>;

struct SimContainerQuery {
    SimObjectId id{~SimObjectId{0}};
    UInt32 type{~UInt32{0}}, mask{~UInt32{0}};
    Int32 detail{};
    Box3F box;
    [[nodiscard]] bool isContained(const SimContainer&) const;
};

// Parent containers own their database, which borrows its children. X/XY use a
// sorted X broad phase; XY also rejects on Y before the final 3D predicate.
class SimContainerDatabase {
public:
    enum class Index { Linear, X, XY };

    explicit SimContainerDatabase(Index index = Index::Linear) : index_(index) {}

    virtual ~SimContainerDatabase() = default;

    [[nodiscard]] std::span<SimContainer* const> objects() const {
        return objects_;
    }

    void findIntersections(const SimContainerQuery&, SimContainerList&) const;
    void findContained(const SimContainerQuery&, SimContainerList&) const;
    [[nodiscard]] SimContainer* findOpenContainer(const SimContainerQuery&) const;

private:
    friend class SimContainer;
    bool add(SimContainer&);
    void remove(SimContainer&);

    void update() {
        dirty_ = true;
    }

    Index index_;
    SimContainerList objects_;
    mutable SimContainerList sorted_;
    mutable bool dirty_{true};
};

class SimXDB : public SimContainerDatabase {
public:
    SimXDB() : SimContainerDatabase(Index::X) {}
};

class SimXYDB : public SimContainerDatabase {
public:
    SimXYDB() : SimContainerDatabase(Index::XY) {}
};

using SimDefaultContainerDatabase = SimContainerDatabase;

// Spatial parentage is separate from SimGroup ownership. Single driving thread;
// manager dispatch keeps callback targets alive. Max depth 64, 65,536 children.
class SimContainer : public SimNetObject {
public:
    SimContainer();
    ~SimContainer() override;
    void onRemove() override;
    void onDeleteNotify(SimObject&) override;

    [[nodiscard]] bool isOpen() const {
        return bool(database_);
    }

    bool installDatabase(std::unique_ptr<SimContainerDatabase>);
    void uninstallDatabase();

    [[nodiscard]] SimContainerDatabase* getDatabase() const {
        return database_.get();
    }

    [[nodiscard]] SimContainer* getContainer() const {
        return container_;
    }

    [[nodiscard]] UInt32 getContainerMask() const {
        return containerMask_;
    }

    bool setContainerMask(UInt32);
    bool setBoundingBox(const Box3F&);

    [[nodiscard]] const Box3F& getBoundingBox() const {
        return box_;
    }

    [[nodiscard]] Point3F getBoxCenter() const {
        return (box_.fMin + box_.fMax) * .5f;
    }

    [[nodiscard]] bool isIntersecting(const Box3F&) const;
    [[nodiscard]] bool isContained(const Box3F&) const;
    [[nodiscard]] bool isIntersecting(const SimContainerQuery&) const;
    [[nodiscard]] bool isContained(const SimContainerQuery&) const;
    bool addObject(SimContainer&);
    bool removeObject(SimContainer&);
    virtual bool updateObject(SimContainer&);
    [[nodiscard]] SimContainer* findOpenContainer(const SimContainer&) const;
    bool findIntersections(const SimContainerQuery&, SimContainerList&) const;
    // LOS uses query.box endpoints, which need not be ordered. Nearest skips
    // triggers except Editor; the all-hit overload includes them, as in legacy.
    bool findLOS(const SimContainerQuery&,
                 SimCollisionInfo&,
                 SimCollisionImageQuery::Detail = SimCollisionImageQuery::Normal);
    bool findLOS(const SimContainerQuery&,
                 SimCollisionInfoList&,
                 SimCollisionImageQuery::Detail = SimCollisionImageQuery::Normal);
    // Renderer/scoper consumers can visit an owned address snapshot in Layer 5/6.
    [[nodiscard]] std::vector<SimAddress> scope(const SimContainerQuery&) const;

protected:
    virtual void enterContainer(SimContainer&);
    virtual void leaveContainer(SimContainer&);

private:
    void detachContents(bool reparent);
    SimContainer* container_{};
    std::unique_ptr<SimContainerDatabase> database_;
    Box3F box_;
    UInt32 containerMask_{};
    bool changing_{};
};

class SimDefaultOpenContainer : public SimContainer {
public:
    explicit SimDefaultOpenContainer(SimObjectId id = 0);
};

struct SimMovementCollisionEvent : SimEvent {
    SimMovementCollisionEvent() {
        type = EventTypeFirst + 31;
    }

    SimAddress object;

    [[nodiscard]] SimObject* getObject(SimManager& manager) const {
        return object.resolve(manager);
    }
};

class SimMovement : public SimContainer {
public:
    enum Flag : UInt32 {
        UseFriction = 1,
        UseAngularVelocity = 2,
        UseElasticity = 4,
        UseCurrent = 8,
        UseDrag = 16,
        TrackContacts = 32,
        RotateBoundingBox = 64,
        AtRest = 128,
        AlwaysCalcAVelocity = 256
    };

    struct Force {
        UInt32 type{};
        bool ignoreMass{};
        Point3F vector;
    };

    struct Coefficient {
        Flt32 mass{1}, invMass{1}, density{1}, drag{}, friction{1}, elasticity{};
    };

    using Material = SimCollisionMaterial;
    using MaterialLookup = std::function<Material(Int32)>;

    struct ObjectImage {
        SimAddress object;
        std::shared_ptr<SimCollisionImage> image;
    };

    using ImageList = std::vector<ObjectImage>;

    struct MovementInfo {
        Int32 retryCount{};
        Flt32 startTime{}, collisionTime{};
        TMat3F transform, target;
        std::shared_ptr<SimCollisionImage> image;
        ImageList imageList;
        SimCollisionInfoList collisionList;
        std::vector<SimAddress> collidedList, triggeredList;
    };

    SimMovement();
    ~SimMovement() override;
    void onRemove() override;
    bool setMass(Flt32);

    bool setWeight(Flt32 value) {
        return setMass(value / 9.8f);
    }

    [[nodiscard]] Flt32 getMass() const {
        return coefficient_.mass;
    }

    [[nodiscard]] Flt32 getVolume() const {
        return coefficient_.mass / coefficient_.density;
    }

    bool setDensity(Flt32);
    bool setDrag(Flt32);
    bool setFriction(Flt32);
    bool setElasticity(Flt32);

    [[nodiscard]] const Coefficient& coefficients() const {
        return coefficient_;
    }

    bool addForce(UInt32 type, const Point3F&, bool ignoreMass = false);
    bool removeForce(UInt32);
    [[nodiscard]] std::optional<Point3F> getForce(UInt32) const;
    [[nodiscard]] Point3F forceSumExcept(UInt32) const;

    void setForceMask(UInt32 mask) {
        forceMask_ = mask;
    }

    void setMaterialLookup(std::shared_ptr<const MaterialLookup> lookup) {
        materials_ = std::move(lookup);
    }

    void setMovementFlags(UInt32 flags);

    [[nodiscard]] UInt32 movementFlags() const {
        return flags_;
    }

    bool setContainerDrag(Flt32);
    bool setContainerCurrent(const Point3F&, Flt32);
    bool setTimeSlice(SimTime);

    [[nodiscard]] SimTime getTimeSlice() const {
        return timeSlice_;
    }

    bool setLinearVelocity(const Point3F&);
    bool setAngularVelocity(const Point3F&);
    bool setAcceleration(const Point3F& linear, const Point3F& angular = {});
    bool setDisplacementVelocity(const Point3F&);
    bool applyImpulse(const Point3F& force);
    bool applyImpulse(const Point3F& offset, const Point3F& force);
    bool collide(const Point3F& worldPoint, const Point3F& normal);
    [[nodiscard]] Point3F getVelocity(const Point3F& offset) const;
    void updateLinearVelocity();
    [[nodiscard]] TMat3F updateMovement();

    [[nodiscard]] const Point3F& getLinearVelocity() const {
        return velocity_;
    }

    [[nodiscard]] const Point3F& getAngularVelocity() const {
        return angularVelocity_;
    }

    [[nodiscard]] const Point3F& getLinearPosition() const {
        return transform_.p;
    }

    [[nodiscard]] const TMat3F& getTransform() const {
        return transform_;
    }

    [[nodiscard]] const TMat3F& getInvTransform() const {
        return inverse_;
    }

    bool setLocalBoundingBox(const Box3F&);

    [[nodiscard]] const Box3F& getLocalBoundingBox() const {
        return localBox_;
    }

    void setCollisionMask(UInt32 mask) {
        collisionMask_ = mask;
    }

    [[nodiscard]] UInt32 getCollisionMask() const {
        return collisionMask_;
    }

    void setExcludedId(SimObjectId id) {
        excludedId_ = id;
    }

    bool setCollisionRetryCount(Int32);
    bool setRotation(const RMat3F&, bool force = false);
    bool setPosition(TMat3F, bool force = false);
    bool testPosition(const TMat3F&, MovementInfo&);
    bool stepPosition(TMat3F, Flt32 minDistance, Flt32 maxDistance);
    bool movePoint(TMat3F);
    bool moveSphere(TMat3F, Flt32 radius);

    [[nodiscard]] bool isAtRest() const {
        return (flags_ & AtRest) != 0;
    }

    [[nodiscard]] bool didContact() const {
        return contact_;
    }

    void clearRestFlag() {
        flags_ &= ~AtRest;
    }

    bool addContact(SimMovement&);
    bool removeContact(SimMovement&);
    void clearContacts();
    // Called after a platform has moved: displace by newPosition-oldPosition,
    // as in the original API. All changes roll back on a blocker or exception.
    bool displaceObjects(const TMat3F& oldTransform);

protected:
    void setContact(bool value) noexcept {
        contact_ = value;
    }

    [[nodiscard]] Material
    collisionMaterial(const MovementInfo&, const SimCollisionInfo&, const CollisionSurface&) const;
    virtual std::shared_ptr<SimCollisionImage> getSelfImage();
    virtual bool processCollision(MovementInfo&);
    virtual bool processSurface(Point3F normal, const CollisionSurface&, UInt32 flags);

    virtual bool processBlocker(SimObject&) {
        return true;
    }

    virtual void onDisplacedObject(SimMovement&, const Point3F&) {}

    virtual void onDisplacement(SimMovement&, const Point3F&) {}

    virtual Box3F buildContainerBox(const TMat3F&) const;

private:
    bool setTransform(const TMat3F&);
    ImageList getImageList(const Box3F&);
    bool testImage(MovementInfo&);
    bool position(TMat3F, bool stepped, Flt32 minDistance, Flt32 maxDistance);
    bool sweep(TMat3F, std::shared_ptr<SimCollisionImage>);
    void notifyCollision(const MovementInfo&);
    void notify(SimAddress to, SimAddress object);
    void testRestCondition();
    void setForceSpeed();
    TMat3F transform_, inverse_;
    Box3F localBox_{{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
    Point3F velocity_, acceleration_, angularVelocity_, angularAcceleration_, currentDirection_;
    Coefficient coefficient_;
    std::vector<Force> forces_;
    std::vector<SimMovement*> contacts_;
    std::shared_ptr<const MaterialLookup> materials_;
    std::optional<Material> surfaceMaterial_; // scoped to processSurface dispatch
    Flt32 containerDrag_{}, currentDrag_{}, currentSpeed_{}, forceSpeed_{}, timeSlice_{1.f / 30.f};
    UInt32 flags_{UseFriction | AtRest | AlwaysCalcAVelocity}, forceMask_{},
        collisionMask_{~UInt32{0}};
    SimObjectId excludedId_{};
    Int32 retryCount_{3};
    bool contact_{}, moving_{}, displacing_{};
};
} // namespace opent1::sim
