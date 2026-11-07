#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/hud_panel.hpp>
#include <algorithm>

namespace {
using namespace opent1;
using namespace opent1::game;

std::shared_ptr<gfx::BitmapFont> historyFont(UInt8 color = 255) {
    auto font = std::make_shared<gfx::BitmapFont>();
    font->info.flags = gfx::FontFixed | gfx::FontBitmap | gfx::FontTransparent;
    font->info.width = font->info.height = 8;
    font->lookupOffset = 0;
    auto sheet = gfx::Bitmap::create(256, 8);
    CHECK(sheet);
    sheet->attributes |= gfx::BitmapTransparent;
    for (Int32 c = 0; c < 256; ++c) {
        font->lookup.push_back(Int16(c));
        font->characters.push_back({0, UInt8(c), 0, 1, 8, 0, {}});
        for (Int32 y = 0; y < 8; ++y) {
            // Distinguish letters as well as line lengths in the rendered result.
            sheet->row(y)[c] = c == ' ' ? std::byte{0} : std::byte(UInt8(color - c % 32));
        }
    }
    font->sheets.bitmaps.push_back(std::move(*sheet));
    CHECK(font->valid());
    return font;
}

struct ExpectedLine {
    std::string text;
    const gfx::BitmapFont* font{};
};

void checkLines(const FearGuiChatDisplay& display,
                std::initializer_list<ExpectedLine> lines,
                bool small = false,
                const gfx::BitmapFont* commandFont = nullptr) {
    auto actual = gfx::SoftwareSurface::create(640, 480);
    auto expected = gfx::SoftwareSurface::create(640, 480);
    CHECK(actual && expected);
    const Point2I offset{10, 10};
    const auto extent = display.fields().extent;
    const auto clip = actual->bitmap().bounds();
    CHECK(display.draw(*actual, offset, clip));
    CHECK(actual->clip() == clip);
    CHECK(drawHudPanel(*expected, {10, 10, 10 + extent.x, 10 + extent.y}));
    expected->setClip({14, 12, 10 + extent.x - 4, 10 + extent.y - 2});
    const auto height = [](const gfx::BitmapFont& font) {
        return Int32((Int64(font.info.height) * font.info.scale.y) >> 16);
    };
    const auto x = offset.x + (small ? 6 : 4);
    auto y = offset.y + extent.y - (small ? 0 : 4);
    if (commandFont) {
        y -= height(*commandFont);
        CHECK(expected->text(*commandFont, {x, y}, display.commandMessage().text));
    }
    for (const auto& line : lines) {
        y -= height(*line.font);
    }
    for (const auto& line : lines) {
        CHECK(expected->text(*line.font, {x, y}, line.text));
        y += height(*line.font);
    }
    CHECK(std::ranges::equal(actual->bitmap().pixels, expected->bitmap().pixels));
}

void wrapping() {
    FearGuiChatDisplay display;
    const auto font = historyFont();
    display.hudFont = font;
    display.messageFonts.fill(font);
    display.smallMessageFonts.fill(font);
    CHECK(display.layout({640, 480}));

    const auto first = std::string(39, 'A');
    (void)display.newMessage(first + " BBBBBBBBBB   ");
    checkLines(display, {{first, font.get()}, {" BBBBBBBBBB", font.get()}});
    // High resolution reserves 16 pixels, allowing 44 eight-pixel characters.
    display.clear();
    (void)display.newMessage(std::string(44, 'A'));
    checkLines(display, {{std::string(44, 'A'), font.get()}});
    (void)display.newMessage("~wsound.wav");
    CHECK(display.messages().size() == 1);

    CHECK(display.layout({320, 240}) && display.fields().extent.x == 240);
    display.clear();
    (void)display.newMessage(std::string(27, 'B'));
    checkLines(display, {{std::string(27, 'B'), font.get()}}, true);

    // Narrow previews exercise long words and UTF-8 boundaries even when a
    // complete character is wider than the available area. Font metrics remain
    // the engine's legacy byte metrics; wrapping must keep encoded units together.
    display.setLowRes(false);
    CHECK(display.resize({}, {56, 30}));
    display.clear();
    (void)display.newMessage("AAAA\xc3\xa9Z");
    checkLines(display, {{"AAAA", font.get()}, {"\xc3\xa9Z", font.get()}});
    display.clear();
    (void)display.newMessage("AA\xf0\x9f\x98\x80Z");
    checkLines(display, {{"AA", font.get()}, {"\xf0\x9f\x98\x80Z", font.get()}});
    display.clear();
    (void)display.newMessage("ABCDEFGHIJK");
    checkLines(display, {{"ABCDE", font.get()}, {"FGHIJ", font.get()}, {"K", font.get()}});
    CHECK(display.resize({}, {17, 30}));
    display.clear();
    (void)display.newMessage("\xc3\xa9Z");
    checkLines(display, {{"\xc3\xa9", font.get()}, {"Z", font.get()}});

    CHECK(display.resize({}, {370, 30}));
    display.clear();
    (void)display.newMessage("A\tB\x01\n\nC  ");
    checkLines(display, {{"A B", font.get()}, {"", font.get()}, {"C", font.get()}});
}

void layout() {
    FearGuiChatDisplay display;
    const auto font = historyFont();
    display.hudFont = font;
    display.messageFonts.fill(font);
    display.smallMessageFonts.fill(font);
    CHECK(display.layout({640, 480}));
    CHECK(display.fields().extent == Point2I(370, 30));
    CHECK(display.resize({}, {500, 30}));
    CHECK(display.layout({800, 600}) && display.fields().extent.x == 500);
    CHECK(display.layout({640, 480}) && display.fields().extent.x == 500);
    CHECK(display.layout({320, 240}) && display.fields().extent.x == 240);
    CHECK(display.resize({}, {290, 30}));
    CHECK(display.layout({400, 300}) && display.fields().extent.x == 290);
    CHECK(display.layout({200, 200}) && display.fields().extent.x == 199);
    CHECK(display.layout({320, 240}) && display.fields().extent.x == 240);
    CHECK(display.layout({640, 480}) && display.fields().extent.x == 370);
    font->info.scale = {98304, 98304};
    CHECK(display.layout({640, 480}) && display.fields().extent.y == 42);
    (void)display.newMessage(std::string(26, 'A') + " BBBBBBBBBB");
    checkLines(display, {{std::string(26, 'A'), font.get()}, {" BBBBBBBBBB", font.get()}});

    // Canvas wake must perform the resolution transition before HudCtrl updates
    // its low-resolution flag; otherwise the initial 370px width survives.
    gui::Control root;
    FearGuiChatDisplay liveDisplay;
    gui::Canvas canvas;
    liveDisplay.hudFont = font;
    CHECK(root.resize({}, {320, 240}) && root.addObject(liveDisplay));
    CHECK(canvas.content(&root));
    CHECK(liveDisplay.isLowRes() && liveDisplay.fields().extent == Point2I(240, 42));
    CHECK(liveDisplay.layout({320, 240}) && liveDisplay.fields().extent.x == 240);
    CHECK(root.resize({}, {640, 480}) && liveDisplay.layout({640, 480}));
    CHECK(!liveDisplay.isLowRes() && liveDisplay.fields().extent.x == 370);
    CHECK(canvas.content(nullptr));
}

void historyState() {
    FearGuiChatDisplay display;
    const auto font = historyFont();
    const auto team = historyFont(180);
    const auto command = historyFont(100);
    display.hudFont = font;
    display.messageFonts[3] = team;
    display.messageFonts[1] = command;
    display.setCommanderChat(true);
    CHECK(display.layout({640, 480}) && !display.pointInControl({1, 1}));
    (void)display.newMessage("first");
    (void)display.newMessage("second");
    (void)display.newMessage("third", FearGuiChatDisplay::MSG_TeamChat);
    (void)display.newMessage("fourth", FearGuiChatDisplay::MSG_IRC);
    checkLines(display, {{"second", font.get()}, {"third", team.get()}, {"fourth", font.get()}});
    display.page(-1);
    CHECK(display.pageUpCount() == 2);
    checkLines(display, {{"first", font.get()}, {"second", font.get()}});
    display.page(1);
    display.setCommandMessage("Defend");
    checkLines(display, {{"third", team.get()}, {"fourth", font.get()}}, false, command.get());
    display.clear();
    CHECK(display.messages().empty() && display.commandMessage().text.empty() &&
          display.pageUpCount() == 0);
}

void voiceMenu() {
    FearGuiChatDisplay display;
    const auto font = historyFont();
    display.hudFont = font;
    display.messageFonts.fill(font);
    display.setSize(3, 8);
    ChatMenu menu("Voice Chat:");
    CHECK(menu.addMenuString("hHello", std::array<std::string_view, 1>{"hello"}));
    menu.setMode(ChatMenu::OneShot);
    display.resolvePlayMenu = [&] { return &menu; };
    auto actual = gfx::SoftwareSurface::create(640, 480);
    auto expected = gfx::SoftwareSurface::create(640, 480);
    CHECK(actual && expected);
    for (const auto offset : {Point2I{10, 10}, Point2I{10, 440}}) {
        actual->clear(0);
        expected->clear(0);
        const auto extent = display.fields().extent;
        const RectI clip{offset.x, offset.y, offset.x + extent.x - 1,
                         offset.y + extent.y - 1};
        actual->setClip(clip);
        CHECK(display.draw(*actual, offset, clip));
        CHECK(actual->clip() == clip);
        CHECK(drawHudPanel(*expected, {offset.x, offset.y,
                                       offset.x + extent.x, offset.y + extent.y}));
        const auto y = offset.y == 10 ? offset.y + extent.y : offset.y - 24;
        CHECK(expected->text(*font, {offset.x, y}, "Voice Chat:"));
        CHECK(expected->text(*font, {offset.x, y + 8}, "h: Hello"));
        CHECK(std::ranges::equal(actual->bitmap().pixels, expected->bitmap().pixels));
    }
    menu.setMode(ChatMenu::Inactive);
    display.setCommanderChat(false);
    checkLines(display, {});
    menu.setMode(ChatMenu::OneShot);
    display.setCommanderChat(true);
    checkLines(display, {});
}
}

int opent1_test_chat_history_wrap() {
    return opent1::test::run(wrapping);
}

int opent1_test_chat_history_layout() {
    return opent1::test::run(layout);
}

int opent1_test_chat_history_state() {
    return opent1::test::run(historyState);
}

int opent1_test_voice_menu() {
    return opent1::test::run(voiceMenu);
}

OPENT1_REGISTER_TEST("game", "chat_history_wrap", opent1_test_chat_history_wrap);
OPENT1_REGISTER_TEST("game", "chat_history_layout", opent1_test_chat_history_layout);
OPENT1_REGISTER_TEST("game", "chat_history_state", opent1_test_chat_history_state);
OPENT1_REGISTER_TEST("game", "voice_menu", opent1_test_voice_menu);
