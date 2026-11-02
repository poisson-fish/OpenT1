#pragma once
#include <SDL3/SDL_gpu.h>
#include <opent1/core/types.hpp>
#include <memory>
#include <algorithm>
#include <optional>
#include <vector>

namespace opent1::gfx::detail {
// Single-mip, clamp-sampled pages. Every slot includes a duplicated one-texel
// gutter; no resource arrays, descriptor indexing or dedicated-memory assumption.
struct LightMapPage {
    static constexpr UInt32 Side = 512;
    static constexpr std::size_t Bytes = Side * Side * 4;

    struct Rect {
        UInt32 x, y, width, height;
    };

    SDL_GPUDevice* device;
    SDL_GPUTexture* texture{};
    std::vector<Rect> free;
    UInt32 x{}, y{}, rowHeight{};

    explicit LightMapPage(SDL_GPUDevice* d) : device(d) {}

    ~LightMapPage() {
        if (texture)
            SDL_ReleaseGPUTexture(device, texture);
    }

    std::optional<Rect> reserve(UInt32 width, UInt32 height) {
        for (auto it = free.begin(); it != free.end(); ++it) {
            if (it->width == width && it->height == height) {
                auto result = *it;
                free.erase(it);
                return result;
            }
        }
        if (x + width > Side) {
            x = 0;
            y += rowHeight;
            rowHeight = 0;
        }
        if (y + height > Side)
            return {};
        Rect result{x, y, width, height};
        x += width;
        rowHeight = std::max(rowHeight, height);
        return result;
    }
};

struct LightMapSlot {
    std::shared_ptr<LightMapPage> page;
    LightMapPage::Rect rect;

    ~LightMapSlot() {
        // If allocation fails during cleanup, the hole is reclaimed with the page.
        try {
            page->free.push_back(rect);
        } catch (...) {
        }
    }
};

class LightMapPages {
    SDL_GPUDevice* device_;
    std::size_t budget_;
    std::vector<std::weak_ptr<LightMapPage>> pages_;
    UInt64 allocations_{};

public:
    LightMapPages(SDL_GPUDevice* device, std::size_t budget) : device_(device), budget_(budget) {}

    std::size_t count() const {
        return std::size_t(std::ranges::count_if(pages_, [](const auto& p) {
            return !p.expired();
        }));
    }

    UInt64 allocations() const {
        return allocations_;
    }

    std::shared_ptr<LightMapSlot> allocate(UInt32 width, UInt32 height) {
        if (width + 2 > LightMapPage::Side || height + 2 > LightMapPage::Side)
            return {};
        std::erase_if(pages_, [](const auto& p) {
            return p.expired();
        });
        for (const auto& weak : pages_) {
            auto page = weak.lock();
            if (auto rect = page->reserve(width + 2, height + 2)) {
                auto slot = std::make_shared<LightMapSlot>();
                slot->page = std::move(page);
                slot->rect = *rect;
                return slot;
            }
        }
        if (pages_.size() >= budget_ / LightMapPage::Bytes)
            return {};
        auto page = std::make_shared<LightMapPage>(device_);
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = info.height = LightMapPage::Side;
        info.layer_count_or_depth = info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        page->texture = SDL_CreateGPUTexture(device_, &info);
        if (!page->texture)
            return {};
        ++allocations_;
        pages_.push_back(page);
        auto slot = std::make_shared<LightMapSlot>();
        slot->rect = *page->reserve(width + 2, height + 2);
        slot->page = std::move(page);
        return slot;
    }
};
}
