#include <opent1/console/console.hpp>
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/game/chat_menu.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/input_handler.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_collision.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/world/ts_shape.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void checkImpl(bool cond, std::string_view expr, std::string_view file, int line) {
    if (!cond) {
        std::cerr << std::format("FAIL [{}:{}]: {}\n", file, line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) checkImpl(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

[[nodiscard]] bool approxEqual(float a, float b, float eps = 1e-4f) noexcept {
    return std::fabs(a - b) <= eps;
}

[[nodiscard]] bool
approxEqualPoint(const opent1::Point3F& a, const opent1::Point3F& b, float eps = 1e-4f) noexcept {
    return approxEqual(a.x, b.x, eps) && approxEqual(a.y, b.y, eps) && approxEqual(a.z, b.z, eps);
}

[[nodiscard]] bool
approxEqualBox(const opent1::Box3F& a, const opent1::Box3F& b, float eps = 1e-4f) noexcept {
    return approxEqualPoint(a.fMin, b.fMin, eps) && approxEqualPoint(a.fMax, b.fMax, eps);
}

class BinaryReader {
public:
    explicit BinaryReader(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    [[nodiscard]] bool readRaw(void* dst, std::size_t len) noexcept {
        if (pos_ + len > data_.size()) {
            return false;
        }
        if (len > 0) {
            std::memcpy(dst, data_.data() + pos_, len);
        }
        pos_ += len;
        return true;
    }

    [[nodiscard]] std::uint32_t readU32() {
        std::uint32_t v = 0;
        CHECK(readRaw(&v, sizeof(v)));
        return v;
    }

    [[nodiscard]] std::int32_t readI32() {
        std::int32_t v = 0;
        CHECK(readRaw(&v, sizeof(v)));
        return v;
    }

    [[nodiscard]] float readF32() {
        float v = 0.0f;
        CHECK(readRaw(&v, sizeof(v)));
        return v;
    }

    [[nodiscard]] opent1::Point3F readPoint3F() {
        const float x = readF32();
        const float y = readF32();
        const float z = readF32();
        return opent1::Point3F{x, y, z};
    }

    [[nodiscard]] opent1::Box3F readBox3F() {
        const auto fMin = readPoint3F();
        const auto fMax = readPoint3F();
        return opent1::Box3F{fMin, fMax};
    }

    [[nodiscard]] std::string readStr() {
        const auto len = readU32();
        std::string s(len, '\0');
        if (len > 0) {
            CHECK(readRaw(s.data(), len));
        }
        return s;
    }

    [[nodiscard]] std::vector<std::uint8_t> readBytes() {
        const auto len = readU32();
        std::vector<std::uint8_t> out(len);
        if (len > 0) {
            CHECK(readRaw(out.data(), len));
        }
        return out;
    }

    [[nodiscard]] bool atEnd() const noexcept {
        return pos_ == data_.size();
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return pos_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return data_.size();
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t pos_{0};
};

template <typename PackFn>
[[nodiscard]] std::pair<std::uint32_t, std::vector<std::uint8_t>> captureBits(PackFn&& fn) {
    std::array<std::byte, 4096> buf{};
    opent1::BitStream bs(std::span<std::byte>(buf.data(), buf.size()));
    fn(bs);
    const auto bitPos = static_cast<std::uint32_t>(bs.getCurPos());
    const auto byteLen = static_cast<std::size_t>((bitPos + 7u) >> 3u);
    std::vector<std::uint8_t> out(byteLen);
    if (byteLen > 0) {
        std::memcpy(out.data(), buf.data(), byteLen);
    }
    return {bitPos, std::move(out)};
}

template <typename WriteFn>
[[nodiscard]] std::vector<std::uint8_t> capturePersistBytes(WriteFn&& fn) {
    std::array<std::byte, 2048> buf{};
    opent1::MemRWStream ms(std::span<std::byte>(buf.data(), buf.size()));
    fn(ms);
    const auto pos = static_cast<std::size_t>(ms.getPosition());
    std::vector<std::uint8_t> out(pos);
    if (pos > 0) {
        std::memcpy(out.data(), buf.data(), pos);
    }
    return out;
}

[[nodiscard]] std::span<const std::byte> asByteSpan(const std::vector<std::uint8_t>& v) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.data()), v.size());
}

void verifyFixtureParity(const std::filesystem::path& fixturePath) {
    std::ifstream in(fixturePath, std::ios::binary | std::ios::ate);
    CHECK(in.good());
    if (!in.good()) {
        return;
    }
    const auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> raw(sz);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz));
    CHECK(in.good());

    BinaryReader r(std::move(raw));
    std::array<char, 4> magic{};
    CHECK(r.readRaw(magic.data(), 4));
    CHECK(std::string_view(magic.data(), 4) == "T1PL");
    CHECK(r.readU32() == 1u);

    using namespace opent1;
    using namespace opent1::game;

    // =========================================================================
    // 1. InputHandler (InputHandler.cpp)
    // =========================================================================
    {
        InputHandler ih;
        CHECK(approxEqual(ih.value(), r.readF32()));
        CHECK(approxEqual(ih.target(), r.readF32()));
        CHECK((ih.analogStickWaiting() ? 1u : 0u) == r.readU32());
        CHECK(approxEqual(ih.analogStickRange(), r.readF32()));
        CHECK(approxEqual(ih.getInput(), r.readF32()));

        ih.sample(0.65f, sim::ActionDelta);
        CHECK(approxEqual(ih.target(), r.readF32()));
        CHECK(approxEqual(ih.getInput(), r.readF32()));
        CHECK(approxEqual(ih.target(), r.readF32()));
        CHECK(approxEqual(ih.getInput(), r.readF32()));

        ih.sample(1.5f, sim::ActionDigital);
        CHECK(approxEqual(ih.target(), r.readF32()));
        ih.sample(0.0f, sim::ActionDigital);
        CHECK(approxEqual(ih.target(), r.readF32()));
        CHECK(approxEqual(ih.value(), r.readF32()));

        ih.setStickRange(0.25f);
        ih.startStickWait(0.1f);
        ih.sample(0.25f, sim::ActionAnalog);
        CHECK((ih.analogStickWaiting() ? 1u : 0u) == r.readU32());
        CHECK(approxEqual(ih.target(), r.readF32()));

        ih.sample(0.55f, sim::ActionAnalog);
        CHECK((ih.analogStickWaiting() ? 1u : 0u) == r.readU32());
        CHECK(approxEqual(ih.target(), r.readF32()));
    }

    // SDL reports individual events between simulation ticks. Their sum must
    // produce the same rotation as DirectInput's single polled delta.
    {
        InputHandler individual, combined;
        for (int i = 0; i < 40; ++i) {
            individual.sample(.002f, sim::ActionDelta);
        }
        combined.sample(.08f, sim::ActionDelta);
        CHECK(approxEqual(individual.getInput(.032f), combined.getInput(.032f)));
        CHECK(approxEqual(individual.getInput(.032f), 0.f));
    }

    // =========================================================================
    // 2. ChatMenu (ChatMenu.cpp)
    // =========================================================================
    {
        Console con;
        sim::SimManager chatMgr;
        ChatMenu cm;
        cm.bindConsole(&con);
        chatMgr.addObject(cm);
        cm.assignName("ChatMenu");

        CHECK(cm.getMode() == r.readI32());
        const std::array<std::string, 1> args{"Voice Menu"};
        CHECK(cm.processArguments(args));
        CHECK(cm.getHeading() == r.readStr());

        sim::SimInputPriorityQuery prioQ{};
        prioQ.priority = 0;
        const bool handledPrio = cm.processQuery(prioQ);
        CHECK((handledPrio ? 1u : 0u) == r.readU32());
        CHECK(prioQ.priority == r.readI32());

        const std::array<std::string_view, 3> cmdHello{"remoteEval", "2048", "sayHello"};
        const std::array<std::string_view, 3> cmdAttack{"remoteEval", "2048", "attackBase"};
        const bool added1 = cm.addMenuString("v\\Voice\\g\\Global\\h\\Hello", cmdHello);
        const bool added2 = cm.addMenuString("v\\Voice\\a\\Attack", cmdAttack);
        const bool addedBad = cm.addMenuString("v\\DuplicateLeaf", cmdHello);
        CHECK((added1 ? 1u : 0u) == r.readU32());
        CHECK((added2 ? 1u : 0u) == r.readU32());
        CHECK((addedBad ? 1u : 0u) == r.readU32());

        cm.setMode(ChatMenu::OneShot);
        CHECK(cm.getMode() == r.readI32());
        CHECK((cm.processKey('v') ? 1u : 0u) == r.readU32());
        CHECK((cm.curMenu() ? cm.curMenu()->heading : std::string{}) == r.readStr());
        CHECK((cm.processKey('g') ? 1u : 0u) == r.readU32());
        CHECK((cm.curMenu() ? cm.curMenu()->heading : std::string{}) == r.readStr());
        CHECK((cm.processKey('h') ? 1u : 0u) == r.readU32());
        CHECK(cm.getMode() == r.readI32());
        CHECK(((cm.curMenu() == cm.rootMenu()) ? 1u : 0u) == r.readU32());

        chatMgr.unregisterObject(cm);
    }

    // =========================================================================
    // 3. PlayerManager & All 13 Network Events (PlayerManager.cpp)
    // =========================================================================
    {
        Console con;
        con.setVariable("BADWORDS", "shazbot dork");
        std::string msgBuf = "You shazbot, total D-O-R-K!";
        PlayerManager::applyLanguageFilter(msgBuf, con.getVariable("BADWORDS"));
        const std::string expectedFiltered = r.readStr();
        CHECK(msgBuf == expectedFiltered);

        sim::SimManager serverMgr;
        PlayerManager serverPm(true);
        serverPm.bindConsole(&con);
        serverMgr.addObject(serverPm);

        con.setVariable("$Server::teamName0", "Blood Eagle");
        con.setVariable("$Server::teamSkin0", "beagle");
        con.setVariable("$Server::teamName1", "Diamond Sword");
        con.setVariable("$Server::teamSkin1", "dsword");
        serverPm.teamAdded(-1);
        serverPm.teamAdded(0);
        serverPm.teamAdded(1);

        CHECK(serverPm.getNumTeams() == r.readI32());
        CHECK(std::string(serverPm.getTeamSkinBase(0)) == r.readStr());
        CHECK(std::string(serverPm.getTeamSkinBase(1)) == r.readStr());

        const Int32 cid1 = serverPm.clientAdded("Alpha", "male1", "beagle", false, 2049);
        const Int32 cid2 = serverPm.clientAdded("Bravo", "female1", "beagle", true, 2050);
        CHECK(cid1 == 2049);
        CHECK(cid2 == 2050);
        auto* c1 = serverPm.findClient(2049);
        auto* c2 = serverPm.findClient(2050);
        CHECK(c1 != nullptr);
        CHECK(c2 != nullptr);
        if (c1 && c2) {
            c1->team = 0;
            c1->ping = 48;
            c1->packetLoss = 0.125f;
            c2->team = 0;
            c2->ping = 64;
            c2->packetLoss = 0.0f;
        }

        serverPm.selectCommander(2050, 2049);
        serverPm.buildCommandTrees();
        CHECK(c2->commanderId == r.readI32());
        CHECK((serverPm.isInCommandChain(2050, 2049) ? 1u : 0u) == r.readU32());
        CHECK((serverPm.isInCommandChain(2049, 2050) ? 1u : 0u) == r.readU32());

        const Point2I cmdWp{340, 712};
        serverPm.issueCommand(2049, 2050, 5, "Attack enemy generator~wattgen", cmdWp, 130);
        CHECK(c2->curCommand == r.readI32());
        CHECK(c2->curCommander == r.readI32());
        CHECK(c2->curCommandTarget == r.readI32());
        CHECK(c2->commandStatus == r.readI32());
        CHECK(c2->wayPoint.x == r.readI32());
        CHECK(c2->wayPoint.y == r.readI32());

        const bool acked = serverPm.commandStatusReport(2050, 1, "Acknowledged~wack");
        CHECK((acked ? 1u : 0u) == r.readU32());
        CHECK(c2->commandStatus == r.readI32());

        serverPm.setTeamScoreHeading("Team\tScore");
        serverPm.setClientScoreHeading("Player\tScore\tPing");
        serverPm.setTeamScore(0, "Blood Eagle\t15", 15);
        serverPm.setClientScore(2049, "Alpha\t120\t48", 120);
        serverPm.setTeamObjective(0, 2, "Capture the enemy flag");

        CHECK(serverPm.teamScoreHeading() == r.readStr());
        CHECK(serverPm.clientScoreHeading() == r.readStr());
        const auto* team0 = serverPm.findTeam(0);
        CHECK(team0 != nullptr);
        if (team0) {
            CHECK(team0->scoreString == r.readStr());
            CHECK(team0->scoreSortValue == r.readI32());
        }
        CHECK(c1->scoreString == r.readStr());
        CHECK(c1->scoreSortValue == r.readI32());
        if (team0) {
            CHECK(team0->objectives[2].text == r.readStr());
        }

        // 3c. All 13 Network Events bit-exact pack/unpack
        // 1) MissionResetEvent
        {
            MissionResetEvent ev;
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            MissionResetEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
        }
        // 2) PingPLEvent
        {
            PingPLEvent ev;
            ev.entries.push_back(PingPLEvent::Entry{2049, 48, 0.125f});
            ev.entries.push_back(PingPLEvent::Entry{2050, 64, 0.0f});
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            PingPLEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.entries.size() == 2u);
            if (unpacked.entries.size() == 2u) {
                CHECK(unpacked.entries[0].clientId == 2049);
                CHECK(unpacked.entries[0].ping == 48);
                CHECK(approxEqual(unpacked.entries[0].packetLoss, 0.125f, 0.02f));
                CHECK(unpacked.entries[1].clientId == 2050);
                CHECK(unpacked.entries[1].ping == 64);
                CHECK(approxEqual(unpacked.entries[1].packetLoss, 0.0f, 0.02f));
            }
        }
        // 3) DeltaScoreEvent (team & client)
        {
            DeltaScoreEvent evTeam;
            evTeam.isTeam = true;
            evTeam.id = 0;
            evTeam.sortValue = 15;
            evTeam.score = "Blood Eagle\t15";
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                evTeam.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            DeltaScoreEvent unpackedTeam;
            BitStream rbs1(asByteSpan(bytes));
            CHECK(unpackedTeam.unpack(rbs1));
            CHECK(unpackedTeam.isTeam == true);
            CHECK(unpackedTeam.id == 0);
            CHECK(unpackedTeam.sortValue == 15);
            CHECK(unpackedTeam.score == "Blood Eagle\t15");

            DeltaScoreEvent evClient;
            evClient.isTeam = false;
            evClient.id = 2049;
            evClient.sortValue = 120;
            evClient.score = "Alpha\t120\t48";
            const auto [bits2, bytes2] = captureBits([&](BitStream& bs) {
                evClient.pack(bs);
            });
            CHECK(bits2 == r.readU32());
            CHECK(bytes2 == r.readBytes());

            DeltaScoreEvent unpackedClient;
            BitStream rbs2(asByteSpan(bytes2));
            CHECK(unpackedClient.unpack(rbs2));
            CHECK(unpackedClient.isTeam == false);
            CHECK(unpackedClient.id == 2049);
            CHECK(unpackedClient.sortValue == 120);
            CHECK(unpackedClient.score == "Alpha\t120\t48");
        }
        // 4) PlayerSkinEvent
        {
            PlayerSkinEvent ev(2049, "cphoenix");
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            PlayerSkinEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.clientId == 2049);
            CHECK(unpacked.skinBase == "cphoenix");
        }
        // 5) PlayerSayEvent
        {
            PlayerSayEvent ev;
            ev.sender = 2049;
            ev.msgType = 2;
            ev.message = "Incoming bombers!~wincbomb";
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            PlayerSayEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.sender == 2049);
            CHECK(unpacked.msgType == 2u);
            CHECK(unpacked.message == "Incoming bombers!~wincbomb");
        }
        // 6) PlayerSelectCmdrEvent
        {
            PlayerSelectCmdrEvent ev(2050, 2049);
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            PlayerSelectCmdrEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.peon == 2050);
            CHECK(unpacked.cmdr == 2049);
        }
        // 7) TeamObjectiveEvent
        {
            TeamObjectiveEvent ev(2, "Capture the enemy flag");
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            TeamObjectiveEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.objNum == 2);
            CHECK(unpacked.text == "Capture the enemy flag");
        }
        // 8) PlayerCommandEvent
        {
            PlayerCommandEvent ev;
            ev.commander = 2049;
            ev.target = 2050;
            ev.command = 5;
            ev.commandTarget = 130;
            ev.wayPoint.set(340, 712);
            ev.status = PlayerManager::CMD_RECEIVED;
            ev.statusOnly = false;
            ev.hasMessage = true;
            ev.sendMessageString = true;
            ev.message = "Attack enemy generator~wattgen";
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            PlayerCommandEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.commander == 2049);
            CHECK(unpacked.target == 2050);
            CHECK(unpacked.command == 5);
            CHECK(unpacked.commandTarget == 130);
            CHECK(unpacked.wayPoint.x == 340);
            CHECK(unpacked.wayPoint.y == 712);
            CHECK(unpacked.status == PlayerManager::CMD_RECEIVED);
            CHECK(unpacked.message == "Attack enemy generator~wattgen");
        }
        // 9) PlayerAddEvent
        {
            PlayerAddEvent ev;
            ev.id = 2049;
            ev.isBaseRepType = false;
            ev.name = "Alpha";
            ev.skinBase = "beagle";
            ev.voiceBase = "male1";
            ev.gender = PlayerManager::Male;
            ev.commanderId = 2048;
            ev.team = 0;
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            PlayerAddEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.id == 2049);
            CHECK(unpacked.isBaseRepType == false);
            CHECK(unpacked.name == "Alpha");
            CHECK(unpacked.skinBase == "beagle");
            CHECK(unpacked.voiceBase == "male1");
            CHECK(unpacked.gender == PlayerManager::Male);
            CHECK(unpacked.commanderId == 2048);
            CHECK(unpacked.team == 0);
        }
        // 10) TeamAddEvent
        {
            TeamAddEvent ev;
            ev.energy = 450.5f;
            ev.id = 0;
            ev.name = "Blood Eagle";
            ev.skinBase = "beagle";
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            TeamAddEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(approxEqual(unpacked.energy, 450.5f));
            CHECK(unpacked.id == 0);
            CHECK(unpacked.name == "Blood Eagle");
            CHECK(unpacked.skinBase == "beagle");
        }
        // 11) VoiceEvent
        {
            VoiceEvent ev(2050, " cheer1 ");
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            VoiceEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.sourceId == 2050);
            CHECK(unpacked.soundFileName == " cheer1 ");
        }
        // 12) SoundEvent
        {
            SoundEvent ev(77, 2, 19, nullptr);
            const auto [bits, bytes] = captureBits([&](BitStream& bs) {
                ev.pack(bs);
            });
            CHECK(bits == r.readU32());
            CHECK(bytes == r.readBytes());

            SoundEvent unpacked;
            BitStream rbs(asByteSpan(bytes));
            CHECK(unpacked.unpack(rbs));
            CHECK(unpacked.ghostIndex == 77);
            CHECK(unpacked.channel == 2);
            CHECK(unpacked.soundId == 19);
        }
        // 13) PlayerRemoveEvent & PlayerTeamChangeEvent
        {
            PlayerRemoveEvent evRem(2050);
            const auto [bits1, bytes1] = captureBits([&](BitStream& bs) {
                evRem.pack(bs);
            });
            CHECK(bits1 == r.readU32());
            CHECK(bytes1 == r.readBytes());

            PlayerRemoveEvent unpackedRem;
            BitStream rbs1(asByteSpan(bytes1));
            CHECK(unpackedRem.unpack(rbs1));
            CHECK(unpackedRem.id == 2050);

            PlayerTeamChangeEvent evTc(2049, 1);
            const auto [bits2, bytes2] = captureBits([&](BitStream& bs) {
                evTc.pack(bs);
            });
            CHECK(bits2 == r.readU32());
            CHECK(bytes2 == r.readBytes());

            PlayerTeamChangeEvent unpackedTc;
            BitStream rbs2(asByteSpan(bytes2));
            CHECK(unpackedTc.unpack(rbs2));
            CHECK(unpackedTc.playerId == 2049);
            CHECK(unpackedTc.teamId == 1);
        }

        serverMgr.unregisterObject(serverPm);
    }

    // =========================================================================
    // 4. PlayerMove & PlayerPSC (fearPlayerPSC.cpp)
    // =========================================================================
    DataBlockManager dbm(true);
    auto* playerData = static_cast<PlayerData*>(
        dbm.createDataBlock("LightArmor", DataBlockManager::PlayerDataType));
    CHECK(playerData != nullptr);
    std::array<ItemData*, 8> testItems{};
    std::array<ItemImageData*, 8> testItemImages{};
    for (int i = 0; i < 8; ++i) {
        testItems[static_cast<std::size_t>(i)] = static_cast<ItemData*>(
            dbm.createDataBlock(std::format("TestItem{}", i), DataBlockManager::ItemDataType));
        testItems[static_cast<std::size_t>(i)]->showInventory = true;
        testItemImages[static_cast<std::size_t>(i)] = static_cast<ItemImageData*>(
            dbm.createDataBlock(std::format("TestImg{}", i), DataBlockManager::ItemImageDataType));
    }

    {
        PlayerMove pm1;
        pm1.forwardAction = 0.8f;
        pm1.backwardAction = 0.0f;
        pm1.leftAction = 0.2f;
        pm1.rightAction = 0.6f;
        pm1.jetting = true;
        pm1.jumpAction = false;
        pm1.crouching = false;
        pm1.trigger = true;
        pm1.useItem = 14;
        pm1.pitch = 0.125f;
        pm1.turnRot = -0.25f;

        const auto [pm1FullBits, pm1FullBytes] = captureBits([&](BitStream& bs) {
            pm1.write(bs, true, nullptr);
        });
        CHECK(pm1FullBits == r.readU32());
        CHECK(pm1FullBytes == r.readBytes());

        PlayerMove pm1Read;
        {
            BitStream rbs(asByteSpan(pm1FullBytes));
            CHECK(pm1Read.read(rbs, true, nullptr));
        }
        CHECK(approxEqual(pm1Read.forwardAction, r.readF32()));
        CHECK(approxEqual(pm1Read.backwardAction, r.readF32()));
        CHECK(approxEqual(pm1Read.leftAction, r.readF32()));
        CHECK(approxEqual(pm1Read.rightAction, r.readF32()));
        CHECK((pm1Read.jetting ? 1u : 0u) == r.readU32());
        CHECK((pm1Read.jumpAction ? 1u : 0u) == r.readU32());
        CHECK((pm1Read.crouching ? 1u : 0u) == r.readU32());
        CHECK((pm1Read.trigger ? 1u : 0u) == r.readU32());
        CHECK(pm1Read.useItem == r.readI32());
        CHECK(approxEqual(pm1Read.pitch, r.readF32()));
        CHECK(approxEqual(pm1Read.turnRot, r.readF32()));

        PlayerMove pm2 = pm1Read;
        pm2.trigger = false;
        pm2.useItem = -1;
        pm2.pitch = 0.0f;
        pm2.turnRot = 0.05f;
        const auto [pm2DeltaBits, pm2DeltaBytes] = captureBits([&](BitStream& bs) {
            pm2.write(bs, true, &pm1Read);
        });
        CHECK(pm2DeltaBits == r.readU32());
        CHECK(pm2DeltaBytes == r.readBytes());

        PlayerPSC clientPsc(false);
        PlayerPSC::playerFov = 90.0f;
        clientPsc.setFirstMoveSeq(42);
        clientPsc.pushMove(pm1Read);
        clientPsc.pushMove(pm2);

        UInt32 dummyKey = 0;
        const auto [cpscBits, cpscBytes] = captureBits([&](BitStream& bs) {
            CHECK(clientPsc.writePacket(bs, dummyKey));
        });
        CHECK(cpscBits == r.readU32());
        CHECK(cpscBytes == r.readBytes());

        PlayerPSC serverPsc(true);
        serverPsc.setLastServerMove(42);
        {
            BitStream rbs(asByteSpan(cpscBytes));
            CHECK(serverPsc.readPacket(rbs, 1000));
        }
        CHECK(serverPsc.lastServerMove() == r.readI32());
        CHECK(static_cast<Int32>(serverPsc.moves().size()) == r.readI32());
        CHECK(approxEqual(serverPsc.moves()[0].forwardAction, r.readF32()));
        CHECK(approxEqual(serverPsc.moves()[1].turnRot, r.readF32()));

        serverPsc.setItemBuyList(3);
        serverPsc.setItemShoppingList(5);
        CHECK((serverPsc.isItemShoppingOn(3) ? 1u : 0u) == r.readU32());
        CHECK((serverPsc.isItemBuyOn(3) ? 1u : 0u) == r.readU32());
        CHECK((serverPsc.isItemShoppingOn(5) ? 1u : 0u) == r.readU32());
        CHECK((serverPsc.isItemBuyOn(5) ? 1u : 0u) == r.readU32());
    }

    // =========================================================================
    // 5. Player Movement Physics & Inventory State Machine
    // =========================================================================
    {
        playerData->maxEnergy = 100.0f;
        playerData->maxDamage = 100.0f;
        playerData->maxForwardSpeed = 12.0f;
        playerData->maxBackwardSpeed = 9.0f;
        playerData->maxSideSpeed = 10.0f;
        playerData->groundForce = 270.0f;
        playerData->groundTraction = 1.0f;
        playerData->maxJetForwardVelocity = 22.0f;
        playerData->maxJetSideForceFactor = 0.8f;
        playerData->minJetEnergy = 3.0f;
        playerData->jetEnergyDrain = 0.8f;
        playerData->jetForce = 216.0f;
        playerData->jumpImpulse = 110.0f;
        playerData->jumpSurfaceMinDot = 0.2f;
        playerData->mass = 9.0f;
        playerData->drag = 0.1f;
        playerData->density = 1.2f;
        playerData->canCrouch = true;
        playerData->boxWidth = 0.6f;
        playerData->boxDepth = 0.6f;
        playerData->boxNormalHeight = 2.4f;
        playerData->boxCrouchHeight = 1.5f;
        playerData->minDamageSpeed = 15.0f;
        playerData->damageScale = 0.02f;

        // Weapon 0: Standard Blaster/Rifle (uses ammo type 1)
        testItems[0]->imageId = 0;
        testItems[0]->showInventory = true;
        testItems[1]->imageId = -1;
        testItems[1]->showInventory = true;

        *testItemImages[0] = ItemImageData{};
        testItemImages[0]->weaponType = Player::Standard;
        testItemImages[0]->ammoType = 1;
        testItemImages[0]->projectile.type = 1;
        testItemImages[0]->activateTime = 0.064f;
        testItemImages[0]->fireTime = 0.064f;
        testItemImages[0]->reloadTime = 0.064f;
        testItemImages[0]->mass = 1.5f;

        // Weapon 2: Spinning Chaingun (uses ammo type 1)
        testItems[2]->imageId = 1;
        *testItemImages[1] = ItemImageData{};
        testItemImages[1]->weaponType = Player::Spinning;
        testItemImages[1]->ammoType = 1;
        testItemImages[1]->projectile.type = 1;
        testItemImages[1]->activateTime = 0.032f;
        testItemImages[1]->spinUpTime = 0.128f;
        testItemImages[1]->spinDownTime = 0.128f;
        testItemImages[1]->fireTime = 0.064f;
        testItemImages[1]->reloadTime = 0.0f;
        testItemImages[1]->mass = 2.0f;

        // Weapon 3: DiscLauncher (uses ammo type 1)
        testItems[3]->imageId = 2;
        *testItemImages[2] = ItemImageData{};
        testItemImages[2]->weaponType = Player::DiscLauncher;
        testItemImages[2]->ammoType = 1;
        testItemImages[2]->projectile.type = 1;
        testItemImages[2]->activateTime = 0.032f;
        testItemImages[2]->reloadTime = 0.064f;
        testItemImages[2]->spinUpTime = 0.064f;
        testItemImages[2]->fireTime = 0.064f;
        testItemImages[2]->mass = 2.5f;

        // Weapon 4: Sustained Energy Gun (ammoType = -1)
        testItems[4]->imageId = 3;
        *testItemImages[3] = ItemImageData{};
        testItemImages[3]->weaponType = Player::Sustained;
        testItemImages[3]->ammoType = -1;
        testItemImages[3]->projectile.type = 1;
        testItemImages[3]->activateTime = 0.032f;
        testItemImages[3]->fireTime = 0.064f;
        testItemImages[3]->reloadTime = 0.032f;
        testItemImages[3]->minEnergy = 5.0f;
        testItemImages[3]->maxEnergy = 25.0f;
        testItemImages[3]->mass = 1.0f;

        sim::SimManager playerMgr;
        sim::SimDefaultOpenContainer rootContainer;
        rootContainer.setId(sim::RootContainerId);
        playerMgr.addObject(rootContainer);
        dbm.setId(DataBlockManagerId);
        playerMgr.addObject(dbm);

        Player pl;
        pl.bindDataBlockManager(&dbm);
        CHECK(pl.initResources(playerData));
        pl.setId(300);
        playerMgr.addObject(pl);
        pl.setTeam(1);
        pl.setEnergy(80.0f);
        pl.setRechargeRate(8.0f);

        CHECK(approxEqualBox(pl.collisionImage().bbox, r.readBox3F()));
        CHECK(approxEqualBox(pl.collisionImage().crouchBox, r.readBox3F()));
        CHECK(approxEqual(pl.collisionImage().sphere.radius, r.readF32()));
        CHECK(approxEqualPoint(pl.collisionImage().sphere.center, r.readPoint3F()));
        CHECK(approxEqual(pl.getMass(), r.readF32()));

        // Case 5a: Diagonal movement clamp
        PlayerMove moveDiag;
        moveDiag.forwardAction = 1.0f;
        moveDiag.rightAction = 1.0f;
        moveDiag.turnRot = 0.25f;
        moveDiag.pitch = 0.5f;
        pl.setTraction(1.0f);
        pl.setJumpSurfaceLastContact(2);
        pl.updateMove(&moveDiag, true);
        CHECK(approxEqual(pl.forwardAxisMovement(), r.readF32()));
        CHECK(approxEqual(pl.sideAxisMovement(), r.readF32()));
        CHECK(approxEqualPoint(pl.getRot(), r.readPoint3F()));
        CHECK(approxEqual(pl.getViewPitch(), r.readF32()));
        CHECK(approxEqual(pl.getEnergyLevel(), r.readF32()));

        // Case 5b: Jump impulse on sloped surface
        pl.setLinearVelocity(Point3F{0.0f, 0.0f, 0.0f});
        pl.setJumpSurfaceLastContact(2, Point3F{0.0f, 0.4472136f, 0.8944272f});
        PlayerMove moveJump;
        moveJump.forwardAction = 1.0f;
        moveJump.jumpAction = true;
        pl.updateMove(&moveJump, true);
        CHECK(pl.jumpSurfaceLastContact() == r.readI32());
        CHECK(approxEqualPoint(pl.getLinearVelocity(), r.readPoint3F(), 1e-3f));

        // Case 5c: Jetting airborne lateral thrust & possibleJetForces
        pl.setJumpSurfaceLastContact(12, pl.jumpSurfaceNormal());
        PlayerMove moveJet;
        moveJet.forwardAction = 1.0f;
        moveJet.jetting = true;
        pl.updateMove(&moveJet, true);
        CHECK((pl.isJetting() ? 1u : 0u) == r.readU32());
        CHECK(approxEqual(pl.getEnergyLevel(), r.readF32()));
        CHECK(approxEqualPoint(pl.getLinearVelocity(), r.readPoint3F(), 1e-3f));

        std::array<Point3F, 4> jetChoices{};
        const Point3F unitLat{0.0f, 1.0f, 0.0f};
        const Int32 numChoices = pl.possibleJetForces(jetChoices, &unitLat);
        CHECK(numChoices == r.readI32());
        for (Int32 i = 0; i < numChoices; ++i) {
            CHECK(
                approxEqualPoint(jetChoices[static_cast<std::size_t>(i)], r.readPoint3F(), 1e-3f));
        }

        // Case 5d: getDamageLocation across legs, torso, and head 9-noctants
        pl.setCrouching(false);
        TMat3F ident{};
        ident.identity();
        pl.setPosition(ident, true);
        const std::array<Point3F, 6> testPts{
            Point3F{-0.2f, 0.2f, 0.4f},
            Point3F{0.2f, -0.2f, 0.4f},
            Point3F{-0.2f, -0.2f, 1.2f},
            Point3F{0.2f, 0.2f, 1.2f},
            Point3F{-0.4f, -0.4f, 2.0f},
            Point3F{0.0f, 0.4f, 2.0f},
        };
        for (std::size_t i = 0; i < testPts.size(); ++i) {
            const std::string expVert = r.readStr();
            const std::string expQuad = r.readStr();
            std::string_view gotVert{};
            std::string_view gotQuad{};
            pl.getDamageLocation(testPts[i], gotVert, gotQuad);
            CHECK(gotVert == expVert);
            CHECK(gotQuad == expQuad);
        }

        // Case 5e: Weapon slot state machines (Standard, Spinning, DiscLauncher, Sustained)
        CHECK(pl.setItemCount(0, 1));
        CHECK(pl.setItemCount(1, 2));
        CHECK(pl.setItemCount(2, 1));
        CHECK(pl.setItemCount(3, 1));
        CHECK(pl.setItemCount(4, 1));

        CHECK(pl.mountItem(0, 0, 1));
        CHECK(pl.getImageState(0) == r.readI32());
        CHECK(approxEqual(pl.getMass(), r.readF32()));
        pl.updateImageState(0, 0.064f);
        CHECK(pl.getImageState(0) == r.readI32());
        pl.setImageTriggerDown(0);
        CHECK(pl.getImageState(0) == r.readI32());
        CHECK(pl.getItemCount(1) == r.readI32());
        CHECK(pl.getItemImageEntry(0).fireCount == r.readI32());
        pl.setImageTriggerUp(0);
        pl.updateImageState(0, 0.064f);
        CHECK(pl.getImageState(0) == r.readI32());
        pl.updateImageState(0, 0.064f);
        CHECK(pl.getImageState(0) == r.readI32());

        // Spinning weapon on slot 1
        CHECK(pl.mountItem(2, 1, 1));
        CHECK(pl.getImageState(1) == r.readI32());
        pl.updateImageState(1, 0.032f);
        CHECK(pl.getImageState(1) == r.readI32());
        pl.setImageTriggerDown(1);
        CHECK(pl.getImageState(1) == r.readI32());
        CHECK(approxEqual(pl.getItemImageEntry(1).delayTime, r.readF32()));
        pl.updateImageState(1, 0.064f);
        pl.setImageTriggerUp(1);
        CHECK(pl.getImageState(1) == r.readI32());
        CHECK(approxEqual(pl.getItemImageEntry(1).delayTime, r.readF32()));
        pl.setImageTriggerDown(1);
        CHECK(pl.getImageState(1) == r.readI32());
        CHECK(approxEqual(pl.getItemImageEntry(1).delayTime, r.readF32()));
        pl.updateImageState(1, 0.064f);
        CHECK(pl.getImageState(1) == r.readI32());
        CHECK(pl.getItemCount(1) == r.readI32());

        // DiscLauncher on slot 2
        CHECK(pl.setItemCount(1, 1));
        CHECK(pl.mountItem(3, 2, 1));
        pl.updateImageState(2, 0.032f);
        CHECK(pl.getImageState(2) == r.readI32());
        pl.updateImageState(2, 0.064f);
        CHECK(pl.getImageState(2) == r.readI32());
        pl.updateImageState(2, 0.064f);
        CHECK(pl.getImageState(2) == r.readI32());

        // Sustained weapon on slot 3
        pl.setEnergy(50.0f);
        CHECK(pl.mountItem(4, 3, 1));
        pl.updateImageState(3, 0.032f);
        CHECK(pl.getImageState(3) == r.readI32());
        pl.setImageTriggerDown(3);
        CHECK(pl.getImageState(3) == r.readI32());
        pl.updateImageState(3, 0.032f);
        CHECK(approxEqual(pl.getEnergyLevel(), r.readF32()));

        // Case 5f: Player::writePacketData / readPacketData
        pl.setSensorPinged(1);
        pl.setPos(Point3F{15.0f, -25.0f, 40.0f});
        pl.setLinearVelocity(Point3F{3.0f, -4.0f, 1.5f});
        pl.setRot(Point3F{0.0f, 0.0f, 1.2f});
        pl.setViewPitch(0.35f);
        pl.setEnergy(62.5f);
        pl.setTraction(0.75f);
        pl.setCrouching(true);
        pl.setJumpSurfaceLastContact(6);

        const auto [pktDataBits, pktDataBytes] = captureBits([&](BitStream& bs) {
            pl.writePacketData(bs);
        });
        CHECK(pktDataBits == r.readU32());
        CHECK(pktDataBytes == r.readBytes());

        Player plUnpackPkt;
        plUnpackPkt.bindDataBlockManager(&dbm);
        CHECK(plUnpackPkt.initResources(playerData));
        {
            BitStream rbs(asByteSpan(pktDataBytes));
            CHECK(plUnpackPkt.readPacketData(rbs));
        }
        CHECK(plUnpackPkt.getSensorPinged() == 1);
        CHECK(approxEqualPoint(plUnpackPkt.getPos(), pl.getPos()));
        CHECK(approxEqualPoint(plUnpackPkt.getLinearVelocity(), pl.getLinearVelocity()));
        CHECK(approxEqual(plUnpackPkt.getRot().z, pl.getRot().z));
        CHECK(approxEqual(plUnpackPkt.getViewPitch(), pl.getViewPitch()));
        CHECK(approxEqual(plUnpackPkt.getEnergy(), pl.getEnergy()));
        CHECK(plUnpackPkt.isCrouching() == true);
        CHECK(plUnpackPkt.jumpSurfaceLastContact() == 6);

        // Case 5g: Player::packItemImages & Player::packPlayerUpdate
        const UInt32 imgMask = static_cast<UInt32>(Player::VisItemMask) |
                               (1u << (Player::ItemImageMaskBits + 0)) |
                               (1u << (Player::ItemImageMaskBits + 3));
        const auto [imgBits, imgBytes] = captureBits([&](BitStream& bs) {
            pl.packItemImages(bs, imgMask);
        });
        CHECK(imgBits == r.readU32());
        CHECK(imgBytes == r.readBytes());

        Player plUnpackImg;
        plUnpackImg.bindDataBlockManager(&dbm);
        CHECK(plUnpackImg.initResources(playerData));
        {
            BitStream rbs(asByteSpan(imgBytes));
            CHECK(plUnpackImg.unpackItemImages(rbs, false));
        }
        CHECK(plUnpackImg.getItemImageEntry(0).imageId == pl.getItemImageEntry(0).imageId);
        CHECK(plUnpackImg.getItemImageEntry(3).imageId == pl.getItemImageEntry(3).imageId);
        CHECK(plUnpackImg.getItemImageEntry(3).triggerDown == true);

        pl.setDamageLevel(25.0f);
        pl.setServerAnimation(Player::ANIM_JUMPRUN);
        const UInt32 ghostMask =
            static_cast<UInt32>(Player::DamageMask) | static_cast<UInt32>(Player::OrientationMask) |
            static_cast<UInt32>(Player::AnimationMask) | static_cast<UInt32>(Player::InfoMask) |
            static_cast<UInt32>(Player::VisItemMask) | (1u << (Player::ItemImageMaskBits + 0)) |
            (1u << (Player::ItemImageMaskBits + 3));
        const auto [ghostBits, ghostBytes] = captureBits([&](BitStream& bs) {
            CHECK(pl.packPlayerUpdate(ghostMask, bs, nullptr));
        });
        CHECK(ghostBits == r.readU32());
        CHECK(ghostBytes == r.readBytes());

        Player plGhost;
        plGhost.bindDataBlockManager(&dbm);
        CHECK(plGhost.initResources(playerData));
        {
            BitStream rbs(asByteSpan(ghostBytes));
            CHECK(plGhost.unpackPlayerUpdate(rbs, nullptr));
        }
        CHECK(approxEqual(plGhost.getDamageLevel(), (15.0f * 100.0f) / 63.0f, 2.0f));
        CHECK(plGhost.currentAnimation() == Player::ANIM_JUMPRUN);
        CHECK(plGhost.isCrouching() == true);

        // Case 5h: Player::write / read persistence
        pl.setDatFileName("LightArmor");
        Persistent::ClassRegistry reg;
        registerPlayerClass(reg);
        Persistent::StoreContext storeCtx(reg);
        const auto persistBytes = capturePersistBytes([&](StreamIO& s) {
            CHECK(pl.write(s, 0, 0, storeCtx) == Persistent::Error::Ok);
        });
        const auto expectedPersist = r.readBytes();
        CHECK(persistBytes == expectedPersist);

        Player plLoaded;
        plLoaded.bindDataBlockManager(&dbm);
        {
            Persistent::LoadContext loadCtx(reg);
            MemRWStream rms(asByteSpan(persistBytes));
            CHECK(plLoaded.read(rms, 0, 0, loadCtx) == Persistent::Error::Ok);
        }
        CHECK(plLoaded.getTeam() == pl.getTeam());
        CHECK(plLoaded.getDatFileName() == "LightArmor");
        CHECK(approxEqualPoint(plLoaded.getPos(), pl.getPos()));
        CHECK(approxEqualPoint(plLoaded.getRot(), pl.getRot()));

        playerMgr.unregisterObject(pl);
        playerMgr.unregisterObject(dbm);
        playerMgr.unregisterObject(rootContainer);
    }

    CHECK(r.atEnd());
}

