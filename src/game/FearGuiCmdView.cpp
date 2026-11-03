#include <opent1/game/commander_huds.hpp>
#include <ranges>

namespace opent1::game {

FGCommandView::FGCommandView() {
    fields_.extent = {640, 480};
}

bool FGCommandView::onAdd() {
    return Parent::onAdd();
}

std::function<void()> FGCommandView::checkpoint() {
    auto base = Parent::checkpoint();
    return [this, base = std::move(base), border = border_, lowRes = lowRes_] {
        base();
        border_ = border;
        lowRes_ = lowRes;
    };
}

bool FGCommandView::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    surface.fill(
        {offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1}, 0);
    if (!drawChildren(surface, offset, clip)) {
        return false;
    }
    if (!border_) {
        return true;
    }
    for (auto i : std::views::iota(Int32{0}, Int32{15})) {
        if (!border_->get(i)) {
            return false;
        }
    }
    // Vanilla's PBA keeps the four stretch strips after the fixed pieces.
    constexpr std::array<Int32, 15> pieces{0, 1, 12, 2, 3, 4, 13, 5, 6, 9, 11, 10, 7, 14, 8};
    const auto b = pieces | std::views::transform([this](Int32 i) -> const gfx::Bitmap& {
                       return *border_->get(i);
                   });
    const auto draw = [&](int i, int x, int y) {
        return surface.blit(b[i], offset + Point2I{x, y});
    };
    const auto stretch = [&](int i, int x, int y, int width, int height) {
        return width <= 0 || height <= 0 ||
               surface.stretch(b[i],
                               {offset.x + x,
                                offset.y + y,
                                offset.x + x + width - 1,
                                offset.y + y + height - 1});
    };
    const auto w = fields_.extent.x, h = fields_.extent.y;
    if (!draw(0, 0, 0) || !draw(1, 0, b[0].height)) {
        return false;
    }
    const auto leftY = b[0].height + b[1].height;
    if (!stretch(2, 0, leftY, b[2].width, h - leftY - b[3].height) ||
        !draw(3, 0, h - b[3].height) || !draw(4, b[3].width, h - b[4].height) ||
        !draw(5, b[3].width + b[4].width, h - b[5].height)) {
        return false;
    }
    auto x = b[3].width + b[4].width + b[5].width;
    const auto bottom = w - x - b[7].width - b[8].width;
    if (!stretch(6, x, h - b[6].height, bottom, b[6].height)) {
        return false;
    }
    x += bottom;
    if (!draw(7, x, h - b[7].height) || !draw(8, x + b[7].width, h - b[8].height)) {
        return false;
    }
    x = 224;
    if (!draw(9, x, 0)) {
        return false;
    }
    x += b[9].width;
    const auto top = w - x - b[11].width - 44 - b[12].width;
    if (!stretch(10, x, 0, top, b[10].height)) {
        return false;
    }
    x += top;
    if (!draw(11, x, 0)) {
        return false;
    }
    x += b[11].width + 44;
    if (!draw(12, x, 0)) {
        return false;
    }
    const auto right = h - b[12].height - b[14].height - b[8].height;
    return stretch(13, w - b[13].width, b[12].height, b[13].width, right) &&
           draw(14, w - b[14].width, b[12].height + right);
}

} // namespace opent1::game
