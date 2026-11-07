#include <opent1/game/game_app.hpp>
#include <ranges>
#include <opent1/game/shell_controls.hpp>
#include <opent1/game/server_irc_controls.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/chat_menu.hpp>
#include <opent1/game/commander_huds.hpp>
#include <numbers>
#include <cmath>
#include <charconv>
#include <fstream>
#include <sstream>

namespace opent1::game {
namespace {
class ConsolePrompt final : public gui::TextEdit {
public:
    ConsolePrompt(Console& console, std::vector<std::string>& history)
        : console_(console), history_(history), historyIndex_(history.size()) {
        maxBytes = 4096;
    }

    void action() override {}

    bool event(const gui::Event& event) override {
        if (event.type == gui::EventType::KeyDown) {
            if (event.key == gui::Key::Return) {
                if (!event.repeat && !text.empty()) {
                    const auto command = text;
                    if (history_.empty() || history_.back() != command) {
                        history_.push_back(command);
                    }
                    if (history_.size() > 128) {
                        history_.erase(history_.begin());
                    }
                    historyIndex_ = history_.size();
                    (void)setText("");
                    auto* console = &console_;
                    canvas()->afterDispatch([console, command] {
                        console->print("> " + command);
                        const auto result = console->evaluate(command, "console");
                        for (const auto& diagnostic : result.diagnostics) {
                            console->print(diagnostic.message);
                        }
                        if (!result.value.empty()) {
                            console->print(result.value);
                        }
                    });
                }
                return true;
            }
            if (event.key == gui::Key::Up || event.key == gui::Key::Down) {
                if (event.key == gui::Key::Up && historyIndex_) {
                    --historyIndex_;
                }
                if (event.key == gui::Key::Down && historyIndex_ < history_.size()) {
                    ++historyIndex_;
                }
                (void)setText(historyIndex_ < history_.size() ? history_[historyIndex_] : "");
                cursor = anchor = text.size();
                return true;
            }
        }
        return gui::TextEdit::event(event);
    }

protected:
    void drawBackground(gfx::SoftwareSurface& surface, Point2I offset) const override {
        surface.fill(
            {offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1},
            0);
    }

private:
    Console& console_;
    std::vector<std::string>& history_;
    std::size_t historyIndex_;
};

class ConsolePanel final : public gui::Control {
public:
    ConsolePanel(Console& console,
                 std::vector<std::string>& history,
                 std::shared_ptr<const gfx::BitmapFont> font)
        : prompt(console, history), console_(console), font_(std::move(font)) {
        assignName("OpenT1Console");
        (void)resize({}, {640, 240});
        prompt.font = font_;
        prompt.cursorColor = 255;
        (void)prompt.resize({8, 212}, {624, 24});
        (void)addObject(prompt);
    }

    bool draw(gfx::SoftwareSurface& surface, Point2I offset, gui::RectI clip) const override {
        surface.fill(
            {offset.x, offset.y, offset.x + fields_.extent.x - 1, offset.y + fields_.extent.y - 1},
            0);
        if (font_) {
            const Int32 height = std::max(1, font_->info.height + 2);
            const auto& lines = console_.log();
            const auto count = std::min(lines.size(), std::size_t(204 / height));
            for (auto i :
                 std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                     return i < count;
                 })) {
                if (!surface.text(*font_,
                                  {offset.x + 8, offset.y + 4 + Int32(i) * height},
                                  lines[lines.size() - count + i])) {
                    return false;
                }
            }
        }
        return drawChildren(surface, offset, clip);
    }

    ConsolePrompt prompt;

private:
    Console& console_;
    std::shared_ptr<const gfx::BitmapFont> font_;
};

template <class T> T* findGameplayControl(gui::Control* root) {
    if (!root) {
        return nullptr;
    }
    if (auto* match = dynamic_cast<T*>(root)) {
        return match;
    }
    for (auto* child : root->objects()) {
        if (auto* match = findGameplayControl<T>(dynamic_cast<gui::Control*>(child))) {
            return match;
        }
    }
    return nullptr;
}

gui::Control* gameplayRoot(gui::Canvas& canvas, bool showingScores) {
    // Vanilla playDelegate::curGui remains the content GUI while score.gui is
    // pushed above it. Other dialogs (including the console) remain modal.
    return showingScores && canvas.dialogCount() == 1 ? canvas.content() : canvas.top();
}
}

bool FearGameApp::consoleVisible() const {
    return guiRuntime_ && dynamic_cast<ConsolePanel*>(guiRuntime_->canvas.top());
}

bool FearGameApp::toggleConsole() {
    if (!guiRuntime_) {
        return false;
    }
    if (consoleVisible()) {
        return guiRuntime_->pop();
    }
    if (auto* psc = fearPlugin_->clientGlobals().playerPSC) {
        psc->resetInput();
    }
    auto font = resources_->load<gfx::BitmapFont>("sf_white_9.pft");
    auto panel = std::make_unique<ConsolePanel>(game_.console(),
                                                consoleHistory_,
                                                font ? std::make_shared<gfx::BitmapFont>(*font)
                                                     : defaultFont_);
    auto* prompt = &panel->prompt;
    if (!guiRuntime_->show(gui::Graph::create(std::move(panel)), true)) {
        return false;
    }
    return guiRuntime_->canvas.makeFirstResponder(prompt);
}

bool FearGameApp::gameplayInputActive() const {
    const auto& cg = fearPlugin_->clientGlobals();
    if (consoleVisible() || missionPending() || !cg.playerPSC ||
        !cg.playerPSC->getControlObject() || !guiRuntime_) {
        return false;
    }
    auto* root = guiRuntime_->canvas.top();
    if (!root) {
        return false;
    }
    // Chat owns keyboard focus without changing Play mode's mouse look.
    // gameplayKeyboardActive() keeps typed keys out of the action maps.
    return !showingScores_ && root->getName() == "playGui";
}

