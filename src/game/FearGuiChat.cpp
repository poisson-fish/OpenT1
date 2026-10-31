#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/hud_panel.hpp>

#include <cmath>
#include <format>

namespace opent1::game {

FearGuiChat::FearGuiChat() {
    fields_.extent = {370, 30};
    visible(false);
}

void FearGuiChat::pushHistory(std::string_view line) {
    if (line.empty()) {
        return;
    }
    if (history_.size() >= HistoryLimit) {
        history_.erase(history_.begin());
    }
    history_.emplace_back(line);
    historyCursor_ = static_cast<Int32>(history_.size());
}

std::string_view FearGuiChat::historyPrev() {
    if (history_.empty()) {
        return {};
    }
    if (historyCursor_ > 0) {
        --historyCursor_;
    } else {
        historyCursor_ = 0;
    }
    (void)setText(history_[static_cast<std::size_t>(historyCursor_)]);
    return text;
}

std::string_view FearGuiChat::historyNext() {
    if (history_.empty()) {
        return {};
    }
    if (historyCursor_ + 1 < static_cast<Int32>(history_.size())) {
        ++historyCursor_;
        (void)setText(history_[static_cast<std::size_t>(historyCursor_)]);
    } else {
        historyCursor_ = static_cast<Int32>(history_.size());
        (void)setText("");
    }
    return text;
}

std::string FearGuiChat::submitLine(std::string_view rawInput) {
    pushHistory(rawInput);
    text.clear();
    lineCount_ = 2;
    if (rawInput.empty()) {
        return {};
    }
    if (rawInput.starts_with("/msg ") || rawInput.starts_with("/MSG ")) {
        const auto rest = rawInput.substr(5);
        const auto sp = rest.find(' ');
        if (sp != std::string_view::npos) {
            const auto target = rest.substr(0, sp);
            const auto body = rest.substr(sp + 1);
            return std::format("ircSend(\"{}\", \"{}\");", body, target);
        }
    }
    return std::format("say({}, \"{}\");", channel_, rawInput);
}

void FearGuiChat::activate(Int32 chatChannel) {
    channel_ = chatChannel;
    visible(true);
    active = true;
    if (canvas()) {
        (void)canvas()->makeFirstResponder(this);
    }
}

void FearGuiChat::deactivate() {
    visible(false);
    if (canvas()) {
        (void)canvas()->makeFirstResponder(nullptr);
    }
}

bool FearGuiChat::event(const gui::Event& e) {
    if (e.type == gui::EventType::KeyDown) {
        if (e.key == gui::Key::Return) {
            const auto line = text;
            pushHistory(line);
            text.clear();
            deactivate();
            if (!line.empty() && sendLine) {
                sendLine(channel_, line);
            }
            return true;
        }
        if (e.key == gui::Key::Escape) {
            text.clear();
            deactivate();
            return true;
        }
        if (e.key == gui::Key::Up) {
            (void)historyPrev();
            return true;
        }
        if (e.key == gui::Key::Down) {
            (void)historyNext();
            return true;
        }
    }
    return Parent::event(e);
}

void FearGuiChat::wake() {
    Parent::wake();
    if (parent()) {
        layout(parent()->fields().extent);
    }
}

bool FearGuiChat::layout(Point2I extent) {
    const bool small = extent.x < 512 || extent.y < 384;
    const auto& selectedFont = small && smallFont ? smallFont : font;
    const auto height = selectedFont ? selectedFont->info.height : 12;
    const auto width = std::min(small ? 184 : 370, std::max(1, extent.x - 12));
    const auto textHeight = height * std::max(1, lineCount_);
    return resize({(extent.x - width) / 2, std::max(0, extent.y - 40 - textHeight)},
                  {width, textHeight + 10});
}

bool FearGuiChat::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const bool small = surface.bitmap().width < 512 || surface.bitmap().height < 384;
    const auto selectedFont =
        small ? (channel_ > 0 ? smallTeamFont : smallFont) : (channel_ > 0 ? teamFont : font);
    const auto* f = selectedFont ? selectedFont.get() : font.get();
    if (!drawHudPanel(
            surface,
            {offset.x, offset.y, offset.x + fields_.extent.x, offset.y + fields_.extent.y})) {
        return false;
    }
    if (!f) {
        return drawChildren(surface, offset, clip);
    }
    const auto saved = surface.clip();

