#pragma once
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include "lightmap_pages.hpp"
#include "texture_arrays.hpp"
#include <SDL3/SDL_gpu.h>
#include <list>
#include <unordered_map>

namespace opent1::gfx::detail {
class GpuMaterialRenderer {
public:
    [[nodiscard]] static std::unique_ptr<GpuMaterialRenderer>
    create(SDL_GPUDevice*,
           std::size_t textureCacheEntries,
           std::size_t meshBytes = 64U * 1024U * 1024U,
           std::size_t textureBytes = 64U * 1024U * 1024U,
           std::size_t pageBytes = 16U * 1024U * 1024U,
           std::size_t arrayBytes = 16U * 1024U * 1024U,
           UInt32 stagingBytes = 128U * 1024U * 1024U);
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
    bool trimCaches();

private:
    explicit GpuMaterialRenderer(SDL_GPUDevice* device,
                                 std::size_t textureCacheEntries,
                                 std::size_t meshBytes,
                                 std::size_t textureBytes,
                                 std::size_t pageBytes,
                                 std::size_t arrayBytes,
                                 UInt32 stagingBytes)
        : device_(device), pages_(device, pageBytes), arrays_(device, arrayBytes),
          textureCacheEntries_(textureCacheEntries), meshCacheBytes_(meshBytes),
          textureCacheBytes_(textureBytes), stagingBytes_(stagingBytes) {}

    SDL_GPUDevice* device_;
    SDL_GPUFence* frameFence_{};
    std::array<SDL_GPUGraphicsPipeline*, 40> pipelines_{};
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
    SDL_GPUTexture* whiteArray_{};
    GpuResourceStats resources_;
    GpuSubmissionStats submission_;

    struct CachedMesh {
        std::shared_ptr<const HardwareMesh> source;
        SDL_GPUBuffer *vertices{}, *indices{};
        std::size_t bytes{};
        bool pinned{};
    };

    std::unordered_map<UInt64, CachedMesh> meshes_;
    bool prepareResources(Int32 width, Int32 height, UInt32 vertices, UInt32 transfer, bool gamma);

    struct CachedTexture {
        std::shared_ptr<const HardwareTexture> source;
        UInt64 hash{};
        std::size_t bytes{};
        SDL_GPUTexture* gpu{};
        bool pinned{};
        std::shared_ptr<LightMapSlot> slot{};
        std::shared_ptr<TextureArraySlot> array{};
    };

    std::list<CachedTexture> textures_; // most recently used first
    using TextureIndex = std::unordered_multimap<UInt64, std::list<CachedTexture>::iterator>;
    TextureIndex textureIndex_;
    GpuTextureCacheStats stats_;
    LightMapPages pages_;
    TextureArrays arrays_;
    std::size_t textureCacheEntries_;
    std::size_t meshCacheBytes_, textureCacheBytes_;
    UInt32 stagingBytes_;
};
} // namespace opent1::gfx::detail
