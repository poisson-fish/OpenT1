#include <opent1/video/sdl_player.hpp>
#include <ranges>
#include <algorithm>
#include <stdexcept>

namespace opent1::video {
SdlOutput::SdlOutput(std::shared_ptr<gfx::SdlGpuSurface> surface,
                     std::shared_ptr<audio::Device> device,
                     std::shared_ptr<OutputControl> control)
    : surface_(std::move(surface)), device_(std::move(device)), control_(std::move(control)) {
    if (!surface_ || !device_) {
        throw std::invalid_argument("missing movie backend");
    }
}

SdlOutput::~SdlOutput() {
    clear();
}

bool SdlOutput::publish(const Presentation& next) {
    if (!next.frame || !surface_->ready()) {
        return false;
    }
    const auto fault = [&](OutputFailure f) {
        if (control_ && control_->fail == f) {
            control_->fail = OutputFailure::None;
            throw std::runtime_error("injected video output failure");
        }
    };
    try {
        std::array<std::shared_ptr<audio::QueuedSource>, 7> staged;
        if (next.refreshAudio) {
            for (auto t : std::views::iota(std::size_t{0}, std::size_t{7})) {
                const auto& pcm = next.audio[t];
                if (pcm.bytes.empty()) {
                    continue;
                }
                const auto block = UInt32(pcm.format.channels * (pcm.format.bits / 8));
                if (!block || !pcm.format.rate || pcm.bytes.size() % block ||
                    pcm.bytes.size() > UInt64(pcm.format.rate) * block) {
                    return false;
                }
                const auto chunk = std::max(1U, pcm.format.rate / 4) * block;
                std::vector<std::shared_ptr<const audio::Buffer>> buffers;
                for (std::size_t at = 0; at < pcm.bytes.size(); at += chunk) {
                    audio::Wave wave;
                    wave.rate = pcm.format.rate;
                    wave.channels = pcm.format.channels;
                    wave.bits = pcm.format.bits;
                    const auto end = std::min(at + chunk, pcm.bytes.size());
                    wave.samples.assign(pcm.bytes.begin() + std::ptrdiff_t(at),
                                        pcm.bytes.begin() + std::ptrdiff_t(end));
                    buffers.push_back(device_->upload(wave));
                }
                staged[t] = device_->queue(buffers);
            }
            fault(OutputFailure::AfterAudioPreparation);
            // Provisional sources remain inaudible until GPU publication.
            for (auto& q : staged) {
                if (q) {
                    q->prepare();
                }
            }
        }
        fault(OutputFailure::BeforeGpu);
        if (frame_ != next.frame && !surface_->uploadBitmap(next.frame->bitmap)) {
            return false;
        }
        if (next.refreshAudio) {
            for (auto& q : queues_) {
                if (q) {
                    q->stop();
                }
            }
            for (auto& q : staged) {
                if (q) {
                    q->activate();
                }
            }
            queues_ = std::move(staged);
        }
        frame_ = next.frame;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void SdlOutput::clear() noexcept {
    for (auto& q : queues_) {
        if (q) {
            q->stop();
            q.reset();
        }
    }
    frame_.reset();
}
} // namespace opent1::video
