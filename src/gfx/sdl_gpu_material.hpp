#pragma once
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <SDL3/SDL_gpu.h>
#include <list>
#include <unordered_map>

namespace opent1::gfx::detail {
class GpuMaterialRenderer {
public:
    [[nodiscard]] static std::unique_ptr<GpuMaterialRenderer> create(SDL_GPUDevice*);
    ~GpuMaterialRenderer();
    [[nodiscard]] SDL_GPUTexture*
    render(const HardwareBatch&, Int32 width, Int32 height, const GpuFrameConfig&, Flt32 gamma);

    [[nodiscard]] GpuTextureCacheStats textureCacheStats() const noexcept {
        return stats_;
    }

    [[nodiscard]] GpuResourceStats resourceStats() const noexcept {
        return resources_;
    }

    [[nodiscard]] GpuSubmissionStats submissionStats() const noexcept {
        return submission_;
    }

    void clearTextureCache() noexcept;

private:
    explicit GpuMaterialRenderer(SDL_GPUDevice* device) : device_(device) {}

    SDL_GPUDevice* device_;
    SDL_GPUFence* frameFence_{};
    std::array<SDL_GPUGraphicsPipeline*, 20> pipelines_{};
    std::array<SDL_GPUSampler*, 24> samplers_{};

    struct Targets {
        SDL_GPUTexture *color{}, *corrected{};
        Int32 width{}, height{};
    };

    std::array<Targets, 2> targets_{}; // preserve the published frame until submission succeeds
    std::size_t nextTarget_{};
    SDL_GPUTexture* depth_{};
    Int32 depthWidth_{}, depthHeight_{};
    SDL_GPUBuffer* vertices_{};
    SDL_GPUTransferBuffer* transfer_{};
    GpuResourceStats resources_;
    GpuSubmissionStats submission_;
    bool prepareResources(Int32 width, Int32 height, UInt32 vertices, UInt32 transfer, bool gamma);

    struct CachedTexture {
        std::shared_ptr<const HardwareTexture> source;
        UInt64 hash{};
        std::size_t bytes{};
        SDL_GPUTexture* gpu{};
        bool pinned{};
    };

    std::list<CachedTexture> textures_; // most recently used first
    using TextureIndex = std::unordered_multimap<UInt64, std::list<CachedTexture>::iterator>;
    TextureIndex textureIndex_;
    GpuTextureCacheStats stats_;
};
} // namespace opent1::gfx::detail
