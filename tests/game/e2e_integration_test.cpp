#include <opent1/console/console.hpp>
#include <ranges>
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/game/flier.hpp>
#include <opent1/game/mine.hpp>
#include <opent1/game/ai.hpp>
#include <opent1/game/ban_list.hpp>
#include <opent1/game/chat_inventory_huds.hpp>
#include <opent1/game/chat_menu.hpp>
#include <opent1/game/commander_huds.hpp>
#include <opent1/game/cpu_speed.hpp>
#include <opent1/game/data_block_manager.hpp>
#include <opent1/game/debris.hpp>
#include <opent1/game/decal_manager.hpp>
#include <opent1/game/drop_points.hpp>
#include <opent1/game/edit_camera.hpp>
#include <opent1/game/explosion.hpp>
#include <opent1/game/f_base.hpp>
#include <opent1/game/fear_cs_delegate.hpp>
#include <opent1/game/fear_dynamic_data_plugin.hpp>
#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/fear_plugin.hpp>
#include <opent1/game/file_plugin.hpp>
#include <opent1/game/flying_camera.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/game/hud_controls.hpp>
#include <opent1/game/interior_shape.hpp>
#include <opent1/game/item.hpp>
#include <opent1/game/marker.hpp>
#include <opent1/game/moveable.hpp>
#include <opent1/game/observer_camera.hpp>
#include <opent1/game/part_debris.hpp>
#include <opent1/game/planet.hpp>
#include <opent1/game/player.hpp>
#include <opent1/game/player_manager.hpp>
#include <opent1/game/proj_bullet.hpp>
#include <opent1/game/proj_rocket.hpp>
#include <opent1/game/projectile_factory.hpp>
#include <opent1/game/script_plugin.hpp>
#include <opent1/game/sensor.hpp>
#include <opent1/game/sensor_manager.hpp>
#include <opent1/game/shape_base.hpp>
#include <opent1/game/shell_dialogs.hpp>
#include <opent1/game/sky.hpp>
#include <opent1/game/snowfall.hpp>
#include <opent1/game/sound.hpp>
#include <opent1/game/star_field.hpp>
#include <opent1/game/static_base.hpp>
#include <opent1/game/status_combat_huds.hpp>
#include <opent1/game/team_group.hpp>
#include <opent1/game/trigger.hpp>
#include <opent1/game/turret.hpp>
#include <opent1/game/vehicle.hpp>
#include <opent1/game/version.hpp>
#include <opent1/gfx/bitmap_palette_font.hpp>
#include <opent1/gfx/palette.hpp>
#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/world/world_collision_sim.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
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

[[nodiscard]] bool approxEqual(float a, float b, float eps = 1e-3f) noexcept {
    return std::fabs(a - b) <= eps;
}

[[nodiscard]] std::size_t countNonZeroPixels(const opent1::gfx::SoftwareSurface& surf) noexcept {
    std::size_t nonZero = 0;
    for (const std::byte b : surf.bitmap().pixels) {
        if (b != std::byte{0}) {
            ++nonZero;
        }
    }
    return nonZero;
}

[[nodiscard]] opent1::gfx::Palette makeTestPalette() {
    opent1::gfx::Palette pal;
    pal.palettes.resize(1);
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{256})) {
        const auto c = static_cast<opent1::UInt8>(i);
        pal.palettes[0].colors[i] =
            opent1::gfx::PaletteColor{static_cast<opent1::UInt8>((c * 37u) & 0xFFu),
                                      static_cast<opent1::UInt8>((c * 73u) & 0xFFu),
                                      static_cast<opent1::UInt8>((c * 109u) & 0xFFu),
                                      0};
        pal.palettes[0].identity[i] = c;
    }
    return pal;
}

[[nodiscard]] std::shared_ptr<opent1::gfx::BitmapFont> makeTestFont() {
    auto font = std::make_shared<opent1::gfx::BitmapFont>();
    font->info.flags = opent1::gfx::FontProportional | opent1::gfx::FontBitmap;
    font->info.height = 10;
    font->info.width = 6;
    font->info.baseline = 8;
    font->info.foreColor = 250;
    font->info.backColor = 0;
    font->info.spacing = 1;
    font->lookupOffset = 32;
    font->lookup.assign(96, 0);

    auto sheet = opent1::gfx::Bitmap::create(8, 10, 8);
    if (sheet) {
        std::fill(sheet->pixels.begin(), sheet->pixels.end(), std::byte{200});
        font->sheets.bitmaps.push_back(std::move(*sheet));
    }
    opent1::gfx::CharInfo ci{};
    ci.bitmap = 0;
    ci.x = 0;
    ci.y = 0;
    ci.width = 6;
    ci.height = 8;
    ci.baseline = 7;
    font->characters.push_back(ci);
    return font;
}

void addSyntheticAssets(opent1::ResourceManager& rm) {
    auto shape = std::make_unique<opent1::world::TsShape>();
    shape->bounds.fMin = {-1.0f, -1.0f, 0.0f};
    shape->bounds.fMax = {1.0f, 1.0f, 2.5f};
    shape->center = {0.0f, 0.0f, 1.25f};
    shape->radius = 2.0f;
    shape->names.push_back("root");
    shape->names.push_back("collision");
    shape->names.push_back("power");
    shape->names.push_back("ambient");
    shape->names.push_back("damage");
    opent1::world::TsNode rootNode{};
    rootNode.name = 0;
    rootNode.parent = -1;
    shape->nodes.push_back(rootNode);
    opent1::world::TsDetail det{};
    det.root = 0;
    det.size = 10.0f;
    shape->details.push_back(det);

    auto shapeCopy1 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy2 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy3 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy4 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy5 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy6 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy7 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy8 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy9 = std::make_unique<opent1::world::TsShape>(*shape);
    auto shapeCopy10 = std::make_unique<opent1::world::TsShape>(*shape);

    CHECK(rm.add("larmor.dts", std::move(shape)));
    CHECK(rm.add("disc.dts", std::move(shapeCopy1)));
    CHECK(rm.add("discb.dts", std::move(shapeCopy2)));
    CHECK(rm.add("generator.dts", std::move(shapeCopy3)));
    CHECK(rm.add("elevator_4x4.dts", std::move(shapeCopy4)));
    CHECK(rm.add("radar.dts", std::move(shapeCopy5)));
    CHECK(rm.add("plasma_turret.dts", std::move(shapeCopy6)));
    CHECK(rm.add("mine.dts", std::move(shapeCopy7)));
    CHECK(rm.add("scout.dts", std::move(shapeCopy8)));
    CHECK(rm.add("debris_chunk.dts", std::move(shapeCopy9)));
    CHECK(rm.add("DropPoint.dts", std::move(shapeCopy10)));

    const std::vector<std::byte> wavBytes{
        std::byte{0x52}, std::byte{0x49}, std::byte{0x46}, std::byte{0x46}};
    CHECK(rm.add("fire_disc.wav", std::make_unique<opent1::RawData>(opent1::RawData{wavBytes})));
}

