#include "test_registry.hpp"
#include "ts_test_support.hpp"
#include <opent1/game/player.hpp>
#include <cmath>

namespace {
using namespace opent1;
using namespace opent1::game;

class WeaponWall final : public sim::SimContainer {
public:
    explicit WeaponWall(UInt32 type = StaticObjectType,
                        bool trigger = false,
                        const TMat3F& placement = {}) {
        setType(type);
        image = std::make_shared<sim::SimCollisionBoxImage>();
        image->box = {{-5, 1.5f, -5}, {5, 1.6f, 5}};
        image->trigger = trigger;
        image->transform = placement;
        Box3F bounds;
        m_mul(image->box, placement, bounds);
        CHECK(setBoundingBox(bounds));
    }

    bool onAdd() override {
        return SimContainer::onAdd() &&
               dynamic_cast<sim::SimContainer*>(getManager()->findObject(sim::RootContainerId))
                   ->addObject(*this);
    }

    bool processQuery(sim::SimQuery& query) override {
        if (auto* collision = dynamic_cast<sim::SimCollisionImageQuery*>(&query)) {
            collision->images = {image};
            return true;
        }
        return SimContainer::processQuery(query);
    }

    std::shared_ptr<sim::SimCollisionBoxImage> image;
};

struct WeaponScene {
    std::shared_ptr<ResourceManager> resources = std::make_shared<ResourceManager>();
    DataBlockManager blocks{false, resources};
    sim::SimWorld world{nullptr, resources};
    Player* player{};
    PlayerData* armor{};
    ItemImageData* image{};
    Int32 itemId{};

    explicit WeaponScene(bool ghost = false) {
        auto body = test::ts::load(test::ts::shape());
        auto weapon = test::ts::load(test::ts::shape());
        CHECK(body && weapon);
        body->transforms[0].translation = {};
        body->transforms[1].translation = {0, 0, 1};
        body->names[1] = "dummy hand";
        auto secondary = body->nodes[1];
        secondary.name = Int16(body->names.size());
        body->names.push_back("dummy unused");
        body->nodes.push_back(secondary);
        weapon->transforms[0].translation = {0, .25f, 0};
        weapon->transforms[1].translation = {0, 1.75f, 0};
        weapon->names[1] = "dummyalways muzzle";
        CHECK(resources->add("viewmodel-body.dts", std::move(body)));
        CHECK(resources->add("viewmodel-weapon.dts", std::move(weapon)));
        CHECK(resources->add("test.bmp", gfx::Bitmap::create(4, 4)));
        armor = dynamic_cast<PlayerData*>(
            blocks.createDataBlock("ViewmodelArmor", DataBlockManager::PlayerDataType));
        image = dynamic_cast<ItemImageData*>(
            blocks.createDataBlock("ViewmodelImage", DataBlockManager::ItemImageDataType));
        auto* item = dynamic_cast<ItemData*>(
            blocks.createDataBlock("ViewmodelItem", DataBlockManager::ItemDataType));
        CHECK(armor && image && item);
        armor->fileName = "viewmodel-body.dts";
        for (auto& animation : armor->animData)
            animation.viewFlags = 7;
        image->shape = resources->load<world::TsShape>("viewmodel-weapon.dts");
        image->mountPoint = 0;
        image->mountOffset = {0, .4f, .25f};
        item->imageId = image->dbmIndex;
        itemId = item->dbmIndex;
        auto owned = std::make_unique<Player>();
        player = owned.get();
        if (ghost)
            CHECK(player->setNetFlags(player->netFlags() | sim::SimNetObject::IsGhost));
        player->bindDataBlockManager(&blocks);
        player->setDatFileName("ViewmodelArmor");
        CHECK(world.manager().addObject(std::move(owned)));
        CHECK(player->mountItem(itemId, 0));
    }

    WeaponWall*
    wall(UInt32 type = StaticObjectType, bool trigger = false, const TMat3F& placement = {}) {
        auto owned = std::make_unique<WeaponWall>(type, trigger, placement);
        auto* result = owned.get();
        CHECK(world.manager().addObject(std::move(owned)));
        return result;
    }

