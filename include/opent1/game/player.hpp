#pragma once
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/player_collision.hpp>
#include <opent1/game/shape_base.hpp>
#include <array>
#include <numbers>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

class Player : public ShapeBase {
    using Parent = ShapeBase;

public:
    static inline constexpr Flt32 kMaxPitch = static_cast<Flt32>(88.0 * std::numbers::pi / 180.0);
    static inline constexpr Flt32 kJetFuelMin = 5.0f;
    static inline Int32 prefShapeDetail = 100;
    static inline Flt32 prefPlayerShadowSize = 10.0f;
    static inline bool prefPlayerShadows = true;
    static inline Int32 prefFreeLook = 1;

    enum Constants : Int32 {
        MaxItemImages = 8,
        MaxImageSlots = 8,
        MaxItemTypes = 200,
        ItemTypeBits = 8,
    };

    enum Animations : Int32 {
        ANIM_STAND = 0,
        ANIM_IDLE = 0,
        ANIM_RUN = 1,
        ANIM_BACK = 2,
        ANIM_SIDE = 3,
        ANIM_SIDE_LEFT = 4,
        ANIM_JET = 19,
        ANIM_JUMP = 5,
        ANIM_JUMPRUN = 6,
        ANIM_FALL = 14,
        ANIM_LAND = 15,
        ANIM_CROUCH = 8,
        ANIM_CROUCH_MOVE = 10,
        ANIM_CROUCH_DIE = 25,
        ANIM_TUMBLE_LOOP = 17,
        ANIM_TUMBLE_END = 18,
        ANIM_FLYER_ROOT = 22,
        ANIM_FLIER = 22,
        ANIM_APC_ROOT = 23,
        ANIM_PDA = 20,
        ANIM_DIE_START = 25,
        ANIM_DIE_HEAD = 27,
        ANIM_DIE_HEAD_BACK = 17,
        ANIM_DIE_HEAD_LEFT = 18,
        ANIM_DIE_HEAD_RIGHT = 19,
        ANIM_DIE_PERP = 20,
        ANIM_DIE_TORSO_FRONT = 26,
        ANIM_DIE_TORSO_BACK = 22,
        ANIM_DIE_TORSO_LEFT = 23,
        ANIM_DIE_TORSO_RIGHT = 24,
        ANIM_DIE_LEGS_LEFT = 25,
        ANIM_DIE_LEGS_RIGHT = 26,
        ANIM_DIE_BLOWN_BACK = 27,
        ANIM_DIE_SPIN = 28,
        ANIM_DIE_FORWARD = 29,
        ANIM_DIE_FORWARD_KNEEL = 30,
        ANIM_DIE_BACK = 31,
        ANIM_DIE_END = 37,
        ANIM_LOOK_ROOT = 32,
        ANIM_CROUCH_LOOK_ROOT = 33,
        ANIM_NONE_POSE = 34,
        ANIM_LIGHT_POSE = 35,
        ANIM_MEDIUM_POSE = 36,
        ANIM_HEAVY_POSE = 37,
        ANIM_PLAYER_FIRST = 38,
        ANIM_OVERHAND_THROW = 38,
        ANIM_STAND_SALUTE = 39,
        ANIM_CROUCH_SALUTE = 40,
        ANIM_CELEBRATE_1 = 41,
        ANIM_CELEBRATE_2 = 42,
        ANIM_CELEBRATE_3 = 43,
        ANIM_TAUNT_1 = 44,
        ANIM_TAUNT_2 = 45,
        ANIM_WAVE = 46,
        ANIM_KNEEL_POSE = 47,
        ANIM_STAND_POSE = 48,
        ANIM_DSIGHT_ROOT = 49,
        ANIM_PLAYER_END = 50,
        NUM_ANIMS = 51,
        NUM_FOOTSOUNDS = 15,
    };

    enum NetMaskBits : UInt32 {
        OrientationMask = 1u << 13,
        AnimationMask = 1u << 14,
        MountMask = 1u << 15,
        NoInterpMask = 1u << 16,
        VisItemMask = 1u << 17,
        ItemImageMaskBits = 20,
        NextFreeMask = 1u << 28,
    };

    enum WeaponType : Int32 {
        Standard = 0,
        Spinning = 1,
        Sustained = 2,
        DiscLauncher = 3,
    };