bool FearGameApp::gameplayKeyboardActive() const {
    const auto& cg = fearPlugin_->clientGlobals();
    if (consoleVisible() || missionPending() || !cg.playerPSC ||
        !cg.playerPSC->getControlObject() || !guiRuntime_ || !guiRuntime_->canvas.top()) {
        return false;
    }
    const auto* chat =
        findGameplayControl<FearGuiChat>(gameplayRoot(guiRuntime_->canvas, showingScores_));
    return !chat || !chat->visible();
}

bool FearGameApp::setGameplayGuiMode(UInt32 mode) {
    if (mode < 1 || mode > 6) {
        return false;
    }
    auto& cg = fearPlugin_->clientGlobals();
    gameplayGuiMode_ = mode;
    // A late control/GUI packet can arrive after editor.cs has opened. Retain
    // the requested play mode without replacing the active editing graph.
    if (const auto* root = guiRuntime_ ? guiRuntime_->canvas.top() : nullptr;
        root && (root->getName() == "editorGui" || root->getName() == "EditorGui")) {
        return true;
    }
    if (!cg.playerPSC || !guiRuntime_) {
        return false;
    }
    // Servers can send their initial GUI mode before the mission ghosts and
    // control object. Keep the loading graph (and MODInfo text) until control
    // arrives; onPlayerControl applies the saved mode then.
    if (!cg.playerPSC->getControlObject()) {
        return true;
    }
    cg.playerPSC->resetInput();
    const auto apply = [this, mode] {
        game_.console().setVariable("InLobbyMode", mode == 6 ? "true" : "false");
        if (mode == 6 && guiRuntime_->canvas.top() &&
            guiRuntime_->canvas.top()->getName() == "playGui") {
            gameplayPalette_ = defaultPalette_;
        }
        constexpr std::array paths{"gui/play.gui",
                                   "gui/command.gui",
                                   "gui/score.gui",
                                   "gui/CmdInventory.gui",
                                   "gui/CmdObjectives.gui",
                                   "gui/Lobby.gui"};
        if (guiRuntime_->open(paths[mode - 1])) {
            showingScores_ = false;
            if (mode != 6) {
                updateGameplayPalette();
            }
        } else {
            game_.console().setVariable("OpenT1::LastGuiError",
                                        std::string("Cannot load ") + paths[mode - 1]);
        }
    };
    if (guiRuntime_->canvas.dispatching()) {
        guiRuntime_->canvas.afterDispatch(apply);
    } else {
        apply();
    }
    return true;
}

void FearGameApp::showGameplayScores(bool visible) {
    if (!guiRuntime_ || visible == showingScores_) {
        return;
    }
    const auto apply = [this, visible] {
        if (visible) {
            if (auto* psc = fearPlugin_->clientGlobals().playerPSC) {
                psc->resetInput();
            }
            showingScores_ = guiRuntime_->open("gui/score.gui", true);
        } else {
            (void)guiRuntime_->pop();
            showingScores_ = false;
        }
    };
    if (guiRuntime_->canvas.dispatching()) {
        guiRuntime_->canvas.afterDispatch(apply);
    } else {
        apply();
    }
}

bool FearGameApp::escapeGameplay() {
    if (consoleVisible()) {
        return toggleConsole();
    }
    auto& cg = fearPlugin_->clientGlobals();
    if (!cg.playerPSC || !guiRuntime_) {
        return false;
    }
    if (auto* chat =
            findGameplayControl<FearGuiChat>(gameplayRoot(guiRuntime_->canvas, showingScores_));
        chat && chat->visible()) {
        chat->deactivate();
        return true;
    }
    bool closedMenu = false;
    for (const auto name : {"PlayChatMenu", "PlayChatMenuInvisible"}) {
        auto* menu = cg.manager ? dynamic_cast<ChatMenu*>(cg.manager->findObject(name)) : nullptr;
        if (menu && menu->getMode() != ChatMenu::Inactive) {
            sim::SimInputEvent escape;
            escape.objInst = ChatMenu::kEscapeScanCode;
            closedMenu |= menu->processEvent(escape);
        }
    }
    if (closedMenu) {
        return true;
    }
    if (showingScores_) {
        const std::array<std::string, 2> args{"2048", "ScoresOff"};
        (void)game_.console().execute("remoteEval", args);
        return true;
    }
    if (guiRuntime_->canvas.dialogCount()) {
        return guiRuntime_->pop();
    }
    const bool inPlay =
        guiRuntime_->canvas.top() && guiRuntime_->canvas.top()->getName() == "playGui";
    if (inPlay) {
        return setGameplayGuiMode(6);
    }
    if (gameplayGuiMode_ == 6) {
        return setGameplayGuiMode(cg.playerPSC->guiMode() >= 1 && cg.playerPSC->guiMode() <= 5
                                      ? cg.playerPSC->guiMode()
                                      : 1);
    }
    if (!game_.console().isFunction("remotePlayMode")) {
        return setGameplayGuiMode(1);
    }
    const std::array<std::string, 2> args{"2048", "PlayMode"};
    return game_.console().execute("remoteEval", args).ok();
}

