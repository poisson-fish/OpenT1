#include "test_registry.hpp"
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace opent1;
using namespace opent1::game;

namespace {
template <class T> T roundTrip(const T& source) {
    std::array<std::byte, 65536> bytes{};
    BitStream writer{std::span<std::byte>(bytes)};
    source.pack(writer, nullptr);
    CHECK(writer.isValid());
    BitStream reader{std::span<const std::byte>(bytes)};
    reader.setLimits(writer.getCurPos(), 0);
    T result;
    CHECK(result.unpack(reader, nullptr));
    CHECK(reader.getCurPos() == writer.getCurPos());
    return result;
}

void legacyDatablockQuantization() {
    PlayerData armor;
    armor.maxJetSideForceFactor = 1.2f; // Wasteland's MutantArmor.
    armor.maxForwardSpeed = 80;
    armor.maxBackwardSpeed = -16;
    armor.jumpSurfaceMinDot = -.25f;
    const auto decoded = roundTrip(armor);
    // Native writeFloat truncates then writes the low bits, without saturation.
    CHECK(std::abs(decoded.maxJetSideForceFactor - 50.f / 255) < .00001f);
    CHECK(std::abs(decoded.maxForwardSpeed - 254.f / 1023 * 64) < .00001f);
    CHECK(std::abs(decoded.maxBackwardSpeed - 769.f / 1023 * 64) < .00001f);
    CHECK(std::abs(decoded.jumpSurfaceMinDot - 193.f / 255) < .00001f);
    CHECK(armor.maxJetSideForceFactor == 1.2f && armor.maxForwardSpeed == 80);

    SoundData sound;
    sound.priority = 2;
    CHECK(std::abs(roundTrip(sound).priority - 62.f / 63) < .00001f);
    SoundProfileData profile;
    profile.profile.baseVolume = 2;
    profile.profile.coneInsideAngle = 450;
    const auto decodedProfile = roundTrip(profile);
    CHECK(std::abs(decodedProfile.profile.baseVolume - 1022.f / 1023) < .00001f);
    CHECK(std::abs(decodedProfile.profile.coneInsideAngle - 254.f / 1023 * 360) < .0001f);

    armor.maxJetSideForceFactor = std::numeric_limits<float>::quiet_NaN();
    test::rejects([&] {
        (void)roundTrip(armor);
    });
    armor.maxJetSideForceFactor = std::numeric_limits<float>::max();
    test::rejects([&] {
        (void)roundTrip(armor);
    });
}

void installedWastelandLoading() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !std::filesystem::exists(std::filesystem::path(assets) / "RPG/EnemyArmors.cs")) {
        return;
    }
    FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = std::filesystem::temp_directory_path() /
                       std::format("opent1-wasteland-{}",
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
    app.console().setVariable("Server::Port", "0");
    app.console().setVariable("Server::Heartbeats", "false");
    app.console().setVariable("PCFG::Name", "WastelandTest");
    app.console().setVariable("PCFG::Script", "");
    app.console().setVariable("PCFG::Info", "regression-password");
    try {
        CHECK(app.loadMission("rpgmap5"));
        for (int i = 0; i < 12; ++i) {
            CHECK(app.stepFrame((i + 1) * .032));
        }
    } catch (...) {
        if (auto* dbm = app.fearPlugin().serverGlobals().dbm) {
            for (int group = 0; group < DataBlockManager::NumDataTypes; ++group) {
                for (int i = 0; i < dbm->getDataGroupSize(group); ++i) {
                    const auto* block = dbm->lookupDataBlock(i, group);
                    std::array<std::byte, 65536> bytes{};
                    BitStream stream{std::span<std::byte>(bytes)};
                    try {
                        block->pack(stream, dbm);
                    } catch (const std::exception& error) {
                        std::fprintf(stderr,
                                     "Datablock %s: %s\n",
                                     block->dbmName.c_str(),
                                     error.what());
                        if (const auto* armor = dynamic_cast<const PlayerData*>(block)) {
                            std::fprintf(stderr,
                                         "  maxJetSideForceFactor=%g speeds=%g,%g,%g jumpDot=%g\n",
                                         armor->maxJetSideForceFactor,
                                         armor->maxForwardSpeed,
                                         armor->maxBackwardSpeed,
                                         armor->maxSideSpeed,
                                         armor->jumpSurfaceMinDot);
                        }
                    }
                }
            }
        }
        throw;
    }
    CHECK(app.loadedMissionName() == "rpgmap5");
    const auto* armor = dynamic_cast<const PlayerData*>(
        app.fearPlugin().serverGlobals().dbm->lookupDataBlock("MutantArmor"));
    CHECK(armor && armor->maxJetSideForceFactor == 1.2f);
    const auto* clientArmor = dynamic_cast<const PlayerData*>(
        app.fearPlugin().clientGlobals().dbm->lookupDataBlock(armor->dbmIndex,
                                                           DataBlockManager::PlayerDataType));
    CHECK(clientArmor && std::abs(clientArmor->maxJetSideForceFactor - 50.f / 255) < .00001f);
    auto surface = gfx::SoftwareSurface::create(640, 480);
    CHECK(surface && app.renderFrame(*surface));
    std::printf("Wasteland rpgmap5: %zu world surfaces, %zu polygons\n",
                app.lastSceneSurfaceCount(),
                app.lastScenePolygonCount());
    CHECK(app.lastSceneSurfaceCount() > 0 && app.lastScenePolygonCount() > 0);
}
}

int opent1_test_legacy_datablock_quantization() {
    return test::run(legacyDatablockQuantization);
}

int opent1_test_installed_wasteland_loading() {
    return test::run(installedWastelandLoading);
}

OPENT1_REGISTER_TEST("game", "legacy_datablock_quantization", opent1_test_legacy_datablock_quantization);
OPENT1_REGISTER_TEST("wasteland", "installed_loading", opent1_test_installed_wasteland_loading);