    struct ItemTypeEntry {
        Int32 count{0};
        Int32 imageSlot{-1};
    };

    struct ItemImageEntry {
        enum State : Int32 {
            Reset = 0,
            Fire = 1,
            NoAmmo = 2,
            Idle = 3,
            Ready = 4,
            Reload = 5,
            SpinUp = 6,
            SpinDown = 7,
            Activate = 8,
            Deactivate = 9,
        };

        Int32 state{Reset};
        Int32 itemId{-1};
        Int32 itemCount{0};
        Int32 imageId{-1};
        Int32 teamId{-1};
        Int32 nextId{-2}, nextTeam{-1};
        Flt32 delayTime{0.0f};
        Int32 fireCount{0};
        bool triggerDown{false};
        bool ammo{false};
        UInt32 animSound{0};
        bool animLoopingSound{false};
        sim::SimAddress sustainedProjectile{};
        FearRenderImage image{};
        std::shared_ptr<world::TsShapeInstance> shape;
        std::optional<world::TsShapeResources> resources;
        Int32 renderedImageId{-1};
        Int32 renderedState{-1};
        std::optional<UInt32> animationThread, ambientThread, spinThread;
    };

    Player();
    ~Player() override;

    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;
    bool initResources(GameBaseData* inData) override;

    [[nodiscard]] std::string_view getScriptDefaultClass() const noexcept override {
        return "Player";
    }

    [[nodiscard]] Int32 getDatGroup() const noexcept override {
        return DataBlockManager::PlayerDataType;
    }

    [[nodiscard]] UInt16 getGhostTag() const override {
        return static_cast<UInt16>(PlayerPersTag);
    }

    // Movement & physics (player.cpp / playerUpdate.cpp)
    void serverUpdateMove(std::span<const PlayerMove> moves) override;
    void serverProcess(UInt32 curTime) override;
    void clientProcess(UInt32 curTime) override;
    void updateMove(const PlayerMove* curMove, bool server);
    void updateAnimation(Flt32 seconds);
    [[nodiscard]] std::optional<world::TsShapePose> animationPose() const;
    void appendMountedImages(world::WorldShapeQuery& query);
    bool getImageTransform(Int32 slot, TMat3F* transform) const;
    bool getMuzzleTransform(Int32 slot, TMat3F* transform) const override;
    void updateImageAnimation(ItemImageEntry& image, Flt32 seconds);
    void ghostSetMove(const PlayerMove& move,
                      const Point3F& newPos,
                      const Point3F& newVel,
                      bool cont,
                      Flt32 newRot,
                      Flt32 newPitch,
                      Int32 skipCount,
                      bool noInterp,
                      std::optional<UInt32> packetTime = std::nullopt);
    void stepDelta(Flt32 dt);
    void fallToGround(Flt32 interval);
    void fallToGround(Flt32 interval, const Point3F& deltaRot);
    Int32 possibleJetForces(std::span<Point3F, 4> forces,
                            const Point3F* unitLateralDir = nullptr) const noexcept;
    void possibleJetForces(Flt32* minForce,
                           Flt32* maxForce,
                           const Point3F& iv,
                           Point3F* dvec) const noexcept;

    void setPlayerControl(bool ctrl) override;

    [[nodiscard]] bool hasPlayerControl() const noexcept {
        return hasFocus_;
    }

    [[nodiscard]] bool isFirstPersonView() const noexcept;
    void setCrouching(bool newCrouch);

    [[nodiscard]] bool isCrouching() const noexcept {
        return crouching_;
    }

    [[nodiscard]] bool isJetting() const noexcept {
        return jetting_;
    }

    void setJetting(bool jetting) noexcept {
        jetting_ = jetting;
    }

    [[nodiscard]] bool isFalling() const noexcept {
        return falling_;
    }

    void setFalling(bool falling) noexcept {
        falling_ = falling;
    }

    [[nodiscard]] bool isDead() const noexcept {
        return dead_;
    }

    void setDead(bool dead);

    [[nodiscard]] bool isExposed() const noexcept {
        return !mount_ || mount_->mountRender();
    }

    [[nodiscard]] bool isBlownUp() const noexcept {
        return blowUpPending_;
    }

