#include <opent1/gui/sdl_terrain_tools.hpp>
#include <ranges>
#include <opent1/gui/sdl_presentation.hpp>

namespace opent1::gui {
namespace {
std::shared_ptr<TerrainTools> require(std::shared_ptr<TerrainTools> tools) {
    if (!tools) {
        throw std::invalid_argument("terrain window tools");
    }
    return tools;
}
}

Canvas& SdlTerrainWindow::canvas() {
    return landscape_ ? tools_->landscape : tools_->ted;
}

SdlTerrainWindow::SdlTerrainWindow(std::shared_ptr<TerrainTools> tools,
                                   bool landscapeWindow,
                                   const platform::WindowConfig& config,
                                   const gfx::GpuSurfaceConfig& gpu,
                                   std::function<float()> requestedScale)
    : tools_(require(std::move(tools))), landscape_(landscapeWindow), window_(config),
      surface_(gfx::SdlGpuSurface::create(window_, config.width, config.height, gpu)),
      input_(std::make_unique<SdlInput>(window_, canvas())),
      requestedScale_(std::move(requestedScale)) {
    if (!surface_ || !tools_->resize(landscape_, {config.width, config.height})) {
        throw std::runtime_error("terrain native window initialization");
    }
}

Presentation SdlTerrainWindow::presentation() {
    const auto state = window_.state();
    return Presentation::fit({state.pixelWidth, state.pixelHeight},
                             requestedScale_ ? requestedScale_() : 0);
}

bool SdlTerrainWindow::process(const platform::Event& event) {
    if (!input_ || event.window != window_.id()) {
        return false;
    }
    if (const auto* resize = std::get_if<platform::WindowEvent>(&event.data);
        resize && resize->type == SDL_EVENT_WINDOW_RESIZED) {
        if (!tools_->resize(landscape_, presentation().extent)) {
            return false;
        }
    }
    input_->presentation(presentation());
    return input_->process(event);
}

bool SdlTerrainWindow::redraw(bool present) {
    if (!tools_) {
        return false;
    }
    gfx::Palette fallback;
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{256})) {
        fallback.palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
    }
    const auto* palette = tools_->editor()->palette ? &*tools_->editor()->palette : &fallback;
    const auto view = presentation();
    input_->presentation(view);
    if (!tools_->resize(landscape_, view.extent)) {
        return false;
    }
    auto frame = canvas().render(view.extent.x, view.extent.y, palette);
    return frame && SdlPresentation::upload(*surface_, *frame, view, present);
}

void SdlTerrainWindow::detach() {
    input_.reset();
    tools_.reset();
}
} // namespace opent1::gui
