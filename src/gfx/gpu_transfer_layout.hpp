#pragma once
#include <opent1/core/types.hpp>
#include <SDL3/SDL_gpu.h>
#include <string_view>

namespace opent1::gfx::detail {
inline UInt32 textureRowAlignment(SDL_GPUDevice* device) {
    // SDL 3.4's Metal uploader ignores pixels_per_row and reads packed rows.
    // Keep D3D12's 256-byte row alignment on the other backends.
    const auto* driver = device ? SDL_GetGPUDeviceDriver(device) : nullptr;
    return driver && std::string_view(driver) == "metal" ? 1U : 256U;
}

inline UInt64 rgbaTransferPitch(UInt32 width, UInt32 alignment) {
    return (UInt64(width) * 4 + alignment - 1) & ~UInt64(alignment - 1);
}
} // namespace opent1::gfx::detail