    struct Restore {
        gfx::SoftwareSurface& surface;
        RectI clip;

        ~Restore() {
            surface.setClip(clip);
        }
    } restore{surface, saved};

    const auto inner = gfx::intersect(clip,
                                      {offset.x + 12,
                                       offset.y + 2,
                                       offset.x + fields_.extent.x - 12,
                                       offset.y + fields_.extent.y - 2});
    if (!inner) {
        return true;
    }
    surface.setClip(*inner);
    const std::string_view prompt = channel_ > 0 ? "SAY TEAM: " : "SAY: ";
    const auto promptWidth = Int32(f->textWidth(prompt));
    const auto width = std::max(1, fields_.extent.x - 24 - promptWidth);
    if (!surface.text(*f, offset + Point2I{12, 0}, prompt)) {
        return false;
    }
    std::size_t start = 0;
    Int32 row = 0;
    do {
        auto end = start;
        while (end < text.size() && (end == start || f->textWidth(std::string_view(text).substr(
                                                         start, end - start + 1)) <= width)) {
            ++end;
        }
        const auto at = offset + Point2I{12 + promptWidth, row * f->info.height};
        const auto line = std::string_view(text).substr(start, end - start);
        const auto [a, b] = selection();
        if (a < end && b > start) {
            const auto left = Int32(f->textWidth(line.substr(0, std::max(a, start) - start)));
            const auto right = Int32(f->textWidth(line.substr(0, std::min(b, end) - start)));
            surface.fill({at.x + left, at.y + 2, at.x + right, at.y + f->info.height}, GREEN_78);
        }
        if (!surface.text(*f, at, line)) {
            return false;
        }
        if (cursorOn && canvas() && canvas()->focus() == this && cursor >= start &&
            (cursor < end || end == text.size())) {
            const auto x =
                at.x + Int32(f->textWidth(line.substr(0, std::min(cursor, end) - start)));
            surface.line({x, at.y + 2}, {x, at.y + f->info.height - 1}, GREEN);
        }
        start = end;
        ++row;
    } while (start < text.size() && row < std::max(1, lineCount_));
    return true;
}

std::function<void()> FearGuiChat::checkpoint() {
    auto base = Parent::checkpoint();
    const auto fp = fracPos_;
    const auto lc = lineCount_;
    const auto ch = channel_;
    return [this, base = std::move(base), fp, lc, ch] {
        base();
        fracPos_ = fp;
        lineCount_ = lc;
        channel_ = ch;
    };
}

Persistent::Error
FearGuiChat::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Point2F fp{};
    Point2I ext{};
    if (!sio.read(fp.x) || !sio.read(fp.y) || !sio.read(ext.x) || !sio.read(ext.y)) {
        return Persistent::Error::ReadError;
    }
    if (!std::isfinite(fp.x) || !std::isfinite(fp.y) || ext.x < 0 || ext.y < 0 || ext.x > 32768 ||
        ext.y > 32768) {
        return Persistent::Error::ReadError;
    }
    visible(false);
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    fracPos_ = fp;
    fields_.extent = ext;
    return Persistent::Error::Ok;
}

Persistent::Error
FearGuiChat::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (!std::isfinite(fracPos_.x) || !std::isfinite(fracPos_.y)) {
        return Persistent::Error::WriteError;
    }
    if (!sio.write(fracPos_.x) || !sio.write(fracPos_.y) || !sio.write(fields_.extent.x) ||
        !sio.write(fields_.extent.y)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
