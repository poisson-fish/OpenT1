#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <cmath>

using namespace opent1;
using namespace opent1::game;

namespace {
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
    plugin.clientGlobals().playerManager->onChatMessage =
        [&](Int32, std::string_view message) { messages.emplace_back(message); };
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

void thrownItems() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createServerWorld({}, true));
    auto& globals = app.fearPlugin().serverGlobals();
    auto& console = app.console();
    CHECK(console.evaluate(R"(
        PlayerData ThrowArmor { mass = 9; boxWidth = 0.5; boxDepth = 0.5; boxNormalHeight = 2.3; };
        ItemData ThrowRock { mass = 2; };
        newObject(ThrowPlayer, Player, ThrowArmor);
        newObject(ThrowRockObject, Item, ThrowRock, 1, false);
        function Item::onCollision(%this, %object) { $PathHit = %object; $PathHits++; }
    )").ok());
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
        recipientMessages();
        chatReentry();
        thrownItems();
    });
}

OPENT1_REGISTER_TEST("game", "rpg_paths", opent1_test_rpg_paths);
