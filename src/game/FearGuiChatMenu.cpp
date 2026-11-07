#include <opent1/game/chat_inventory_huds.hpp>

#include <format>

namespace opent1::game {

namespace {
inline constexpr Int32 kCtrlVersion = 0;
inline constexpr Int32 kIdItgObjectName = 200151;
} // namespace

void FGChatMenu::setMenuName(std::string_view name) {
    menuName_ = std::string(name.substr(0, 63));
}

bool FGChatMenu::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const auto* menu = resolveMenu ? resolveMenu() : nullptr;
    if (menu && font) {
        auto position = offset + Point2I{3, 1};
        const auto rows = buildVisibleRows(*menu);
        if (rows.empty() && !surface.text(*font, position, "Menu inactive")) {
            return false;
        }
        for (const auto& row : rows) {
            if (!surface.text(*font, position, row)) {
                return false;
            }
            position.y += font->info.height;
        }
    }
    return drawChildren(surface, offset, clip);
}

std::vector<std::string> FGChatMenu::buildVisibleRows(const ChatMenu& menu) const {
    std::vector<std::string> rows{};
    if (menu.getMode() == ChatMenu::Inactive) {
        return rows;
    }
    const auto* cur = menu.curMenu();
    if (cur == nullptr) {
        return rows;
    }
    rows.emplace_back(menu.curMenuQuery().empty() ? menu.getHeading() : menu.curMenuQuery());
    for (const auto* walk = cur; walk != nullptr; walk = walk->nextSibling.get()) {
        if (walk->key != '\0') {
            rows.push_back(std::format("{}: {}", walk->key, walk->heading));
        }
    }
    return rows;
}

std::function<void()> FGChatMenu::checkpoint() {
    auto base = Parent::checkpoint();
    auto name = menuName_;
    return [this,
            base = std::move(base),
            name = std::move(name),
            font = font,
            resolveMenu = resolveMenu] {
        base();
        menuName_ = name;
        this->font = font;
        this->resolveMenu = resolveMenu;
    };
}

bool FGChatMenu::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgObjectName, menuName_);
    return true;
}

bool FGChatMenu::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, kIdItgObjectName, v)) {
        return false;
    }
    setMenuName(std::get<std::string>(v));
    return true;
}

Persistent::Error
FGChatMenu::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 ver{};
    std::string name{};
    if (!sio.read(ver) || ver != kCtrlVersion || !sio.readString(name) || name.size() >= 64) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    menuName_ = std::move(name);
    return Persistent::Error::Ok;
}

Persistent::Error
FGChatMenu::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (menuName_.size() >= 64 || !sio.write(kCtrlVersion) || !sio.writeString(menuName_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