    void disableSupressionField();
    void setSupressionField(Flt32 distance);

    [[nodiscard]] Flt32 getSupressionField() const noexcept {
        return supressionDist_;
    }

    [[nodiscard]] Player* nextSup() const noexcept {
        return nextSup_;
    }

    [[nodiscard]] Player* prevSup() const noexcept {
        return prevSup_;
    }

    void setSupressorLinks(Player* next, Player* prev) noexcept {
        nextSup_ = next;
        prevSup_ = prev;
    }

    void setDetectParams(Flt32 minSize, Flt32 hazeFactor) noexcept {
        detectMinSize_ = minSize;
        detectHazeFactor_ = hazeFactor;
    }

    bool detectItem(GameBase* obj);

    [[nodiscard]] GameBase* getTargetObject() const noexcept {
        return targetObject_;
    }

    void setTargetObject(GameBase* obj) noexcept {
        targetObject_ = obj;
    }

    void setBlownUp(bool blownUp) noexcept {
        blowUpPending_ = blownUp;
    }

    [[nodiscard]] bool isAiControlled() const noexcept {
        return aiControlled_;
    }

    void setAiControlled(bool ai) noexcept {
        aiControlled_ = ai;
    }

    [[nodiscard]] bool isPDA() const noexcept {
        return pdaing_;
    }

    void setPDA(bool tf) noexcept {
        pdaing_ = tf;
    }

    [[nodiscard]] Int32 updateSkipCount() const noexcept {
        return updateSkipCount_;
    }

    [[nodiscard]] Int32 updateDebt() const noexcept {
        return updateDebt_;
    }

    [[nodiscard]] UInt32 getLastJetTime() const noexcept {
        return lastJetTime_;
    }

    void setUpdateSkipCount(Int32 skip) noexcept {
        updateSkipCount_ = skip;
    }

    [[nodiscard]] Flt32 getViewPitch() const noexcept {
        return viewPitch_;
    }

    void setViewPitch(Flt32 pitch) noexcept;

    [[nodiscard]] Flt32 getTraction() const noexcept {
        return traction_;
    }

    void setTraction(Flt32 traction) noexcept {
        traction_ = traction;
    }

    [[nodiscard]] Int32 jumpSurfaceLastContact() const noexcept {
        return jumpSurfaceLastContact_;
    }

    void setJumpSurfaceLastContact(Int32 ticks,
                                   const Point3F& normal = {0.0f, 0.0f, 1.0f}) noexcept {
        jumpSurfaceLastContact_ = ticks;
        jumpSurfaceNormal_ = normal;
    }

    [[nodiscard]] Int32 lastContactCount() const noexcept {
        return lastContactCount_;
    }

    void setLastContactCount(Int32 ticks) noexcept {
        lastContactCount_ = ticks;
    }

    [[nodiscard]] Flt32 forwardAxisMovement() const noexcept {
        return forwardAxisMovement_;
    }

    [[nodiscard]] Flt32 sideAxisMovement() const noexcept {
        return sideAxisMovement_;
    }

    [[nodiscard]] const Point3F& jumpSurfaceNormal() const noexcept {
        return jumpSurfaceNormal_;
    }

    [[nodiscard]] Int32 surfaceType() const noexcept {
        return surfaceType_;
    }

    [[nodiscard]] Flt32 lastImpactSpeed() const noexcept {
        return lastImpactSpeed_;
    }

    [[nodiscard]] const PlayerMove& lastPlayerMove() const noexcept {
        return lastPlayerMove_;
    }

    // Damage, flash, skin & death
    void applyDamage(Int32 type,
                     Flt32 value,
                     const Point3F& pos,
                     const Point3F& vec,
                     const Point3F& mom,
                     sim::SimObjectId objectId) override;
    void setDamageLevel(Flt32 level) override;
    void setDamageFlash(Flt32 df) noexcept;

    [[nodiscard]] Flt32 getDamageFlash() const noexcept override {
        return damageFlash_;
    }

    [[nodiscard]] std::string getDamageLocation(const Point3F& pos) const;
    void getDamageLocation(const Point3F& pos,
                           std::string_view& outVert,
                           std::string_view& outQuad) const noexcept;
    [[nodiscard]] std::string_view getSkinBase() const noexcept override;
    [[nodiscard]] Int32 getDamageSkinLevel(Int32 detailLevel) const noexcept override;

