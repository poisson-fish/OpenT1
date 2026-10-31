#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace opent1::gfx {
// Runtime image lists grow with valid scenes. Embedders may explicitly select
// resource budgets for previews or untrusted workloads; file/geometry validation
// and representable GPU indices remain enforced independently.
struct RenderLimits {
    std::size_t images{512}, polygons{4096}, vertices{65536}, outputVertices{196608};
    std::size_t textures{4096}, textureBytes{64U * 1024U * 1024U};
    std::size_t edges{65536}, spans{1024U * 1024U}, fragments{4096},
        operations{16U * 1024U * 1024U};
    std::size_t shadows{32}, containers{65536}, links{32768};

    [[nodiscard]] static constexpr RenderLimits dynamic() {
        constexpr auto count = std::size_t(std::numeric_limits<std::uint32_t>::max());
        constexpr auto size = std::numeric_limits<std::size_t>::max();
        return {count,
                count,
                count,
                count,
                count,
                size,
                size,
                size,
                size,
                size,
                count,
                std::size_t(std::numeric_limits<std::int32_t>::max()),
                size};
    }
};

inline thread_local RenderLimits currentRenderLimits = RenderLimits::dynamic();

[[nodiscard]] inline const RenderLimits& renderLimits() {
    return currentRenderLimits;
}

class ScopedRenderLimits {
public:
    explicit ScopedRenderLimits(RenderLimits limits) : saved_(currentRenderLimits) {
        currentRenderLimits = limits;
    }

    ~ScopedRenderLimits() {
        currentRenderLimits = saved_;
    }

    ScopedRenderLimits(const ScopedRenderLimits&) = delete;
    ScopedRenderLimits& operator=(const ScopedRenderLimits&) = delete;

private:
    RenderLimits saved_;
};
}
