#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/core/mem_stream.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/shell_controls.hpp>
#include <opent1/game/sound.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

using namespace opent1;
using namespace opent1::game;

namespace {
std::shared_ptr<gfx::BitmapFont> chatTestFont();

void scriptHudLifecycle() {
    FearGameApp app;
    CHECK(app.init() && app.setupLiveDemoSession());
    auto& console = app.console();
    auto runtime = app.guiRuntime();
    CHECK(console.evaluate(R"(
        $Frame = newObject("HUD::Test", FearGui::FearGuiMenu, 20, 30, 170, 200);
        $Text = newObject("HUD::Test::0", FearGuiFormattedText, 4, 0, 996, 201);
        addToSet($Frame, $Text);
        $Hidden = Control::SetVisible("HUD::Test", false);
    )").ok());
    CHECK(console.getVariable("Hidden") == "True");
    auto* frame = runtime->findControl("HUD::Test");
    auto* text = dynamic_cast<FearGuiFormattedText*>(runtime->findControl("HUD::Test::0"));
    CHECK(frame && text && text->parent() == frame);
    CHECK(frame->fields().position == Point2I(20, 30));
    CHECK(frame->fields().extent == Point2I(170, 200));
    CHECK(!frame->visible());
    for (auto& slot : text->fonts) {
        slot.resource = chatTestFont();
    }
    CHECK(console.evaluate(R"(
        $Updated = Control::SetValue(Object::GetName($Text), "<f0>HP: 50 / 100\nMP: 20");
        Control::SetVisible("HUD::Test", true);
        function playGui::onOpen() { $Attached = addToSet(playGui, $Frame); }
        function playGui::onClose() { $Detached = removeFromSet(playGui, $Frame); }
    )").ok());
    CHECK(console.getVariable("Updated") == "True" && text->fields().extent.x == 170);
    for (Int32 iteration = 0; iteration < 2; ++iteration) {
        auto root = std::make_unique<gui::Control>();
        root->assignName("playGui");
        CHECK(root->resize({}, {640, 480}));
        CHECK(runtime->show(gui::Graph::create(std::move(root))));
        CHECK(console.getVariable("Attached") == "True" && frame->parent() == runtime->canvas.top());
        CHECK(console.evaluate("return isObject(playGui);").value == "True");
        CHECK(runtime->show(gui::Graph::create(std::make_unique<gui::Control>())));
        CHECK(console.getVariable("Detached") == "True" && !frame->parent());
        CHECK(runtime->findControl("HUD::Test::0") == text);
    }
    CHECK(console.evaluate("return deleteObject($Frame);").value == "True");
    CHECK(!runtime->findControl("HUD::Test") && !runtime->findControl("HUD::Test::0"));
}

void recipientMessages() {
    FearGameApp app;
    CHECK(app.init());
    auto& plugin = app.fearPlugin();
    CHECK(plugin.createServerWorld({}, true));
    CHECK(app.console().execute("localConnect").value == "True");
    auto* players = plugin.serverGlobals().playerManager;
    const auto bot = players->aiAdded("Caster", "male2", "base", true);
    const auto other = players->clientAdded("Remote", "male1", "base", true);
    std::vector<std::string> messages;
    plugin.clientGlobals().playerManager->onChatMessage = [&](Int32, std::string_view message) {
        messages.emplace_back(message);
    };
    for (const auto recipient : {bot, other}) {
        const std::array<std::string, 3> args{std::to_string(recipient), "0", "Casting fireball."};
        CHECK(app.console().execute("Client::sendMessage", args).value == "True");
        CHECK(messages.empty());
    }
    const std::array<std::string, 3> args{"2049", "0", "Caster is casting on you."};
    CHECK(app.console().execute("Client::sendMessage", args).value == "True");
    CHECK(messages == std::vector<std::string>{"Caster is casting on you."});
    const std::array<std::string, 3> ambient{"2049", "0", "~wambient.wav"};
    CHECK(app.console().execute("Client::sendMessage", ambient).value == "True");
    CHECK(messages.size() == 1);

    FearGuiChatDisplay display;
    (void)display.newMessage("Earlier message", FearGuiChatDisplay::MSG_Game);
    (void)display.newMessage("~wambient.wav", FearGuiChatDisplay::MSG_Game);
    CHECK(display.messages().size() == 1);
    (void)display.newMessage("", FearGuiChatDisplay::MSG_Game);
    CHECK(display.messages().size() == 1);
}

void chatReentry() {
    gui::Control root;
    FearGuiChat chat;
    gui::Canvas canvas;
    CHECK(root.addObject(chat));
    CHECK(root.resize({}, {640, 480}));
    CHECK(canvas.content(&root));
    std::vector<std::string> sent;
    chat.sendLine = [&](Int32 channel, std::string_view text) {
        CHECK(channel == 0);
        sent.emplace_back(text);
    };
    gui::Event text;
    text.type = gui::EventType::Text;
    text.text = "first message";
    gui::Event key;
    key.type = gui::EventType::KeyDown;
    key.key = gui::Key::Return;
    chat.activate();
    CHECK(canvas.dispatch(text));
    CHECK(canvas.dispatch(key));
    CHECK(!chat.visible() && chat.text.empty() && !canvas.focus());
    chat.activate();
    text.text = "second";
    CHECK(canvas.dispatch(text));
    CHECK(chat.text == "second");
    key.key = gui::Key::Escape;
    CHECK(canvas.dispatch(key));
    chat.activate();
    CHECK(chat.text == "second"); // Escape preserves the native draft.
    key.key = gui::Key::Backspace;
    CHECK(canvas.dispatch(key)); // The first delete/backspace clears the draft.
    CHECK(chat.text.empty());
    text.text = "third";
    CHECK(canvas.dispatch(text));
    CHECK(chat.text == "third");
    key.key = gui::Key::Up;
    CHECK(canvas.dispatch(key));
    CHECK(chat.text == "first message");
    text.text = "!";
    CHECK(canvas.dispatch(text));
    CHECK(chat.text == "first message!");
    key.key = gui::Key::Return;
    CHECK(canvas.dispatch(key));
    CHECK(sent == std::vector<std::string>({"first message", "first message!"}));
    CHECK(chat.setText("alias input"));
    CHECK(chat.submitLine(chat.text) == "say(0, \"alias input\");");
    CHECK(chat.text.empty() && chat.cursor == 0 && chat.anchor == 0);
    CHECK(chat.submitLine(R"(quote " and \)") == R"(say(0, "quote \" and \\");)");
    CHECK(canvas.content(nullptr));
    root.removeObject(chat);
}

std::shared_ptr<gfx::BitmapFont> chatTestFont() {
    auto font = std::make_shared<gfx::BitmapFont>();
    font->info.flags = gfx::FontMono | gfx::FontBitmap | gfx::FontTransparent;
    font->info.height = 16;
    font->info.width = 8;
    font->info.baseline = 15;
    font->lookupOffset = 32;
    font->lookup.assign(96, 0);
    font->characters.push_back({0, 0, 0, 8, 16, 15, {}});
    auto sheet = gfx::Bitmap::create(8, 16);
    CHECK(sheet);
    sheet->attributes |= gfx::BitmapTransparent;
    font->sheets.bitmaps.push_back(std::move(*sheet));
    CHECK(font->valid());
    return font;
}

void chatEntryParity() {
    struct RejectingChat : FearGuiChat {
        bool reject{};

        bool event(const gui::Event& event) override {
            return FearGuiChat::event(event) && !reject;
        }
    };

    gui::Control root, scores;
    RejectingChat chat;
    gui::Canvas canvas;
    auto font = chatTestFont();
    chat.font = chat.teamFont = chat.smallFont = chat.smallTeamFont = font;
    CHECK(root.resize({}, {640, 480}) && root.addObject(chat) && canvas.content(&root));
    chat.activate();
    CHECK(chat.fields().extent == Point2I(370, 42));
    // A score dialog has no chat of its own. Explicit keyboard focus may reach
    // the covered content, while ordinary focus and mouse capture remain modal.
    CHECK(canvas.pushDialog(scores));
    CHECK(!canvas.makeFirstResponder(&chat) && !canvas.lockMouse(&chat));
    chat.activate();
    CHECK(canvas.focus() == &chat && canvas.content() == &root && canvas.top() == &scores);
    canvas.rebuildTabs();
    CHECK(canvas.focus() == &chat);

    gui::Event text;
    text.type = gui::EventType::Text;
    text.text = std::string(30, 'A') + " " + std::string(10, 'B');
    CHECK(canvas.dispatch(text));
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface);
    const auto origin = chat.fields().position;
    const auto caretX = origin.x + FearGuiChat::DisplayBorderX + 11 * 8;
    CHECK(chat.draw(*surface, origin, surface->bitmap().bounds()));
    // The word after the first wrap uses the whole inset width; the prompt is
    // reserved once. The two occupied rows are centered with their caret.
    CHECK(surface->bitmap().row(origin.y + 37)[caretX] == std::byte{GREEN});
    CHECK(surface->bitmap().row(origin.y + 38)[caretX] != std::byte{GREEN});
    CHECK(canvas.focus() == &chat);
    chat.reject = true;
    gui::Event key;
    key.type = gui::EventType::KeyDown;
    key.key = gui::Key::Return;
    CHECK(!canvas.dispatch(key));
    CHECK(chat.visible() && chat.text == text.text && chat.history().empty() &&
          canvas.focus() == &chat);
    chat.reject = false;
    text.text = std::string(180, 'A');
    CHECK(canvas.dispatch(text));
    CHECK(chat.lineCount() > 2 && chat.fields().extent.y == chat.lineCount() * 16 + 10);
    CHECK(canvas.dispatch(key));
    CHECK(!chat.visible() && chat.lineCount() == 2 && !canvas.focus());
    chat.activate(1);
    CHECK(chat.fields().extent.y == 42);
    key.key = gui::Key::Up;
    CHECK(canvas.dispatch(key) && chat.text.size() == 221 && chat.lineCount() > 2);
    key.key = gui::Key::Up;
    CHECK(canvas.dispatch(key) && chat.text.empty());
    key.key = gui::Key::Down;
    CHECK(canvas.dispatch(key) && chat.text.size() == 221);
    CHECK(canvas.popDialog());
    CHECK(root.resize({}, {320, 240}));
    chat.setLineCount(2);
    CHECK(chat.layout({320, 240}) && chat.fields().extent.x == 319);
    CHECK(chat.lineCount() > 2);
    CHECK(chat.setText(""));
    chat.setLineCount(2);
    chat.activate(1);
    auto lowSurface = gfx::SoftwareSurface::create(320, 240);
    CHECK(lowSurface);
    const auto lowOrigin = chat.fields().position;
    const auto lowCaretX = lowOrigin.x + FearGuiChat::DisplayBorderX + 10 * 8;
    CHECK(chat.draw(*lowSurface, lowOrigin, lowSurface->bitmap().bounds()));
    CHECK(lowSurface->bitmap().row(lowOrigin.y + 29)[lowCaretX] == std::byte{GREEN});
    CHECK(lowSurface->bitmap().row(lowOrigin.y + 30)[lowCaretX] != std::byte{GREEN});
    font->info.scale.y = 98304;
    CHECK(chat.layout({320, 240}) && chat.fields().extent.y == 58);
    const auto scaledOrigin = chat.fields().position;
    lowSurface->clear(0);
    CHECK(chat.draw(*lowSurface, scaledOrigin, lowSurface->bitmap().bounds()));
    CHECK(lowSurface->bitmap().row(scaledOrigin.y + 41)[lowCaretX] == std::byte{GREEN});
    CHECK(lowSurface->bitmap().row(scaledOrigin.y + 42)[lowCaretX] != std::byte{GREEN});
    CHECK(canvas.content(nullptr));
}

void chatEntryCentering() {
    const auto makeFont = [](Int32 height, UInt8 glyphHeight, Int32 baseline) {
        auto font = chatTestFont();
        font->info.height = height;
        font->info.baseline = baseline;
        font->characters[0].height = glyphHeight;
        font->characters[0].baseline = 0;
        std::ranges::fill(font->sheets.bitmaps[0].pixels, std::byte{77});
        // Installed fonts have a transparent space below the visible capitals.
        font->sheets.bitmaps.push_back(*gfx::Bitmap::create(8, 2));
        font->characters.push_back({1, 0, 0, 8, 2, 1, {}});
        font->lookup[0] = 1;
        return font;
    };
    const std::array fonts{
        makeFont(13, 9, 12), makeFont(17, 11, 15), makeFont(8, 5, 6), makeFont(10, 7, 8)};
    gui::Control root;
    FearGuiChat chat;
    gui::Canvas canvas;
    chat.font = fonts[0];
    chat.teamFont = fonts[1];
    chat.smallFont = fonts[2];
    chat.smallTeamFont = fonts[3];
    CHECK(root.addObject(chat) && canvas.content(&root));
    for (const auto extent : {Point2I{640, 480}, Point2I{320, 240}}) {
        CHECK(root.resize({}, extent));
        for (const auto scale : {65536, 98304, 131072}) {
            for (const auto& font : fonts) {
                font->info.scale.y = scale;
            }
            for (const auto channel : {0, 1}) {
                for (const auto& text :
                     {std::string{}, std::string{"Hello"}, std::string(100, 'A')}) {
                    CHECK(chat.setText(text));
                    chat.setLineCount(2);
                    chat.activate(channel);
                    chat.anchor = 0; // Include selection and the caret in the rendered check.
                    auto surface = gfx::SoftwareSurface::create(extent.x, extent.y);
                    CHECK(surface);
                    const auto origin = chat.fields().position;
                    CHECK(chat.draw(*surface, origin, surface->bitmap().bounds()));
                    Int32 glyphTop = extent.y, glyphBottom = -1;
                    Int32 caretTop = extent.y, caretBottom = -1;
                    for (Int32 y = 0; y < extent.y; ++y) {
                        const auto row = surface->bitmap().row(y);
                        for (Int32 x = origin.x + FearGuiChat::DisplayBorderX;
                             x < origin.x + chat.fields().extent.x - FearGuiChat::DisplayBorderX;
                             ++x) {
                            if (row[x] == std::byte{77}) {
                                glyphTop = std::min(glyphTop, y);
                                glyphBottom = std::max(glyphBottom, y);
                            } else if (row[x] == std::byte{GREEN}) {
                                caretTop = std::min(caretTop, y);
                                caretBottom = std::max(caretBottom, y);
                            }
                        }
                    }
                    const auto topGap = glyphTop - origin.y;
                    const auto bottomGap = origin.y + chat.fields().extent.y - 1 - glyphBottom;
                    CHECK(glyphBottom >= glyphTop && std::abs(topGap - bottomGap) <= 1);
                    CHECK(caretBottom >= caretTop && caretTop >= origin.y + 4 &&
                          caretBottom <= origin.y + chat.fields().extent.y - 4);
                    const auto& font = fonts[(extent.x < 512 ? 2 : 0) + channel];
                    const auto glyphHeight = (font->characters[0].height * scale) >> 16;
                    // The caret stays centered on the last occupied row, including after wrapping.
                    CHECK(std::abs(caretTop + caretBottom - (2 * glyphBottom - glyphHeight + 1)) <=
                          2);
                }
            }
        }
    }
    CHECK(canvas.content(nullptr));
}

void chatOverServerMenu() {
    FearGuiChat chat;
    FearGameApp app;
    CHECK(app.init() && app.setupLiveDemoSession());
    auto runtime = app.guiRuntime();
    auto root = std::make_unique<gui::Control>();
    root->assignName("playGui");
    CHECK(root->resize({}, {640, 480}) && root->addObject(chat));
    CHECK(runtime->show(gui::Graph::create(std::move(root))));
    // Use a real serialized score dialog through showGameplayScores, without
    // requiring an installation for the input-routing regression.
    gui::Control scores;
    scores.assignName("ScoreGui");
    CHECK(scores.resize({}, {640, 480}));
    std::vector<std::byte> bytes(65536);
    MemRWStream stream{std::span<std::byte>(bytes)};
    Persistent::StoreContext store(app.game().classes());
    CHECK(store.store(stream, scores) == Persistent::Error::Ok);
    bytes.resize(stream.getPosition());
    runtime->loadFile = [bytes](std::string_view) {
        return std::optional(bytes);
    };
    app.showGameplayScores(true);
    CHECK(runtime->canvas.dialogCount() == 1);
    CHECK(app.console()
              .evaluate(R"(
        newActionMap("actionMap.sae");
        bindAction(keyboard0, make, "t", TO, 220200, 0);
        bindAction(keyboard0, make, "y", TO, 220200, 1);
        function chatParityMenuSelect() { $ChatParityMenuPicks++; }
    )")
              .ok());
    auto& cg = app.fearPlugin().clientGlobals();
    auto ownedMenu = std::make_unique<ChatMenu>();
    auto* menu = ownedMenu.get();
    menu->assignName("CurServerMenu");
    CHECK(cg.manager->addObject(std::move(ownedMenu)));
    menu->bindConsole(&app.console());
    CHECK(menu->addMenuString("1Skill points",
                              std::array<std::string_view, 1>{"chatParityMenuSelect"}));
    menu->setMode(ChatMenu::Active);
    CHECK(app.bindGameplayActions() && app.gameplayKeyboardActive());
    auto* input = dynamic_cast<sim::SimInputManager*>(cg.manager->findObject(sim::InputManagerId));
    CHECK(input);
    const auto press = [&](char key) {
        sim::SimInputEvent event;
        event.objInst = sim::inputKeyCode(key);
        event.fValue = 1;
        CHECK(input->submit(event));
        event.action = sim::InputBreak;
        event.fValue = 0;
        (void)input->submit(event);
    };
    std::vector<std::pair<Int32, std::string>> sent;
    chat.sendLine = [&](Int32 channel, std::string_view line) {
        sent.emplace_back(channel, line);
    };
    for (const auto key : {'t', 'y'}) {
        press(key);
        CHECK(chat.visible() && runtime->canvas.focus() == &chat && !app.gameplayKeyboardActive());
        CHECK(!app.gameplayInputActive());
        CHECK(app.console().getVariable("pref::msgChannel") == (key == 't' ? "0" : "1"));
        gui::Event text;
        text.type = gui::EventType::Text;
        text.text = "100";
        CHECK(runtime->canvas.dispatch(text));
        CHECK(menu->executedCommands().empty());
        gui::Event enter;
        enter.type = gui::EventType::KeyDown;
        enter.key = gui::Key::Return;
        CHECK(runtime->canvas.dispatch(enter));
        CHECK(!chat.visible() && app.gameplayKeyboardActive() &&
              runtime->canvas.dialogCount() == 1);
        CHECK(menu->getMode() == ChatMenu::Active);
    }
    CHECK((sent == std::vector<std::pair<Int32, std::string>>{{0, "100"}, {1, "100"}}));
    press('t');
    CHECK(app.escapeGameplay() && !chat.visible() && runtime->canvas.dialogCount() == 1);
    press('1');
    CHECK(app.console().getVariable("ChatParityMenuPicks") == "1");
    app.showGameplayScores(false);
    CHECK(runtime->canvas.dialogCount() == 0 && runtime->canvas.top()->getName() == "playGui");
    for (const auto key : {'t', 'y'}) {
        CHECK(app.gameplayInputActive() && app.gameplayKeyboardActive());
        press(key);
        CHECK(chat.visible() && runtime->canvas.focus() == &chat);
        CHECK(app.gameplayInputActive() && !app.gameplayKeyboardActive());
        CHECK(app.escapeGameplay() && !chat.visible());
        CHECK(app.gameplayInputActive() && app.gameplayKeyboardActive());
    }
    CHECK(app.toggleConsole());
    CHECK(!app.gameplayInputActive() && !app.gameplayKeyboardActive());
    CHECK(app.toggleConsole());
    CHECK(runtime->clear());
}

void installedClientHuds() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::is_regular_file(
                       std::filesystem::path(assets) / "config/Presto/HUD.cs")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-client-huds-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};
    FearGameApp app(config);
    CHECK(app.init(std::array<std::string_view, 2>{"-mod", "RPG"}));
    auto& console = app.console();
    auto runtime = app.guiRuntime();
    auto* frame = runtime->findControl("HUD::DeusRPGhud");
    auto* text = dynamic_cast<FearGuiFormattedText*>(runtime->findControl("HUD::DeusRPGhud::0"));
    CHECK(frame && text && frame->fields().extent.x == 165 && frame->visible());
    CHECK(text->fonts[0].resource && text->fonts[1].resource && text->fonts[2].resource);
    CHECK(runtime->open("gui/play.gui"));
    CHECK(frame->parent() == runtime->canvas.top());
    CHECK(console.evaluate("return HUD::GetDisplayed(DeusRPGhud);").value == "1");
    CHECK(!text->text().empty() && text->fields().extent.x == 165);
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && frame->draw(*surface, frame->fields().position, surface->bitmap().bounds()));
    CHECK(std::ranges::any_of(surface->bitmap().pixels, [](auto pixel) { return pixel != std::byte{}; }));
    CHECK(app.setGameplayGuiMode(1) && app.bindGameplayActions());
    auto* manager = app.fearPlugin().clientGlobals().manager;
    auto* input = dynamic_cast<sim::SimInputManager*>(manager->findObject(sim::InputManagerId));
    CHECK(input);
    sim::SimInputEvent key;
    key.objInst = sim::inputKeyCode('v');
    CHECK(input->submit(key));
    CHECK(app.stepFrame(0) && app.stepFrame(.032));
    auto* menu = dynamic_cast<ChatMenu*>(manager->findObject("PlayChatMenu"));
    CHECK(menu && menu->getMode() == ChatMenu::OneShot && menu->curMenu());
    key.action = sim::InputBreak;
    (void)input->submit(key);
    key.action = sim::InputMake;
    // This installation binds V to Presto's wrapper around the vanilla menu.
    key.objInst = sim::inputKeyCode(menu->curMenu()->key);
    CHECK(input->submit(key));
    CHECK(app.stepFrame(.064));
    CHECK(menu->curMenu() != menu->rootMenu());
    CHECK(app.escapeGameplay() && menu->getMode() == ChatMenu::Inactive);
    // Presto's custom menus replace the named ChatMenu for each submenu.
    CHECK(console.evaluate("Menu::Display(menuChat);").ok());
    CHECK(app.stepFrame(.096));
    menu = dynamic_cast<ChatMenu*>(manager->findObject("PlayChatMenu"));
    CHECK(menu && menu->getMode() == ChatMenu::OneShot && menu->curMenu());
    const auto menuNumber = menu->getNumber();
    key.objInst = sim::inputKeyCode(menu->curMenu()->key);
    CHECK(input->submit(key));
    CHECK(app.stepFrame(.128));
    menu = dynamic_cast<ChatMenu*>(manager->findObject("PlayChatMenu"));
    CHECK(menu && menu->getMode() == ChatMenu::OneShot && menu->curMenu());
    CHECK(menu->getNumber() != menuNumber);
    CHECK(app.escapeGameplay() && menu->getMode() == ChatMenu::Inactive);
    CHECK(runtime->open("gui/MainMenu.gui") && !frame->parent());
    CHECK(runtime->open("gui/play.gui") && frame->parent() == runtime->canvas.top());
    CHECK(!text->text().empty());
    console.setVariable("PCFG::Name", "HudRegression");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "hud-regression-test");
    console.setVariable("Server::Port", "0");
    console.setVariable("Server::Heartbeats", "false");
    CHECK(app.loadMission("rpgmap5"));
    for (Int32 tick = 0; tick < 8; ++tick) {
        CHECK(app.stepFrame(.16 + tick * .032));
    }
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate(R"(
        processMenupickgroup(2049, 3);
        processMenupickclass(2049, 1);
    )").ok());
    (void)app.fearPlugin().syncLoopbackState(false);
    CHECK(app.setGameplayGuiMode(1));
    CHECK(app.fearPlugin().focusClient());
    CHECK(console.evaluate("HUD::Update(DeusRPGhud);").ok());
    text = dynamic_cast<FearGuiFormattedText*>(runtime->findControl("HUD::DeusRPGhud::0"));
    CHECK(text && text->text().find("Fighter") != std::string::npos &&
          text->text().find("HP:") != std::string::npos);
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate("2049.lastSayTime = -100;").ok());
    CHECK(app.fearPlugin().focusClient());
    // Follow the installed V-menu emote through remoteSay, the RPG's local
    // feedback and its sound-only PlayerSayEvent broadcast.
    auto messages = std::make_shared<std::vector<std::string>>();
    auto& plugin = app.fearPlugin();
    auto previous = plugin.onChatMessage;
    plugin.onChatMessage = [messages, previous](Int32 type, std::string_view message) {
        messages->emplace_back(message);
        if (previous) {
            previous(type, message);
        }
    };
    CHECK(console.evaluate("Say::Public(yellDoh);").ok());
    CHECK(app.stepFrame(.416));
    plugin.onChatMessage = std::move(previous);
    CHECK(std::ranges::any_of(*messages, [](const auto& message) {
        return message.find("Doh!") != std::string::npos;
    }));
    const auto prefix = plugin.clientGlobals().playerManager->findClient(2049)->name + ": ";
    CHECK(std::ranges::none_of(*messages, [&](const auto& message) { return message == prefix; }));

    // Select the installed animation entry, including its separate spatial voice.
    for (Int32 tick = 0; tick < 8; ++tick) {
        CHECK(app.stepFrame(.666 + tick * .25));
    }
    auto* body = dynamic_cast<Player*>(plugin.clientGlobals().playerPSC->getControlObject());
    CHECK(body && !body->isDead());
    UInt32 voices = 0;
    auto playback = std::make_unique<SoundPlayback>(nullptr);
    playback->playVoice = [&](Int32 speaker,
                              std::string_view filename,
                              const SoundProfileData&,
                              const TMat3F& transform,
                              const Point3F&) {
        CHECK(speaker == 2049 && filename.ends_with(".wovrhere.wav"));
        CHECK((transform.p - body->getPos()).len() < .01f);
        ++voices;
        return SoundHandle(voices);
    };
    CHECK(manager->addObject(std::move(playback)));
    CHECK(console.evaluate("Menu::Display(menuChat);").ok());
    CHECK(app.stepFrame(2.448));
    menu = dynamic_cast<ChatMenu*>(manager->findObject("PlayChatMenu"));
    CHECK(menu && menu->processKey('a'));
    CHECK(app.stepFrame(2.480));
    menu = dynamic_cast<ChatMenu*>(manager->findObject("PlayChatMenu"));
    CHECK(menu && menu->processKey('o'));
    CHECK(app.stepFrame(2.512));
    CHECK(voices == 1);
    CHECK(body->currentAnimation() == Player::ANIM_PLAYER_FIRST);
    CHECK(body->shapeInstance());
    const auto& shape = *body->shapeInstance()->shape();
    const auto& thread = body->shapeInstance()->threads().front();
    CHECK(shape.names[shape.sequences[thread.sequence].name] == "sign over here");
}

