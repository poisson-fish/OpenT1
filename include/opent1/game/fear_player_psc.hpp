#pragma once
#include <opent1/console/console.hpp>
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/stream_io.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/input_handler.hpp>
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/sim/sim_net.hpp>
#include <array>
#include <map>
#include <span>
#include <vector>

namespace opent1::game {

class Player;

struct PlayerMove {
    static inline constexpr Flt32 kMaxTurnSpeed = 1.0f;

    Flt32 forwardAction{0.0f};
    Flt32 backwardAction{0.0f};
    Flt32 leftAction{0.0f};
    Flt32 rightAction{0.0f};
    bool jetting{false};
    bool jumpAction{false};
    bool crouching{false};
    Flt32 turnRot{0.0f};
    Flt32 pitch{0.0f};
    bool trigger{false};
    Int32 useItem{-1};

    constexpr PlayerMove() noexcept = default;
    void reset() noexcept;

    void write(BitStream& stream, bool full = true, const PlayerMove* prev = nullptr) const;
    bool read(BitStream& stream, bool full = true, const PlayerMove* prev = nullptr);

    [[nodiscard]] bool operator==(const PlayerMove&) const = default;
};

class PlayerPSC : public sim::network::PacketStreamClient {
    using Parent = sim::network::PacketStreamClient;

public:
    enum Constants : Int32 {
        ItemShoppingListWords = 7,
        VisibleListWords = 8,
        MaxItemTypes = 200,
        ItemTypeBits = 8,
        MaxMoves = 30,
        InitialGuiMode = 0,
        PlayGuiMode = 1,
        CommandGuiMode = 2,
        InventoryGuiMode = 4,
    };

    static inline constexpr Flt32 DefaultFov = 90.0f;
    static inline constexpr Flt32 MinFovDeg = 5.625f;
    static inline constexpr Flt32 MaxFovDeg = 120.0f;
    static inline constexpr Flt32 CamSpeed = 15.0f;
    static inline constexpr Flt32 CamMaxDist = 4.0f;

    struct InvUpdate {
        Int32 item{-1};
        Int32 delta{0};
    };

    struct PacketHead {
        std::array<UInt32, ItemShoppingListWords> xorShoppingList{};
        std::array<UInt32, ItemShoppingListWords> xorBuyList{};
        std::array<UInt32, VisibleListWords> xorVisibleList{};
        std::vector<InvUpdate> invUpdates{};
        Int32 moveCount{0};
        UInt32 fov{0};
        bool resetting{false};
    };

    struct CommandTarget {
        Point2I position{};
        Int32 team{}, damage{}, status{}, dataGroup{}, dataId{-1};
        bool inUse{};
    };

    static inline Flt32 playerFov = DefaultFov;
    static inline Flt32 sniperFov = DefaultFov / 4;
    static inline Flt32 zoomSpeed = 0.2f;
    static void validateFov() noexcept;

    explicit PlayerPSC(bool inIsServer = false);
    ~PlayerPSC() override;

    [[nodiscard]] bool isServer() const noexcept {
        return isServer_;
    }

    void setGuiMode(UInt32 mode) noexcept {
        guiMode_ = mode;
    }

    [[nodiscard]] UInt32 guiMode() const noexcept {
        return guiMode_;
    }

    [[nodiscard]] bool scoresVisible() const noexcept {
        return scoresVisible_;
    }

    [[nodiscard]] Flt32 damageFlash() const noexcept {
        return damageFlash_;
    }

    [[nodiscard]] Point2I waypoint() const noexcept {
        return waypoint_;
    }

    [[nodiscard]] const auto& commandTargets() const noexcept {
        return commandTargets_;
    }

    [[nodiscard]] const auto& visibleList() const noexcept {
        return visibleList_;
    }

    std::function<void(UInt32, bool)> onGuiState;
    std::function<void(GameBase*)> onControlObject;

    void bindConsole(Console* console) noexcept {
        boundConsole_ = console;
    }

    // Control object & observer state
    void setControlObject(GameBase* obj) noexcept;

    [[nodiscard]] GameBase* getControlObject() const noexcept {
        return controlObject_;
    }

    [[nodiscard]] bool isFirstPerson() const noexcept {
        return firstPerson_;
    }

    void setFirstPerson(bool fp) noexcept {
        firstPerson_ = fp;
    }

    [[nodiscard]] Flt32 camDist() const noexcept {
        return camDist_;
    }