void verifyMalformedPacketAndStreamRollback() {
    using namespace opent1;
    using namespace opent1::game;

    DataBlockManager dbm(true);
    auto* playerData = static_cast<PlayerData*>(
        dbm.createDataBlock("LightArmor", DataBlockManager::PlayerDataType));
    CHECK(playerData != nullptr);
    if (playerData) {
        playerData->maxEnergy = 100.0f;
        playerData->maxDamage = 100.0f;
        playerData->boxWidth = 0.5f;
        playerData->boxDepth = 0.5f;
        playerData->boxNormalHeight = 2.3f;
        playerData->boxCrouchHeight = 1.6f;
    }
    for (int i = 0; i < 8; ++i) {
        (void)dbm.createDataBlock(std::format("Item{}", i), DataBlockManager::ItemDataType);
        (void)dbm.createDataBlock(std::format("Img{}", i), DataBlockManager::ItemImageDataType);
    }

    // 1. PlayerMove truncated bit-by-bit rollback
    {
        PlayerMove src;
        src.forwardAction = 0.75f;
        src.rightAction = 0.5f;
        src.jetting = true;
        src.useItem = 9;
        src.pitch = 0.25f;
        src.turnRot = -0.15f;

        const auto [totalBits, bytes] = captureBits([&](BitStream& bs) {
            bs.setCurPos(3);
            src.write(bs, true, nullptr);
        });
        CHECK(totalBits > 10u);

        for (std::uint32_t truncBits = 3; truncBits < totalBits; ++truncBits) {
            BitStream rbs(asByteSpan(bytes));
            rbs.setLimits(static_cast<std::size_t>(truncBits), 0);
            rbs.setCurPos(3);
            PlayerMove dst;
            dst.forwardAction = 0.123f;
            dst.useItem = 42;
            const auto before = dst;
            CHECK(!dst.read(rbs, true, nullptr));
            CHECK(rbs.getCurPos() == 3);
            CHECK(approxEqual(dst.forwardAction, before.forwardAction));
            CHECK(dst.useItem == before.useItem);
        }
    }

    // 2. PlayerPSC client->server and server->client truncated rollback
    {
        PlayerPSC clientPsc(false);
        PlayerMove pm{};
        pm.forwardAction = 1.0f;
        pm.trigger = true;
        clientPsc.pushMove(pm);

        UInt32 key = 0;
        const auto [cBits, cBytes] = captureBits([&](BitStream& bs) {
            CHECK(clientPsc.writePacket(bs, key));
        });
        for (std::uint32_t trunc = 0; trunc < cBits; ++trunc) {
            BitStream rbs(asByteSpan(cBytes));
            rbs.setLimits(static_cast<std::size_t>(trunc), 0);
            PlayerPSC srvPsc(true);
            srvPsc.setLastServerMove(10);
            CHECK(!srvPsc.readPacket(rbs, 1000));
            CHECK(srvPsc.lastServerMove() == 10);
            CHECK(srvPsc.moves().empty());
        }
    }

    // 3. Player::readPacketData, unpackItemImages, unpackPlayerUpdate, and Player::read rollback
    {
        Player pl;
        pl.bindDataBlockManager(&dbm);
        CHECK(pl.initResources(playerData));
        pl.setTeam(2);
        pl.setPos(Point3F{10.0f, 20.0f, 30.0f});
        pl.setEnergy(77.0f);
        pl.setDamageLevel(33.0f);

        const auto [pktBits, pktBytes] = captureBits([&](BitStream& bs) {
            pl.writePacketData(bs);
        });
        for (std::uint32_t trunc = 0; trunc < pktBits; trunc += 7) {
            BitStream rbs(asByteSpan(pktBytes));
            rbs.setLimits(static_cast<std::size_t>(trunc), 0);
            Player target;
            target.bindDataBlockManager(&dbm);
            CHECK(target.initResources(playerData));
            target.setPos(Point3F{1.0f, 2.0f, 3.0f});
            target.setEnergy(55.0f);
            CHECK(!target.readPacketData(rbs));
            CHECK(rbs.getCurPos() == 0);
            CHECK(approxEqualPoint(target.getPos(), Point3F{1.0f, 2.0f, 3.0f}));
            CHECK(approxEqual(target.getEnergy(), 55.0f));
        }

        const UInt32 mask = static_cast<UInt32>(Player::DamageMask) |
                            static_cast<UInt32>(Player::OrientationMask) |
                            static_cast<UInt32>(Player::InfoMask);
        const auto [updBits, updBytes] = captureBits([&](BitStream& bs) {
            CHECK(pl.packPlayerUpdate(mask, bs, nullptr));
        });
        for (std::uint32_t trunc = 0; trunc < updBits; trunc += 5) {
            BitStream rbs(asByteSpan(updBytes));
            rbs.setLimits(static_cast<std::size_t>(trunc), 0);
            Player target;
            target.bindDataBlockManager(&dbm);
            CHECK(target.initResources(playerData));
            target.setTeam(1);
            target.setDamageLevel(12.0f);
            CHECK(!target.unpackPlayerUpdate(rbs, nullptr));
            CHECK(rbs.getCurPos() == 0);
            CHECK(target.getTeam() == 1);
            CHECK(approxEqual(target.getDamageLevel(), 12.0f));
        }

        pl.setDatFileName("LightArmor");
        Persistent::ClassRegistry reg;
        registerPlayerClass(reg);
        Persistent::StoreContext storeCtx(reg);
        const auto persistBytes = capturePersistBytes([&](StreamIO& s) {
            CHECK(pl.write(s, 0, 0, storeCtx) == Persistent::Error::Ok);
        });
        for (std::size_t truncLen = 0; truncLen < persistBytes.size(); ++truncLen) {
            std::span<const std::byte> truncated(
                reinterpret_cast<const std::byte*>(persistBytes.data()), truncLen);
            MemRWStream rms(truncated);
            Persistent::LoadContext loadCtx(reg);
            Player target;
            target.setTeam(2);
            target.setDatFileName("Sentinel");
            CHECK(target.read(rms, 0, 0, loadCtx) != Persistent::Error::Ok);
            CHECK(rms.getPosition() == 0);
            CHECK(target.getTeam() == 2);
            CHECK(target.getDatFileName() == "Sentinel");
        }
    }
}

