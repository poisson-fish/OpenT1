#include <opent1/gfx/sdl_gpu_surface.hpp>
#include "sdl_gpu_material.hpp"
#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace opent1::gfx {
namespace {
UInt32 framebufferPitch(Int32 width, Int32 height) {
    if (width <= 0 || height <= 0 || width > Bitmap::MaxDimension ||
        height > Bitmap::MaxDimension) {
        return 0;
    }
    const auto pitch = (UInt64(UInt32(width)) * 4 + 255) & ~UInt64{255};
    return pitch * UInt32(height) <= Bitmap::MaxFileBytes ? UInt32(pitch) : 0;
}

SDL_GPUPresentMode nativeMode(GpuPresentMode mode) {
    return mode == GpuPresentMode::Immediate ? SDL_GPU_PRESENTMODE_IMMEDIATE
           : mode == GpuPresentMode::Mailbox ? SDL_GPU_PRESENTMODE_MAILBOX
                                             : SDL_GPU_PRESENTMODE_VSYNC;
}
}

std::vector<std::string> gpuDrivers() {
    std::vector<std::string> result;
    for (int i = 0; i < SDL_GetNumGPUDrivers(); ++i) {
        if (const auto* name = SDL_GetGPUDriver(i)) {
            result.emplace_back(name);
        }
    }
    return result;
}

struct SdlGpuSurface::Impl {
    std::unique_ptr<SoftwareSurface> software;
    std::unique_ptr<detail::GpuMaterialRenderer> materials;
    SDL_GPUDevice* device{};
    SDL_GPUTexture* texture{};
    SDL_GPUTexture* frame{}; // borrowed from texture or the material renderer's targets
    SDL_GPUTransferBuffer* transfer{};
    SDL_Window* window{};
    std::shared_ptr<void> cleanup;
    std::vector<std::byte> rgba;
    UInt32 pitch{}, transferBytes{};
    Flt32 gamma{1};
    bool uploaded{};
    std::string driver;
    GpuPresentMode presentMode{GpuPresentMode::VSync};
    std::size_t textureCacheEntries{};
    std::size_t meshCacheBytes{}, textureCacheBytes{}, lightMapPageBytes{};

    ~Impl() {
        cleanup.reset();
        shutdown();
    }

    void shutdown() noexcept {
        if (!device) {
            return;
        }
        SDL_WaitForGPUIdle(device);
        materials.reset();
        if (window) {
            SDL_ReleaseWindowFromGPUDevice(device, std::exchange(window, nullptr));
        }
        if (transfer) {
            SDL_ReleaseGPUTransferBuffer(device, std::exchange(transfer, nullptr));
        }
        if (texture) {
            SDL_ReleaseGPUTexture(device, std::exchange(texture, nullptr));
        }
        SDL_DestroyGPUDevice(std::exchange(device, nullptr));
        uploaded = false;
    }

    SDL_GPUTextureRegion region() const noexcept {
        SDL_GPUTextureRegion r{};
        r.texture = texture;
        r.w = UInt32(software->bitmap().width);
        r.h = UInt32(software->bitmap().height);
        r.d = 1;
        return r;
    }

    bool resize(Int32 width, Int32 height) {
        const auto newPitch = framebufferPitch(width, height);
        if (!newPitch) {
            return SDL_SetError("Invalid GPU framebuffer dimensions");
        }
        if (software && software->bitmap().width == width && software->bitmap().height == height) {
            return true;
        }
        auto next = SoftwareSurface::create(width, height);
        if (!next) {
            return false;
        }
        if (software && software->palette() && !next->setPalette(*software->palette())) {
            return false;
        }
        std::vector<std::byte> nextRgba(std::size_t(width) * std::size_t(height) * 4);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = UInt32(width);
        info.height = UInt32(height);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        auto* nextTexture = SDL_CreateGPUTexture(device, &info);
        if (!nextTexture) {
            return false;
        }
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = newPitch * UInt32(height);
        auto* nextTransfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        if (!nextTransfer) {
            SDL_ReleaseGPUTexture(device, nextTexture);
            return false;
        }
        if (texture) {
            SDL_ReleaseGPUTexture(device, texture);
        }
        if (transfer) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        texture = nextTexture;
        transfer = nextTransfer;
        software = std::move(next);
        rgba = std::move(nextRgba);
        pitch = newPitch;
        transferBytes = transferInfo.size;
        frame = nullptr;
        uploaded = false;
        return true;
    }
};

