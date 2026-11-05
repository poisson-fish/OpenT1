#include <opent1/game/chat_inventory_huds.hpp>
#include <ranges>
#include <opent1/game/hud_panel.hpp>

#include <algorithm>

namespace opent1::game {

namespace {
inline constexpr Int32 kIdItgChatDisplayLines = 200110;
inline constexpr Int32 kIdStrCommanderVersion = 132105;
inline constexpr Int32 kDisplayBorderY = 6;

Int32 fontHeight(const gfx::BitmapFont& font) {
    return std::max(1, Int32((Int64(font.info.height) * font.info.scale.y) >> 16));
}

std::vector<std::string>
wrapMessage(std::string_view text, const gfx::BitmapFont& font, Int32 width) {
    std::string cleaned;
    for (const char ch : text) {
        if (static_cast<unsigned char>(ch) >= 32 || ch == '\n') {
            cleaned.push_back(ch);
        } else if (ch == '\t') {
            cleaned.push_back(' ');
        }
    }

    std::vector<std::string> lines;
    std::size_t paragraph = 0;
    do {
        const auto newline = cleaned.find('\n', paragraph);
        const auto stop = newline == std::string::npos ? cleaned.size() : newline;
        auto end = stop;
        while (end > paragraph && cleaned[end - 1] == ' ') {
            --end;
        }
        const auto part = std::string_view(cleaned).substr(paragraph, end - paragraph);
        std::size_t start = 0;
        do {
            auto next = start;
            auto space = start;
            while (next < part.size()) {
                auto boundary = next + 1;
                while (boundary < part.size() && (UInt8(part[boundary]) & 0xc0) == 0x80) {
                    ++boundary;
                }
                // An over-wide glyph still consumes one complete character.
                if (next > start && font.textWidth(part.substr(start, boundary - start)) > width) {
                    break;
                }
                if (part[next] == ' ' && next > start) {
                    space = next;
                }
                next = boundary;
            }
            if (next < part.size() && part[next] != ' ' && space > start) {
                next = space;
            }
            auto line = part.substr(start, next - start);
            while (!line.empty() && line.back() == ' ') {
                line.remove_suffix(1);
            }
            lines.emplace_back(line);
            // Like vanilla, the space at a wrap belongs to the next line.
            start = next;
        } while (start < part.size());
        paragraph = stop + 1;
    } while (paragraph <= cleaned.size());
    return lines;
}
} // namespace

FearGuiChatDisplay::FearGuiChatDisplay() {
    fields_.extent = {370, 42};
    lineCount_ = 3;
}

bool FearGuiChatDisplay::draw(gfx::SoftwareSurface& surface, Point2I offset, RectI clip) const {
    if (lineCount_ <= 0) {
        return true;
    }
    if (!drawHudPanel(
            surface,
            {offset.x, offset.y, offset.x + fields_.extent.x, offset.y + fields_.extent.y})) {
        return false;
    }
    if (!hudFont) {
        return true;
    }

    struct Line {
        std::string text;
        const gfx::BitmapFont* font;
    };

    std::vector<Line> lines;
    const auto& fonts = lowResMode_ ? smallMessageFonts : messageFonts;
    const auto fontFor = [&](Int32 type) {
        const auto index = type == MSG_TeamChat                      ? 3
                           : type == MSG_Chat || type == MSG_IRC     ? 2
                           : type == MSG_Command || type == MSG_Game ? 1
                                                                     : 0;
        return fonts[index] ? fonts[index].get() : hudFont.get();
    };
    const auto borderX = lowResMode_ ? 12 : 8;
    const auto width = std::max(1, fields_.extent.x - 2 * borderX);
    for (const auto& entry : messages_) {
        const auto* font = fontFor(entry.type);
        for (auto& line : wrapMessage(entry.text, *font, width)) {
            lines.push_back({std::move(line), font});
        }
    }
    const auto end = std::max(0, static_cast<Int32>(lines.size()) - pgUpCount_);
    const bool command = !commandSlot_.text.empty();
    const auto start = std::max(0, end - std::max(0, lineCount_ - Int32(command)));
    const auto saved = surface.clip();

    struct Restore {
        gfx::SoftwareSurface& surface;
        RectI clip;

        ~Restore() {
            surface.setClip(clip);
        }
    } restore{surface, saved};

    const auto inner = gfx::intersect(clip,
                                      {offset.x + 4,
                                       offset.y + 2,
                                       offset.x + fields_.extent.x - 4,
                                       offset.y + fields_.extent.y - 2});
    if (!inner) {
        return true;
    }
    surface.setClip(*inner);
    // Native chat fills from the bottom, keeping the newest line immediately
    // above the command slot even while the history contains only a few lines.
    auto y = offset.y + fields_.extent.y - (lowResMode_ ? 0 : 4);
    if (command) {
        y -= fontHeight(*fontFor(MSG_Command));
        if (!surface.text(*fontFor(MSG_Command), {offset.x + borderX / 2, y}, commandSlot_.text)) {
            return false;
        }
    }
    for (auto i : std::views::iota(start) | std::views::take_while([&](auto i) {
                      return i < end;
                  })) {
        y -= fontHeight(*lines[i].font);
    }
    for (auto i : std::views::iota(start) | std::views::take_while([&](auto i) {
                      return i < end;
                  })) {
        if (!surface.text(*lines[i].font, {offset.x + borderX / 2, y}, lines[i].text)) {
            return false;
        }
        y += fontHeight(*lines[i].font);
    }
    surface.setClip(saved);
    return drawChildren(surface, offset, clip);
}

bool FearGuiChatDisplay::layout(Point2I extent) {
    const bool small = extent.x < 512 || extent.y < 384;
    const auto& selected = small ? smallMessageFonts[2] : messageFonts[2];
    const auto& font = selected ? selected : hudFont;
    const auto minimum = small ? 240 : 370;
    const auto width = small != lowResMode_ ? minimum : std::max(minimum, fields_.extent.x);
    fields_.extent = {std::min(width, std::max(1, extent.x - 1)),
                      lineCount_ * (font ? fontHeight(*font) : 12) + kDisplayBorderY};
    return Parent::parentResized(extent, extent);
}

void FearGuiChatDisplay::wake() {
    if (parent()) {
        (void)layout(parent()->fields().extent);
    }
}

void FearGuiChatDisplay::clear() noexcept {
    messages_.clear();
    commandSlot_ = {};
    pgUpCount_ = 0;
}

FearGuiChatDisplay::MessageEntry FearGuiChatDisplay::newMessage(std::string_view rawMsg,
                                                                Int32 msgType) {
    MessageEntry entry{};
    entry.type = msgType;
    const auto tildePos = rawMsg.find('~');
    if (tildePos != std::string_view::npos) {
        entry.text = std::string(rawMsg.substr(0, tildePos));
        entry.soundName = std::string(rawMsg.substr(tildePos + 1));
    } else {
        entry.text = std::string(rawMsg);
    }
    // Native sound-only messages do not consume a line in chat history.
    if (msgType != MSG_Command && entry.text.empty()) {
        return entry;
    }
    if (msgType == MSG_Command) {
        commandSlot_ = entry;
    } else {
        if (messages_.size() >= MessageHistoryLimit) {
            messages_.pop_front();
        }
        messages_.push_back(entry);
    }
    return entry;
}

void FearGuiChatDisplay::setCommandMessage(std::string_view rawMsg) {
    (void)newMessage(rawMsg, MSG_Command);
}

void FearGuiChatDisplay::setSize(Int32 value, Int32 fontHeight) {
    if (value < 0) {
        switch (lineCount_) {
            case 3:
                lineCount_ = 5;
                break;
            case 5:
                lineCount_ = 20;
                break;
            case 20:
            default:
                lineCount_ = 3;
                break;
        }
    } else {
        lineCount_ = std::max<Int32>(0, value);
    }
    pgUpCount_ = 0;
    const Int32 fh = std::max<Int32>(1, fontHeight);
    resize(fields_.position, {fields_.extent.x, lineCount_ * fh + kDisplayBorderY});
    visible(true);
}

void FearGuiChatDisplay::page(Int32 delta) {
    if (delta < 0) {
        pgUpCount_ = std::min<Int32>(pgUpCount_ + std::max<Int32>(1, lineCount_ - 1),
                                     static_cast<Int32>(messages_.size()));
    } else if (delta > 0) {
        pgUpCount_ = std::max<Int32>(0, pgUpCount_ - std::max<Int32>(1, lineCount_ - 1));
    }
}

bool FearGuiChatDisplay::pointInControl(Point2I p) const {
    if (commanderChat_) {
        return false;
    }
    return Parent::pointInControl(p);
}

bool FearGuiChatDisplay::onSimActionEvent(const sim::SimActionEvent& event) {
    if (event.action == 220201) { // IDACTION_CHAT_DISP_SIZE
        setSize(-1);
        return true;
    }
    if (event.action == 220202) { // IDACTION_CHAT_DISP_PAGE
        page(event.fValue < 0.0f ? -1 : 1);
        return true;
    }
    return Parent::onSimActionEvent(event);
}

bool FearGuiChatDisplay::processEvent(const sim::SimEvent& event) {
    if (event.type == sim::ActionEventType || event.type == SimActionEventType) {
        if (const auto* act = dynamic_cast<const sim::SimActionEvent*>(&event)) {
            if (onSimActionEvent(*act)) {
                return true;
            }
        }
    }
    return Parent::processEvent(event);
}

std::function<void()> FearGuiChatDisplay::checkpoint() {
    auto base = Parent::checkpoint();
    const auto lc = lineCount_;
    const auto pg = pgUpCount_;
    const auto cmd = commanderChat_;
    return [this, base = std::move(base), lc, pg, cmd] {
        base();
        lineCount_ = lc;
        pgUpCount_ = pg;
        commanderChat_ = cmd;
    };
}

bool FearGuiChatDisplay::inspectWrite(sim::Inspect& inspector) const {
    inspector.write(kIdItgChatDisplayLines, static_cast<Flt32>(lineCount_));
    inspector.write(kIdStrCommanderVersion, commanderChat_);
    return true;
}

bool FearGuiChatDisplay::inspectRead(sim::Inspect& inspector) {
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::RealF, kIdItgChatDisplayLines, v)) {
        return false;
    }
    const Int32 lc = std::max<Int32>(0, static_cast<Int32>(std::get<Flt32>(v)));
    if (!inspector.read(sim::Inspect::Type::Bool, kIdStrCommanderVersion, v)) {
        return false;
    }
    lineCount_ = lc;
    commanderChat_ = std::get<bool>(v);
    return true;
}

Persistent::Error
FearGuiChatDisplay::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Point2I ext{};
    Int32 lc{};
    Int32 vis{};
    Int32 cmd{};
    if (!sio.read(ext.x) || !sio.read(ext.y) || !sio.read(lc) || !sio.read(vis) || !sio.read(cmd)) {
        return Persistent::Error::ReadError;
    }
    if (ext.x < 0 || ext.y < 0 || ext.x > 32768 || ext.y > 32768 || lc < 0 || lc > 1024) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    fields_.extent = ext;
    lineCount_ = lc;
    visible(vis != 0);
    commanderChat_ = (cmd != 0);
    return Persistent::Error::Ok;
}

Persistent::Error FearGuiChatDisplay::write(StreamIO& sio,
                                            Int32 version,
                                            Int32 user,
                                            Persistent::StoreContext& ctx) const {
    if (lineCount_ < 0 || lineCount_ > 1024) {
        return Persistent::Error::WriteError;
    }
    const Int32 vis = visible() ? 1 : 0;
    const Int32 cmd = commanderChat_ ? 1 : 0;
    if (!sio.write(fields_.extent.x) || !sio.write(fields_.extent.y) || !sio.write(lineCount_) ||
        !sio.write(vis) || !sio.write(cmd)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