void verifyFallingAnimationPose() {
    using namespace opent1;
    using namespace opent1::game;

    // Like the armor assets, fall and jet allow the aim track and torso
    // override, while tumble loop keeps the body's authored forward tilt.
    auto shape = std::make_unique<world::TsShape>();
    shape->names = {
        "bounds", "hips", "lowerback", "head", "root", "fall", "jet", "tumble loop", "looks"};
    shape->nodes = {{0, -1, 0, 0, 0}, {1, 0, 3, 0, 0}, {2, 1, 1, 3, 0}, {3, 2, 0, 0, 1}};
    for (Int32 name = 4; name < 9; ++name) {
        shape->sequences.push_back({name, 1, 1.f, 0});
    }
    shape->transforms = {{{}, {}},
                         {{}, {0, 0, 1}},
                         {QuatF(EulerF{.65f, 0, 0}), {}},
                         {QuatF(EulerF{-.35f, 0, 0}), {}}};
    shape->subSequences = {{1, 1, 0}, {2, 1, 1}, {3, 1, 2}, {4, 1, 3}};
    shape->keyframes = {{0, 2, 0x8000}, {0, 3, 0x8000}, {0, 2, 0x8000}, {0, 0, 0x8000}};
    ResourceManager resources;
    CHECK(world::registerTsLoaders(resources));
    CHECK(resources.add<world::TsShape>("falling-player.dts", std::move(shape)));
    PlayerData data;
    data.shape = resources.load<world::TsShape>("falling-player.dts");
    data.animData[Player::ANIM_STAND] = {"root", -1, 1, 7};
    data.animData[Player::ANIM_FALL] = {"fall", -1, 1, 7};
    data.animData[Player::ANIM_JET] = {"jet", -1, 1, 7};
    data.animData[Player::ANIM_TUMBLE_LOOP] = {"tumble loop", -1, 1, 1};

    sim::SimManager manager;
    auto ownedPlayer = std::make_unique<Player>();
    auto& player = *ownedPlayer;
    CHECK(player.setNetFlags(player.netFlags() | sim::SimNetObject::IsGhost));
    CHECK(player.initResources(&data));
    player.setId(500);
    CHECK(manager.addObject(std::move(ownedPlayer)));
    auto ownedPSC = std::make_unique<PlayerPSC>();
    auto& psc = *ownedPSC;
    psc.setId(PlayerPSCId);
    CHECK(manager.addObject(std::move(ownedPSC)));
    psc.setFirstPerson(false);
    psc.setControlObject(&player);
    CHECK(player.hasPlayerControl() && !player.isFirstPersonView());
    psc.setCamDist(2.f);
    psc.setFirstPerson(true);
    for (UInt32 time = 8; time <= 120; time += 8) {
        psc.clientCollectInput(time - 8, time, .008f);
        CHECK(psc.camDist() > 0 && !player.isFirstPersonView());
        world::WorldShapeQuery visible;
        CHECK(player.processQuery(visible) && visible.image);
    }
    psc.clientCollectInput(120, 256, .032f);
    CHECK(psc.camDist() == 0 && player.isFirstPersonView());
    psc.setFirstPerson(false);
    player.setJumpSurfaceLastContact(0);

    const auto expectPose = [&](Int32 animation, Int32 sequence, Flt32 hipPitch) {
        player.updateAnimation(.032f);
        CHECK(player.currentAnimation() == animation);
        CHECK(player.shapeInstance()->threads().front().sequence == sequence);
        const auto pose = player.animationPose();
        CHECK(pose);
        if (pose) {
            // Keep the authored lower-body pose, but aim the upper body upright.
            CHECK(approxEqual(pose->nodeTransforms()[1].m[2][2], std::cos(hipPitch)));
            CHECK(approxEqualPoint(pose->nodeTransforms()[3].p - pose->nodeTransforms()[2].p,
                                   {0, 0, 1}));
        }
    };
    expectPose(Player::ANIM_STAND, 0, 0);
    player.setFalling(true);
    expectPose(Player::ANIM_FALL, 1, .65f);
    player.setJetting(true); // Still descending when the jets engage.
    expectPose(Player::ANIM_JET, 2, -.35f);
    player.setJetting(false);
    expectPose(Player::ANIM_FALL, 1, .65f);
    player.setFalling(false);
    expectPose(Player::ANIM_STAND, 0, 0);

    psc.setControlObject(nullptr);
}