bool FearGameApp::handleGameplayAction(const sim::SimActionEvent& event) {
    if (!guiRuntime_) {
        return false;
    }
    auto* root = gameplayRoot(guiRuntime_->canvas, showingScores_);
    switch (event.action) {
        case 220200: {
            auto* chat = findGameplayControl<FearGuiChat>(root);
            if (!chat) {
                return false;
            }
            chat->activate(static_cast<Int32>(event.fValue));
            game_.console().setVariable("pref::msgChannel", std::to_string(chat->channel()));
            if (auto* psc = fearPlugin_->clientGlobals().playerPSC) {
                psc->resetInput();
            }
            return true;
        }
        case 220201:
        case 220202:
            if (auto* display = findGameplayControl<FearGuiChatDisplay>(root)) {
                return display->onSimActionEvent(event);
            }
            return false;
        case 220203: {
            const std::array<std::string, 2> args{"2048",
                                                  showingScores_ ? "ScoresOff" : "ScoresOn"};
            (void)game_.console().execute("remoteEval", args);
            return true;
        }
        case 220204:
            return escapeGameplay();
        default:
            if (auto* commander = findGameplayControl<TSCommander>(root)) {
                return commander->onSimActionEvent(event);
            }
            return false;
    }
}

void FearGameApp::displayChatMessage(Int32 type, std::string_view message) {
    if (chatMessages_.size() == FearGuiChatDisplay::MessageHistoryLimit) {
        chatMessages_.pop_front();
    }
    chatMessages_.emplace_back(type, message);
    // Invalidating the binding also handles messages received while another
    // screen is open; the next play screen receives the complete history.
    chatDisplayLifetime_.reset();
}

