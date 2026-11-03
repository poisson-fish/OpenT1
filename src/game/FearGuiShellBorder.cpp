#include <opent1/game/shell_controls.hpp>
#include <ranges>

namespace opent1::game {

bool FGShellBorder::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    if (background_) {
        // Native FGShellBorder draws its background through BitmapCtrl at the
        // full shell extent. Center-blitting leaves the parent's loading art
        // exposed around it when the canvas grows or changes aspect ratio.
        gui::Bitmap background;
        background.bitmap = background_;
        background.stretch = true;
        if (!background.resize({}, fields_.extent) || !background.draw(surface, offset, clip)) {
            return false;
        }
    } else {
        surface.fill(
            {offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1},
            fields_.style.fill);
    }
    if (!drawChildren(surface, offset, clip)) {
        return false;
    }
    if (!border_ || fields_.extent.x < 512 || fields_.extent.y < 384) {
        return true;
    }
    for (auto i : std::views::iota(0, 15)) {
        if (!border_->get(i)) {
            return false;
        }
    }
    const auto& b = border_->bitmaps;
    const auto draw = [&](int i, int x, int y) {
        return surface.blit(b[i], {offset.x + x, offset.y + y});
    };
    const auto stretch = [&](int i, int x, int y, int w, int h) {
        return w <= 0 || h <= 0 ||
               surface.stretch(
                   b[i], {offset.x + x, offset.y + y, offset.x + x + w - 1, offset.y + y + h - 1});
    };
    const auto w = fields_.extent.x, h = fields_.extent.y;
    int x = 224;
    if (!draw(9, x, 0)) {
        return false;
    }
    x += b[9].width;
    const auto top = w - x - b[10].width - 44 - b[13].width;
    if (!stretch(8, x, 0, top, b[8].height)) {
        return false;
    }
    x += top;
    if (!draw(10, x, 0)) {
        return false;
    }
    x += b[10].width + 44;
    if (!draw(13, x, 0) || !draw(14, w - b[14].width, b[13].height)) {
        return false;
    }
    const auto rightY = b[13].height + b[14].height;
    if (!stretch(7, w - b[7].width, rightY, b[7].width, h - rightY - b[3].height) ||
        !draw(3, w - b[3].width, h - b[3].height)) {
        return false;
    }
    if (!draw(11, 0, 0) || !draw(12, 0, b[11].height)) {
        return false;
    }
    const auto leftY = b[11].height + b[12].height;
    if (!stretch(6, 0, leftY, b[6].width, h - leftY - b[0].height) ||
        !draw(0, 0, h - b[0].height) || !draw(1, b[0].width, h - b[1].height)) {
        return false;
    }
    x = b[0].width + b[1].width;
    return stretch(5, x, h - b[5].height, w - x - b[4].width - b[3].width, b[5].height) &&
           draw(4, w - b[3].width - b[4].width, h - b[4].height);
}

namespace {
inline constexpr Int32 kIdItgBmpFileName = 110065;
inline constexpr UInt8 kVersionByte = 0xFFU;
} // namespace

std::function<void()> FGShellBorder::checkpoint() {
    auto base = Parent::checkpoint();
    auto bmp = bitmapName_;
    const auto background = background_;
    const auto border = border_;
    return [this, base = std::move(base), bmp = std::move(bmp), background, border] {
        base();
        bitmapName_ = bmp;
        background_ = background;
        border_ = border;
    };
}

bool FGShellBorder::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgBmpFileName, bitmapName_);
    return true;
}

bool FGShellBorder::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, kIdItgBmpFileName, v)) {
        return false;
    }
    bitmapName_ = std::get<std::string>(v);
    return true;
}

Persistent::Error
FGShellBorder::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    UInt8 verByte{};
    if (!sio.read(verByte)) {
        return Persistent::Error::ReadError;
    }
    std::string bmp{};
    if (verByte == kVersionByte) {
        if (!sio.readString(bmp)) {
            return Persistent::Error::ReadError;
        }
    } else {
        if (!sio.setPosition(sio.getPosition() - 1)) {
            return Persistent::Error::ReadError;
        }
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    bitmapName_ = std::move(bmp);
    return Persistent::Error::Ok;
}

Persistent::Error FGShellBorder::write(StreamIO& sio,
                                       Int32 version,
                                       Int32 user,
                                       Persistent::StoreContext& ctx) const {
    if (!sio.write(kVersionByte) || !sio.writeString(bitmapName_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
