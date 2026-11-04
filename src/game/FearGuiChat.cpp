#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/hud_panel.hpp>

#include <cmath>
#include <format>

namespace opent1::game {
namespace {
std::string escapeChatArgument(std::string_view value) {
    std::string escaped;
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string_view chatPrompt(Int32 channel) {
    return channel > 0 ? "SAY TEAM: " : "SAY: ";
}

Int32 fontHeight(const gfx::BitmapFont& font) {
    return std::max(1, Int32((Int64(font.info.height) * font.info.scale.y) >> 16));
}

// FearGuiChat::formatMessage reserves the prompt on the first line, then
// wraps at spaces. A single long word still makes progress.
std::vector<std::size_t>
lineEnds(std::string_view text, const gfx::BitmapFont& font, Int32 width, Int32 channel) {
    std::vector<std::size_t> ends;
    std::size_t start = 0;
    auto available = std::max<Int64>(1, width - font.textWidth(chatPrompt(channel)));
    do {
        auto end = start;
        auto space = start;
        while (end < text.size()) {
            auto next = end + 1;
            while (next < text.size() && (UInt8(text[next]) & 0xc0) == 0x80) {
                ++next;
            }
            if (end > start && font.textWidth(text.substr(start, next - start)) > available) {
                break;
            }
            if (text[end] == ' ' && end > start) {
                space = end;
            }
            end = next;
        }
        if (end < text.size() && text[end] != ' ' && space > start) {
            end = space;
        }
        ends.push_back(end);
        start = end;
        available = std::max(1, width);
    } while (start < text.size());
    return ends;
}
}

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
    historyCursor_ = -1;
}

std::string_view FearGuiChat::historyPrev() {
    if (history_.empty()) {
        return {};
    }
    historyCursor_ =
        historyCursor_ < 0 ? static_cast<Int32>(history_.size()) - 1 : historyCursor_ - 1;
    (void)setText(historyCursor_ < 0 ? "" : history_[static_cast<std::size_t>(historyCursor_)]);
    cursor = anchor = text.size();
    return text;
}

std::string_view FearGuiChat::historyNext() {
    if (history_.empty()) {
        return {};
    }
    if (historyCursor_ < 0) {
        historyCursor_ = static_cast<Int32>(history_.size()) - 1;
    } else {
        historyCursor_ = (historyCursor_ + 1) % static_cast<Int32>(history_.size());
        if (historyCursor_ == static_cast<Int32>(history_.size()) - 1) {
            historyCursor_ = -1;
        }
    }
    (void)setText(historyCursor_ < 0 ? "" : history_[static_cast<std::size_t>(historyCursor_)]);
    cursor = anchor = text.size();
    return text;
}

std::string FearGuiChat::submitLine(std::string_view rawInput) {
    const std::string line(rawInput);
    pushHistory(line);
    (void)setText("");
    lineCount_ = 2;
    reflow();
    if (line.empty()) {
        return {};
    }
    if (line.starts_with("/msg ") || line.starts_with("/MSG ")) {
        const auto rest = std::string_view(line).substr(5);
        const auto sp = rest.find(' ');
        if (sp != std::string_view::npos) {
            const auto target = rest.substr(0, sp);
            const auto body = rest.substr(sp + 1);
            return std::format(
                "ircSend(\"{}\", \"{}\");", escapeChatArgument(body), escapeChatArgument(target));
        }
    }
    return std::format("say({}, \"{}\");", channel_, escapeChatArgument(line));
}

void FearGuiChat::activate(Int32 chatChannel) {
    channel_ = std::clamp(chatChannel, 0, 31);
    firstKeyPress_ = true;
    // Keep an escaped draft, but reset inherited selection/undo state.
    (void)setText(text);
    cursor = anchor = text.size();
    visible(true);
    active = true;
    reflow();
    if (canvas()) {
        (void)canvas()->makeFirstResponder(this, true);
    }
}

void FearGuiChat::deactivate() {
    historyCursor_ = -1;
    visible(false);
    if (canvas() && canvas()->focus() == this) {
        (void)canvas()->makeFirstResponder(nullptr);
    }
}

bool FearGuiChat::event(const gui::Event& e) {
    if (e.type == gui::EventType::KeyDown) {
        if (firstKeyPress_ && (e.key == gui::Key::Delete || e.key == gui::Key::Backspace)) {
            firstKeyPress_ = false;
            (void)setText("");
            reflow();
            return true;
        }
        firstKeyPress_ = false;
        if (e.key == gui::Key::Return) {
            const auto line = text;
            pushHistory(line);
            (void)setText("");
            lineCount_ = 2;
            reflow();
            deactivate();
            if (!line.empty() && sendLine) {
                sendLine(channel_, line);
            }
            return true;
        }
        if (e.key == gui::Key::Escape) {
            deactivate();
            return true;
        }
        if (e.key == gui::Key::Up) {
            (void)historyPrev();
            reflow();
            return true;
        }
        if (e.key == gui::Key::Down) {
            (void)historyNext();
            reflow();
            return true;
        }
    }
    if (e.type == gui::EventType::Text) {
        firstKeyPress_ = false;
    }
    const auto handled = Parent::event(e);
    if (handled && (e.type == gui::EventType::Text || e.type == gui::EventType::KeyDown)) {
        reflow();
    }
    return handled;
}

void FearGuiChat::wake() {
    Parent::wake();
    if (parent()) {
        layout(parent()->fields().extent);
    }
}

const gfx::BitmapFont* FearGuiChat::messageFont(bool small) const {
    const auto& selected =
        small ? (channel_ > 0 ? smallTeamFont : smallFont) : (channel_ > 0 ? teamFont : font);
    return selected ? selected.get() : font.get();
}

void FearGuiChat::reflow() {
    if (parent()) {
        (void)layout(parent()->fields().extent);
    }
}

bool FearGuiChat::layout(Point2I extent) {
    const bool small = extent.x < 512 || extent.y < 384;
    const auto* f = messageFont(small);
    const auto height = f ? fontHeight(*f) : 12;
    const auto width =
        std::min(std::max(fields_.extent.x, small ? 200 : 370), std::max(1, extent.x - 1));
    if (f) {
        const auto lines = lineEnds(text, *f, width - 2 * DisplayBorderX, channel_);
        lineCount_ = std::max(lineCount_, Int32(lines.size()));
    }
    lineCount_ = std::clamp(lineCount_, 1, std::max(1, (extent.y - DisplayBorderY - 1) / height));
    const auto textHeight = height * lineCount_;
    return resize({(extent.x - width) / 2, std::max(0, extent.y - 40 - textHeight)},
                  {width, textHeight + DisplayBorderY});
}

bool FearGuiChat::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    const bool small = surface.bitmap().width < 512 || surface.bitmap().height < 384;
    const auto* f = messageFont(small);
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
                                       offset.y + 4,
                                       offset.x + fields_.extent.x - 12,
                                       offset.y + fields_.extent.y - 4});
    if (!inner) {
        return true;
    }
    surface.setClip(*inner);
    const auto prompt = chatPrompt(channel_);
    const auto promptWidth = Int32(f->textWidth(prompt));
    const auto height = fontHeight(*f);
    const auto textY = offset.y - (small ? 0 : 2);
    if (!surface.text(*f, {offset.x + DisplayBorderX, textY}, prompt)) {
        return false;
    }
    std::size_t start = 0;
    Int32 row = 0;
    bool cursorDrawn = false;
    for (const auto end : lineEnds(text, *f, fields_.extent.x - 2 * DisplayBorderX, channel_)) {
        const Point2I at{offset.x + DisplayBorderX + (row == 0 ? promptWidth : 0),
                         textY + row * height};
        const auto line = std::string_view(text).substr(start, end - start);
        const auto [a, b] = selection();
        if (a < end && b > start) {
            const auto left = Int32(f->textWidth(line.substr(0, std::max(a, start) - start)));
            const auto right = Int32(f->textWidth(line.substr(0, std::min(b, end) - start)));
            surface.fill({at.x + left, at.y + 2, at.x + right, at.y + height}, GREEN_78);
        }
        if (!surface.text(*f, at, line)) {
            return false;
        }
        if (!cursorDrawn && cursorOn && canvas() && canvas()->focus() == this && cursor >= start &&
            cursor <= end) {
            const auto x =
                at.x + Int32(f->textWidth(line.substr(0, std::min(cursor, end) - start)));
            const auto y = at.y + (small ? 0 : 3);
            surface.line({x, y}, {x, y + height}, GREEN);
            cursorDrawn = true;
        }
        start = end;
        ++row;
        if (row >= std::max(1, lineCount_)) {
            break;
        }
    }
    return true;
}

std::function<void()> FearGuiChat::checkpoint() {
    auto base = Parent::checkpoint();
    const auto fp = fracPos_;
    const auto lc = lineCount_;
    const auto ch = channel_;
    const auto history = history_;
    const auto historyCursor = historyCursor_;
    const auto firstKeyPress = firstKeyPress_;
    return [this, base = std::move(base), fp, lc, ch, history, historyCursor, firstKeyPress] {
        base();
        fracPos_ = fp;
        lineCount_ = lc;
        channel_ = ch;
        history_ = history;
        historyCursor_ = historyCursor;
        firstKeyPress_ = firstKeyPress;
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