void FearGameApp::updateGameplayGui() {
    if (!guiRuntime_ || !guiRuntime_->canvas.top()) {
        return;
    }
    auto& cg = fearPlugin_->clientGlobals();
    if (auto* list = findGameplayControl<ServerListCtrl>(guiRuntime_->canvas.top())) {
        const auto* server = list->selectedServer();
        game_.console().setVariable("Server::Address", server ? server->transportAddress : "");
        if (auto* button = dynamic_cast<gui::Active*>(list->findRootTag(140006))) {
            button->active = server && !server->transportAddress.empty();
        }
        if (auto* button = dynamic_cast<gui::Active*>(list->findRootTag(140018))) {
            button->active = server && server->status == ServerListCtrl::STAT_RESPONDING;
        }
    }
    auto* commander = findGameplayControl<TSCommander>(guiRuntime_->canvas.top());
    if (commander && cg.playerPSC && cg.playerManager && cg.dbm && cg.manager) {
        commander->refreshTargets(*cg.playerPSC,
                                  *cg.playerManager,
                                  *cg.dbm,
                                  cg.sensorManager,
                                  Int32(cg.manager->getId()));
        if (const auto* object = cg.playerPSC->getControlObject()) {
            EulerF angle;
            object->getEyeTransform().angles(angle);
            commander->facing = angle.z;
        }
        const auto preference = [this](std::string_view name, bool fallback) {
            const auto value = game_.console().getVariable(name);
            return value.empty() ? fallback : script::toBool(value);
        };
        commander->showNames = preference("pref::mapNames", true);
        commander->showSensorRange = preference("pref::mapSensorRange", false);
        commander->translucentSensors = preference("pref::mapSensorTranslucent", false);
        const auto filter = game_.console().getVariable("pref::mapFilter");
        UInt32 bits = TSCommander::MapFilterPlayers;
        if (!filter.empty()) {
            (void)std::from_chars(filter.data(), filter.data() + filter.size(), bits);
        }
        commander->setMapFilter(bits);
        if (!commander->hasMap() && cg.world && defaultPalette_) {
            const auto* center =
                dynamic_cast<MissionCenterPos*>(cg.manager->findObject(MissionCenterPosId));
            (void)commander->rebuildMap(*cg.world,
                                        *defaultPalette_,
                                        center ? center->pos : Point2F{1230, 1230},
                                        center ? center->extent : Point2F{1400, 1400});
        }
    }
    const auto update = [&](auto&& self, gui::Control& control) -> void {
        const auto selectItem = [this, name = std::string(control.getName())](Int32 item,
                                                                              bool doubleClick) {
            const auto function = name + (doubleClick ? "::onDoubleClick" : "::onSelect");
            if (!name.empty() && game_.console().isFunction(function)) {
                const std::array<std::string, 1> args{std::to_string(item)};
                (void)game_.console().execute(function, args);
            }
        };
        if (auto* list = dynamic_cast<FearGuiInventory*>(&control)) {
            list->itemSelected = selectItem;
        }
        const auto selectedAddress = game_.console().getVariable("Server::Address");
        if (cg.csDelegate) {
            for (const auto& server : cg.csDelegate->discoveredServers()) {
                if (server.transportAddress != selectedAddress) {
                    continue;
                }
                if (auto* detail = dynamic_cast<ServerDetailCtrl*>(&control)) {
                    detail->setServer(server);
                }
                if (auto* info = dynamic_cast<ServerInfoCtrl*>(&control)) {
                    info->setServer(server);
                }
            }
        }
        if (auto* list = dynamic_cast<ScoreListCtrl*>(&control); list && cg.playerManager) {
            list->rebuildScores(*cg.playerManager);
            list->clientSelected = [this](Int32 clientId) {
                game_.withWorld(sim::WorldType::Client, [&] {
                    const std::array<std::string, 3> args{
                        "2048", "SelectClient", std::to_string(clientId)};
                    (void)game_.console().execute("remoteEval", args);
                });
            };
        }
        if (auto* list = dynamic_cast<CommandTurretCtrl*>(&control); list && commander) {
            list->populateFromCommander(*commander);
        }
        if (auto* list = dynamic_cast<CommandItemCtrl*>(&control); list && commander) {
            list->populateFromCommander(*commander);
        }
        if (auto* list = dynamic_cast<CommandTeamCtrl*>(&control);
            list && cg.playerManager && cg.manager) {
            if (const auto* me = cg.playerManager->findClient(Int32(cg.manager->getId()))) {
                list->buildTeamHierarchy(*cg.playerManager, me->team);
            }
        }
        if (auto* objectives = dynamic_cast<FGHCommandObjectivePage*>(&control);
            objectives && cg.playerManager && cg.manager) {
            if (const auto* me = cg.playerManager->findClient(Int32(cg.manager->getId()))) {
                if (const auto* team = cg.playerManager->findTeam(me->team)) {
                    std::vector<std::string> lines;
                    for (const auto& objective : team->objectives) {
                        lines.push_back(objective.text);
                    }
                    objectives->setObjectives(std::move(lines));
                    objectives->tick(game_.simulationTime());
                }
            }
        }
        if (auto* observe = dynamic_cast<CMDObserve*>(&control);
            observe && observe->visible() && cg.playerPSC && cg.world) {
            if (const auto* object = cg.playerPSC->getControlObject()) {
                auto* scene = observe->sceneControl();
                sim::EnvironmentCamera view;
                view.worldToCamera = object->getEyeTransform();
                view.worldToCamera.inverse();
                const auto extent = scene->fields().extent;
                view.viewport = {0, 0, extent.x - 1, extent.y - 1};
                view.center = {Flt32(extent.x) * .5f, Flt32(extent.y) * .5f};
                const auto focal =
                    Flt32(extent.x) * .5f /
                    std::tan(cg.playerPSC->getFov() * std::numbers::pi_v<Flt32> / 360);
                view.focal = {focal, focal};
                view.nearDistance = .5f;
                view.farDistance = world::sceneVisibility(*cg.manager).visibleDistance;
                (void)scene->prepare(*cg.world, view);
            }
        }
        if (auto* list = dynamic_cast<FearGuiInventory*>(&control)) {
            std::vector<Int32> favorites;
            Int32 item{};
            std::istringstream input(game_.console().getVariable("OpenT1::InventoryFavorites"));
            while (input >> item) {
                favorites.push_back(item);
            }
            if (auto* purchase = dynamic_cast<FearGuiPurchase*>(list)) {
                purchase->populateFromShoppingList(cg.playerPSC, cg.dbm, favorites);
            } else {
                list->populateFromPSC(cg.playerPSC, cg.dbm, favorites);
            }
        }
        if (auto* list = dynamic_cast<InventoryListCtrl*>(&control)) {
            list->syncFromPSC(cg.playerPSC, cg.dbm);
        }
        if (auto* list = dynamic_cast<FGHInventory*>(&control)) {
            list->populateOwnedItems(cg.playerPSC, cg.dbm);
        }
        if (auto* list = dynamic_cast<FGHBuySell*>(&control)) {
            list->populateShoppingItems(cg.playerPSC, cg.dbm);
        }
        if (auto* list = dynamic_cast<FGArrayCtrl*>(&control);
            list && !dynamic_cast<ScoreListCtrl*>(list) && list->cells.x == 1 && list->parent()) {
            const auto inset = dynamic_cast<FearGuiInventory*>(list) ? 0 : 18;
            list->cellSize.x = std::max(1, list->parent()->fields().extent.x - inset);
            (void)list->setSize(list->cells);
        }
        if (auto* display = dynamic_cast<MenuDisplayCtrl*>(&control)) {
            display->heading.clear();
            display->entries.clear();
            display->selectEntry = [this](std::size_t row) {
                auto* manager = fearPlugin_->clientGlobals().manager;
                auto* menu = manager ? dynamic_cast<ChatMenu*>(manager->findObject("CurServerMenu"))
                                     : nullptr;
                if (!menu || menu->getMode() == ChatMenu::Inactive) {
                    return;
                }
                auto* node = menu->curMenu();
                while (node && row--) {
                    node = node->nextSibling.get();
                }
                if (node && node->key) {
                    (void)menu->processKey(node->key);
                }
            };
            const auto* menu =
                cg.manager ? dynamic_cast<ChatMenu*>(cg.manager->findObject("CurServerMenu"))
                           : nullptr;
            if (menu && menu->getMode() != ChatMenu::Inactive) {
                display->heading = menu->curMenuQuery().empty() ? menu->getHeading()
                                                              : menu->curMenuQuery();
                for (auto* node = menu->curMenu(); node; node = node->nextSibling.get()) {
                    display->entries.push_back(std::string(1, node->key) + ": " + node->heading);
                }
            }
        }
        if (auto* chat = dynamic_cast<FearGuiChat*>(&control)) {
            if (chat->parent()) {
                (void)chat->layout(chat->parent()->fields().extent);
            }
            chat->sendLine = [this](Int32 channel, std::string_view line) {
                const std::array<std::string, 2> args{std::to_string(channel), std::string(line)};
                (void)game_.console().execute("say", args);
            };
        }
        if (auto* display = dynamic_cast<FearGuiChatDisplay*>(&control)) {
            if (display->parent()) {
                (void)display->layout(display->parent()->fields().extent);
            }
            if (chatDisplayLifetime_.lock() != display->lifetime().lock()) {
                display->clear();
                for (const auto& [type, message] : chatMessages_) {
                    (void)display->newMessage(message, type);
                }
                chatDisplayLifetime_ = display->lifetime();
            }
        }
        for (auto* child : control.objects()) {
            if (auto* c = dynamic_cast<gui::Control*>(child)) {
                self(self, *c);
            }
        }
    };
    auto* root = gameplayRoot(guiRuntime_->canvas, showingScores_);
    if (root != guiRuntime_->canvas.top()) {
        update(update, *root);
    }
    update(update, *guiRuntime_->canvas.top());
}