void verifyJumpAnimationPlayback() {
    using namespace opent1;
    using namespace opent1::game;

    // The shipped light armor has jump run, but no jump stand sequence.
    auto shape = std::make_unique<world::TsShape>();
    shape->names = {"bounds", "hips", "root", "run", "jump run", "fall", "jet"};
    shape->nodes = {{0, -1, 0, 0, 0}, {1, 0, 1, 0, 0}};
    shape->sequences = {
        {2, 1, 1.f, 0}, {3, 1, 1.f, 0}, {4, 0, .5f, 0}, {5, 1, 1.f, 0}, {6, 1, 1.f, 0}};
    shape->transforms = {{{}, {}}, {{}, {0, 0, 1}}};
    shape->subSequences = {{2, 2, 0}};
    shape->keyframes = {{0, 0, 0x8000}, {1, 1, 0x8000}};
    ResourceManager resources;
    CHECK(world::registerTsLoaders(resources));
    CHECK(resources.add<world::TsShape>("jumping-player.dts", std::move(shape)));
    PlayerData data;
    data.shape = resources.load<world::TsShape>("jumping-player.dts");
    data.jumpImpulse = 100.f;
    data.maxForwardSpeed = 10.f;
    data.animData[Player::ANIM_STAND] = {"root", -1, 1, 7};
    data.animData[Player::ANIM_RUN] = {"run", -1, 1, 7};
    data.animData[Player::ANIM_JUMP] = {"jump stand", -1, 1, 5};
    data.animData[Player::ANIM_JUMPRUN] = {"jump run", -1, 1, 5};
    data.animData[Player::ANIM_FALL] = {"fall", -1, 1, 7};
    data.animData[Player::ANIM_JET] = {"jet", -1, 1, 7};

    Player player;
    CHECK(player.setNetFlags(player.netFlags() | sim::SimNetObject::IsGhost));
    player.setPlayerControl(true);
    CHECK(player.initResources(&data));
    const auto expectJump = [&](Flt32 position) {
        CHECK(player.currentAnimation() == Player::ANIM_JUMPRUN);
        const auto& thread = player.shapeInstance()->threads().front();
        CHECK(thread.sequence == 2);
        CHECK(approxEqual(thread.position, position));
        const auto pose = player.animationPose();
        CHECK(pose);
        if (pose) {
            CHECK(approxEqual(pose->nodeTransforms()[1].p.z, position));
        }
    };
    for (const bool moving : {false, true}) {
        PlayerMove move{};
        move.forwardAction = moving ? 1.f : 0.f;
        move.jumpAction = true;
        player.setJumpSurfaceLastContact(0);
        player.updateMove(&move, true);
        player.updateAnimation(.032f);
        CHECK(player.getLinearVelocity().z > 0.f);
        expectJump(.064f);
        move.jumpAction = false;
        player.updateMove(&move, true);
        player.updateAnimation(.032f);
        expectJump(.128f);
        player.updateAnimation(.436f);
        expectJump(1.f);
        player.updateAnimation(.032f);
        CHECK(player.currentAnimation() == (moving ? Player::ANIM_RUN : Player::ANIM_STAND));
        player.updateAnimation(.6f);
        CHECK(player.currentAnimation() == (moving ? Player::ANIM_RUN : Player::ANIM_STAND));
    }

    // Ordinary airtime does not manufacture another takeoff animation.
    player.setJumpSurfaceLastContact(100);
    PlayerMove idle{};
    player.updateMove(&idle, true);
    player.updateAnimation(.032f);
    CHECK(player.currentAnimation() == Player::ANIM_STAND);

    // A quick second takeoff restarts the clip, even if the first has not ended.
    PlayerMove jump{};
    jump.jumpAction = true;
    player.setJumpSurfaceLastContact(0);
    player.updateMove(&jump, true);
    player.updateAnimation(.032f);
    player.updateAnimation(.1f);
    player.setJumpSurfaceLastContact(0);
    player.updateMove(&jump, true);
    player.updateAnimation(.032f);
    expectJump(.064f);

    player.setAnimation(Player::ANIM_JUMPRUN);
    player.updateAnimation(.1f);
    expectJump(.2f);
    player.setJetting(true);
    player.updateAnimation(.032f);
    CHECK(player.currentAnimation() == Player::ANIM_JET);
    player.setJetting(false);
    player.setFalling(true);
    player.updateAnimation(.032f);
    CHECK(player.currentAnimation() == Player::ANIM_FALL);
    player.setFalling(false);
    player.updateAnimation(.032f);
    CHECK(player.currentAnimation() == Player::ANIM_STAND);

    // A replicated jump must start (and restart) the same one-shot as a local jump.
    Player sender;
    sender.setAnimation(Player::ANIM_JUMPRUN);
    const auto [bits, bytes] = captureBits([&](BitStream& stream) {
        CHECK(sender.packPlayerUpdate(Player::AnimationMask, stream, nullptr));
    });
    for (int repeat = 0; repeat < 2; ++repeat) {
        BitStream stream(asByteSpan(bytes));
        CHECK(player.unpackPlayerUpdate(stream, nullptr));
        player.updateAnimation(.1f);
        expectJump(.2f);
    }
    // A rejected packet cannot restart the jump already in progress.
    BitStream truncated(asByteSpan(bytes));
    truncated.setLimits(bits - 1, 0);
    CHECK(!player.unpackPlayerUpdate(truncated, nullptr));
    player.updateAnimation(.1f);
    expectJump(.4f);
    player.updateAnimation(.3f);
    expectJump(1.f);
    player.updateAnimation(.032f);
    CHECK(player.currentAnimation() == Player::ANIM_STAND);

    // Missing optional jump assets must not leave the player in a stuck jump.
    player.setAnimation(Player::ANIM_JUMP);
    player.updateAnimation(.032f);
    CHECK(player.currentAnimation() == Player::ANIM_STAND);
}

