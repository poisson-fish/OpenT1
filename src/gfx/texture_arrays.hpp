#pragma once
#include <opent1/gfx/hardware_batch.hpp>
#include <ranges>
#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <memory>

namespace opent1::gfx::detail {
struct TextureArrayPage {
    static constexpr UInt32 Layers = 16;
    SDL_GPUDevice* device;
    SDL_GPUTexture* texture{};
    Int32 width{}, height{};
    UInt32 levels{};
    std::size_t bytes{};
    std::vector<UInt32> free;

    explicit TextureArrayPage(SDL_GPUDevice* d) : device(d) {}

    ~TextureArrayPage() {
        if (texture)
            SDL_ReleaseGPUTexture(device, texture);
    }
};

struct TextureArraySlot {
    std::shared_ptr<TextureArrayPage> page;
    UInt32 layer{};

    ~TextureArraySlot() {
        page->free.push_back(layer);
    } // capacity reserved for all layers
};

// RGBA8 textures share storage only with identical dimensions and authored mip
// layout. Sampling/filter/alpha rules stay per draw, and draw order is unchanged.
class TextureArrays {
    SDL_GPUDevice* device_;
    std::size_t budget_;
    std::vector<std::weak_ptr<TextureArrayPage>> pages_;

public:
    TextureArrays(SDL_GPUDevice* device, std::size_t budget) : device_(device), budget_(budget) {}

    std::size_t count() const {
        return std::size_t(std::ranges::count_if(pages_, [](const auto& p) {
            return !p.expired();
        }));
    }

    std::size_t bytes() const {
        std::size_t sum{};
        for (const auto& weak : pages_)
            if (const auto p = weak.lock())
                sum += p->bytes;
        return sum;
    }

    std::shared_ptr<TextureArraySlot> allocate(const HardwareTexture& source) {
        if (source.lightMap || source.dynamic)
            return {};
        const auto levels = UInt32(source.mips.size() + 1);
        for (auto i : std::views::iota(UInt32{1}) | std::views::take_while([&](UInt32 i) {
                          return i < levels;
                      })) {
            if (source.mips[i - 1].width != std::max(1, source.width >> i) ||
                source.mips[i - 1].height != std::max(1, source.height >> i))
                return {};
        }
        std::erase_if(pages_, [](const auto& p) {
            return p.expired();
        });
        for (const auto& weak : pages_) {
            auto page = weak.lock();
            if (page->width == source.width && page->height == source.height &&
                page->levels == levels && !page->free.empty()) {
                auto slot = std::make_shared<TextureArraySlot>();
                slot->page = std::move(page);
                slot->layer = slot->page->free.back();
                slot->page->free.pop_back();
                return slot;
            }
        }
        auto layerBytes = source.rgba.size();
        for (const auto& mip : source.mips)
            layerBytes += mip.rgba.size();
        const auto required = layerBytes * TextureArrayPage::Layers;
        if (required > budget_ || bytes() > budget_ - required)
            return {};
        auto page = std::make_shared<TextureArrayPage>(device_);
        page->width = source.width;
        page->height = source.height;
        page->levels = levels;
        page->bytes = required;
        page->free.reserve(TextureArrayPage::Layers);
        for (UInt32 i = TextureArrayPage::Layers; i > 0; --i)
            page->free.push_back(i - 1);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = UInt32(source.width);
        info.height = UInt32(source.height);
        info.layer_count_or_depth = TextureArrayPage::Layers;
        info.num_levels = levels;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        page->texture = SDL_CreateGPUTexture(device_, &info);
        if (!page->texture)
            return {};
        pages_.push_back(page);
        auto slot = std::make_shared<TextureArraySlot>();
        slot->page = std::move(page);
        slot->layer = slot->page->free.back();
        slot->page->free.pop_back();
        return slot;
    }
};
}
