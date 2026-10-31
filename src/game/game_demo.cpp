#include <opent1/game/game_app.hpp>
#include <opent1/game/fear_player_psc.hpp>
#include <opent1/game/item.hpp>
#include <opent1/world/scene_objects.hpp>
#include <opent1/world/world_scene.hpp>
#include <format>

namespace opent1::game {

bool FearGameApp::setupLiveDemoSession() {
    if (!initialized_ && !init()) {
        return false;
    }

    // Explicit synthetic demo only: these fixture meshes never enter mission loading.
    if (resources_) {
        auto makeFallbackShape = []() {
            auto shape = std::make_unique<world::TsShape>();
            shape->bounds.fMin = {-1.0f, -1.0f, 0.0f};
            shape->bounds.fMax = {1.0f, 1.0f, 2.5f};
            shape->center = {0.0f, 0.0f, 1.25f};
            shape->radius = 2.0f;
            shape->names.push_back("root");
            shape->names.push_back("collision");
            shape->names.push_back("power");
            shape->names.push_back("ambient");
            shape->names.push_back("damage");
            world::TsNode rootNode{};
            rootNode.name = 0;
            rootNode.parent = -1;
            shape->nodes.push_back(rootNode);
            world::TsDetail det{};
            det.root = 0;
            det.size = 10.0f;
            shape->details.push_back(det);
            shape->transforms.push_back({});
            world::TsObject object{};
            object.mesh = 0;
            object.node = 0;
            shape->objects.push_back(object);
            world::TsMesh mesh;
            mesh.verticesPerFrame = 8;
            mesh.textureVerticesPerFrame = 1;
            mesh.radius = 2;
            for (UInt8 z : {UInt8{0}, UInt8{1}}) {
                for (UInt8 y : {UInt8{0}, UInt8{1}}) {
                    for (UInt8 x : {UInt8{0}, UInt8{1}}) {
                        mesh.vertices.push_back({x, y, z, 0});
                    }
                }
            }
            mesh.textureVertices.push_back({});
            mesh.frames.push_back({0, {2, 2, 2.5f}, {-1, -1, 0}});
            constexpr std::array<std::array<Int32, 3>, 12> faces{{{0, 2, 3},
                                                                  {0, 3, 1},
                                                                  {4, 5, 7},
                                                                  {4, 7, 6},
                                                                  {0, 1, 5},
                                                                  {0, 5, 4},
                                                                  {2, 6, 7},
                                                                  {2, 7, 3},
                                                                  {0, 4, 6},
                                                                  {0, 6, 2},
                                                                  {1, 3, 7},
                                                                  {1, 7, 5}}};
            for (const auto& indices : faces) {
                world::TsFace face;
                for (std::size_t i = 0; i < 3; ++i) {
                    face.corners[i] = {indices[i], 0};
                }
                mesh.faces.push_back(face);
            }
            shape->meshes.push_back(std::move(mesh));
            world::TsMaterial material;
            material.flags = 1;
            material.index = 253;
            shape->materials = world::TsMaterialList{4, 1, 1, {material}};
            return shape;
        };
        constexpr std::array<std::string_view, 11> kFallbackShapes{{
            "__demo_larmor.dts",
            "__demo_disc.dts",
            "__demo_discb.dts",
            "__demo_generator.dts",
            "__demo_elevator_4x4.dts",
            "__demo_radar.dts",
            "__demo_plasma_turret.dts",
            "__demo_mine.dts",
            "__demo_scout.dts",
            "__demo_debris_chunk.dts",
            "__demo_DropPoint.dts",
        }};
        for (const std::string_view shapeName : kFallbackShapes) {
            if (!resources_->contains(shapeName)) {
                (void)resources_->add(shapeName, makeFallbackShape());
            }
        }
    }

    if (game_.console().evaluate("newServer(\"SyntheticDemo\", false);").value != "True") {
        return false;
    }

    constexpr std::string_view kDemoSessionScript = R"CS(
        SoundProfileData BaseSoundProfile {
            baseVolume = 0.85;
            minDistance = 20.0;
            maxDistance = 350.0;
        };
        SoundData FireDiscSound {
            wavFileName = "button5.wav";
            profile = BaseSoundProfile;
            priority = 0.75;
        };
        DamageSkinData ArmorDamageSkins {
            bmpName[0] = "dskin0";
            bmpName[1] = "dskin1";
        };
        ExplosionData DiscExp {
            shapeName = "__demo_discb.dts";
            soundId = FireDiscSound;
            timeScale = 1.25;
            faceCamera = true;
            hasLight = true;
            lightRange = 18.0;
        };
        DebrisData PlayerDebris {
            shapeName = "__demo_discb.dts";
            mass = 5.0;
            maxTimeout = 4.0;
            spawnedExplosionID = DiscExp;
        };
        RocketData DiscShell {
            bulletShapeName = "__demo_discb.dts";
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
        ItemImageData DiscLauncherImg {
            shapeFile = "__demo_disc.dts";
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
            shapeFile = "__demo_discb.dts";
            showInventory = true;
            price = 2;
        };
        ItemData DiscLauncher {
            description = "Disc Launcher";
            shapeFile = "__demo_disc.dts";
            imageId = DiscLauncherImg;
            showInventory = true;
            price = 50;
        };
        PlayerData LightArmor {
            shapeFile = "__demo_larmor.dts";
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
            shapeFile = "__demo_generator.dts";
            maxDamage = 200.0;
            maxEnergy = 150.0;
            visibleToSensor = true;
            description = "Generator";
        };
        SensorData PulseSensor {
            shapeFile = "__demo_radar.dts";
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
            shapeFile = "__demo_plasma_turret.dts";
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
        MarkerData DropPointMarkerData {
            fileName = "__demo_disc.dts";
        };

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
        instant Planet "MissionSun" {
            fileName = "lushdaysun.bmp";
            azimuth = "180.0";
            incidence = "45.0";
            castShadows = "True";
            useLensFlare = "False";
            intensity = "0.7 0.7 0.7";
            ambient = "0.3 0.3 0.3";
            size = "180.0";
            distance = "1200.0";
        };
        addToSet("MissionGroup", "MissionSun");
        instant Snowfall "MissionSnow" {
            intensity = "0.35";
            wind = "1.0 0.5 -4.0";
            suspendRendering = "False";
            precipType = "0";
        };
        addToSet("MissionGroup", "MissionSnow");
        newObject("MissionSky", "Sky", "0.4", "0.5", "0.7", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15");
        addToSet("MissionGroup", "MissionSky");
        newObject("MissionStars", "StarField");
        addToSet("MissionGroup", "MissionStars");
    )CS";

    if (!game_.console().evaluate(kDemoSessionScript).ok()) {
        return false;
    }
    auto& sg = fearPlugin_->serverGlobals();
    if (sg.dbm) {
        if (auto* imgData = dynamic_cast<ItemImageData*>(
                sg.dbm->lookupDataBlock("DiscLauncherImg", DataBlockManager::ItemImageDataType))) {
            imgData->ammoType = 0;
        }
    }
    if (game_.console().evaluate("preloadServerDataBlocks();").value != "True") {
        return false;
    }
    if (game_.console().evaluate("newClient();").value != "True") {
        return false;
    }
    if (defaultPalette_) {
        setActivePalette(defaultPalette_);
    }
    if (game_.console().evaluate("localConnect();").value != "True") {
        return false;
    }
    const std::string spawnedId =
        game_.console().evaluate("spawnPlayer(\"LightArmor\", \"0 0 0\", \"0 0 0\");").value;
    if (spawnedId.empty() || spawnedId == "-1") {
        return false;
    }
    (void)game_.console().evaluate(
        std::format("Player::setItemCount({}, \"DiscAmmo\", 15);", spawnedId));
    (void)game_.console().evaluate(
        std::format("Player::setItemCount({}, \"DiscLauncher\", 1);", spawnedId));
    (void)game_.console().evaluate(
        std::format("Player::useItem({}, \"DiscLauncher\");", spawnedId));
    (void)fearPlugin_->syncLoopbackState();
    auto* manager = fearPlugin_->clientGlobals().manager;
    for (const auto& bounds :
         {Box3F{{70, 175, 47}, {130, 245, 49}}, Box3F{{94, 224, 49}, {106, 234, 60}}}) {
        world::WorldSurfaceInstance image;
        image.mesh = world::boxSurface(bounds);
        if (!image.mesh ||
            !manager->addObject(std::make_unique<world::SimWorldSurface>(std::move(image)))) {
            return false;
        }
    }
    auto light = std::make_unique<world::SimSceneLight>();
    sim::SimLight sun;
    sun.type = sim::SimLight::Type::Directional;
    sun.direction = {0, 0, -1};
    sun.intensity = {.7f, .7f, .7f};
    if (!light->setLight(sun, {.3f, .3f, .3f}) || !manager->addObject(std::move(light))) {
        return false;
    }
    if (guiRuntime_) {
        guiRuntime_->canvas.content(nullptr);
    }
    loadedMissionName_ = "SyntheticDemo";
    return true;
}

} // namespace opent1::game