    void tick() {
        PlayerMove move{};
        move.crouching = player->isCrouching();
        player->updateMove(&move, !player->isGhost());
    }
};

void weaponPullIn() {
    WeaponScene scene;
    auto& player = *scene.player;
    const auto image = [&] {
        TMat3F result;
        CHECK(player.getImageTransform(0, &result));
        return result;
    };
    const auto muzzle = [&] {
        TMat3F result;
        CHECK(player.getMuzzleTransform(0, &result));
        return result;
    };
    const auto resting = image();
    const auto restingMuzzle = muzzle();
    CHECK((resting.p - Point3F{0, .4f, 1.25f}).len() < .0001f);
    CHECK((restingMuzzle.p - Point3F{0, 2.4f, 1.25f}).len() < .0001f);
    scene.tick();
    CHECK((image().p - resting.p).len() < .0001f);

    auto* wall = scene.wall();
    scene.tick();
    // Vanilla projects the obstruction onto body-forward and leaves 0.2 units
    // of muzzle clearance. The DTS root contributes to the weapon's length.
    CHECK((image().p - Point3F{0, -.7f, 1.25f}).len() < .0001f);
    CHECK((muzzle().p - Point3F{0, 1.3f, 1.25f}).len() < .0001f);
    world::WorldShapeQuery rendered;
    CHECK(player.processQuery(rendered) && rendered.attachments.size() == 1);
    CHECK((rendered.attachments.front().shapeToWorld.p - Point3F{0, -.45f, 1.25f}).len() < .0001f);
    for (int i = 0; i < 5; ++i) {
        scene.tick();
        CHECK((muzzle().p - Point3F{0, 1.3f, 1.25f}).len() < .0001f);
    }
    wall->deleteObject();
    scene.tick();
    CHECK((image().p - resting.p).len() < .0001f);

    for (const bool trigger : {false, true}) {
        wall = scene.wall(trigger ? StaticObjectType : CorpseObjectType, trigger);
        scene.tick();
        CHECK((image().p - resting.p).len() < .0001f);
        wall->deleteObject();
    }

    // A primary mount in any image slot participates; secondary mounts do not.
    CHECK(player.unmountItem(0) && player.mountItem(scene.itemId, 3));
    wall = scene.wall();
    scene.tick();
    TMat3F other;
    CHECK(player.getMuzzleTransform(3, &other));
    CHECK((other.p - Point3F{0, 1.3f, 1.25f}).len() < .0001f);
    scene.image->mountPoint = 1;
    scene.tick();
    CHECK(player.getMuzzleTransform(3, &other));
    CHECK((other.p - restingMuzzle.p).len() < .0001f);
    scene.image->mountPoint = 0;
    CHECK(player.getMuzzleTransform(3, &other));
    CHECK((other.p - restingMuzzle.p).len() < .0001f);
    wall->deleteObject();
    scene.tick();
    CHECK(player.getMuzzleTransform(3, &other));
    CHECK((other.p - restingMuzzle.p).len() < .0001f);
}

void weaponPullInAngles() {
    for (const bool ghost : {false, true})
        for (const Flt32 yaw : {0.f, 1.2f})
            for (const Flt32 pitch : {-.4f, 0.f, .4f})
                for (const bool crouching : {false, true}) {
                    WeaponScene scene{ghost};
                    auto& player = *scene.player;
                    scene.armor->canCrouch = true;
                    player.setCrouching(crouching);
                    player.setPos({10, 20, 3});
                    player.setRot({0, 0, yaw});
                    player.setViewPitch(pitch);
                    const auto body = player.getTransform();
                    scene.wall(StaticObjectType, false, body);
                    scene.tick();
                    // The wall is at body-local Y=1.5. The authored weapon
                    // ends at Y=2.4*cos(pitch)-.25*sin(pitch) before pull-in.
                    const auto authoredY = 2.4f * std::cos(pitch) - .25f * std::sin(pitch);
                    const auto pull = 1.5f - authoredY - .2f;
                    TMat3F muzzle;
                    CHECK(player.getMuzzleTransform(0, &muzzle));
                    const auto y = authoredY + pull * std::cos(pitch);
                    const auto z = 1 + (2.4f + pull) * std::sin(pitch) + .25f * std::cos(pitch);
                    CHECK((muzzle.p - Point3F{0, y, z}).len() < .0001f);
                    TMat3F worldMuzzle;
                    m_mul(muzzle, body, worldMuzzle);
                    Point3F expected;
                    m_mul(Point3F{0, y, z}, body, expected);
                    CHECK((worldMuzzle.p - expected).len() < .0001f);
                }
}

void cameraBounce() {
    struct BouncePlayer : Player {
        using Player::setContact;
    };

    GameBase mount;
    BouncePlayer player;
    player.setViewPitch(.3f);
    player.setRot({0, 0, .7f});
    const auto eye = player.getEyeTransform();
    TMat3F first, chase;
    player.getCameraTransform(0, &first);
    player.getCameraTransform(4, &chase);
    const auto restingFirst = first.p, restingChase = chase.p;
    CHECK(restingFirst == eye.p);

    UInt32 time = 0;
    const auto animate = [&] {
        time += 100;
        // Isolate the render clock from movement prediction.
        player.setLastProcessTime(time);
        player.clientProcess(time);
    };
    const auto checkBounce = [&](Flt32 phase) {
        const auto offset = std::sin(phase * 2) / 120;
        player.getCameraTransform(0, &first);
        player.getCameraTransform(4, &chase);
        CHECK((first.p - restingFirst - Point3F{0, 0, offset}).len() < .00001f);
        CHECK((chase.p - restingChase - Point3F{0, 0, offset}).len() < .00001f);
        CHECK(player.getEyeTransform().p == eye.p && player.getLOSTransform().p == eye.p);
    };
    player.setContact(true);
    player.setLinearVelocity({2, 0, 0});
    animate();
    constexpr Flt32 step = .174f * 30 * .1f;
    checkBounce(step);
    animate();
    checkBounce(step * 2);

    player.setLinearVelocity({0, 0, 2});
    animate();
    checkBounce(step * 2);
    player.setLinearVelocity({0, 2, 0});
    player.setContact(false);
    animate();
    checkBounce(step * 2);
    player.setContact(true);
    player.setJetting(true);
    animate();
    checkBounce(step * 2);
    player.setJetting(false);
    player.setMountObject(&mount, 0);
    animate();
    checkBounce(step * 2);
    player.setMountObject(nullptr, 0);
    animate();
    checkBounce(step * 3);
}
} // namespace

int opent1_test_weapon_pull_in() {
    return opent1::test::run(weaponPullIn);
}

int opent1_test_camera_bounce() {
    return opent1::test::run(cameraBounce);
}

int opent1_test_weapon_pull_in_angles() {
    return opent1::test::run(weaponPullInAngles);
}

OPENT1_REGISTER_TEST("game", "weapon_pull_in", opent1_test_weapon_pull_in);
OPENT1_REGISTER_TEST("game", "weapon_pull_in_angles", opent1_test_weapon_pull_in_angles);
OPENT1_REGISTER_TEST("game", "camera_bounce", opent1_test_camera_bounce);