    [[nodiscard]] std::string_view voiceBase() const noexcept {
        return voiceBase_;
    }

    void setVoiceBase(std::string_view vb) {
        voiceBase_ = std::string(vb);
    }

    // Animation & mount
    void setAnimation(UInt32 anim);

    void setServerAnimation(Int32 anim) noexcept {
        serverAnimation_ = anim;
    }

    [[nodiscard]] Int32 currentAnimation() const noexcept {
        return currentAnimation_;
    }

    [[nodiscard]] Int32 serverAnimation() const noexcept {
        return serverAnimation_;
    }

    [[nodiscard]] Int32 pickAnim() const noexcept;
    void setMountObject(GameBase* obj, Int32 mountPoint);

    [[nodiscard]] GameBase* getMountObject() const noexcept {
        return mount_;
    }

    [[nodiscard]] Int32 getMountPoint() const noexcept {
        return mountPoint_;
    }

    [[nodiscard]] UInt32 curGuiMode() const noexcept {
        return curGuiMode_;
    }

    void setCurGuiMode(UInt32 mode) noexcept {
        curGuiMode_ = mode;
    }

    // Collision image & camera
    [[nodiscard]] const PlayerCollisionImage& collisionImage() const noexcept {
        return collisionImage_;
    }

    [[nodiscard]] PlayerCollisionImage& collisionImage() noexcept {
        return collisionImage_;
    }

    [[nodiscard]] const TMat3F& getEyeTransform() const override;
    void getThrowVector(Point3F* pos, Point3F* vec) const override;
    [[nodiscard]] TMat3F getLOSTransform() const noexcept override;
    void getCameraTransform(Flt32 camDist, TMat3F* transform) const override;

    [[nodiscard]] bool cameraZoomAndCrosshairsEnabled() const noexcept override {
        return !dead_;
    }

    // Inventory & Weapon Slots (playerInventory.cpp)
    bool setItemCount(Int32 type, Int32 count);
    bool incItemCount(Int32 type, Int32 count = 1);
    bool decItemCount(Int32 type, Int32 count = 1);
    [[nodiscard]] Int32 getItemCount(Int32 type) const noexcept;

    bool useItem(Int32 type);
    bool dropItem(Int32 type);
    bool deployItem(Int32 type);

    bool mountItem(Int32 type, Int32 imageSlot, Int32 team = -1);
    bool unmountItem(Int32 imageSlot);
    [[nodiscard]] Int32 getMountedItem(Int32 imageSlot) const noexcept;

    void setImageTriggerDown(Int32 imageSlot);
    void setImageTriggerUp(Int32 imageSlot);
    [[nodiscard]] bool isImageTriggerDown(Int32 imageSlot) const noexcept;

    void resetImageSlot(Int32 imageSlot);
    void setImageState(Int32 imageSlot, Int32 state, bool force = false);
    [[nodiscard]] Int32 getImageState(Int32 imageSlot) const noexcept;
    void updateImageState(Int32 imageSlot, Flt32 dt);
    void updateItemImages(Flt32 dt);
    void fireImage(Int32 imageSlot);
    void scriptImageCallback(Int32 imageSlot, std::string_view callback);
    void updateImageFire(Int32 imageSlot);
    void stopImageFire(Int32 imageSlot);
    void updateImageMass();

    [[nodiscard]] const ItemImageEntry& getItemImageEntry(Int32 slot) const noexcept {
        return itemImageList_[static_cast<std::size_t>(slot)];
    }

    [[nodiscard]] ItemImageEntry& getItemImageEntry(Int32 slot) noexcept {
        return itemImageList_[static_cast<std::size_t>(slot)];
    }

    [[nodiscard]] const ItemImageData* getImageData(Int32 imageId) const noexcept;
    [[nodiscard]] const ItemData* getItemData(Int32 itemId) const noexcept;

    [[nodiscard]] Int32 firedShotCount(Int32 imageSlot) const noexcept {
        return (imageSlot >= 0 && imageSlot < MaxImageSlots)
                   ? itemImageList_[static_cast<std::size_t>(imageSlot)].fireCount
                   : 0;
    }