    void setCamDist(Flt32 dist) noexcept {
        camDist_ = dist;
    }

    [[nodiscard]] bool isInObserverMode() const noexcept {
        return fInObserverMode_;
    }

    [[nodiscard]] bool isInEditMode() const noexcept {
        return fInEditMode_;
    }

    void toggleObserverCamera();
    void toggleEditCamera();

    // FOV & zoom
    [[nodiscard]] Flt32 getFov() const noexcept {
        return curFov_;
    }

    void setFov(Flt32 fov) noexcept {
        curFov_ = fov;
    }

    [[nodiscard]] Flt32 targetFov() const noexcept {
        return targetFov_;
    }

    [[nodiscard]] Flt32 startFov() const noexcept {
        return startFov_;
    }

    [[nodiscard]] UInt32 fovStartTime() const noexcept {
        return fovStartTime_;
    }

    [[nodiscard]] UInt32 fovTargetTime() const noexcept {
        return fovTargetTime_;
    }

    void setTargetFov(Flt32 fov, UInt32 currentTimeMs = 0) noexcept;
    void updateFov(UInt32 currentTimeMs) noexcept;

    // Move collection & replay
    void clientCollectInput(UInt32 startTime, UInt32 endTime, Flt32 explicitInputDt = -1.0f);
    [[nodiscard]] PlayerMove* getClientMove(UInt32 curTime) noexcept;
    [[nodiscard]] const PlayerMove* getClientMove(UInt32 curTime) const noexcept;

    [[nodiscard]] PlayerMove* getCurrentMove() noexcept {
        return &curMove_;
    }

    [[nodiscard]] const PlayerMove& currentMove() const noexcept {
        return curMove_;
    }

    [[nodiscard]] std::span<const PlayerMove> moves() const noexcept {
        return moves_;
    }

    void pushMove(const PlayerMove& move) {
        moves_.push_back(move);
        moveTimes_.clear();
    }

    void clearMoves() noexcept {
        moves_.clear();
        moveTimes_.clear();
    }

    void consumeMove() {
        if (!moves_.empty()) {
            moves_.erase(moves_.begin());
            if (!moveTimes_.empty()) {
                moveTimes_.erase(moveTimes_.begin());
            }
        }
    }

    void deferMoveProcessing(bool value) noexcept {
        deferMoveProcessing_ = value;
    }

    [[nodiscard]] bool moveProcessingDeferred() const noexcept {
        return deferMoveProcessing_;
    }

    [[nodiscard]] const PlayerMove* playerMove(Int32 index) const noexcept;

    [[nodiscard]] Int32 firstMoveSeq() const noexcept {
        return firstMoveSeq_;
    }

    void setFirstMoveSeq(Int32 seq) noexcept {
        firstMoveSeq_ = seq;
    }

    void setLocalGhostManager(sim::network::GhostManager* ghosts) {
        localGhostManager_ = ghosts ? ghosts->address() : sim::SimAddress{};
    }

    [[nodiscard]] Int32 lastServerMove() const noexcept {
        return lastServerMove_;
    }

    void setLastServerMove(Int32 seq) noexcept {
        lastServerMove_ = seq;
    }

    [[nodiscard]] Int32 triggerCount() const noexcept {
        return triggerCount_;
    }

    [[nodiscard]] InputHandler& yawHandler() noexcept {
        return yawHandler_;
    }

    [[nodiscard]] InputHandler& pitchHandler() noexcept {
        return pitchHandler_;
    }

    // Shopping list, buy list, and inventory synchronization
    void setInShoppingList(Int32 item) noexcept;
    void clearFromShoppingList(Int32 item) noexcept;
    [[nodiscard]] bool isInShoppingList(Int32 item) const noexcept;
    void clearShoppingList() noexcept;

    void setInBuyList(Int32 item) noexcept;
    void clearFromBuyList(Int32 item) noexcept;
    [[nodiscard]] bool isInBuyList(Int32 item) const noexcept;
    void clearBuyList() noexcept;

    void setItemShoppingList(Int32 item) noexcept {
        setInShoppingList(item);
    }

    void clearItemShoppingList(Int32 item) noexcept {
        clearFromShoppingList(item);
    }

    [[nodiscard]] bool isItemShoppingOn(Int32 item) const noexcept {
        return isInShoppingList(item);
    }

    void setItemBuyList(Int32 item) noexcept {
        setInBuyList(item);
    }

