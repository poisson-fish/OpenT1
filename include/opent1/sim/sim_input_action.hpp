#pragma once
#include <opent1/sim/sim_base.hpp>
#include <opent1/console/console.hpp>
#include <map>

namespace opent1::sim {
// Retain the original file values without including DirectInput or Windows.
inline constexpr UInt8 InputDevice = 1, InputMouse = 2, InputKeyboard = 3, InputJoystick = 4;
inline constexpr UInt8 InputMake = 1, InputBreak = 2, InputMove = 3;
inline constexpr UInt8 InputButton = 2, InputPov = 3, InputXPov = 4, InputYPov = 5, InputUpPov = 6,
                       InputDownPov = 7, InputLeftPov = 8, InputRightPov = 9, InputKey = 10,
                       InputXAxis = 11, InputYAxis = 12, InputZAxis = 13, InputRxAxis = 14,
                       InputRyAxis = 15, InputRzAxis = 16;
inline constexpr UInt8 InputLShift = 1, InputRShift = 2, InputShift = 3, InputLCtrl = 4,
                       InputRCtrl = 8, InputCtrl = 12, InputLAlt = 16, InputRAlt = 32,
                       InputAlt = 48;
inline constexpr SimObjectId ActionHandlerId = 1, InputManagerId = 24, InputConsumerSetId = 25;
inline constexpr Int32 InputEventType = EventTypeFirst + 22, ActionEventType = EventTypeFirst + 11,
                       InputPriorityQueryType = EventTypeFirst + 9;

enum ActionFlag : UInt32 {
    ActionNone = 1,
    ActionValue = 2,
    ActionFlip = 4,
    ActionCenter = 8,
    ActionSquare = 16,
    ActionCube = 32,
    ActionDeadZone = 64,
    ActionScale = 128
};

enum ActionDevice : Int32 { ActionAnalog = 1, ActionDigital = 2, ActionDelta = 4, ActionPov = 8 };

struct SimInputEvent : SimEvent, Persistent::Base {
    SimInputEvent() {
        type = InputEventType;
    }

    UInt32 deviceInst{};
    Flt32 fValue{};
    UInt8 deviceType{InputKeyboard}, objType{InputKey}, objInst{}, action{InputMake}, ascii{},
        modifier{};
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

struct SimActionEvent : SimEvent, Persistent::Base {
    SimActionEvent() {
        type = ActionEventType;
    }

    Int32 action{}, device{};
    Flt32 fValue{}, deadZone{}, scale{1};
    std::optional<std::string> consoleCommand;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;
};

struct SimInputPriorityQuery : SimQuery {
    SimInputPriorityQuery() {
        type = InputPriorityQueryType;
    }

    Int32 priority{2000};
};

struct InputKeyDescription {
    UInt8 lower{}, upper{};
    std::string_view name;
};

[[nodiscard]] const InputKeyDescription* inputKeyDescription(UInt32 legacyScanCode);
[[nodiscard]] UInt8 inputAscii(UInt32 legacyScanCode, UInt8 modifiers);
[[nodiscard]] UInt8 inputKeyCode(std::string_view name);
[[nodiscard]] UInt8 inputKeyCode(char ascii);
// Parses legacy "make ctrl-shift-a" and generic shift/ctrl/alt bindings.
[[nodiscard]] std::optional<SimInputEvent> parseInputKey(std::string_view);

class SimActionMap : public Persistent::Base {
public:
    struct Node {
        UInt32 flags{};
        SimActionEvent action;
        SimInputEvent event;
    };

    static constexpr std::size_t MaxBindings = 4096;
    bool mapEvent(const SimInputEvent&, const SimActionEvent&, UInt32 flags = ActionNone);
    bool unmapEvent(const SimInputEvent&);

    void clear() {
        nodes_.clear();
    }

    [[nodiscard]] std::span<const Node> nodes() const {
        return nodes_;
    }

    [[nodiscard]] const Node* findMatch(const SimInputEvent&, bool boundCheck = false) const;
    [[nodiscard]] static bool compareModifiers(const SimInputEvent&, const SimInputEvent&);
    [[nodiscard]] static bool isMakeBreakPair(const SimInputEvent&, const SimInputEvent&);
    // Returns no action for malformed/nonfinite parameters or results.
    [[nodiscard]] static std::optional<SimActionEvent> transform(const Node&, const SimInputEvent&);
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    std::vector<Node> nodes_;
};

class SimInputConsumerSet : public SimSet {
public:
    SimInputConsumerSet() {
        setId(InputConsumerSetId);
        setStable(true);
    }

    bool processEvent(const SimEvent&) override;
};

class SimActionHandler : public SimObject {
public:
    explicit SimActionHandler(Console* console = nullptr) : console_(console) {
        setId(ActionHandlerId);
    }

    bool onAdd() override;
    void onRemove() override;
    void onDeleteNotify(SimObject&) override;
    bool processEvent(const SimEvent&) override;
    bool processQuery(SimQuery&) override;
    bool push(SimObject&, std::shared_ptr<const SimActionMap>);
    void pop(SimObject&, const SimActionMap* map = nullptr);
    void breakAllMadeKeys();
    void releaseDevice(UInt8 type, UInt32 instance);
    [[nodiscard]] bool hasMake(const SimInputEvent&) const;

    [[nodiscard]] std::size_t heldCount() const {
        return held_.size();
    }

private:
    struct Binding {
        UInt64 serial{};
        SimAddress target;
        std::shared_ptr<const SimActionMap> map;
    };

    bool input(SimInputEvent);
    bool route(SimInputEvent, UInt64* acceptedBinding = nullptr);
    void breakKey(const SimInputEvent&);
    Console* console_{}; // must outlive the handler
    std::vector<Binding> bindings_;

    struct Held {
        SimInputEvent event;
        UInt64 owner{};
    };

    std::vector<Held> held_;
    UInt64 nextSerial_{};
    bool changing_{};
};

class SimInputManager : public SimObject {
public:
    SimInputManager() {
        setId(InputManagerId);
    }

    bool processEvent(const SimEvent&) override;
    void onRemove() override;
    // SDL adapter sends normalized events here. Callback targets may remove the
    // manager/handler safely; graph access still needs external synchronization.
    bool submit(SimInputEvent);
    bool activateDevice(std::string_view name, bool active = true);
    void release();
    void releaseDevice(UInt8 type, UInt32 instance);

    [[nodiscard]] UInt8 modifiers() const {
        return modifiers_;
    }

private:
    UInt8 modifiers_{};
    std::map<std::pair<UInt8, UInt32>, bool> deviceActive_;
    std::map<UInt8, bool> typeActive_;
    bool allActive_{true};
};

bool registerSimInputClasses(Persistent::ClassRegistry&);
bool registerSimActionMapResource(ResourceManager&);
} // namespace opent1::sim
