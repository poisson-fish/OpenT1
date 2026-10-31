#include "opent1/game/shell_dialogs.hpp"

#include <array>

namespace opent1::game {

bool FGDlgBox::draw(gfx::SoftwareSurface& surface, Point2I p, RectI clip) const {
    surface.fill({p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1}, 244);
    if (std::ranges::all_of(bitmaps_, [](const auto& b) {
            return b && b->width > 0 && b->height > 0;
        })) {
        const auto draw = [&](int i, int x, int y, gfx::Flip flip = gfx::Flip::None) {
            return surface.blit(*bitmaps_[i], {p.x + x, p.y + y}, flip);
        };
        const auto right = fields_.extent.x, bottom = fields_.extent.y;
        for (int x = bitmaps_[0]->width; x < right - bitmaps_[0]->width; x += bitmaps_[1]->width) {
            if (!draw(1, x, 0)) {
                return false;
            }
        }
        for (int y = bitmaps_[0]->height; y < bottom - bitmaps_[3]->height;
             y += bitmaps_[2]->height) {
            if (!draw(2, 0, y) || !draw(2, right - bitmaps_[2]->width, y, gfx::Flip::X)) {
                return false;
            }
        }
        for (int x = bitmaps_[3]->width; x < right - bitmaps_[3]->width; x += bitmaps_[4]->width) {
            if (!draw(4, x, bottom - bitmaps_[4]->height)) {
                return false;
            }
        }
        if (!draw(0, 0, 0) || !draw(0, right - bitmaps_[0]->width, 0, gfx::Flip::X) ||
            !draw(3, 0, bottom - bitmaps_[3]->height) ||
            !draw(3, right - bitmaps_[3]->width, bottom - bitmaps_[3]->height, gfx::Flip::X)) {
            return false;
        }
    }
    return drawChildren(surface, p, clip);
}

namespace {

constexpr std::array<std::string_view, 8> kModalDialogNames{{
    "ExitGameDialog",
    "RemoveDemoDialog",
    "RemovePlayerDialog",
    "MOTDDialog",
    "MessageDialog",
    "ServerInfoDialog",
    "HostWarnDialog",
    "DNSWarningDialog",
}};

inline constexpr UInt32 kIdCtgDialog = 200200U;

gui::Control* findChildControlByName(gui::Control* root, std::string_view targetName) noexcept {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->getName() == targetName) {
        return root;
    }
    for (auto* obj : root->objects()) {
        if (auto* ctrl = dynamic_cast<gui::Control*>(obj); ctrl != nullptr) {
            if (auto* found = findChildControlByName(ctrl, targetName); found != nullptr) {
                return found;
            }
        }
    }
    return nullptr;
}

} // namespace

FGDlgBox::FGDlgBox() {
    fields_.tag = kIdCtgDialog;
    active = true;
}

bool FGDlgBox::isModalNamedDialog() const noexcept {
    const auto myName = getName();
    for (const auto modal : kModalDialogNames) {
        if (myName == modal) {
            return true;
        }
    }
    return false;
}

bool FGDlgBox::handleReturnKey(gui::Control* rootSearch) {
    gui::Control* scope = (rootSearch != nullptr) ? rootSearch : this;
    auto* ctrl = findChildControlByName(scope, "DialogReturnButton");
    if (ctrl == nullptr) {
        ctrl = findChildControlByName(scope, "DialogCloseButton");
    }
    if (auto* button = dynamic_cast<FGUniversalButton*>(ctrl); button != nullptr) {
        button->action();
        return true;
    }
    return false;
}

bool FGDlgBox::handleEscapeKey(gui::Control* rootSearch) {
    gui::Control* scope = (rootSearch != nullptr) ? rootSearch : this;
    auto* ctrl = findChildControlByName(scope, "DialogEscapeButton");
    if (ctrl == nullptr) {
        ctrl = findChildControlByName(scope, "DialogCloseButton");
    }
    if (auto* button = dynamic_cast<FGUniversalButton*>(ctrl); button != nullptr) {
        button->action();
        return true;
    }
    return false;
}

Persistent::Error
FGDlgBox::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    auto snap = checkpoint();
    Int32 localVersion = 0;
    if (!sio.read(localVersion) || localVersion != DlgBoxVersion) {
        snap();
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        snap();
    }
    return err;
}

Persistent::Error
FGDlgBox::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    const Int32 localVersion = DlgBoxVersion;
    if (!sio.write(localVersion)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