void FearGameApp::installShellCommands() {
    auto& console = game_.console();
    console.addCommand(0,
                       "isFullscreenMode",
                       [](Console& con, Int32, std::span<const std::string> args) -> std::string {
                           return args.size() <= 2 &&
                                          script::toBool(con.getVariable("pref::VideoFullScreen"))
                                      ? "True"
                                      : "False";
                       });
    console.addCommand(0,
                       "setFullscreenMode",
                       [](Console& con, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 3) {
                               return "False";
                           }
                           con.setVariable("pref::VideoFullScreen", args[2]);
                           return "True";
                       });
    console.addCommand(0,
                       "setFSResolution",
                       [](Console& con, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 3) {
                               return "False";
                           }
                           const auto separator = args[2].find('x');
                           if (separator == std::string::npos) {
                               return "False";
                           }
                           Int32 width{}, height{};
                           const auto begin = args[2].data(), end = begin + args[2].size();
                           const auto a = std::from_chars(begin, begin + separator, width);
                           const auto b = std::from_chars(begin + separator + 1, end, height);
                           if (a.ec != std::errc{} || a.ptr != begin + separator ||
                               b.ec != std::errc{} || b.ptr != end || width < 640 || height < 480 ||
                               width > 16384 || height > 16384) {
                               return "False";
                           }
                           con.setVariable("pref::VideoFullScreenRes", args[2]);
                           return "True";
                       });
    for (const auto name : {"inputActivate", "inputDeactivate"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* manager = game_.manager();
                auto* input = manager ? dynamic_cast<sim::SimInputManager*>(
                                            manager->findObject(sim::InputManagerId))
                                      : nullptr;
                return input && args.size() == 2 &&
                               input->activateDevice(args[1],
                                                     std::string_view(name) == "inputActivate")
                           ? "True"
                           : "False";
            });
    }
    const auto integer = [](std::string_view value) {
        Int32 result{};
        (void)std::from_chars(value.data(), value.data() + value.size(), result);
        return result;
    };
    for (const auto name : {"ShapeView::setShape", "ShapeView::setSkin"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* view = args.size() == 3 && guiRuntime_
                                 ? dynamic_cast<ShapeView*>(guiRuntime_->findControl(args[1]))
                                 : nullptr;
                if (!view) {
                    return "False";
                }
                if (std::string_view(name) == "ShapeView::setShape") {
                    view->setShapeName(args[2]);
                }
                // Native ShapeViewSetSkin validates the control but its skin call
                // is commented out. FGSkin::set owns the actual player skin view.
                return "True";
            });
    }
    console.addCommand(0,
                       "resetPlayDelegate",
                       [this](Console&, Int32, std::span<const std::string>) -> std::string {
                           showGameplayScores(false);
                           gameplayGuiMode_ = 0;
                           showingScores_ = false;
                           if (auto* commander = guiRuntime_ ? findGameplayControl<TSCommander>(
                                                                   guiRuntime_->canvas.top())
                                                             : nullptr) {
                               commander->setCommandMode(TSCommander::SelectingMode);
                           }
                           return "True";
                       });
    console.addCommand(0,
                       "renderCanvas",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2) {
                               return "False";
                           }
                           auto* manager = game_.manager();
                           auto* canvas =
                               manager ? dynamic_cast<sim::SimCanvas*>(manager->findObject(args[1]))
                                       : nullptr;
                           if (!canvas && guiRuntime_ && guiRuntime_->canvas.getName() == args[1]) {
                               canvas = &guiRuntime_->canvas;
                           }
                           if (!canvas) {
                               return "False";
                           }
                           canvas->renderFrame();
                           return "True";
                       });
    console.addCommand(0,
                       "RegisterStaticTextures",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2) {
                               return "false";
                           }
                           const auto* manager = fearPlugin_->clientGlobals().manager;
                           const auto* canvas = manager ? dynamic_cast<const sim::SimCanvas*>(
                                                              manager->findObject(args[1]))
                                                        : nullptr;
                           if (!canvas && guiRuntime_ && guiRuntime_->canvas.getName() == args[1]) {
                               canvas = &guiRuntime_->canvas;
                           }
                           // Native SimTerrain registers textures only for the old OpenGL surface:
                           // it draws/flips each material once to warm that backend's global cache.
                           // Modern batches own their textures and upload them on submission, so
                           // they require no registration pass (as with native non-OpenGL devices).
                           return canvas ? "true" : "false";
                       });
    for (const auto name : {"issueCommandI", "issueCommand", "issueTargCommand"}) {
        console.addCommand(
            0,
            name,
            [this, name, integer](
                Console&, Int32, std::span<const std::string> args) -> std::string {
                const bool target = std::string_view(name) == "issueTargCommand";
                auto& state = fearPlugin_->serverGlobals();
                if (!state.playerManager || args.size() != (target ? 6U : 7U)) {
                    return "False";
                }
                Point2I waypoint{};
                if (!target) {
                    waypoint = {integer(args[5]), integer(args[6])};
                    if (std::string_view(name) == "issueCommand") {
                        const auto* center =
                            state.manager ? dynamic_cast<MissionCenterPos*>(
                                                state.manager->findObject(MissionCenterPosId))
                                          : nullptr;
                        const auto origin = center ? center->pos : Point2F{1230, 1230},
                                   extent = center ? center->extent : Point2F{1400, 1400};
                        Flt32 x{}, y{};
                        (void)std::from_chars(args[5].data(), args[5].data() + args[5].size(), x);
                        (void)std::from_chars(args[6].data(), args[6].data() + args[6].size(), y);
                        if (extent.x <= 0 || extent.y <= 0) {
                            return "False";
                        }
                        waypoint = {Int32((x - origin.x) * 1023 / extent.x),
                                    Int32((y - origin.y) * 1023 / extent.y)};
                    }
                }
                state.playerManager->issueCommand(integer(args[1]),
                                                  integer(args[2]),
                                                  integer(args[3]),
                                                  args[4],
                                                  waypoint,
                                                  target ? integer(args[5]) : -1);
                return "True";
            });
    }
    console.addCommand(
        0,
        "setCommandStatus",
        [this, integer](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* players = fearPlugin_->serverGlobals().playerManager;
            return args.size() == 4 && players &&
                           players->commandStatusReport(integer(args[1]), integer(args[2]), args[3])
                       ? "True"
                       : "False";
        });
    console.addCommand(
        0,
        "useItem",
        [this, integer](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto& state = fearPlugin_->clientGlobals();
            const auto item = args.size() == 2 ? integer(args[1]) : -1;
            if (!state.playerPSC || !state.dbm || item < 0 ||
                item >= state.dbm->getDataGroupSize(DataBlockManager::ItemDataType)) {
                return "False";
            }
            sim::SimActionEvent action;
            action.action = IDACTION_USEITEM;
            action.fValue = Flt32(item);
            return state.playerPSC->onSimActionEvent(action) ? "True" : "False";
        });
    // Modern RGBA displays and explicit socket connects do not enter the old
    // 8-bit desktop/autodial warning conditions. Keep their script contract.
    for (const auto name : {"GUI::issue8BitWarning", "GUI::issueInternetWarning"}) {
        console.addCommand(
            0, name, [](Console&, Int32, std::span<const std::string>) -> std::string {
                return "True";
            });
    }
    console.addCommand(
        0,
        "setIssueCommand",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* commander =
                guiRuntime_ ? findGameplayControl<TSCommander>(guiRuntime_->canvas.top()) : nullptr;
            Int32 command = -1;
            if (!commander || args.size() != 3) {
                return "False";
            }
            const auto parsed =
                std::from_chars(args[1].data(), args[1].data() + args[1].size(), command);
            if (parsed.ec != std::errc{} || parsed.ptr != args[1].data() + args[1].size()) {
                return "False";
            }
            commander->issueCommand(command, args[2]);
            return "True";
        });
    console.addCommand(
        0,
        "rebuildCommandMap",
        [this](Console&, Int32, std::span<const std::string>) -> std::string {
            auto* commander =
                guiRuntime_ ? findGameplayControl<TSCommander>(guiRuntime_->canvas.top()) : nullptr;
            const auto& state = fearPlugin_->clientGlobals();
            if (!commander || !state.world || !state.manager || !defaultPalette_) {
                return "False";
            }
            const auto* center =
                dynamic_cast<MissionCenterPos*>(state.manager->findObject(MissionCenterPosId));
            return commander->rebuildMap(*state.world,
                                         *defaultPalette_,
                                         center ? center->pos : Point2F{1230, 1230},
                                         center ? center->extent : Point2F{1400, 1400})
                       ? "True"
                       : "False";
        });
    console.addCommand(
        0,
        "Client::toggleCMDObserver",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* observer =
                guiRuntime_ ? findGameplayControl<CMDObserve>(guiRuntime_->canvas.top()) : nullptr;
            auto* commander =
                guiRuntime_ ? findGameplayControl<TSCommander>(guiRuntime_->canvas.top()) : nullptr;
            return args.size() == 2 && observer &&
                           observer->activate(script::toBool(args[1]),
                                              commander ? commander->selectedPlayerId() : -1)
                       ? "True"
                       : "False";
        });
    console.addCommand(
        0,
        "Client::cmdObservePlayer",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* observer =
                guiRuntime_ ? findGameplayControl<CMDObserve>(guiRuntime_->canvas.top()) : nullptr;
            Int32 player = -1;
            if (!observer || args.size() != 2) {
                return "False";
            }
            const auto parsed =
                std::from_chars(args[1].data(), args[1].data() + args[1].size(), player);
            return parsed.ec == std::errc{} && observer->observePlayer(player) ? "True" : "False";
        });
    console.addCommand(
        0,
        "addGameServer",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* delegate = fearPlugin_->clientGlobals().csDelegate;
            if (!delegate || args.size() < 6 || args.size() > 11) {
                return "False";
            }
            ServerListCtrl::ServerInfo server;
            server.transportAddress = args[1];
            server.name = args[2];
            server.version = args[3];
            (void)std::from_chars(args[4].data(), args[4].data() + args[4].size(), server.pingTime);
            server.favorite = args[5] == "TRUE" || args[5] == "true" || args[5] == "1";
            if (args.size() > 6) {
                server.modType = args[6];
            }
            if (args.size() > 7) {
                server.missionName = args[7];
            }
            if (args.size() > 8) {
                server.missionType = args[8];
            }
            if (args.size() > 9) {
                (void)std::from_chars(
                    args[9].data(), args[9].data() + args[9].size(), server.cpuSpeed);
            }
            if (args.size() > 10) {
                server.neverPing = args[10] == "TRUE" || args[10] == "true" || args[10] == "1";
            }
            delegate->addBrowserServer(server);
            (void)delegate->setBrowserPreference(
                server.transportAddress, server.favorite, server.neverPing);
            return "True";
        });
    console.addCommand(
        0,
        "rebuildServerList",
        [this](Console& con, Int32, std::span<const std::string>) -> std::string {
            auto* delegate = fearPlugin_->clientGlobals().csDelegate;
            if (!delegate) {
                return "False";
            }
            std::vector<std::string> masters;
            for (auto i : std::views::iota(0, 64)) {
                auto master = con.getVariable("Server::MasterAddressN" + std::to_string(i));
                if (master.empty()) {
                    continue;
                }
                const auto tab = master.find('\t');
                if (tab != master.npos) {
                    master.erase(0, tab + 1);
                }
                if (delegate->delegate()) {
                    master = delegate->delegate()->translateAddress(master, kQueryMasterPort);
                }
                if (!master.empty()) {
                    masters.push_back(std::move(master));
                }
            }
            delegate->refreshBrowser(masters,
                                     con.getVariable("pref::JoinGameMode") == "LAN" ||
                                         con.getVariable("pref::lanOnly") == "TRUE" ||
                                         con.getVariable("pref::lanOnly") == "true");
            return "True";
        });
    for (const auto name : {"queryServer", "pingServer"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console& con, Int32, std::span<const std::string> args) -> std::string {
                auto* delegate = fearPlugin_->clientGlobals().csDelegate;
                const auto address = args.size() > 1 ? args[1] : con.getVariable("Server::Address");
                return delegate && delegate->queryBrowserServer(
                                       address, std::string_view(name) == "queryServer")
                           ? "True"
                           : "False";
            });
    }
    console.addCommand(0,
                       "JoinSelectedServer",
                       [](Console& con, Int32, std::span<const std::string>) -> std::string {
                           const auto address = con.getVariable("Server::Address");
                           if (address.empty()) {
                               return "False";
                           }
                           if (con.isFunction("JoinGame")) {
                               return con.execute("JoinGame").value;
                           }
                           const std::array<std::string, 1> args{address};
                           return con.execute("connect", args).value;
                       });
    console.addCommand(0,
                       "Server::ResortList",
                       [this](Console&, Int32, std::span<const std::string>) -> std::string {
                           auto* list =
                               guiRuntime_
                                   ? findGameplayControl<ServerListCtrl>(guiRuntime_->canvas.top())
                                   : nullptr;
                           if (!list) {
                               return "False";
                           }
                           list->tick(game_.simulationTime());
                           return "True";
                       });
    console.addCommand(
        0,
        "ShapeView::setItem",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* view = args.size() == 3 && guiRuntime_
                             ? dynamic_cast<ShapeView*>(guiRuntime_->findControl(args[1]))
                             : nullptr;
            Int32 item = -1;
            if (!view) {
                return "False";
            }
            const auto parsed =
                std::from_chars(args[2].data(), args[2].data() + args[2].size(), item);
            const auto* dbm = fearPlugin_->clientGlobals().dbm;
            const auto* data =
                parsed.ec == std::errc{} && parsed.ptr == args[2].data() + args[2].size() && dbm
                    ? dynamic_cast<const ItemData*>(
                          dbm->lookupDataBlock(item, DataBlockManager::ItemDataType))
                    : nullptr;
            if (!data) {
                return "False";
            }
            // The authored ItemData shapeFile is a stem. Native ShapeViewSetItem
            // adds .dts before loading the inventory preview.
            view->setShapeName(data->fileName + ".dts");
            return "True";
        });
    console.addCommand(
        0,
        "FGSkin::set",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* skin = args.size() == 4 && guiRuntime_
                             ? dynamic_cast<SkinView*>(guiRuntime_->findControl(args[1]))
                             : nullptr;
            if (!skin) {
                return "False";
            }
            skin->setSkin(args[2], args[3] == "MALE" || args[3] == "Male" || args[3] == "male");
            return "True";
        });
    console.addCommand(0,
                       "FGSkin::cycleArmor",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* skin =
                               args.size() == 2 && guiRuntime_
                                   ? dynamic_cast<SkinView*>(guiRuntime_->findControl(args[1]))
                                   : nullptr;
                           if (!skin) {
                               return "False";
                           }
                           skin->cycleArmor();
                           return "True";
                       });
    console.addCommand(0, "Client::exitLobbyMode", [this](Console& con, Int32, auto) {
        const std::array<std::string, 2> args{"2048", "PlayMode"};
        (void)con.execute("remoteEval", args);
        // The server may already be in play mode and send no GUI change.
        // Leaving the local lobby must restore the play screen immediately.
        return std::string(setGameplayGuiMode(1) ? "True" : "False");
    });
    console.addCommand(0,
                       "CmdInventory::getVisibleSet",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           auto* inventory = args.size() == 2 && guiRuntime_
                                                 ? dynamic_cast<FearGuiInventory*>(
                                                       guiRuntime_->findControl(args[1]))
                                                 : nullptr;
                           if (!inventory) {
                               return {};
                           }
                           return inventory->getVisibleSetString();
                       });
    console.addCommand(0,
                       "CmdInventory::setFavorites",
                       [](Console& con, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() != 2) {
                               return "False";
                           }
                           con.setVariable("OpenT1::InventoryFavorites", args[1]);
                           return "True";
                       });
    console.addCommand(0,
                       "saveActionMap",
                       [this](Console&, Int32, std::span<const std::string> args) -> std::string {
                           if (args.size() < 3) {
                               return "False";
                           }
                           auto script = inputMaps_.script(args.subspan(2));
                           if (!script) {
                               return "False";
                           }
                           auto name = std::filesystem::path(args[1]);
                           if (name.empty() || name.is_absolute() || name.has_root_name()) {
                               return "False";
                           }
                           for (const auto& part : name) {
                               if (part == "..") {
                                   return "False";
                               }
                           }
                           if (name.extension().empty()) {
                               name += ".cs";
                           }
                           const auto path = config_.writeRoot / name;
                           std::error_code error;
                           if (path.has_parent_path()) {
                               std::filesystem::create_directories(path.parent_path(), error);
                               if (error) {
                                   return "False";
                               }
                           }
                           std::ofstream out(path, std::ios::binary | std::ios::trunc);
                           out << *script;
                           out.close();
                           return out ? "True" : "False";
                       });
    for (const auto name : {"Client::getMouseSensitivity",
                            "Client::getMouseXAxisFlip",
                            "Client::getMouseYAxisFlip"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                const auto map = args.size() == 2 ? inputMaps_.find(args[1]) : nullptr;
                const bool sensitivity = std::string_view(name) == "Client::getMouseSensitivity";
                const auto axis = std::string_view(name) == "Client::getMouseYAxisFlip"
                                      ? sim::InputYAxis
                                      : sim::InputXAxis;
                if (map) {
                    for (const auto& node : map->nodes()) {
                        if (node.event.deviceType == sim::InputMouse &&
                            node.event.objType == axis) {
                            return sensitivity ? std::to_string(node.action.scale)
                                   : (node.flags & sim::ActionFlip) ? "False"
                                                                    : "True";
                        }
                    }
                }
                return sensitivity ? "0.001" : "False";
            });
    }
    console.addCommand(
        0, "OpenT1::videoModes", [](Console&, Int32, std::span<const std::string>) -> std::string {
            return "640x480";
        });
    for (const auto name : {"isGfxDriver", "setWindowedDevice", "setFullscreenDevice"}) {
        console.addCommand(
            0, name, [name](Console& con, Int32, std::span<const std::string> args) -> std::string {
                if (args.size() != 3 ||
                    (args[2] != "Software" &&
                     (args[2] != "Hardware" ||
                      con.getVariable("OpenT1::HardwareAvailable") != "True"))) {
                    return "False";
                }
                if (std::string_view(name) == "setWindowedDevice") {
                    con.setVariable("pref::VideoWindowedDriver", args[2]);
                }
                if (std::string_view(name) == "setFullscreenDevice") {
                    con.setVariable("pref::VideoFullScreenDriver", args[2]);
                }
                return "True";
            });
    }
    for (const auto name : {"GUI::isIn8BitMode", "isVirtualFS"}) {
        console.addCommand(
            0, name, [](Console&, Int32, std::span<const std::string>) -> std::string {
                return "False";
            });
    }
    for (const auto name :
         {"FGMasterList::clear", "FGMasterList::addEntry", "FGMasterList::setSelected"}) {
        console.addCommand(
            0,
            name,
            [this, name](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* list = args.size() > 1 && guiRuntime_
                                 ? dynamic_cast<FGMasterList*>(guiRuntime_->findControl(args[1]))
                                 : nullptr;
                if (!list) {
                    return "False";
                }
                if (std::string_view(name) == "FGMasterList::clear") {
                    list->clearMasters();
                    return "True";
                }
                if (std::string_view(name) == "FGMasterList::addEntry" && args.size() == 5) {
                    list->addMasterServer(args[2], args[3]);
                    return "True";
                }
                if (std::string_view(name) == "FGMasterList::setSelected" && args.size() == 3) {
                    int row{};
                    auto r = std::from_chars(args[2].data(), args[2].data() + args[2].size(), row);
                    return r.ec == std::errc{} && r.ptr == args[2].data() + args[2].size() &&
                                   list->select({0, row})
                               ? "True"
                               : "False";
                }
                return "False";
            });
    }
    game_.console().addCommand(
        0,
        "FGSlider::setDiscretePositions",
        [this](Console&, Int32, std::span<const std::string> args) -> std::string {
            auto* slider = args.size() == 3 && guiRuntime_
                               ? dynamic_cast<FGSlider*>(guiRuntime_->findControl(args[1]))
                               : nullptr;
            int count{};
            if (!slider) {
                return "False";
            }
            const auto r = std::from_chars(args[2].data(), args[2].data() + args[2].size(), count);
            if (r.ec != std::errc{} || r.ptr != args[2].data() + args[2].size() || count < 0 ||
                count > 65536) {
                return "False";
            }
            slider->setDiscretePositions(count);
            return "True";
        });
    for (const auto command : {"FGCombo::clear",
                               "FGCombo::addEntry",
                               "FGCombo::deleteEntry",
                               "FGCombo::setSelected",
                               "FGCombo::getSelected",
                               "FGCombo::findEntry",
                               "FGCombo::selectNext",
                               "FGCombo::selectPrev",
                               "FGTextList::sort"}) {
        game_.console().addCommand(
            0,
            command,
            [this, command](Console&, Int32, std::span<const std::string> args) -> std::string {
                auto* control =
                    guiRuntime_ && args.size() >= 2 ? guiRuntime_->findControl(args[1]) : nullptr;
                if (std::string_view(command) == "FGTextList::sort") {
                    auto* list = dynamic_cast<gui::TextList*>(control);
                    if (!list) {
                        return "False";
                    }
                    list->sort();
                    return "True";
                }
                auto* combo = dynamic_cast<FGComboBox*>(control);
                if (!combo) {
                    return "False";
                }
                const std::string_view operation(command);
                if (operation == "FGCombo::selectNext" || operation == "FGCombo::selectPrev") {
                    return combo->selectAdjacent(operation == "FGCombo::selectNext" ? 1 : -1)
                               ? "True"
                               : "False";
                }
                if (operation == "FGCombo::clear") {
                    combo->clearEntries();
                    return "True";
                }
                if (operation == "FGCombo::getSelected") {
                    return std::to_string(combo->getSelectedEntry());
                }
                if (operation == "FGCombo::findEntry" && args.size() == 3) {
                    return std::to_string(combo->findEntry(args[2]));
                }
                if (args.size() < 3) {
                    return "False";
                }
                Int32 id{};
                const auto& text =
                    operation == "FGCombo::addEntry" && args.size() == 4 ? args[3] : args[2];
                const auto result = std::from_chars(text.data(), text.data() + text.size(), id);
                if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                    return "False";
                }
                if (operation == "FGCombo::addEntry" && args.size() == 4) {
                    combo->addEntry(args[2], id);
                } else if (operation == "FGCombo::setSelected") {
                    combo->setSelectedEntry(id);
                } else if (operation == "FGCombo::deleteEntry") {
                    combo->deleteEntry(id);
                } else {
                    return "False";
                }
                return "True";
            });
    }
}
} // namespace opent1::game