void verifyRetainedResourcesAndWorldLifetimes() {
    using namespace opent1;
    using namespace opent1::game;

    DataBlockManager dbm(true);
    auto* playerData = static_cast<PlayerData*>(
        dbm.createDataBlock("LightArmor", DataBlockManager::PlayerDataType));
    CHECK(playerData != nullptr);
    if (playerData) {
        playerData->maxEnergy = 100.0f;
        playerData->maxDamage = 100.0f;
        playerData->boxWidth = 0.5f;
        playerData->boxDepth = 0.5f;
        playerData->boxNormalHeight = 2.3f;
        playerData->boxCrouchHeight = 1.6f;
    }

    // Verify ChatMenu valid multi-level hierarchy & command execution
    {
        Console con;
        ChatMenu menu("Options:");
        menu.bindConsole(&con);
        const std::array<std::string_view, 3> cmdHello{"remoteEval", "2048", "sayHello"};
        const std::array<std::string_view, 3> cmdAttack{"remoteEval", "2048", "attackBase"};
        CHECK(menu.addMenuString("vVoice\\gGlobal\\hHello", cmdHello));
        CHECK(menu.addMenuString("vVoice\\aAttack", cmdAttack));
        CHECK(!menu.addMenuString("vDuplicateLeaf", cmdHello));
        menu.setMode(ChatMenu::OneShot);
        CHECK(menu.getMode() == ChatMenu::OneShot);
        CHECK(menu.processKey('v'));
        CHECK(menu.curMenu() != nullptr && menu.curMenu()->heading == "Global");
        CHECK(menu.processKey('g'));
        CHECK(menu.curMenu() != nullptr && menu.curMenu()->heading == "Hello");
        CHECK(menu.processKey('h'));
        CHECK(menu.getMode() == ChatMenu::Inactive);
        CHECK(menu.executedCommands().size() == 1u);
    }

    sim::SimManager mgr;
    auto root = std::make_unique<sim::SimDefaultOpenContainer>();
    root->setId(sim::RootContainerId);
    mgr.addObject(*root);

    auto player = std::make_unique<Player>();
    player->bindDataBlockManager(&dbm);
    CHECK(player->initResources(playerData));
    player->setId(500);
    mgr.addObject(*player);

    // Verify SimCollisionImageQuery returns PlayerCollisionImage with standing vs crouching bbox
    {
        sim::SimCollisionImageQuery collQ{};
        player->setCrouching(false);
        CHECK(player->processQuery(collQ));
        CHECK(collQ.images.size() == 1u);
        CHECK(
            approxEqual(player->collisionImage().bbox.fMax.z - player->collisionImage().bbox.fMin.z,
                        playerData->boxNormalHeight));

        collQ.images.clear();
        player->setCrouching(true);
        CHECK(player->processQuery(collQ));
        CHECK(collQ.images.size() == 1u);
        CHECK(
            approxEqual(player->collisionImage().bbox.fMax.z - player->collisionImage().bbox.fMin.z,
                        playerData->boxCrouchHeight));
    }

    // Mount lifetime notification: deleting mounted vehicle clears Player::getMountObject()
    auto vehicle = std::make_unique<GameBase>();
    vehicle->setId(501);
    mgr.addObject(*vehicle);
    player->setMountObject(vehicle.get(), 1);
    CHECK(player->getMountObject() == vehicle.get());
    CHECK(player->getMountPoint() == 1);

    mgr.unregisterObject(*vehicle);
    vehicle.reset();
    CHECK(player->getMountObject() == nullptr);

    // PlayerPSC control object lifetime notification
    PlayerPSC psc(true);
    psc.setId(502);
    mgr.addObject(psc);
    psc.setControlObject(player.get());
    CHECK(psc.getControlObject() == player.get());

    // Corpse state transition
    player->setDead(true);
    CHECK(player->isDead());
    CHECK((player->getType() & static_cast<UInt32>(CorpseObjectType)) != 0u);

    mgr.unregisterObject(*player);
    player.reset();
    CHECK(psc.getControlObject() == nullptr);

    mgr.unregisterObject(psc);
    mgr.unregisterObject(*root);
}

} // namespace

int opent1_test_game_player() {
    verifyFixtureParity(std::filesystem::path(OPENT1_GAME_PLAYER_FIXTURE));
    verifyMalformedPacketAndStreamRollback();
    verifyFallingAnimationPose();
    verifyJumpAnimationPlayback();
    verifyRetainedResourcesAndWorldLifetimes();

    if (g_failures != 0) {
        std::cerr << std::format("opent1_game_player_tests FAILED with {} failure(s)\n",
                                 g_failures);
        return 1;
    }
    std::cout << "opent1_game_player_tests PASSED\n";
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("game", "game_player", opent1_test_game_player);
