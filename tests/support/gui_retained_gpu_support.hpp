#pragma once
#include "test_support.hpp"
#include <opent1/gfx/software_rasterizer.hpp>
#ifdef OPENT1_GUI_RETAINED_GPU
#include <opent1/gfx/sdl_gpu_surface.hpp>
#endif
namespace opent1::test {
inline void retainedGuiGpu(std::span<const std::shared_ptr<const gfx::Bitmap>> frames,
                           std::string_view driver) {
#ifdef OPENT1_GUI_RETAINED_GPU
    if (driver.empty()) {
        return;
    }
    CHECK(!frames.empty());
    platform::SdlApplication app;
    CHECK(app.initialize({}) == SDL_APP_CONTINUE);
    platform::WindowConfig config;
    config.hidden = true;
    config.width = frames[0]->width;
    config.height = frames[0]->height;
    auto& window = app.createWindow(config);
    gfx::GpuSurfaceConfig gpu;
    gpu.driver = std::string(driver);
    auto surface = gfx::SdlGpuSurface::create(window, config.width, config.height, gpu);
    CHECK(surface);
    gfx::Palette palette;
    for (UInt32 i = 0; i < 256; ++i) {
        palette.palettes[0].colors[i] = {UInt8(i), UInt8(255 - i), UInt8(i * 3), 0};
    }
    std::vector<std::byte> last;
    for (auto& frame : frames) {
        auto bitmap = *frame;
        bitmap.palette = palette;
        CHECK(surface->uploadBitmap(bitmap));
        auto cpu = gfx::SoftwareSurface::create(bitmap.width, bitmap.height);
        CHECK(cpu && cpu->setPalette(palette) && cpu->blit(bitmap, {}));
        std::vector<std::byte> rgba(std::size_t(bitmap.width * bitmap.height) * 4),
            readback(rgba.size());
        CHECK(cpu->rgba(rgba) && surface->readback(readback) && rgba == readback);
        last = std::move(readback);
    }
    gfx::Bitmap invalid;
    CHECK(!surface->uploadBitmap(invalid));
    std::vector<std::byte> after(last.size());
    CHECK(surface->readback(after) && after == last);
    app.destroyWindow(window.id());
    CHECK(!surface->ready());
#else
    (void)frames;
    (void)driver;
#endif
}
}