SdlGpuSurface::SdlGpuSurface(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SdlGpuSurface::~SdlGpuSurface() = default;

std::unique_ptr<SdlGpuSurface> SdlGpuSurface::create(platform::SdlWindow& window,
                                                     Int32 width,
                                                     Int32 height,
                                                     const GpuSurfaceConfig& config) {
    if (!window.nativeHandle() || !framebufferPitch(width, height) ||
        config.presentMode > GpuPresentMode::Mailbox ||
        config.driver.find('\0') != config.driver.npos || !config.textureCacheEntries) {
        SDL_SetError("Invalid GPU framebuffer configuration");
        return {};
    }
    auto impl = std::make_unique<Impl>();
    impl->textureCacheEntries = config.textureCacheEntries;
    impl->meshCacheBytes = config.meshCacheBytes;
    impl->textureCacheBytes = config.textureCacheBytes;
    impl->lightMapPageBytes = config.lightMapPageBytes;
    constexpr auto formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC |
                             SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    impl->device = SDL_CreateGPUDevice(
        formats, config.debug, config.driver.empty() ? nullptr : config.driver.c_str());
    if (!impl->device) {
        return {};
    }
    if (!SDL_ClaimWindowForGPUDevice(impl->device, window.nativeHandle())) {
        return {};
    }
    impl->window = window.nativeHandle();
    impl->driver = SDL_GetGPUDeviceDriver(impl->device);
    if (config.presentMode != GpuPresentMode::VSync &&
        (!SDL_WindowSupportsGPUPresentMode(
             impl->device, impl->window, nativeMode(config.presentMode)) ||
         !SDL_SetGPUSwapchainParameters(impl->device,
                                        impl->window,
                                        SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                        nativeMode(config.presentMode)))) {
        return {};
    }
    impl->presentMode = config.presentMode;
    if (!impl->resize(width, height)) {
        return {};
    }
    impl->cleanup = window.beforeNativeDestroy([state = impl.get()] {
        state->shutdown();
    });
    return std::unique_ptr<SdlGpuSurface>(new SdlGpuSurface(std::move(impl)));
}

SoftwareSurface& SdlGpuSurface::software() noexcept {
    return *impl_->software;
}

const SoftwareSurface& SdlGpuSurface::software() const noexcept {
    return *impl_->software;
}

bool SdlGpuSurface::ready() const noexcept {
    return impl_->device && impl_->window;
}

std::string_view SdlGpuSurface::driver() const noexcept {
    return impl_->driver;
}

bool SdlGpuSurface::supportsPresentMode(GpuPresentMode mode) const noexcept {
    return ready() && mode <= GpuPresentMode::Mailbox &&
           SDL_WindowSupportsGPUPresentMode(impl_->device, impl_->window, nativeMode(mode));
}

GpuPresentMode SdlGpuSurface::presentMode() const noexcept {
    return impl_->presentMode;
}

bool SdlGpuSurface::setPresentMode(GpuPresentMode mode) {
    if (!supportsPresentMode(mode)) {
        return SDL_SetError("Unsupported GPU present mode");
    }
    if (mode == impl_->presentMode) {
        return true;
    }
    if (!SDL_SetGPUSwapchainParameters(
            impl_->device, impl_->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, nativeMode(mode))) {
        return false;
    }
    impl_->presentMode = mode;
    return true;
}

bool SdlGpuSurface::resize(Int32 width, Int32 height) {
    if (!ready()) {
        return SDL_SetError("GPU framebuffer window is closed");
    }
    return impl_->resize(width, height);
}

Flt32 SdlGpuSurface::gamma() const noexcept {
    return impl_->gamma;
}

bool SdlGpuSurface::setGamma(Flt32 gamma) noexcept {
    if (!std::isfinite(gamma) || gamma < 0.05f || gamma > 20.f) {
        return false;
    }
    impl_->gamma = gamma;
    return true;
}

bool SdlGpuSurface::upload() {
    auto& s = *impl_;
    if (!ready() || !s.software->rgba(s.rgba, s.gamma)) {
        return SDL_SetError("GPU framebuffer requires a live window and palette");
    }
    auto* data = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(s.device, s.transfer, true));
    if (!data) {
        return false;
    }
    std::memset(data, 0, s.transferBytes);
    const auto rowBytes = std::size_t(s.software->bitmap().width) * 4;
    for (Int32 y = 0; y < s.software->bitmap().height; ++y) {
        std::memcpy(
            data + std::size_t(y) * s.pitch, s.rgba.data() + std::size_t(y) * rowBytes, rowBytes);
    }
    SDL_UnmapGPUTransferBuffer(s.device, s.transfer);
    auto* commands = SDL_AcquireGPUCommandBuffer(s.device);
    if (!commands) {
        return false;
    }
    auto* copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = s.transfer;
    source.pixels_per_row = s.pitch / 4;
    source.rows_per_layer = UInt32(s.software->bitmap().height);
    const auto region = s.region();
    SDL_UploadToGPUTexture(copy, &source, &region, true);
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(commands)) {
        return false;
    }
    s.frame = s.texture;
    s.uploaded = true;
    return true;
}