    // Packet data, ghost update & persistence
    void packItemImages(BitStream& stream, UInt32 mask) const;
    bool unpackItemImages(BitStream& stream, bool initialUpdate = false);
    void writePacketData(BitStream& stream) const override;
    bool readPacketData(BitStream& stream) override;

    UInt32
    packUpdate(sim::network::GhostManager& gm, UInt32 mask, BitStream& stream) const override;
    bool unpackUpdate(sim::network::GhostManager& gm, BitStream& stream) override;
    bool packPlayerUpdate(UInt32 mask,
                          BitStream& stream,
                          sim::network::GhostManager* gm = nullptr) const;
    bool unpackPlayerUpdate(BitStream& stream, sim::network::GhostManager* gm = nullptr);

    Persistent::Error
    read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) override;
    Persistent::Error
    write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const override;

protected:
    std::shared_ptr<sim::SimCollisionImage> getSelfImage() override;
    bool processCollision(MovementInfo& info) override;

private:
    void updateClientEffects();
    void stopJetSound();
    void appendJetFlame(world::WorldShapeQuery&);
    void stopImageSound(Int32 slot, bool loopingOnly = false);
    void setImageType(Int32 slot, Int32 type, Int32 team);
    void scriptItemCallback(Int32 type, std::string_view callback, bool deployment = false);
    PlayerData* playerData_{nullptr};
    PlayerCollisionImage collisionImage_{};
    std::shared_ptr<PlayerCollisionImage> sharedCollisionImage_{};

    Int32 jumpSurfaceLastContact_{100};
    Int32 lastContactCount_{0};
    Point3F jumpSurfaceNormal_{0.0f, 0.0f, 1.0f};
    Flt32 forwardAxisMovement_{0.0f};
    Flt32 sideAxisMovement_{0.0f};
    Int32 currentAnimation_{-1};
    Int32 serverAnimation_{-1};
    bool animationPending_{false};
    Int32 lastAnimTime_{0};

    Int32 surfaceType_{-1};
    Flt32 traction_{0.0f};
    Flt32 viewPitch_{0.0f};
    Flt32 damageFlash_{0.0f};
    Flt32 lastImpactSpeed_{0.0f};

    UInt32 interpDoneTime_{0};
    Point3F deltaPos_{0.0f, 0.0f, 0.0f};
    Flt32 deltaRot_{0.0f};
    Flt32 deltaPitch_{0.0f};

    PlayerMove lastPlayerMove_{};
    Int32 updateSkipCount_{0};
    Int32 updateDebt_{0};
    UInt32 lastJetTime_{0};
    UInt32 jetSound_{0};
    bool inWorld_{true};
    bool lastFreeLook_{true};

    bool dead_{false};
    bool falling_{false};
    bool jetting_{false};
    bool crouching_{false};
    bool hasFocus_{false};
    bool blowUpPending_{false};
    bool aiControlled_{false};
    bool pdaing_{false};

    GameBase* mount_{nullptr};
    std::optional<UInt32> animationThread_, lookThread_;
    std::shared_ptr<world::TsShapeInstance> flameShape_;
    std::optional<UInt32> flameThread_;
    std::optional<world::TsShapeResources> flameResources_;
    std::string renderedSkin_;
    std::optional<world::TsMaterialResources> skinMaterials_;
    Int32 mountPoint_{0};
    UInt16 mountGhostIndex_{sim::InvalidGhostIndex};
    UInt32 curGuiMode_{1};
    std::string voiceBase_{"male1"};
    mutable TMat3F eyeTransformCache_{};

    Player* nextSup_{nullptr};
    Player* prevSup_{nullptr};
    Flt32 supressionDist_{0.0f};
    Flt32 detectMinSize_{0.0f};
    Flt32 detectHazeFactor_{0.0f};
    Flt32 oldFov_{0.0f};
    Flt32 cosFov_{1.0f};
    Flt32 tanFov_{1.0f};
    GameBase* targetObject_{nullptr};

    std::array<ItemTypeEntry, MaxItemTypes> itemTypeList_{};
    std::array<ItemImageEntry, MaxImageSlots> itemImageList_{};
};

bool registerPlayerClass(Persistent::ClassRegistry& registry);
bool registerPlayerNetwork(sim::network::Registry& registry);

} // namespace opent1::game
