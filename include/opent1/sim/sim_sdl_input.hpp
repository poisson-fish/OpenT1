#pragma once
#include <opent1/sim/sim_input_action.hpp>
#include <opent1/platform/sdl_events.hpp>
#include <memory>

namespace opent1::sim {
// Main-thread SDL input owner/adapter. Removal of its input service detaches the
// adapter, so the simulation manager may be destroyed before the host rebinds it.
// Route the application's owned platform events here; UTF-8 TextEvent remains a
// separate UI event and is never synthesized from the US-ASCII compatibility API.
class SdlSimInput {
public:
    SdlSimInput(SimInputManager&, platform::WindowId);
    ~SdlSimInput();
    SdlSimInput(const SdlSimInput&) = delete;
    SdlSimInput& operator=(const SdlSimInput&) = delete;
    bool process(const platform::Event&);
    void capture(bool enabled);
    void activate(UInt8 deviceType, bool enabled);
    void activateDevice(UInt8 deviceType, UInt32 instance, bool enabled);
    void release();
    [[nodiscard]] std::size_t gamepadCount() const;
    [[nodiscard]] std::vector<UInt32> gamepadSlots() const;
    [[nodiscard]] std::optional<UInt32> gamepadSlot(UInt32 sdlInstance) const;
    [[nodiscard]] static UInt8 legacyScanCode(UInt32 sdlScanCode);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace opent1::sim