void installedRpgBulkChat() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets ||
        !std::filesystem::is_regular_file(std::filesystem::path(assets) / "RPG/scripts.vol")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-chat-parity-{}",
                                   std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{config.writeRoot};

    FearGameApp app(config);
    CHECK(app.init(std::array<std::string_view, 2>{"-mod", "RPG"}));
    auto& console = app.console();
    console.setVariable("PCFG::Name", "ChatParityTest");
    console.setVariable("PCFG::Script", "");
    console.setVariable("PCFG::Info", "chat-parity-test");
    console.setVariable("Server::Port", "0");
    CHECK(app.loadMission("rpgmap5"));
    for (Int32 frame = 0; frame < 8; ++frame) {
        CHECK(app.stepFrame(frame * .032));
    }
    CHECK(app.fearPlugin().focusServer());
    CHECK(!script::toBool(console.evaluate("return 2049.IsInvalid;").value));
    CHECK(console
              .evaluate(R"(
        processMenupickgroup(2049, 3);
        processMenupickclass(2049, 1);
    )")
              .ok());
    (void)app.fearPlugin().syncLoopbackState(false);
    CHECK(app.setGameplayGuiMode(1));
    CHECK(app.fearPlugin().focusServer());
    CHECK(console
              .evaluate(R"(
        storeData(2049, "LVL", 100);
        storeData(2049, "SPcredits", 50);
        $PlayerSkill[2049, 1] = 0;
        2049.lastSayTime = -10000;
        MenuSP(2049, 1);
    )")
              .ok());
    CHECK(app.fearPlugin().focusClient());
    auto& cg = app.fearPlugin().clientGlobals();
    auto runtime = app.guiRuntime();
    CHECK(cg.playerPSC->scoresVisible() && runtime->canvas.dialogCount() == 1);
    const auto* menu = dynamic_cast<ChatMenu*>(cg.manager->findObject("CurServerMenu"));
    CHECK(menu && menu->curMenu());
    const auto skillKey = menu->curMenu()->key;
    auto* root = runtime->canvas.content();
    const auto findChat = [&](auto&& self, gui::Control& control) -> FearGuiChat* {
        if (auto* chat = dynamic_cast<FearGuiChat*>(&control)) {
            return chat;
        }
        for (auto* child : control.objects()) {
            if (auto* chat = self(self, *static_cast<gui::Control*>(child))) {
                return chat;
            }
        }
        return nullptr;
    };
    auto* chat = findChat(findChat, *root);
    CHECK(chat && app.bindGameplayActions());
    auto* input = dynamic_cast<sim::SimInputManager*>(cg.manager->findObject(sim::InputManagerId));
    CHECK(input);
    const auto press = [&](char key) {
        sim::SimInputEvent event;
        event.objInst = sim::inputKeyCode(key);
        event.fValue = 1;
        CHECK(input->submit(event));
        event.action = sim::InputBreak;
        event.fValue = 0;
        (void)input->submit(event);
    };
    press('t'); // Installed actionMap -> playDelegate chat action beneath score.gui.
    CHECK(chat->visible() && runtime->canvas.focus() == chat && !app.gameplayKeyboardActive());
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    CHECK(runtime->canvas.focus() == chat);
    gui::Event text;
    text.type = gui::EventType::Text;
    text.text = "5";
    CHECK(runtime->canvas.dispatch(text));
    CHECK(app.renderFrame(*surface));
    gui::Event enter;
    enter.type = gui::EventType::KeyDown;
    enter.key = gui::Key::Return;
    CHECK(runtime->canvas.dispatch(enter));
    CHECK(!chat->visible() && runtime->canvas.dialogCount() == 1 && app.gameplayKeyboardActive());
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate("return 2049.bulkNum;").value == "5");
    CHECK(console.evaluate("return fetchData(2049, \"SPcredits\");").value == "50");
    CHECK(app.fearPlugin().focusClient());
    press(skillKey); // The installed skill menu consumes the entered bulk count.
    CHECK(app.fearPlugin().focusServer());
    CHECK(console.evaluate("return fetchData(2049, \"SPcredits\");").value == "45");
    CHECK(script::toNumber(console.evaluate("return $PlayerSkill[2049, 1];").value) > 0);
}

