#pragma once
#include <opent1/gfx/hardware_batch.hpp>
#include <opent1/platform/sdl_window.hpp>

namespace opent1::gfx {
enum class PresentResult { Presented, Skipped, Error };
enum class GpuPresentMode : UInt8 { VSync, Immediate, Mailbox };
// Built-in backend names, in SDL's preference order. Device creation still
// determines whether a backend is usable on this host.
[[nodiscard]] std::vector<std::string> gpuDrivers();

struct GpuSurfaceConfig {
    std::string driver; // empty selects an available SDL_GPU backend
    bool debug{};
    GpuPresentMode presentMode{GpuPresentMode::VSync};
    // Retained content entries, not preallocated textures. Pixel payload is
    // separately bounded to 64 MiB; driver alignment/metadata cost is extra.
    std::size_t textureCacheEntries{8192};
};

struct GpuFrameConfig {
    Point3F clearColor{}, hazeColor{};
    Flt32 clearAlpha{1};
    std::optional<RectI> clip; // clear is whole-frame; polygon draws are clipped
};

struct GpuTextureCacheStats {
    std::size_t textures{}, bytes{};
    UInt64 hits{}, uploads{}, evictions{};
    UInt64 reuses{}; // uploads into recycled dynamic texture allocations
};

struct GpuResourceStats {
    UInt64 targetAllocations{}, vertexAllocations{}, uploadAllocations{};
    UInt32 vertexCapacity{}, uploadCapacity{};
};

struct GpuSubmissionStats {
    UInt32 inputDraws{}, draws{}, pipelineBinds{}, samplerBinds{}, vertexUniformPushes{},
        fragmentUniformPushes{};
    double fenceWaitMs{}; // CPU time in the preceding hardware submission's fence wait
};

// Indexed software drawing/presentation and an RGBA/depth hardware material path.
// All operations, including destruction, belong to the SDL main thread. The
// window may close first: its cleanup token releases all GPU resources before
// the native handle dies. The owned indexed framebuffer remains accessible.
class SdlGpuSurface {
public:
    [[nodiscard]] static std::unique_ptr<SdlGpuSurface>
    create(platform::SdlWindow&, Int32 width, Int32 height, const GpuSurfaceConfig& = {});
    ~SdlGpuSurface();
    SdlGpuSurface(const SdlGpuSurface&) = delete;
    SdlGpuSurface& operator=(const SdlGpuSurface&) = delete;
    [[nodiscard]] SoftwareSurface& software() noexcept;
    [[nodiscard]] const SoftwareSurface& software() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view driver() const noexcept;
    [[nodiscard]] bool supportsPresentMode(GpuPresentMode) const noexcept;
    [[nodiscard]] GpuPresentMode presentMode() const noexcept;
    bool setPresentMode(GpuPresentMode); // unsupported/invalid requests retain the mode/frame
    // Rebuilds the logical framebuffer while retaining palette, gamma and the
    // material texture cache. New pixels are zero and require upload/render.
    // Same-size requests are no-ops; failure preserves the current frame.
    // Successful changes invalidate borrowed software()/pixel references.
    bool resize(Int32 width, Int32 height);
    bool setGamma(Flt32) noexcept; // same finite range and curve as SoftwareSurface
    [[nodiscard]] Flt32 gamma() const noexcept;
    bool upload(); // no swapchain acquisition; also works with a hidden window
    // Stage an owned indexed bitmap/palette and fresh native resources. Failure
    // retains dimensions, software pixels and the last GPU frame.
    bool uploadBitmap(const Bitmap&);
    PresentResult present(); // scales to current native pixel size; skips minimized windows
    // Clears and draws an owned batch into reusable offscreen color/depth targets.
    // Failure retains the last uploaded/rendered frame. Hardware RGB shading,
    // haze, material blending and gamma run on the GPU. Does not alter
    // the indexed software framebuffer. Depth clears each frame (larger 1/z wins).
    // Reuses bindings and merges adjacent compatible triangle ranges within
    // each material/haze layer, preserving primitive and transparency order.
    // Waits for this surface's preceding hardware submission to bound memory.
    bool render(const HardwareBatch&, const GpuFrameConfig& = {});
    // Per-device content cache: configurable retained entries (8192 by default),
    // expanded when needed by the current frame; 64 MiB of RGBA payload with shared
    // immutable CPU snapshots for collision-safe comparison. Hits/uploads count
    // successful frames; evictions include explicit clears and failed frames.
    // Payload bytes exclude driver allocation alignment, descriptors and metadata.
    [[nodiscard]] GpuTextureCacheStats textureCacheStats() const noexcept;
    // Cumulative successful allocation counts and retained buffer capacities.
    [[nodiscard]] GpuResourceStats resourceStats() const noexcept;
    // Last successfully submitted frame, including deferred haze and gamma.
    [[nodiscard]] GpuSubmissionStats submissionStats() const noexcept;
    void clearTextureCache() noexcept; // does not discard the published frame
    // Presents the last uploaded/rendered frame without uploading software again.
    PresentResult presentFrame();
    // Synchronous readback of the last successfully uploaded/rendered RGBA frame.
    bool readback(std::span<std::byte>);

private:
    struct Impl;
    explicit SdlGpuSurface(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace opent1::gfx