void verifyFullStackEndToEndIntegration() {
    using namespace opent1;
    using namespace opent1::game;

    const auto tempRoot = std::filesystem::temp_directory_path() / "opent1_e2e_integration_test";
    std::error_code ec;
    std::filesystem::create_directories(tempRoot, ec);

    // Write a small startup config script to exercise +exec during FearGameApp::init
    const auto bootScriptPath = tempRoot / "autoexec_e2e.cs";
    {
        std::ofstream out(bootScriptPath, std::ios::binary);
        out << "$E2E::BootScriptLoaded = \"true\";\n";
        out << "$Server::HostName = \"OpenT1 Loopback Host\";\n";
    }

    // Retain handles across world and app teardown to verify lifetime safety
    std::shared_ptr<const world::TsShapeInstance> retainedPlayerShape;
    ShapeBase::RenderImage retainedRenderImage;
    PlayerCollisionImage retainedPlayerCollision;

    {
        FearGameApp::Config cfg{};
        cfg.writeRoot = tempRoot;
        cfg.overrideCpuLoops = 1290;
        cfg.registerDefaultPlugins = true;

        FearGameApp app(cfg);
        addSyntheticAssets(app.resources());

        const std::array<std::string_view, 5> bootArgs{{
            "-dedicated",
            "$pref::PlayerName",
            "TribesWarrior",
            "+exec",
            "autoexec_e2e.cs",
        }};

        // =====================================================================
        // 1. Game Boot & Plugin Registration
        // =====================================================================
        CHECK(app.init(bootArgs));
        CHECK(app.isInitialized());
        CHECK(app.console().getVariable("Fear::Version") == "1.11");
        CHECK(app.console().getVariable("Version") == "1.11");
        CHECK(app.console().getVariable("Server::Dedicated") == "true");
        CHECK(app.console().getVariable("pref::PlayerName") == "TribesWarrior");
        CHECK(app.console().getVariable("E2E::BootScriptLoaded") == "true");
        CHECK(app.console().getVariable("Server::HostName") == "OpenT1 Loopback Host");
        CHECK(app.console().getVariable("cargc") == "6");
        CHECK(app.console().getVariable("cargv0") == "Tribes.exe");
        CHECK(app.console().getVariable("cargv1") == "-dedicated");
        CHECK(!app.console().getVariable("CPU::Vendor").empty());
        CHECK(!app.console().getVariable("CPU::EstimatedSpeed").empty());

        // Verify network ghost registry instantiates all key ghost classes
        const auto& netReg = app.fearPlugin().networkRegistry();
        CHECK(netReg != nullptr);
        if (netReg) {
            CHECK(dynamic_cast<StaticShape*>(netReg->createGhost(StaticShapePersTag).get()) !=
                  nullptr);
            CHECK(dynamic_cast<Moveable*>(netReg->createGhost(FearMoveablePersTag).get()) !=
                  nullptr);
            CHECK(dynamic_cast<Player*>(netReg->createGhost(PlayerPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Item*>(netReg->createGhost(ItemPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Turret*>(netReg->createGhost(FearTurretPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Sensor*>(netReg->createGhost(SensorPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Mine*>(netReg->createGhost(MinePersTag).get()) != nullptr);
            CHECK(dynamic_cast<Vehicle*>(netReg->createGhost(VehiclePersTag).get()) != nullptr);
            CHECK(dynamic_cast<Flier*>(netReg->createGhost(FlierPersTag).get()) != nullptr);
            CHECK(dynamic_cast<RocketDumb*>(netReg->createGhost(RocketPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Bullet*>(netReg->createGhost(BulletPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Sky*>(netReg->createGhost(TribesSkyPersTag).get()) != nullptr);
            CHECK(dynamic_cast<StarField*>(netReg->createGhost(StarFieldPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Planet*>(netReg->createGhost(PlanetPersTag).get()) != nullptr);
            CHECK(dynamic_cast<Snowfall*>(netReg->createGhost(SnowfallPersTag).get()) != nullptr);
            CHECK(dynamic_cast<InteriorShape*>(netReg->createGhost(InteriorShapePersTag).get()) !=
                  nullptr);
            CHECK(dynamic_cast<Trigger*>(netReg->createGhost(TriggerPersTag).get()) != nullptr);
            CHECK(dynamic_cast<ObserverCamera*>(netReg->createGhost(ObserverCamPersTag).get()) !=
                  nullptr);
        }

        // =====================================================================
        // 2. Mission Loading & Server Datablock Preloading
        // =====================================================================
        CHECK(app.console().evaluate("newServer(\"Raindance\", false);").value == "True");
        CHECK(app.console().getVariable("MissionName") == "Raindance");
        CHECK(InteriorShape::sm_missionName == "Raindance");

        auto& sg = app.fearPlugin().serverGlobals();
        CHECK(sg.world != nullptr);
        CHECK(sg.manager != nullptr);
        CHECK(sg.dbm != nullptr);
        CHECK(sg.csDelegate != nullptr);
        CHECK(sg.playerManager != nullptr);
        CHECK(sg.sensorManager != nullptr);
        CHECK(sg.decalManager != nullptr);
        CHECK(sg.aiManager != nullptr);
        CHECK(sg.playerPSC != nullptr);
        CHECK(sg.ghostManager != nullptr);

        // Declare server datablocks across all major gameplay categories via Console script
        constexpr std::string_view kDataBlockScript = R"CS(
            SoundProfileData BaseSoundProfile {
                baseVolume = 0.85;
                minDistance = 20.0;
                maxDistance = 350.0;
            };
            SoundData FireDiscSound {
                wavFileName = "fire_disc.wav";
                profile = BaseSoundProfile;
                priority = 0.75;
            };
            DamageSkinData ArmorDamageSkins {
                bmpName[0] = "dskin0";
                bmpName[1] = "dskin1";
            };
            ExplosionData DiscExp {
                shapeName = "discb.dts";
                soundId = FireDiscSound;
                timeScale = 1.25;
                faceCamera = true;
                hasLight = true;
                lightRange = 18.0;
            };
            DebrisData PlayerDebris {
                shapeName = "debris_chunk.dts";
                mass = 5.0;
                maxTimeout = 4.0;
                spawnedExplosionID = DiscExp;
            };
            RocketData DiscShell {
                bulletShapeName = "discb.dts";
                explosionTag = DiscExp;
                damageValue = 45.0;
                damageType = 2;
                explosionRadius = 12.0;
                kickBackStrength = 150.0;
                muzzleVelocity = 65.0;
                terminalVelocity = 80.0;
                acceleration = 5.0;
                totalTime = 6.0;
                liveTime = 5.5;
                lightRange = 8.0;
            };
            BulletData ChaingunBullet {
                bulletShapeName = "discb.dts";
                explosionTag = DiscExp;
                damageValue = 12.0;
                damageType = 1;
                muzzleVelocity = 120.0;
                totalTime = 2.0;
                liveTime = 2.0;
                tracerLength = 15;
            };
            ItemImageData DiscLauncherImg {
                shapeFile = "disc.dts";
                mountPoint = 0;
                weaponType = 0;
                projectileType = DiscShell;
                ammoType = 0;
                activateTime = 0.032;
                fireTime = 0.064;
                reloadTime = 0.064;
                sfxFire = FireDiscSound;
                sfxActivate = FireDiscSound;
            };
            ItemData DiscAmmo {
                description = "Disc Ammo";
                shapeFile = "discb.dts";
                showInventory = true;
                price = 2;
            };
            ItemData DiscLauncher {
                description = "Disc Launcher";
                shapeFile = "disc.dts";
                imageId = DiscLauncherImg;
                showInventory = true;
                price = 50;
            };
            PlayerData LightArmor {
                shapeFile = "larmor.dts";
                damageSkinData = ArmorDamageSkins;
                debrisId = PlayerDebris;
                explosionId = DiscExp;
                maxEnergy = 100.0;
                maxDamage = 100.0;
                repairRate = 0.2;
                minJetEnergy = 5.0;
                jetEnergyDrain = 0.8;
                jetForce = 240.0;
                maxJetForwardVelocity = 24.0;
                maxJetSideForceFactor = 0.8;
                jumpImpulse = 115.0;
                jumpSurfaceMinDot = 0.2;
                maxForwardSpeed = 14.0;
                maxBackwardSpeed = 10.0;
                maxSideSpeed = 11.0;
                groundForce = 300.0;
                groundTraction = 1.0;
                mass = 9.0;
                drag = 0.1;
                density = 1.2;
                canCrouch = true;
                boxNormalHeight = 2.4;
                boxCrouchHeight = 1.5;
                boxWidth = 0.6;
                boxDepth = 0.6;
            };
            StaticShapeData Generator {
                shapeFile = "generator.dts";
                maxDamage = 200.0;
                maxEnergy = 150.0;
                visibleToSensor = true;
                description = "Generator";
            };
            MoveableData Elevator4x4 {
                shapeFile = "elevator_4x4.dts";
                maxDamage = 500.0;
                speed = 8.0;
                displace = true;
            };
            SensorData PulseSensor {
                shapeFile = "radar.dts";
                maxDamage = 100.0;
                range = 350;
                dopplerVelocity = 25;
                castLOS = false;
                supression = false;
                pinger = true;
                visibleToSensor = true;
                description = "Pulse Sensor";
            };
            TurretData PlasmaTurret {
                shapeFile = "plasma_turret.dts";
                maxDamage = 250.0;
                maxEnergy = 120.0;
                range = 250;
                gunRange = 200.0;
                speed = 3.0;
                reloadDelay = 0.064;
                projectileType = DiscShell;
                visibleToSensor = true;
                description = "Plasma Turret";
            };
            MineData AntipersonnelMine {
                shapeFile = "mine.dts";
                maxDamage = 20.0;
                explosionId = DiscExp;
                damageValue = 65.0;
                explosionRadius = 10.0;
                kickBackStrength = 120.0;
            };
            TriggerData BaseTriggerData {
                rate = 0.064;
            };
            MarkerData DropPointMarkerData {
                fileName = "DropPoint.dts";
            };
            FlierData ScoutFlier {
                shapeFile = "scout.dts";
                maxDamage = 180.0;
                maxEnergy = 150.0;
                mass = 25.0;
                lift = 12.0;
                maxVertical = 18.0;
                accel = 22.0;
                maxSpeed = 35.0;
            };
        )CS";

        (void)app.console().evaluate(kDataBlockScript);
        CHECK(app.console().evaluate("preloadServerDataBlocks();").value == "True");

        // Verify all declared datablocks exist on the server DataBlockManager
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::SoundProfileDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::SoundDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::DamageSkinDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::ExplosionDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::DebrisDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::RocketDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::BulletDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::ItemImageDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::ItemDataType) == 2);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::PlayerDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::StaticShapeDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::MoveableDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::SensorDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::TurretDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::MineDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::TriggerDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::MarkerDataType) == 1);
        CHECK(sg.dbm->getDataGroupSize(DataBlockManager::FlierDataType) == 1);

        // Fix upDiscLauncherImg ammoType index to DiscAmmo (0)
        if (auto* imgData = dynamic_cast<ItemImageData*>(
                sg.dbm->lookupDataBlock("DiscLauncherImg", DataBlockManager::ItemImageDataType))) {
            CHECK(imgData->projectile.type == DataBlockManager::RocketDataType);
            CHECK(imgData->projectile.dataType == 0);
            imgData->ammoType = 0; // DiscAmmo
        }

        // Build MissionGroup hierarchy & mission objects via console script
        constexpr std::string_view kMissionGraphScript = R"CS(
            function StaticShape::onDamage(%this, %type, %value, %pos, %vec, %mom, %object) {
                GameBase::setDamageLevel(%this, GameBase::getDamageLevel(%this) + %value);
                $E2E::LastDamagedObject = %this;
                $E2E::LastDamageAmount = %value;
            }
            function Player::onDamage(%this, %type, %value, %pos, %vec, %mom, %vertical, %quadrant, %object) {
                GameBase::setDamageLevel(%this, GameBase::getDamageLevel(%this) + %value);
                $E2E::LastDamagedPlayer = %this;
            }
            function BaseTriggerData::onEnter(%this, %object) {
                $E2E::TriggerEnteredBy = %object;
            }
            function BaseTriggerData::onContact(%this, %object) {
                $E2E::TriggerContactBy = %object;
            }

            $Server::teamName0 = "Blood Eagle";
            $Server::teamSkin0 = "beagle";
            $Server::teamName1 = "Diamond Sword";
            $Server::teamSkin1 = "dsword";

            newObject("Teams", "SimGroup");
            newObject("team0", "TeamGroup");
            newObject("team1", "TeamGroup");
            newObject("DropPoints", "DropPointGroup");
            newObject("StartDrop1", "DropPointMarker", "DropPointMarkerData");
            GameBase::setPosition("StartDrop1", "100 200 50");
            addToSet("DropPoints", "StartDrop1");
            addToSet("team0", "DropPoints");

            newObject("BaseGenerator", "StaticShape", "Generator");
            GameBase::setPosition("BaseGenerator", "100 212 50");
            GameBase::setTeam("BaseGenerator", 1);
            addToSet("team0", "BaseGenerator");

            newObject("BaseTurret", "Turret", "PlasmaTurret");
            GameBase::setPosition("BaseTurret", "105 205 52");
            GameBase::setTeam("BaseTurret", 0);
            addToSet("team0", "BaseTurret");

            newObject("BaseSensor", "Sensor", "PulseSensor");
            GameBase::setPosition("BaseSensor", "95 195 55");
            GameBase::setTeam("BaseSensor", 0);
            addToSet("team0", "BaseSensor");

            addToSet("Teams", "team0");
            addToSet("Teams", "team1");
            addToSet("MissionGroup", "Teams");

            newObject("MissionSky", "Sky", "0.4", "0.5", "0.7", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15");
            addToSet("MissionGroup", "MissionSky");
            newObject("MissionStars", "StarField");
            addToSet("MissionGroup", "MissionStars");
            newObject("MissionSun", "Planet", "sun.bmp", "45", "35", "true", "true", "0.8", "0.8", "0.7", "0.2", "0.2", "0.25", "2000", "100");
            addToSet("MissionGroup", "MissionSun");
            newObject("MissionSnow", "Snowfall", "0.6", "4.0", "-2.0", "-30.0", "0", "0");
            addToSet("MissionGroup", "MissionSnow");
            newObject("BaseBunker", "InteriorShape", "bunker.dis", "0");
            addToSet("MissionGroup", "BaseBunker");

            newObject("GroundPack", "Item", "DiscLauncher", "1", "false", "true");
            GameBase::setPosition("GroundPack", "102 202 50");
            addToSet("MissionGroup", "GroundPack");

            newObject("GuardMine", "Mine", "AntipersonnelMine");
            GameBase::setPosition("GuardMine", "120 220 50");
            addToSet("MissionGroup", "GuardMine");

            newObject("BaseTrigger", "Trigger", "BaseTriggerData");
            GameBase::setPosition("BaseTrigger", "100 202 50");
            addToSet("MissionGroup", "BaseTrigger");

            newObject("AIPatrolMarker", "Marker", "DropPointMarkerData");
            GameBase::setPosition("AIPatrolMarker", "110 215 50");
            // Native jet preparation requires facing the endpoint while stopped.
            // This airborne synthetic spawn cannot wait to turn before gravity acts.
            GameBase::setRotation("AIPatrolMarker", "0 0 -1.0303768");
            addToSet("MissionGroup", "AIPatrolMarker");
        )CS";

        (void)app.console().evaluate(kMissionGraphScript);

        // Set up a SimPath + Moveable elevator inside team0
        if (auto* team0 =
                dynamic_cast<sim::SimGroup*>(sg.manager->findObject("MissionGroup/Teams/team0"))) {
            auto pathGroup = std::make_unique<sim::SimGroup>();
            pathGroup->assignName("ElevatorGroup");
            auto* pathGroupPtr =
                dynamic_cast<sim::SimGroup*>(sg.manager->addObject(std::move(pathGroup), team0));
            CHECK(pathGroupPtr != nullptr);

            world::PathState pState{};
            pState.looping = false;
            pState.compressed = true;
            pState.waypoints.push_back(world::PathWaypoint{.position = {90.0f, 190.0f, 50.0f}});
            pState.waypoints.push_back(world::PathWaypoint{.position = {90.0f, 190.0f, 65.0f}});
            auto simPath = std::make_unique<world::SimPath>();
            CHECK(simPath->setState(std::move(pState)));
            auto* simPathPtr = dynamic_cast<world::SimPath*>(
                sg.manager->addObject(std::move(simPath), pathGroupPtr));
            CHECK(simPathPtr != nullptr && simPathPtr->publish());

            auto elev = std::make_unique<Moveable>();
            elev->assignName("BaseElevator");
            elev->bindDataBlockManager(sg.dbm);
            if (auto* pm = dynamic_cast<world::SimPathManager*>(
                    sg.manager->findObject(world::PathManagerId))) {
                elev->bindPathManager(pm);
            }
            if (auto* elevData = dynamic_cast<MoveableData*>(
                    sg.dbm->lookupDataBlock("Elevator4x4", DataBlockManager::MoveableDataType))) {
                CHECK(elev->initResources(elevData));
            }
            auto* elevPtr =
                dynamic_cast<Moveable*>(sg.manager->addObject(std::move(elev), pathGroupPtr));
            CHECK(elevPtr != nullptr);
            const auto pid = elevPtr->findPathId();
            CHECK(pid != Moveable::UndefinedPathId);
            elevPtr->setPathId(pid);
            CHECK(elevPtr->setWaypointPosition(0));
            CHECK(elevPtr->moveToWaypoint(1));
        }

        // Verify missionSaveObjectPersist / missionLoadObjectPersist round-trip
        CHECK(app.console()
                  .evaluate("missionSaveObjectPersist(\"BaseSensor\", \"sensor_persist.bin\");")
                  .value == "True");
        CHECK(app.console()
                  .evaluate("missionLoadObjectPersist(\"LoadedSensor\", \"sensor_persist.bin\");")
                  .value == "True");
        CHECK(sg.manager->findObject("LoadedSensor") != nullptr);

        // =====================================================================
        // 3. Client-Server Loopback Session & Datablock/Ghost Replication
        // =====================================================================
        CHECK(app.console().evaluate("newClient();").value == "True");
        CHECK(app.console().evaluate("localConnect();").value == "True");
        CHECK(app.fearPlugin().isConnected());
        CHECK(app.fearPlugin().connectAddress() == "LOOPBACK:28001");
        CHECK(app.fearPlugin().isDataFinished());
        CHECK(app.fearPlugin().downloadedDataBlocks() == 19);
        CHECK(app.console().getVariable("DataBlock::Finished") == "true");

        auto& cg = app.fearPlugin().clientGlobals();
        CHECK(cg.world != nullptr);
        CHECK(cg.manager != nullptr);
        CHECK(cg.dbm != nullptr);
        CHECK(cg.playerManager != nullptr);
        CHECK(cg.sensorManager != nullptr);
        CHECK(cg.decalManager != nullptr);
        CHECK(cg.playerPSC != nullptr);
        CHECK(cg.chatMenu != nullptr);
        CHECK(cg.hudManager != nullptr);

        // Verify client DataBlockManager received all 19 datablocks identically
        const auto* cliArmor = dynamic_cast<const PlayerData*>(
            cg.dbm->lookupDataBlock(0, DataBlockManager::PlayerDataType));
        CHECK(cliArmor != nullptr);
        if (cliArmor) {
            CHECK(approxEqual(cliArmor->maxEnergy, 100.0f));
            CHECK(approxEqual(cliArmor->jetForce, 240.0f));
            CHECK(approxEqual(cliArmor->jumpImpulse, 115.0f));
            CHECK(approxEqual(cliArmor->groundForce, 300.0f));
        }
        const auto* cliRocket = dynamic_cast<const RocketData*>(
            cg.dbm->lookupDataBlock(0, DataBlockManager::RocketDataType));
        CHECK(cliRocket != nullptr);
        if (cliRocket) {
            CHECK(approxEqual(cliRocket->muzzleVelocity, 65.0f));
            CHECK(approxEqual(cliRocket->terminalVelocity, 80.0f));
            CHECK(approxEqual(cliRocket->explosionRadius, 12.0f));
        }
        const auto* cliTurret = dynamic_cast<const TurretData*>(
            cg.dbm->lookupDataBlock(0, DataBlockManager::TurretDataType));
        CHECK(cliTurret != nullptr);
        if (cliTurret) {
            CHECK(cliTurret->iRange == 250);
            CHECK(approxEqual(cliTurret->gunRange, 200.0f));
        }

        // Verify PlayerManager teams and client representative replicated to client
        CHECK(cg.playerManager->getNumTeams() == 2);
        CHECK(cg.playerManager->findTeam(0) != nullptr &&
              cg.playerManager->findTeam(0)->name == "Blood Eagle");
        CHECK(cg.playerManager->findTeam(1) != nullptr &&
              cg.playerManager->findTeam(1)->name == "Diamond Sword");
        CHECK(cg.playerManager->findClient(2049) != nullptr &&
              cg.playerManager->findClient(2049)->name == "PlayerOne");

        // Verify server mission objects are ghosted in the client world
        for (const auto name : {"BaseGenerator", "BaseTurret", "BaseSensor"}) {
            auto* source = sg.manager->findObject(
                std::string("MissionGroup/Teams/team0/") +
                (std::string_view(name) == "StartDrop1" ? "DropPoints/" : "") + name);
            CHECK(source && app.fearPlugin().findGhostForServerObject(source->getId()));
        }
        auto* srvGen = dynamic_cast<StaticShape*>(
            sg.manager->findObject("MissionGroup/Teams/team0/BaseGenerator"));
        CHECK(srvGen != nullptr);
        if (srvGen) {
            auto* cliGen = dynamic_cast<StaticShape*>(
                app.fearPlugin().findGhostForServerObject(srvGen->getId()));
            CHECK(cliGen != nullptr);
            if (cliGen) {
                CHECK(cliGen->isGhost());
                CHECK(approxEqual(cliGen->getPos().x, 100.0f));
                CHECK(approxEqual(cliGen->getPos().y, 212.0f));
                CHECK(approxEqual(cliGen->getPos().z, 50.0f));
            }
        }

        // =====================================================================
        // 4. Player Spawn, Movement, Inventory, Combat, AI, Turret & Triggers
        // =====================================================================
        const std::string spawnedIdStr =
            app.console().evaluate("spawnPlayer(\"LightArmor\", \"0 0 0\", \"0 0 0\");").value;
        CHECK(spawnedIdStr != "-1");
        CHECK(app.console()
                  .evaluate(std::format(
                      "Client::setOwnedObject(2049, {0}); Client::setControlObject(2049, {0});",
                      spawnedIdStr))
                  .ok());
        CHECK(app.console().evaluate("Client::getOwnedObject(2049);").value == spawnedIdStr);
        CHECK(app.console().evaluate("Client::getControlObject(2049);").value == spawnedIdStr);
        CHECK(app.console().evaluate("Client::getObserverCamera(2049);").value != "-1");

        auto* srvPlayer = dynamic_cast<Player*>(sg.playerPSC->getControlObject());
        CHECK(srvPlayer != nullptr);
        if (srvPlayer) {
            // Verify DropPointGroup placement at (100, 200, 50)
            CHECK(approxEqual(srvPlayer->getPos().x, 100.0f));
            CHECK(approxEqual(srvPlayer->getPos().y, 200.0f));
            CHECK(approxEqual(srvPlayer->getPos().z, 50.0f));

            // Equip DiscLauncher + DiscAmmo via script commands
            CHECK(app.console()
                      .evaluate(
                          std::format("Player::setItemCount({}, \"DiscAmmo\", 10);", spawnedIdStr))
                      .value == "True");
            CHECK(app.console()
                      .evaluate(std::format("Player::setItemCount({}, \"DiscLauncher\", 1);",
                                            spawnedIdStr))
                      .value == "True");
            CHECK(app.console()
                      .evaluate(std::format("Player::useItem({}, \"DiscLauncher\");", spawnedIdStr))
                      .value == "True");
            CHECK(app.console()
                      .evaluate(std::format("Player::getMountedItem({}, 0);", spawnedIdStr))
                      .value == "DiscLauncher");
            CHECK(srvPlayer->getImageState(0) == Player::ItemImageEntry::Activate);
            CHECK(app.console()
                      .evaluate(std::format("Player::getNextMountedItem({}, 0);", spawnedIdStr))
                      .value == "-1");
            CHECK(app.console()
                      .evaluate(std::format("Player::setSensorSupression({}, 17.5);", spawnedIdStr))
                      .value == "True");
            CHECK(app.console()
                      .evaluate(std::format("Player::getSensorSupression({});", spawnedIdStr))
                      .value == "17.500000");
            CHECK(approxEqual(srvPlayer->getSupressionField(), 17.5f));
            CHECK(app.console()
                      .evaluate(std::format("Player::setSensorSupression({}, 0);", spawnedIdStr))
                      .value == "True");
            CHECK(app.console()
                      .evaluate(
                          std::format("Player::setDetectParameters({}, 0.027, 0.2);", spawnedIdStr))
                      .value == "True");
            CHECK(app.console().evaluate("Player::blowUp(-999);").value == "True");
        }

        // Native AI spends its first tick selecting a state and only jumps once
        // stationary. Supply a real collision floor for this grounded jump test.
        const std::array<Flt32, 4> botGroundHeights{};
        const std::array<world::TerrainCollisionCell, 1> botGroundCells{};
        auto botGround =
            world::TerrainCollision::prepare(1, 1, 4.f, botGroundHeights, botGroundCells);
        CHECK(botGround.has_value());
        if (botGround) {
            TMat3F groundTransform(EulerF{}, Point3F{108, 213, 50});
            CHECK(sg.manager->addObject(std::make_unique<world::SimWorldCollider>(
                      world::SimTerrainCollisionImage(
                          std::make_shared<const world::TerrainCollision>(std::move(*botGround))),
                      groundTransform)) != nullptr);
        }

        // Spawn an enemy AI bot at AIPatrolMarker and assign waypoint + target directives
        CHECK(app.console().evaluate("focusServer();").value == "True");
        CHECK(app.console()
                  .evaluate("AI::Spawn(\"BotAlpha\", \"LightArmor\", \"110 215 50\", \"0 0 "
                            "-1.0303768\", \"Bot Display\", \"male1\");")
                  .value == "true");
        auto* spawnedBot = sg.aiManager->findAI("BotAlpha");
        CHECK(spawnedBot && spawnedBot->player() && spawnedBot->player()->isAiControlled());
        CHECK(spawnedBot && sg.manager->findObject(spawnedBot->scriptObjectId));
        CHECK(spawnedBot && spawnedBot->player()->getOwnerClient() == spawnedBot->id);
        CHECK(spawnedBot && approxEqual(spawnedBot->player()->getRot().z, -1.0303768f));
        CHECK(app.console()
                  .evaluate("function CaptureBotClient(%id) {$E2E::BotClient=%id;} "
                            "AI::CallWithId(\"BotAlpha\", CaptureBotClient);")
                  .value == "true");
        CHECK(spawnedBot &&
              app.console().getVariable("E2E::BotClient") == std::to_string(spawnedBot->id));
        CHECK(app.console()
                  .evaluate("AI::SetVar(\"BotAlpha\", customModValue, 42); return "
                            "$AI::BotAlpha::customModValue;")
                  .value == "42");
        CHECK(app.console().evaluate("AI::DirectiveGuard(\"BotAlpha\", \"0 0 0\");").value ==
              "false");
        CHECK(app.console().evaluate("$AI::BotAlpha::iq=90; return $AI::BotAlpha::iq;").value ==
              "90.00");
        CHECK(app.console().evaluate("AI::SetVar(\"BotAlpha\", \"iq\", 90);").value == "true");
        CHECK(app.console().evaluate("AI::SetVar(\"BotAlpha\", \"jetNavigation\", 40);").value ==
              "true");
        CHECK(app.console()
                  .evaluate("AI::DirectiveWaypoint(\"BotAlpha\", \"135 230 60\", 0);")
                  .value == "true");
        CHECK(app.console().evaluate("AI::GetCount();").value == "1");

        // The synthetic player has no mesh faces. Keep its origin inside the
        // trigger during the native one-input movement delay so the host's
        // bounds-only fallback is exercised independently of mesh collision.
        if (auto* trigger =
                dynamic_cast<Trigger*>(sg.manager->findObject("MissionGroup/BaseTrigger"))) {
            trigger->setDimensions({2, 2, 2});
        }
        // Queue PlayerMoves on the client PlayerPSC (forward movement + jump + jetting + weapon trigger)
        for (auto step : std::views::iota(0, 6)) {
            PlayerMove mv{};
            mv.forwardAction = 1.0f;
            mv.jumpAction = (step == 0);
            mv.jetting = true;
            mv.turnRot = 0.05f;
            mv.pitch = 0.0f;
            mv.trigger = (step >= 2);
            mv.useItem = -1;
            cg.playerPSC->pushMove(mv);

            CHECK(app.stepFrame(static_cast<double>(step + 1) * 0.032));
        }

        // Verify player moved forward, jetted upward, fired DiscLauncher, consumed ammo, and spawned a Rocket
        if (srvPlayer) {
            CHECK(srvPlayer->getPos().y > 200.0f);
            CHECK(srvPlayer->getEnergy() < 100.0f);
            CHECK(srvPlayer->getItemCount(0) < 10); // DiscAmmo consumed

            // The displayed client pose includes unacknowledged input. Verify
            // the authoritative correction, then replay to the displayed time.
            auto* cliPlayer = dynamic_cast<Player*>(cg.playerPSC->getControlObject());
            CHECK(cliPlayer != nullptr);
            if (cliPlayer) {
                CHECK(cliPlayer->isGhost());
                const auto predictedPosition = cliPlayer->getPos();
                const auto predictedRotation = cliPlayer->getRot();
                CHECK(cliPlayer->getLastProcessTime() == cg.currentTime);
                CHECK(app.fearPlugin().syncLoopbackState(false) > 0);
                CHECK(approxEqual(cliPlayer->getPos().x, srvPlayer->getPos().x, 0.05f));
                CHECK(approxEqual(cliPlayer->getPos().y, srvPlayer->getPos().y, 0.05f));
                CHECK(approxEqual(cliPlayer->getPos().z, srvPlayer->getPos().z, 0.05f));
                cliPlayer->clientProcess(cg.currentTime);
                CHECK((cliPlayer->getPos() - predictedPosition).len() < .0001f);
                CHECK((cliPlayer->getRot() - predictedRotation).len() < .0001f);
                CHECK(cliPlayer->getItemCount(0) == srvPlayer->getItemCount(0));
            }

            // Retain player render/collision handles before teardown
            retainedRenderImage = srvPlayer->renderImage();
            retainedPlayerShape = retainedRenderImage.sharedShapeInstance();
            retainedPlayerCollision = srvPlayer->collisionImage();
        }

        // Verify BaseTrigger fired onEnter when the player moved through (100, 202, 50)
        CHECK(!app.console().getVariable("E2E::TriggerEnteredBy").empty());

        // Verify BaseElevator moved along its SimPath
        if (const auto* elev = dynamic_cast<const Moveable*>(
                sg.manager->findObject("MissionGroup/Teams/team0/ElevatorGroup/BaseElevator"))) {
            CHECK(elev->getWaypointPosition() > 0.0f);
        }

        // Verify AI bot spawned a Player body, moved toward waypoint, and was detected by SensorManager
        const auto* botAi = sg.aiManager->findAI("BotAlpha");
        CHECK(botAi != nullptr);
        if (botAi) {
            CHECK(botAi->player() != nullptr);
            if (botAi->player()) {
                CHECK(botAi->player()->getPos().z > 50.0f);
                CHECK(app.fearPlugin().findGhostForServerObject(botAi->player()->getId()) !=
                      nullptr);
            }
        }
        CHECK(app.console()
                  .evaluate("AI::DirectiveTargetPoint(\"BotAlpha\", \"100 200 50\", 1);")
                  .value == "true");

        // Verify projectile radius damage against BaseGenerator & replication of damage to client ghost
        if (srvGen) {
            Explosion::applyRadiusDamage(sg.manager,
                                         2,
                                         Point3F{100.0f, 210.0f, 50.0f},
                                         Point3F{0.0f, 10.0f, 0.0f},
                                         15.0f,
                                         80.0f,
                                         100.0f,
                                         srvPlayer ? srvPlayer->getId() : 0);
            CHECK(srvGen->getDamageLevel() > 0.0f);
            CHECK(!app.console().getVariable("E2E::LastDamageAmount").empty());

            CHECK(app.stepFrame(0.250));
            const auto* cliGen = dynamic_cast<const StaticShape*>(
                app.fearPlugin().findGhostForServerObject(srvGen->getId()));
            CHECK(cliGen != nullptr);
            if (cliGen) {
                CHECK(cliGen->getDamageLevel() > 0.0f);
            }
        }

        // =====================================================================
        // 5. HUD & Shell GUI Rendering into SoftwareSurface
        // =====================================================================
        {
            auto surfacePtr = gfx::SoftwareSurface::create(640, 480);
            CHECK(surfacePtr != nullptr);
            if (surfacePtr) {
                auto& surface = *surfacePtr;
                auto pal = makeTestPalette();
                InitializePaletteTable(&pal);
                CHECK(surface.setPalette(pal));
                surface.clear(0);
                const auto font = makeTestFont();
                const RectI fullClip{0, 0, 639, 479};

                // 5a. Shell GUI & Dialogs (MainMenuDelegate, PlayDelegate, FearGuiBox, InventoryListCtrl, ScoreListCtrl)
                // The shell border covers the full canvas, including its
                // bitmap-free fill. Paint content above that background.
                MMShellBorder border;
                border.setBitmapName("MainMenuBkgnd.bmp");
                CHECK(border.resize({0, 0}, {640, 480}));
                CHECK(border.draw(surface, {0, 0}, fullClip));
                CHECK(surface.text(*font, {16, 16}, "Starsiege: Tribes"));
                MainMenuDelegate mainMenu;
                CHECK(mainMenu.onMessage(nullptr, IDDLG_QuickStart) == 1U);
                CHECK(mainMenu.activeScreen() == "Connect.gui");

                PlayDelegate playDlg;
                CHECK(playDlg.onMessage(nullptr, IDDLG_MainMenu) == 1U);

                FearGuiBox shellBox;
                CHECK(shellBox.resize({10, 10}, {240, 120}));
                CHECK(shellBox.draw(surface, {10, 10}, fullClip));

                InventoryListCtrl invList;
                CHECK(invList.resize({20, 100}, {220, 140}));
                invList.syncFromPSC(cg.playerPSC, cg.dbm);
                CHECK(!invList.rows().empty());
                CHECK(invList.draw(surface, {20, 100}, fullClip));

                if (cg.playerManager) {
                    cg.playerManager->setTeamScoreHeading("Team Name\t\xFF\x32Score");
                    cg.playerManager->setClientScoreHeading(
                        "Player Name\t\xFF\x32Score\t\xFF\x50Ping");
                    cg.playerManager->setTeamScore(0, "%t\t12", 12);
                    cg.playerManager->setClientScore(2049, "%n\t42\t%p", 42);
                }
                ScoreListCtrl scoreList;
                CHECK(scoreList.resize({260, 100}, {340, 160}));
                if (cg.playerManager) {
                    scoreList.rebuildScores(*cg.playerManager);
                    CHECK(!scoreList.lines().empty());
                }
                CHECK(scoreList.draw(surface, {260, 100}, fullClip));

                const std::size_t shellPixels = countNonZeroPixels(surface);
                CHECK(shellPixels > 0u);

                // 5b. Live HUD rendering driven by client PlayerPSC & loopback session state
                surface.clear(0);

                HealthHud healthHud;
                CHECK(healthHud.resize({8, 8}, {126, 23}));
                if (srvPlayer) {
                    srvPlayer->setDamageLevel(35.0f);
                    (void)app.stepFrame(0.282);
                }
                healthHud.updateFromPSC(cg.playerPSC, false, 100);
                CHECK(approxEqual(healthHud.metrics().energyLevel, 0.65f, 0.05f));
                CHECK(healthHud.draw(surface, {8, 8}, fullClip));

                JetPackHud jetHud;
                CHECK(jetHud.resize({8, 36}, {126, 23}));
                jetHud.updateFromPSC(cg.playerPSC, false);
                CHECK(jetHud.draw(surface, {8, 36}, fullClip));

                CompassHud compassHud;
                CHECK(compassHud.resize({540, 8}, {80, 80}));
                compassHud.updateState(
                    Point3F{100.0f, 200.0f, 10.0f}, 0.5f, Point2F{135.0f, 230.0f}, true, false);
                CHECK(compassHud.state().wayPointActive);
                CHECK(compassHud.draw(surface, {540, 8}, fullClip));

                ClockHud clockHud;
                CHECK(clockHud.resize({280, 8}, {80, 24}));
                clockHud.setClockTime(125.0f);
                CHECK(!clockHud.formattedText().empty());
                CHECK(clockHud.draw(surface, {280, 8}, fullClip));

                CurrentWeaponHud curWeapHud;
                CHECK(curWeapHud.resize({220, 440}, {200, 32}));
                curWeapHud.updateFromPSC(cg.playerPSC, cg.dbm, false);
                CHECK(curWeapHud.draw(surface, {220, 440}, fullClip));

                Crosshair crosshair;
                CHECK(crosshair.resize({304, 224}, {32, 32}));
                const auto* ctrlPlayer = dynamic_cast<const Player*>(
                    cg.playerPSC ? cg.playerPSC->getControlObject() : nullptr);
                const auto ret = Crosshair::computeReticleMetrics({640, 480},
                                                                  22.5f,
                                                                  22.5f,
                                                                  22.5f,
                                                                  true,
                                                                  0,
                                                                  ctrlPlayer,
                                                                  "EnemyTarget",
                                                                  80.0f,
                                                                  Point3F{100.0f, 200.0f, 10.0f});
                crosshair.updateReticle(ret);
                CHECK(crosshair.reticleMetrics().zoomReticle);
                CHECK(crosshair.draw(surface, {304, 224}, fullClip));

                FGHSensorPing sensorPingHud;
                CHECK(sensorPingHud.resize({500, 8}, {28, 28}));
                sensorPingHud.updatePing(1U, 0.25f, false);
                CHECK(sensorPingHud.frameState() != FGHSensorPing::FrameHidden);
                CHECK(sensorPingHud.draw(surface, {500, 8}, fullClip));

                FGLagIndicator lagHud;
                CHECK(lagHud.resize({500, 40}, {28, 28}));
                CHECK(lagHud.evaluateLag(1000, 100, false));
                CHECK(lagHud.isLagging());
                CHECK(lagHud.draw(surface, {500, 40}, fullClip));

                CenterPrint centerPrint;
                CHECK(centerPrint.resize({160, 200}, {320, 48}));
                centerPrint.setMessage("Mission Raindance Active", CenterPrint::Center, 5.0f, 0.0f);
                CHECK(centerPrint.computeLayout({640, 480}, 16, 1.0f).active);
                CHECK(centerPrint.draw(surface, {160, 200}, fullClip));

                FearGuiChatDisplay chatDisplay;
                CHECK(chatDisplay.resize({8, 360}, {300, 64}));
                chatDisplay.newMessage("PlayerOne: Attack the enemy generator!",
                                       FearGuiChatDisplay::MSG_TeamChat);
                CHECK(!chatDisplay.messages().empty());
                CHECK(chatDisplay.draw(surface, {8, 360}, fullClip));

                if (cg.chatMenu) {
                    const std::array<std::string_view, 3> cmdAtt{"remoteEval", "2048", "attackGen"};
                    CHECK(cg.chatMenu->addMenuString("vVoice\\aAttack Generator", cmdAtt));
                    cg.chatMenu->setMode(ChatMenu::Active);
                }
                FGChatMenu chatMenuHud;
                CHECK(chatMenuHud.resize({8, 260}, {200, 80}));
                chatMenuHud.setMenuName("VoiceChatMenu");
                if (cg.chatMenu) {
                    CHECK(!chatMenuHud.buildVisibleRows(*cg.chatMenu).empty());
                }
                CHECK(chatMenuHud.draw(surface, {8, 260}, fullClip));

                TSCommander commanderMap;
                CHECK(commanderMap.resize({400, 260}, {220, 200}));
                commanderMap.setZoomMode(true);
                TSCommander::SensorTargetInfo cmdPlayerUnit{};
                cmdPlayerUnit.datId = 1;
                cmdPlayerUnit.datGroup = DataBlockManager::PlayerDataType;
                cmdPlayerUnit.playerId = 2049;
                cmdPlayerUnit.team = 0;
                cmdPlayerUnit.pos = Point2I{512, 512};
                cmdPlayerUnit.name = "PlayerOne";
                commanderMap.addTarget(cmdPlayerUnit);
                CHECK(commanderMap.targets().size() == 1u);
                CHECK(commanderMap.selectTargetByKey(0));
                CHECK(commanderMap.draw(surface, {400, 260}, fullClip));

                if (cg.hudManager) {
                    cg.hudManager->addHud(&healthHud, 101, 102);
                    cg.hudManager->addHud(&jetHud, 103, 104);
                    cg.hudManager->addHud(&compassHud, 105, 106);
                    CHECK(cg.hudManager->entries().size() == 3u);
                    CHECK(cg.hudManager->draw(surface, {8, 64}, fullClip));
                }

                const std::size_t hudPixels = countNonZeroPixels(surface);
                CHECK(hudPixels > 0u);
            }
        }

        // A native drone death callback runs once with the original name, then
        // the controller is renamed and its bound variables are removed.
        CHECK(app.console()
                  .evaluate("function E2EBotDied(%name) {$E2E::DiedName=%name; $E2E::DiedCount++;} "
                            "AI::CallbackDied(BotAlpha,E2EBotDied);")
                  .value == "true");
        if (auto* dying = sg.aiManager->findAI("BotAlpha"); dying && dying->player()) {
            const auto oldClient = dying->id;
            dying->player()->setDead(true);
            CHECK(app.stepFrame(.346));
            CHECK(app.console().getVariable("E2E::DiedName") == "BotAlpha");
            CHECK(app.console().getVariable("E2E::DiedCount") == "1");
            CHECK(!app.console().hasVariable("AI::BotAlpha::iq"));
            const auto* corpse = sg.aiManager->findByPlayerId(oldClient);
            CHECK(corpse && corpse->name == "Corpse0 BotAlpha");
            CHECK(app.console().evaluate("AI::Spawn(BotAlpha,LightArmor,\"110 215 50\");").value ==
                  "true");
            CHECK(app.console().evaluate("AI::Delete(BotAlpha);").value == "true");
            CHECK(sg.aiManager->findAI("BotAlpha") != nullptr); // deletion finishes next AI tick
            CHECK(app.stepFrame(.378));
            CHECK(sg.aiManager->findAI("BotAlpha") == nullptr);
            CHECK(app.console().getVariable("E2E::DiedCount") == "1");
        }

        // =====================================================================
        // 6. Clean Teardown & Retained Handle Safety
        // =====================================================================
        CHECK(app.console().evaluate("disconnect();").value == "True");
        CHECK(!app.fearPlugin().isConnected());
        CHECK(app.fearPlugin().ghostCount() == 0u);
        CHECK(app.console().evaluate("deleteClient();").value == "True");
        CHECK(app.fearPlugin().clientGlobals().world == nullptr);
        CHECK(app.console().evaluate("deleteServer();").value == "True");
        CHECK(app.fearPlugin().serverGlobals().world == nullptr);
        CHECK(app.console().evaluate("quit();").value == "True");
        CHECK(app.fearPlugin().quitRequested());
        CHECK(!app.stepFrame(1.0));

        app.shutdown();
        CHECK(!app.isInitialized());
    }

    // Verify retained resource & collision handles survive FearGameApp + SimWorld destruction
    CHECK(retainedPlayerShape != nullptr);
    CHECK(retainedRenderImage.shapeInstance() != nullptr);
    CHECK(retainedPlayerCollision.shapeInstance() != nullptr);
    if (retainedPlayerShape && retainedPlayerShape->shape()) {
        CHECK(approxEqual(retainedPlayerShape->shape()->radius, 2.0f));
    }

    // Verify runFearMain end-to-end execution
    constexpr std::array<std::string_view, 4> mainArgs{{
        "-dedicated",
        "$Server::HostName",
        "TribesMainTest",
        "+quit",
    }};
    CHECK(opent1::game::runFearMain(mainArgs) == 0);

    // =========================================================================
    // 6b. Synthetic 3D World Scene + HUD Rendering Verification (No Volumes Required)
    // =========================================================================
    {
        FearGameApp::Config synthCfg{};
        synthCfg.writeRoot = tempRoot;
        synthCfg.mountDiscoveredAssets = false;
        FearGameApp synthApp(synthCfg);
        CHECK(synthApp.init());
        CHECK(synthApp.setupLiveDemoSession());
        CHECK(synthApp.loadedMissionName() == "SyntheticDemo");

        auto synthSurf = gfx::SoftwareSurface::create(640, 480);
        CHECK(synthSurf != nullptr);
        if (synthSurf) {
            for (auto f : std::views::iota(0, 3)) {
                PlayerMove mv{};
                mv.forwardAction = 0.25f;
                mv.trigger = (f == 1);
                mv.useItem = -1;
                if (synthApp.fearPlugin().clientGlobals().playerPSC) {
                    synthApp.fearPlugin().clientGlobals().playerPSC->pushMove(mv);
                }
                CHECK(synthApp.stepFrame(static_cast<double>(f + 1) * 0.032));
                CHECK(synthApp.renderFrame(*synthSurf));
            }
            CHECK(synthApp.lastSceneSurfaceCount() >= 2u);
            CHECK(synthApp.lastSceneShapeCount() >= 4u);
            CHECK(synthApp.lastScenePolygonCount() > 0u);
            CHECK(synthApp.lastSceneLightCount() >= 1u);
            CHECK(countNonZeroPixels(*synthSurf) > 10000u);
        }
        synthApp.shutdown();
    }

    // =========================================================================
    // 7. Live TRIBES / PCRPG Base Volume Mounting, Live .mis Loading & Full 3D Scene Rendering
    // =========================================================================
    // Match the other acceptance cases: local installations are opt-in and
    // must not change the standard suite's results through auto-discovery.
    const auto* installedAssets = std::getenv("OPENT1_TEST_ASSETS");
    if (installedAssets && *installedAssets) {
        FearGameApp::Config liveCfg{};
        liveCfg.writeRoot = tempRoot;
        liveCfg.assetRoot = installedAssets;
        liveCfg.mountDiscoveredAssets = true;

        FearGameApp liveApp(liveCfg);
        CHECK(liveApp.init());
        CHECK(liveApp.mountedVolumeCount() >= 10u);
        CHECK(!liveApp.mountedAssetRoot().empty());
        CHECK(liveApp.console().getVariable("Console::GFXFont") == "interface.pft");
        CHECK(liveApp.defaultPalette() != nullptr);
        CHECK(liveApp.defaultFont() != nullptr);
        CHECK(liveApp.resources().contains("lush.day.ppl"));
        CHECK(liveApp.resources().contains("interface.pft"));
        CHECK(liveApp.resources().contains("larmor.dts"));
        CHECK(liveApp.resources().contains("disc.dts"));
        CHECK(liveApp.resources().contains("discb.dts"));
        CHECK(liveApp.resources().contains("generator.dts"));
        CHECK(liveApp.resources().contains("radar.dts"));
        CHECK(liveApp.resources().contains("mine.dts"));
        CHECK(liveApp.resources().contains("button5.wav"));
        CHECK(liveApp.resources().contains("Raindance.mis"));
        CHECK(liveApp.resources().contains("Broadside.mis"));
        CHECK(liveApp.resources().contains("dynlogoF.smk"));

        CHECK(liveApp.setupLiveDemoSession());
        auto liveSurface = gfx::SoftwareSurface::create(640, 480);
        CHECK(liveSurface != nullptr);
        if (liveSurface) {
            for (auto f : std::views::iota(0, 4)) {
                PlayerMove mv{};
                mv.forwardAction = 0.25f;
                mv.jetting = true;
                mv.trigger = (f >= 1);
                mv.useItem = -1;
                if (liveApp.fearPlugin().clientGlobals().playerPSC) {
                    liveApp.fearPlugin().clientGlobals().playerPSC->pushMove(mv);
                }
                CHECK(liveApp.stepFrame(static_cast<double>(f + 1) * 0.032));
                CHECK(liveApp.renderFrame(*liveSurface));
            }
            CHECK(liveApp.steppedFrames() == 4u);
            CHECK(liveApp.renderedFrames() == 4u);
            CHECK(liveApp.lastSceneSurfaceCount() >= 2u);
            CHECK(liveApp.lastSceneShapeCount() >= 4u);
            CHECK(liveApp.lastScenePolygonCount() > 0u);
            CHECK(liveApp.lastSceneLightCount() >= 1u);
            CHECK(countNonZeroPixels(*liveSurface) > 50000u);
            CHECK(liveApp.listenerTransform().p.y >= 200.0f);

            // Live Raindance.mis mission script, .ted/.dtf terrain, .dis/.dig interiors & .dts shapes
            CHECK(liveApp.loadMission("Raindance"));
            CHECK(liveApp.loadedMissionName() == "Raindance");
            CHECK(liveApp.console().getVariable("MissionName") == "Raindance");
            CHECK(liveApp.fearPlugin().ghostCount() >= 20u);
            if (auto* cliMgr = liveApp.fearPlugin().clientGlobals().manager) {
                CHECK(dynamic_cast<world::SimMissionTerrain*>(cliMgr->findObject(sim::TerrainId)) !=
                      nullptr);
                if (auto* rset = dynamic_cast<sim::SimSet*>(cliMgr->findObject(sim::RenderSetId))) {
                    CHECK(rset->objects().size() >= 15u);
                }
            }
            for (auto f : std::views::iota(0, 3)) {
                PlayerMove mv{};
                mv.forwardAction = 0.5f;
                mv.trigger = (f == 1);
                mv.useItem = -1;
                if (liveApp.fearPlugin().clientGlobals().playerPSC) {
                    liveApp.fearPlugin().clientGlobals().playerPSC->pushMove(mv);
                }
                CHECK(liveApp.stepFrame(static_cast<double>(f + 5) * 0.032));
                CHECK(liveApp.renderFrame(*liveSurface));
            }
            CHECK(liveApp.lastSceneSurfaceCount() >= 1u);
            CHECK(liveApp.lastSceneShapeCount() >= 1u);
            CHECK(liveApp.lastScenePolygonCount() > 0u);
            CHECK(liveApp.lastSceneLightCount() >= 1u);
            CHECK(countNonZeroPixels(*liveSurface) > 50000u);

            // Live Broadside.mis mission script, .ted/.dtf terrain, .dis/.dig floating bases & .dts shapes
            CHECK(liveApp.loadMission("Broadside"));
            CHECK(liveApp.loadedMissionName() == "Broadside");
            CHECK(liveApp.console().getVariable("MissionName") == "Broadside");
            CHECK(liveApp.fearPlugin().ghostCount() >= 25u);
            if (auto* cliMgr = liveApp.fearPlugin().clientGlobals().manager) {
                CHECK(dynamic_cast<world::SimMissionTerrain*>(cliMgr->findObject(sim::TerrainId)) !=
                      nullptr);
            }
            for (auto f : std::views::iota(0, 3)) {
                PlayerMove mv{};
                mv.forwardAction = 0.5f;
                mv.trigger = (f == 1);
                mv.useItem = -1;
                if (liveApp.fearPlugin().clientGlobals().playerPSC) {
                    liveApp.fearPlugin().clientGlobals().playerPSC->pushMove(mv);
                }
                CHECK(liveApp.stepFrame(static_cast<double>(f + 9) * 0.032));
                CHECK(liveApp.renderFrame(*liveSurface));
            }
            CHECK(liveApp.lastSceneSurfaceCount() >= 1u);
            CHECK(liveApp.lastSceneShapeCount() >= 1u);
            CHECK(liveApp.lastScenePolygonCount() > 0u);
            CHECK(liveApp.lastSceneLightCount() >= 1u);
            CHECK(countNonZeroPixels(*liveSurface) > 50000u);
        }
        liveApp.shutdown();

        // Verify movement in an active match; the normal mission warmup now
        // correctly keeps control in the pregame observer camera.
        const std::array<std::string_view, 10> missionRuntimeArgs{{
            "--smoke",
            "--verify-input",
            "--assets",
            installedAssets,
            "$Server::warmupTime",
            "0",
            "--mission",
            "Raindance",
            "--frames",
            "6",
        }};
        CHECK(opent1::game::runFearMain(missionRuntimeArgs) == 0);
    }

    constexpr std::array<std::string_view, 5> smokeRuntimeArgs{{
        "--smoke",
        "--no-assets",
        "--spawn-demo",
        "--frames",
        "4",
    }};
    CHECK(opent1::game::runFearMain(smokeRuntimeArgs) == 0);

    constexpr std::array<std::string_view, 7> headlessRuntimeArgs{{
        "--smoke", "--headless", "--no-audio", "--no-assets", "--spawn-demo", "--frames", "4",
    }};
    CHECK(opent1::game::runFearMain(headlessRuntimeArgs) == 0);

    std::filesystem::remove_all(tempRoot, ec);
}

} // namespace

int opent1_test_game_e2e_integration() {
    verifyFullStackEndToEndIntegration();

    if (g_failures != 0) {
        std::cerr << std::format("opent1_game_e2e_integration_tests FAILED with {} failure(s)\n",
                                 g_failures);
        return 1;
    }
    std::cout << "opent1_game_e2e_integration_tests: all checks passed.\n";
    return 0;
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("game", "game_e2e_integration", opent1_test_game_e2e_integration);