void thrownItems() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    CHECK(console
              .evaluate(R"(
        PlayerData ThrowArmor { mass = 9; boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2.3; };
        ItemData ThrowRock { mass = 2; };
        newObject(ThrowPlayer, Player, ThrowArmor);
        newObject(ThrowRockObject, Item, ThrowRock, 1, false);
        function Item::onCollision(%this, %object) { $PathHit = %object; $PathHits++; }
    )")
              .ok());
    auto* player = dynamic_cast<Player*>(globals.manager->findObject("ThrowPlayer"));
    auto* rock = dynamic_cast<Item*>(globals.manager->findObject("ThrowRockObject"));
    CHECK(player && rock);
    player->setPos({100, 200, 300});
    player->setRot({});
    player->setViewPitch(0.25f);
    player->setLinearVelocity({1, 2, 3});
    Point3F origin, direction;
    player->getThrowVector(&origin, &direction);
    CHECK((origin - player->getBoxCenter()).len() < .001f);
    CHECK(std::abs(direction.len() - 1) < .0001f);
    CHECK(std::abs(direction.z - std::sin(.25f + .5f * std::cos(.25f))) < .0001f);
    rock->throwObject(player, 20, false);
    const auto velocity = direction * 10 + player->getLinearVelocity();
    CHECK((rock->getLinearVelocity() - velocity).len() < .001f);
    rock->setForceMask(0);
    const auto start = rock->getPos();
    rock->serverProcess(32);
    CHECK((rock->getPos() - start - velocity * .032f).len() < .001f);
    rock->onCollisionNotify(player);
    CHECK(console.getVariable("PathHit").empty());
    rock->serverProcess(1032);
    CHECK(!rock->throwSource());
    globals.manager->advanceToTime(.1f);
    rock->onCollisionNotify(player);
    CHECK(console.getVariable("PathHit") == player->scriptThis());
    CHECK(console.getVariable("PathHits") == "1");
    rock->throwObject(player, 20, true);
    CHECK(rock->getOwnerId() == player->getId());
    CHECK(rock->throwSource() == player);
    const auto tossed = rock->getLinearVelocity() - player->getLinearVelocity();
    CHECK(tossed.z >= 5 && tossed.z <= 10);
    CHECK(std::abs(tossed.x) <= 7.33f && std::abs(tossed.y) <= 7.33f);
    rock->onCollisionNotify(player);
    CHECK(console.getVariable("PathHits") == "1");
    rock->setNetFlags(sim::SimNetObject::IsGhost);
    console.setVariable("PathHit", "");
    rock->onCollisionNotify(player);
    CHECK(console.getVariable("PathHit").empty());
}
}

int opent1_test_rpg_paths() {
    return opent1::test::run([] {
        scriptHudLifecycle();
        recipientMessages();
        chatReentry();
        chatEntryParity();
        chatEntryCentering();
        chatOverServerMenu();
        thrownItems();
    });
}

OPENT1_REGISTER_TEST("game", "rpg_paths", opent1_test_rpg_paths);

int opent1_test_installed_rpg_bulk_chat() {
    return opent1::test::run(installedRpgBulkChat);
}

OPENT1_REGISTER_TEST("chat", "installed_rpg_bulk_chat", opent1_test_installed_rpg_bulk_chat);

int opent1_test_installed_client_huds() {
    return opent1::test::run(installedClientHuds);
}

OPENT1_REGISTER_TEST("hud", "installed_client_huds", opent1_test_installed_client_huds);