    void clearItemBuyList(Int32 item) noexcept {
        clearFromBuyList(item);
    }

    [[nodiscard]] bool isItemBuyOn(Int32 item) const noexcept {
        return isInBuyList(item);
    }

    void setCommandBandLimit(bool limited) noexcept {
        limitCommandBandwidth_ = limited;
    }

    [[nodiscard]] bool commandBandLimited() const noexcept {
        return limitCommandBandwidth_;
    }

    [[nodiscard]] Int32 itemCount(Int32 item) const noexcept;
    void setItemCount(Int32 item, Int32 count) noexcept;
    Int32 deltaItemCount(Int32 item, Int32 delta) noexcept;
    void resetItems() noexcept;

    [[nodiscard]] std::span<const UInt32, ItemShoppingListWords>
    shoppingListWords() const noexcept {
        return shoppingList_;
    }

    [[nodiscard]] std::span<const UInt32, ItemShoppingListWords> buyListWords() const noexcept {
        return buyList_;
    }

    [[nodiscard]] std::span<const UInt32, ItemShoppingListWords>
    clientShoppingListWords() const noexcept {
        return clientShoppingList_;
    }

    [[nodiscard]] std::span<const UInt32, ItemShoppingListWords>
    clientBuyListWords() const noexcept {
        return clientBuyList_;
    }

    // SimActionEvent handling
    bool onSimActionEvent(const sim::SimActionEvent& event);
    std::function<bool(const sim::SimActionEvent&)> guiAction;
    void resetInput() noexcept;

    // PacketStreamClient overrides
    bool writePacket(BitStream& bstream, UInt32& key) override;
    bool readPacket(BitStream& bstream, UInt32 currentTime) override;
    void packetReceived(UInt32 key) override;
    void packetDropped(UInt32 key) override;
    bool processRecorderStream(StreamIO& sio, UInt32 currentTime) override;
    void notifyPlaybackOver() override;

    bool onAdd() override;
    void onDeleteNotify(sim::SimObject& object) override;
    bool processEvent(const sim::SimEvent& event) override;
    bool processQuery(sim::SimQuery& query) override;

private:
    bool isServer_{false};
    bool deferMoveProcessing_{};
    bool limitCommandBandwidth_{};
    UInt32 guiMode_{InitialGuiMode};
    bool scoresVisible_{};
    Flt32 damageFlash_{};
    Point2I waypoint_{};
    UInt32 lastSentTarget_{};
    std::array<CommandTarget, 256> commandTargets_{};
    Console* boundConsole_{nullptr};
    GameBase* controlObject_{nullptr};
    Player* controlPlayer_{nullptr};

    std::vector<PlayerMove> moves_{};
    // Local tick for each predicted move. Network stalls can leave gaps in
    // this timeline even though the wire sequence remains contiguous.
    std::vector<UInt32> moveTimes_{};
    PlayerMove curMove_{};
    PlayerMove playbackMove_{};
    Int32 firstMoveSeq_{0};
    sim::SimAddress localGhostManager_{};
    Int32 lastServerMove_{0};
    Int32 lastPlayerMoveCount_{0};
    UInt32 clientInputTime_{};

    Int32 triggerCount_{0};
    Int32 prevTriggerCount_{0};
    bool fInObserverMode_{false};
    bool fInEditMode_{false};
    bool firstPerson_{true};
    bool sneak_{false};
    Flt32 camDist_{0.0f};

    Flt32 curFov_{DefaultFov};
    Flt32 startFov_{DefaultFov};
    Flt32 targetFov_{DefaultFov};
    UInt32 fovStartTime_{0};
    UInt32 fovTargetTime_{0};
    UInt32 prevFov_{0};

    InputHandler yawHandler_{};
    InputHandler pitchHandler_{};

    std::array<UInt32, ItemShoppingListWords> shoppingList_{};
    std::array<UInt32, ItemShoppingListWords> buyList_{};
    std::array<UInt32, ItemShoppingListWords> clientShoppingList_{};
    std::array<UInt32, ItemShoppingListWords> clientBuyList_{};
    std::array<UInt32, VisibleListWords> visibleList_{};
    std::array<Int16, MaxItemTypes> itemTypeCount_{};
    std::array<Int16, MaxItemTypes> clientItemTypeCount_{};
    bool fReset_{false};

    UInt32 nextPacketKey_{1};
    std::map<UInt32, PacketHead> pendingPackets_{};
};

} // namespace opent1::game
