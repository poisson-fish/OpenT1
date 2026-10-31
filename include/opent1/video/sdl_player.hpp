#pragma once
#include <opent1/video/player.hpp>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/audio/audio.hpp>

namespace opent1::video {
enum class OutputFailure { None, BeforeGpu, AfterAudioPreparation };

struct OutputControl {
    OutputFailure fail{};
};

// SDL main thread. Surface and device facades are retained; the SDL window may
// close first. clear stops audio and retains the last uploaded GPU image.
class SdlOutput final : public Output {
public:
    SdlOutput(std::shared_ptr<gfx::SdlGpuSurface>,
              std::shared_ptr<audio::Device>,
              std::shared_ptr<OutputControl> = {});
    ~SdlOutput() override;
    bool publish(const Presentation&) override;
    void clear() noexcept override;

    [[nodiscard]] std::array<std::shared_ptr<audio::QueuedSource>, 7> queues() const {
        return queues_;
    }

private:
    std::shared_ptr<gfx::SdlGpuSurface> surface_;
    std::shared_ptr<audio::Device> device_;
    std::shared_ptr<OutputControl> control_;
    std::array<std::shared_ptr<audio::QueuedSource>, 7> queues_;
    std::shared_ptr<const Frame> frame_;
};
} // namespace opent1::video