bool SdlGpuSurface::uploadBitmap(const Bitmap& bitmap) {
    auto& s = *impl_;
    const auto pitch = framebufferPitch(bitmap.width, bitmap.height);
    if (!ready() || !pitch || !bitmap.valid() || bitmap.bitDepth != 8 || !bitmap.palette) {
        return SDL_SetError("Invalid GPU bitmap upload");
    }
    auto software = SoftwareSurface::create(bitmap.width, bitmap.height);
    if (!software || !software->setPalette(*bitmap.palette)) {
        return false;
    }
    // Video pixels are opaque even when a caller's Bitmap carries GUI flags.
    for (Int32 y = 0; y < bitmap.height; ++y) {
        const auto row = bitmap.row(y);
        auto target = software->pixels().subspan(std::size_t(y) * software->bitmap().stride(),
                                                 std::size_t(bitmap.width));
        std::copy_n(row.begin(), bitmap.width, target.begin());
    }
    std::vector<std::byte> rgba(std::size_t(bitmap.width) * std::size_t(bitmap.height) * 4);
    if (!software->rgba(rgba, s.gamma)) {
        return false;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = UInt32(bitmap.width);
    info.height = UInt32(bitmap.height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    const auto deleteTexture = [&](SDL_GPUTexture* p) {
        if (p) {
            SDL_ReleaseGPUTexture(s.device, p);
        }
    };
    std::unique_ptr<SDL_GPUTexture, decltype(deleteTexture)> texture(
        SDL_CreateGPUTexture(s.device, &info), deleteTexture);
    if (!texture) {
        return false;
    }
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = pitch * UInt32(bitmap.height);
    const auto deleteTransfer = [&](SDL_GPUTransferBuffer* p) {
        if (p) {
            SDL_ReleaseGPUTransferBuffer(s.device, p);
        }
    };
    std::unique_ptr<SDL_GPUTransferBuffer, decltype(deleteTransfer)> transfer(
        SDL_CreateGPUTransferBuffer(s.device, &ti), deleteTransfer);
    if (!transfer) {
        return false;
    }
    auto* mapped =
        static_cast<std::byte*>(SDL_MapGPUTransferBuffer(s.device, transfer.get(), false));
    if (!mapped) {
        return false;
    }
    std::memset(mapped, 0, ti.size);
    for (Int32 y = 0; y < bitmap.height; ++y) {
        std::memcpy(mapped + std::size_t(y) * pitch,
                    rgba.data() + std::size_t(y) * std::size_t(bitmap.width) * 4,
                    std::size_t(bitmap.width) * 4);
    }
    SDL_UnmapGPUTransferBuffer(s.device, transfer.get());
    auto* commands = SDL_AcquireGPUCommandBuffer(s.device);
    if (!commands) {
        return false;
    }
    auto* copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer.get();
    source.pixels_per_row = pitch / 4;
    source.rows_per_layer = UInt32(bitmap.height);
    SDL_GPUTextureRegion region{};
    region.texture = texture.get();
    region.w = UInt32(bitmap.width);
    region.h = UInt32(bitmap.height);
    region.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(commands)) {
        return false;
    }
    // All allocations, validation and submission precede this publication.
    SDL_ReleaseGPUTexture(s.device, std::exchange(s.texture, texture.release()));
    SDL_ReleaseGPUTransferBuffer(s.device, std::exchange(s.transfer, transfer.release()));
    s.software = std::move(software);
    s.rgba = std::move(rgba);
    s.pitch = pitch;
    s.transferBytes = ti.size;
    s.frame = s.texture;
    s.uploaded = true;
    return true;
}

PresentResult SdlGpuSurface::present() {
    if (!ready()) {
        SDL_SetError("GPU framebuffer window is closed");
        return PresentResult::Error;
    }
    if ((SDL_GetWindowFlags(impl_->window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
        return PresentResult::Skipped;
    }
    if (!upload()) {
        return PresentResult::Error;
    }
    return presentFrame();
}

bool SdlGpuSurface::render(const HardwareBatch& batch, const GpuFrameConfig& config) {
    auto& s = *impl_;
    if (!ready()) {
        return SDL_SetError("GPU material window is closed");
    }
    if (!s.materials) {
        s.materials = detail::GpuMaterialRenderer::create(s.device,
                                                          s.textureCacheEntries,
                                                          s.meshCacheBytes,
                                                          s.textureCacheBytes,
                                                          s.lightMapPageBytes);
        if (!s.materials) {
            return false;
        }
    }
    auto* texture = s.materials->render(
        batch, s.software->bitmap().width, s.software->bitmap().height, config, s.gamma);
    if (!texture) {
        return false;
    }
    s.frame = texture;
    s.uploaded = true;
    return true;
}

GpuTextureCacheStats SdlGpuSurface::textureCacheStats() const noexcept {
    return impl_->materials ? impl_->materials->textureCacheStats() : GpuTextureCacheStats{};
}

GpuResourceStats SdlGpuSurface::resourceStats() const noexcept {
    return impl_->materials ? impl_->materials->resourceStats() : GpuResourceStats{};
}

GpuSubmissionStats SdlGpuSurface::submissionStats() const noexcept {
    return impl_->materials ? impl_->materials->submissionStats() : GpuSubmissionStats{};
}

void SdlGpuSurface::clearTextureCache() noexcept {
    if (impl_->materials) {
        impl_->materials->clearTextureCache();
    }
}

PresentResult SdlGpuSurface::presentFrame() {
    auto& s = *impl_;
    if (!ready() || !s.uploaded) {
        SDL_SetError("GPU framebuffer has no live uploaded/rendered frame");
        return PresentResult::Error;
    }
    if ((SDL_GetWindowFlags(s.window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
        return PresentResult::Skipped;
    }
    auto* commands = SDL_AcquireGPUCommandBuffer(s.device);
    if (!commands) {
        return PresentResult::Error;
    }
    SDL_GPUTexture* swapchain{};
    UInt32 width{}, height{};
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, s.window, &swapchain, &width, &height)) {
        SDL_CancelGPUCommandBuffer(commands);
        return PresentResult::Error;
    }
    if (!swapchain) {
        SDL_CancelGPUCommandBuffer(commands);
        return PresentResult::Skipped;
    }
    SDL_GPUBlitInfo blit{};
    blit.source.texture = s.frame;
    blit.source.w = UInt32(s.software->bitmap().width);
    blit.source.h = UInt32(s.software->bitmap().height);
    blit.destination.texture = swapchain;
    blit.destination.w = width;
    blit.destination.h = height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commands, &blit);
    return SDL_SubmitGPUCommandBuffer(commands) ? PresentResult::Presented : PresentResult::Error;
}

bool SdlGpuSurface::readback(std::span<std::byte> output) {
    auto& s = *impl_;
    if (!ready() || !s.uploaded || output.size() != s.rgba.size()) {
        return SDL_SetError("Invalid GPU framebuffer readback");
    }
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = s.transferBytes;
    auto* download = SDL_CreateGPUTransferBuffer(s.device, &info);
    if (!download) {
        return false;
    }
    const auto release = [&] {
        SDL_ReleaseGPUTransferBuffer(s.device, download);
    };
    auto* commands = SDL_AcquireGPUCommandBuffer(s.device);
    if (!commands) {
        release();
        return false;
    }
    auto* copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) {
        SDL_CancelGPUCommandBuffer(commands);
        release();
        return false;
    }
    SDL_GPUTextureTransferInfo target{};
    target.transfer_buffer = download;
    target.pixels_per_row = s.pitch / 4;
    target.rows_per_layer = UInt32(s.software->bitmap().height);
    auto region = s.region();
    region.texture = s.frame;
    SDL_DownloadFromGPUTexture(copy, &region, &target);
    SDL_EndGPUCopyPass(copy);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        release();
        return false;
    }
    const bool waited = SDL_WaitForGPUFences(s.device, true, &fence, 1);
    SDL_ReleaseGPUFence(s.device, fence);
    if (!waited) {
        release();
        return false;
    }
    const auto* data =
        static_cast<const std::byte*>(SDL_MapGPUTransferBuffer(s.device, download, false));
    if (!data) {
        release();
        return false;
    }
    const auto rowBytes = std::size_t(s.software->bitmap().width) * 4;
    for (Int32 y = 0; y < s.software->bitmap().height; ++y) {
        std::memcpy(
            output.data() + std::size_t(y) * rowBytes, data + std::size_t(y) * s.pitch, rowBytes);
    }
    SDL_UnmapGPUTransferBuffer(s.device, download);
    release();
    return true;
}
} // namespace opent1::gfx
