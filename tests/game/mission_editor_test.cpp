#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/game/game_app.hpp>
#include <opent1/game/mission_editor_controls.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/gui/containers.hpp>
#include <opent1/world/terrain_editor.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/game/game_base.hpp>
#include <opent1/core/file_stream.hpp>
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <fstream>

using namespace opent1;

namespace {
void consoleOptions() {
    Console console;
    sim::SimManager manager;
    manager.setConsole(&console);
    auto* editor = static_cast<game::FearMissionEditor*>(
        manager.addObject(std::make_unique<game::FearMissionEditor>()));
    CHECK(editor);
    editor->getConsoleOptions();
    CHECK(editor->flags() & game::FearMissionEditor::ShowHandles);
    CHECK(editor->rotationSnap() == 90.f && editor->granularity().z == .001f);
    constexpr auto mask = game::FearMissionEditor::SnapToGrid |
                          game::FearMissionEditor::ConstrainX |
                          game::FearMissionEditor::ShowHandles;
    for (const auto truth : {"1", "true", "True", "TRUE"}) {
        for (const auto name : {"ME::SnapToGrid", "ME::ConstrainX", "ME::ShowGrabHandles"}) {
            console.setVariable(name, truth);
        }
        editor->getConsoleOptions();
        CHECK((editor->flags() & mask) == mask);
        console.setVariable("ME::SnapToGrid", "false");
        console.setVariable("ME::ConstrainX", "0");
        console.setVariable("ME::ShowGrabHandles", "false");
        editor->getConsoleOptions();
        CHECK((editor->flags() & mask) == 0);
    }
}

struct EditorSession {
    game::FearGameApp app;

    EditorSession() {
        CHECK(app.init());
        CHECK(app.fearPlugin().createServerWorld());
        CHECK(app.fearPlugin().createClientWorld());
        app.game().withWorld(sim::WorldType::Server, [&] {
            CHECK(app.game().createObject("SimGroup", "MissionGroup"));
        });
        CHECK(app.fearPlugin().focusClient());
        CHECK(app.console().evaluate("newObject(MainWindow, \"SimGui::Canvas\");").ok());
        CHECK(run("ME::Create(MainWindow);") == "True");
    }

    std::string run(std::string_view script) {
        const auto result = app.console().evaluate(script);
        if (!result.ok()) {
            std::fprintf(stderr, "Editor script failed: %.*s\n", int(script.size()), script.data());
            for (const auto& diagnostic : result.diagnostics)
                std::fprintf(stderr, "%s\n", diagnostic.message.c_str());
        }
        CHECK(result.ok());
        return result.value;
    }

    sim::SimManager& server() {
        return app.game().world(sim::WorldType::Server)->manager();
    }

    sim::SimManager& client() {
        return app.game().world(sim::WorldType::Client)->manager();
    }

    game::FearMissionEditor& editor() {
        return *app.missionPlugin().editor();
    }
};

void consoleContracts() {
    EditorSession session;
    auto& editor = session.editor();
    CHECK(editor.getManager() == &session.client());
    CHECK(editor.targetManager() == &session.server());
    CHECK(editor.getAddGroup() == session.server().findObject("MissionGroup"));
    CHECK(session.run("ME::SetGrabMask(-68);") == "True" && editor.grabMask() == UInt32(-68));
    CHECK(session.run("ME::SetDefaultPlaceMask(-8);") == "True" &&
          editor.defaultPlaceMask() == UInt32(-8));
    CHECK(session.run("ME::RegisterType(SimGroup, 0);") == "True");
    CHECK(session.run("ME::SetPlaceMask(simgroup, -8);") == "True");
    CHECK(session.run("ME::RegisterType(SIMGROUP, 4);") == "False");
    CHECK(session.run("ME::CreateGroup();") == "True");
    auto* group = session.server().findObject("MissionGroup/SimGroup");
    CHECK(group && editor.placementMask(group) == UInt32(-8));
    CHECK(session.run("ME::SetPlaceMask(SimGroup);") == "True");
    CHECK(editor.placementMask(group) == UInt32(-8));
    session.app.console().setVariable("ME::InspectObject", std::to_string(group->getId()));
    CHECK(session.run("ME::Redo();") == "False");
    auto* destination = editor.getAddGroup();
    const auto created = session.run("MissionCreateObject(EditorGroup, SimGroup);");
    auto* object = session.server().findObject(created);
    CHECK(object && object->getName() == "EditorGroup1");
    CHECK(object->getManager() == &session.server());
    CHECK(object->getGroup() == destination);
    CHECK(!session.client().findObject(created));
    CHECK(session.run("focusServer(); ME::GetConsoleOptions();") == "True");
    CHECK(&session.editor() == &editor);
    CHECK(session.run("ME::Create(DoesNotExist);") == "False");
    CHECK(&session.editor() == &editor);
    CHECK(session.run("focusClient(); ME::Create(MainWindow);") == "True");
    CHECK(session.editor().selection().objects().empty());
    session.app.fearPlugin().destroyServerWorld();
    CHECK(!session.app.missionPlugin().editor());
    CHECK(session.app.fearPlugin().createServerWorld());
    CHECK(session.run("ME::Create(MainWindow);") == "True");
    CHECK(session.editor().targetManager() == &session.server());
}

class TransformObject : public sim::SimObject {
public:
    TMat3F transform;

    TransformObject() {
        transform.identity();
    }

    bool processQuery(sim::SimQuery& query) override {
        if (auto* q = dynamic_cast<sim::SimObjectTransformQuery*>(&query)) {
            q->transform = transform;
            return true;
        }
        return false;
    }

    bool processEvent(const sim::SimEvent& event) override {
        if (const auto* e = dynamic_cast<const sim::SimObjectTransformEvent*>(&event)) {
            transform = e->transform;
            return true;
        }
        return false;
    }
};

void undoAndSelectionLifetime() {
    EditorSession session;
    auto& editor = session.editor();
    auto* group = dynamic_cast<sim::SimGroup*>(session.server().findObject("MissionGroup"));
    auto* child = static_cast<TransformObject*>(
        session.server().addObject(std::make_unique<TransformObject>(), group));
    CHECK(child);
    child->transform.p = {1, 2, 3};
    CHECK(editor.selection().addObject(*group));
    CHECK(editor.selection().addObject(*child));
    editor.saveInfo();
    CHECK(editor.undoEntries().size() == 1);
    editor.moveObjectRelative(&editor.selection(), {10, 0, 0});
    CHECK(child->transform.p == Point3F(11, 2, 3));
    editor.selection().clearSet();
    editor.restoreInfo();
    CHECK(child->transform.p == Point3F(1, 2, 3) && editor.undoEntries().empty());
    CHECK(editor.selection().addObject(*child));
    editor.saveInfo();
    const auto id = child->getId();
    editor.selection().clearSet();
    CHECK(child->deleteObject());
    auto* replacement = static_cast<TransformObject*>(
        session.server().addObject(std::make_unique<TransformObject>(), group));
    CHECK(replacement && session.server().assignId(*replacement, id));
    replacement->transform.p = {70, 80, 90};
    editor.restoreInfo();
    CHECK(replacement->transform.p == Point3F(70, 80, 90));
    CHECK(editor.selection().addObject(*group));
    CHECK(editor.selection().addObject(*replacement));
    editor.deleteSelection();
    CHECK(!session.server().findObject(id));
    CHECK(editor.selection().objects().empty());
}

void focusAndCameraMap() {
    EditorSession session;
    auto camera = std::make_unique<game::EditCamera>();
    auto map = std::make_shared<sim::SimActionMap>();
    bool loaded{};
    camera->loadActionMap = [&](std::string_view name) {
        loaded = name == "editor.sae";
        return map;
    };
    auto* raw = static_cast<game::EditCamera*>(session.client().addObject(std::move(camera)));
    CHECK(raw);
    raw->assignName("editCamera");
    CHECK(raw->processArguments(std::array<std::string, 1>{"editor.sae"}) && loaded);
    CHECK(session.run("unfocus(editCamera);") == "True");
    session.client().advanceToTime(session.client().getCurrentTime());
    CHECK(!raw->hasFocus());
    CHECK(session.run("focus(editCamera);") == "True");
    session.client().advanceToTime(session.client().getCurrentTime());
    CHECK(raw->hasFocus());
    raw->setPosition({10, 20, 30}, {.1f, .2f, .3f});
    auto* object = static_cast<TransformObject*>(
        session.server().addObject(std::make_unique<TransformObject>()));
    CHECK(object);
    object->transform.p = {1, 2, 3};
    session.editor().onDrop(object, game::FearMissionEditor::DropAtCamera);
    CHECK(object->transform.p == Point3F(11, 22, 33));
    session.editor().placeBookmark(0);
    raw->setPos({90, 90, 90});
    session.editor().gotoBookmark(0);
    CHECK(raw->getPos() == Point3F(10, 20, 30));
    sim::SimGainFocusEvent event;
    CHECK(session.editor().processEvent(event));
}

void guiPersistenceAndPopups() {
    sim::SimGame game;
    CHECK(gui::registerClasses(game.classes()));
    CHECK(gui::registerExtendedClasses(game.classes()));
    CHECK(game::registerMissionEditorGuiClasses(game.classes()));
    const std::array<std::pair<std::string_view, UInt32>, 9> types{
        {{"METextList", makeFourCC('M', 'e', 't', 'L')},
         {"MEButton", makeFourCC('M', 'e', 'b', 'T')},
         {"MEPopup", makeFourCC('M', 'e', 'p', 'L')},
         {"MEPopupButton", makeFourCC('M', 'e', 'p', 'b')},
         {"MECheck", makeFourCC('M', 'e', 'c', 'K')},
         {"MERadio", makeFourCC('M', 'e', 'r', 'b')},
         {"METextEdit", makeFourCC('M', 'e', 'T', 'e')},
         {"MEInspectTagList", makeFourCC('M', 'e', 'I', 'T')},
         {"MEObjectList", makeFourCC('M', 'e', 'O', 'L')}}};
    for (const auto& [name, tag] : types) {
        const auto* descriptor = game.classes().find(name);
        CHECK(descriptor && descriptor == game.classes().find(tag));
        const auto object = descriptor->create();
        CHECK(dynamic_cast<gui::Control*>(object.get()));
        std::vector<std::byte> storage(65536);
        MemRWStream bytes{std::span<std::byte>(storage)};
        Persistent::StoreContext store(game.classes());
        CHECK(store.store(bytes, *object) == Persistent::Error::Ok);
        CHECK(bytes.setPosition(0));
        Persistent::LoadContext load(game.classes());
        CHECK(load.load(bytes));
    }
    auto ui = std::make_shared<gui::Runtime>(game.console(), game.classes());
    ui->install();
    game::installMissionEditorGuiCommands(game.console(), ui, game);
    auto object = game.classes().find("MEPopup")->create();
    auto* popup = dynamic_cast<gui::Control*>(object.get());
    CHECK(popup);
    popup->assignName("TestPopup");
    object.release();
    CHECK(ui->show(gui::Graph::create(std::unique_ptr<gui::Control>(popup))));
    const auto run = [&](std::string_view script) {
        const auto result = game.console().evaluate(script);
        CHECK(result.ok());
        return result.value;
    };
    CHECK(run("Popup::addLine(TestPopup, \"first\", 7);") == "True");
    CHECK(run("Popup::addLine(TestPopup, \"second\", -8);") == "True");
    CHECK(run("Popup::setSelected(TestPopup, -8);") == "True");
    CHECK(run("Popup::getSelected(TestPopup);") == "-8");
    CHECK(run("Control::getValue(TestPopup);") == "-8");
    CHECK(run("Popup::clear(TestPopup);") == "True");
    CHECK(run("Popup::getSelected(TestPopup);") == "0");
    CHECK(run("Popup::setSelected(TestPopup, invalid);") == "False");
    ui->loadFile = [](std::string_view) {
        return std::optional(std::vector<std::byte>(8));
    };
    game.console().setVariable("ME::Loaded", "True");
    CHECK(run("GuiLoadContentCtrl(\"gui/editor.gui\");") == "False");
    CHECK(game.console().getVariable("ME::Loaded") == "False");
    CHECK(ui->canvas.top() == popup);
    ResourceManager resources;
    CHECK(game::bindMissionEditorGuiControl(*popup, game, resources));
    CHECK(popup->resize({}, {400, 200}));
    CHECK(run("Popup::addLine(TestPopup, \"last\", 9);") == "True");
    game.console().addCommand(
        0,
        "DestroyPopup",
        [weak = std::weak_ptr(ui)](Console&, Int32, std::span<const std::string>) {
            const auto runtime = weak.lock();
            return runtime && runtime->clear() ? "True" : "False";
        });
    CHECK(game.console()
              .evaluate("function TestPopup::onSelect(%text, %id) { DestroyPopup(); }")
              .ok());
    auto* combo = dynamic_cast<gui::ComboBox*>(popup);
    CHECK(combo && combo->open());
    CHECK(combo->dismiss(true));
    CHECK(!ui->canvas.top());
    CHECK(run("Popup::getSelected(TestPopup);") == "False");
}

void terrainConsoleStatus() {
    Console console;
    auto editor = std::make_shared<world::TerrainEditor>();
    bool usedDefault{};
    editor->terrainLoader = [&](std::string_view name) {
        usedDefault = name.empty();
        return world::TerrainDocument::create({1, 1}, 4, 0);
    };
    editor->attachedFile = "missions/test.ted";
    world::installTerrainEditorCommands(console, editor);
    CHECK(console.execute("Ted::attachToTerrain").value == "True" && usedDefault);
    CHECK(console.getVariable("TED::success") == "True");
    CHECK(console.getVariable("TED::currFile") == "missions/test.ted");
    CHECK(console.execute("Ted::getBrushDetail").value == "0");
    CHECK(console.getVariable("TED::success") == "True");
    CHECK(console.execute("Ted::setHeightVal", std::array<std::string, 1>{"invalid"}).value ==
          "False");
    CHECK(console.getVariable("TED::success") == "False");
    editor.reset();
    CHECK(console.execute("Ted::attachToTerrain").value == "False");
    CHECK(console.getVariable("TED::success") == "False");
}

void missionSaveRoundTrip() {
    EditorSession session;
    const auto root = std::filesystem::temp_directory_path() /
                      std::format("opent1-editor-save-{}",
                                  std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};

    session.app.console().setFileRoot(root);
    session.app.console().setVariable("missionFile", "missions/EditorTest.mis");
    session.app.console().removeVariable("TED::currFile");
    auto* group = dynamic_cast<sim::SimGroup*>(session.server().findObject("MissionGroup"));
    CHECK(group);
    group->setFieldValue("note", "quoted \"text\" and \\path\nnext line");
    CHECK(session.run("MissionCreateObject(SavedGroup, SimGroup);") != "False");
    CHECK(session.run("ME::Save();") == "True");
    const auto path = root / "missions/EditorTest.mis";
    std::ifstream input(path, std::ios::binary);
    const std::string script{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    CHECK(!script.empty());
    CHECK(group->deleteObject());
    session.app.game().withWorld(sim::WorldType::Server, [&] {
        CHECK(session.app.console().evaluate(script).ok());
    });
    group = dynamic_cast<sim::SimGroup*>(session.server().findObject("MissionGroup"));
    CHECK(group && group->findObject("SavedGroup1"));
    CHECK(group->getFieldValue("note") == "quoted \"text\" and \\path\nnext line");
    // A failed write must preserve the previous complete mission.
    session.app.console().setVariable("missionFile", "missions/EditorTest.mis/blocked.mis");
    CHECK(session.run("ME::Save();") == "False");
    std::ifstream preserved(path, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(preserved),
                      std::istreambuf_iterator<char>()) == script);
}

void viewportMouseEditing() {
    EditorSession session;
    auto* camera = static_cast<game::EditCamera*>(
        session.client().addObject(std::make_unique<game::EditCamera>()));
    CHECK(camera);
    camera->assignName("editCamera");
    camera->setPosition({0, 0, 10}, {});
    auto* object = static_cast<TransformObject*>(session.server().addObject(
        std::make_unique<TransformObject>(), session.editor().getAddGroup()));
    CHECK(object);
    object->setType(sim::MissionObjectType);
    object->transform.p = {0, 20, 10};
    auto* view = static_cast<gui::TSControl*>(
        session.client().addObject(std::make_unique<gui::TSControl>()));
    CHECK(view && view->resize({}, {320, 240}));
    view->assignName("EditorGui");
    game::bindMissionEditorViewport(
        *view, session.app.game(), session.app.fearPlugin().terrainEditor());
    CHECK(view->sceneWorld && view->input && view->overlay);
    CHECK(session.app.guiRuntime()->canvas.content(view));
    CHECK(session.run("postAction(EditorGui, Attach, editCamera);") == "True");
    CHECK(view->attached().resolve(session.client()) == camera);
    CHECK(session.run("postAction(EditorGui, UnknownAction, editCamera);") == "False");
    sim::SimGainFocusEvent gain;
    CHECK(session.editor().processEvent(gain));
    view->tick(0);
    CHECK(view->frame() && view->cameraView());
    auto& canvas = session.app.guiRuntime()->canvas;
    gui::Event event;
    event.type = gui::EventType::Down;
    event.point = {160, 120};
    CHECK(canvas.dispatch(event));
    CHECK(session.editor().selection().objects().size() == 1);
    CHECK(session.editor().selection().objects().front() == object);
    event.type = gui::EventType::Drag;
    event.control = true;
    event.point.y -= 20;
    CHECK(canvas.dispatch(event));
    CHECK(object->transform.p.z == 14);
    event.type = gui::EventType::Up;
    CHECK(canvas.dispatch(event) && !canvas.capture());
    CHECK(session.run("ME::Undo();") == "True" && object->transform.p.z == 10);
    event.type = gui::EventType::Down;
    event.control = false;
    event.point = {160, 120};
    CHECK(canvas.dispatch(event));
    event.type = gui::EventType::Drag;
    event.shift = true;
    event.point.x += 20;
    CHECK(canvas.dispatch(event));
    CHECK(object->transform.m[0][0] < 1);
    event.type = gui::EventType::Cancel;
    CHECK(canvas.dispatch(event));
    CHECK(object->transform.m[0][0] == 1 && !canvas.capture());
    event.type = gui::EventType::Down;
    event.shift = false;
    event.button = 3;
    CHECK(canvas.dispatch(event));
    CHECK(session.app.console().getVariable("ME::MouseLook") == "True");
    event.type = gui::EventType::Up;
    CHECK(canvas.dispatch(event));
    CHECK(session.app.console().getVariable("ME::MouseLook") == "False");
    CHECK(canvas.render(320, 240));
    session.app.fearPlugin().destroyServerWorld();
    view->tick(0);
    CHECK(!view->frame());
    CHECK(session.app.console().getVariable("ME::Loaded") == "False");
}

void terrainFocusAndDisplay() {
    EditorSession session;
    const auto model = session.app.fearPlugin().terrainEditor();
    CHECK(session.run("Ted::initTed();") == "True");
    CHECK(session.client().findObject("TedObject"));
    CHECK(session.run("focus(TedObject);") == "True");
    session.client().advanceToTime(session.client().getCurrentTime());
    CHECK(model->focused);
    CHECK(session.run("Ted::setSelectShow(true, false, true);") == "True");
    CHECK(model->selectionDisplay.show == 5);
    CHECK(session.run("Ted::setSelectFrameColor(42);") == "True" &&
          model->selectionDisplay.frameColor == 42);
    CHECK(session.run("Ted::setHilightFillColor(17);") == "True" &&
          model->highlightDisplay.fillColor == 17);
    CHECK(session.run("Ted::setShadowShow(false, true, false);") == "True" &&
          model->shadowDisplay.show == 2);
    CHECK(session.run("Ted::setBlockOutline(true);") == "True" && model->blockOutline);
    CHECK(session.run("Ted::setSelectFrameColor(-1);") == "False" &&
          model->selectionDisplay.frameColor == 42);
    CHECK(session.run("Ted::setRButtonAction(\"\");") == "True");
    CHECK(model->settings().rightAction == world::TerrainEditor::actions().size());
    CHECK(session.run("unfocus(TedObject);") == "True");
    session.client().advanceToTime(session.client().getCurrentTime());
    CHECK(!model->focused);
    CHECK(session.run("$TED::heightVal = 75; $TED::flagEdit = true; $TED::pasteMaterial = false; "
                      "$TED::pasteHeight = true; Ted::GetConsoleOptions();") == "True");
    CHECK(model->settings().height == 75 && model->settings().flags == 64 &&
          model->settings().paste == 2);
}

void prefabPasteAndLighting() {
    EditorSession session;
    session.app.console().setScriptLoader([](std::string_view name) -> std::optional<std::string> {
        if (name == "prefab.cs")
            return "instant SimGroup Prefab { instant SimGroup Child; };";
        if (name == "invalid.cs")
            return "instant SimGroup Partial; instant MissingClass Broken;";
        return {};
    });
    CHECK(session.run("ME::PasteFile(\"prefab.cs\");") == "True");
    auto* group = dynamic_cast<sim::SimGroup*>(session.server().findObject("MissionGroup"));
    auto* prefab = group->findObject("Prefab");
    CHECK(prefab && dynamic_cast<sim::SimGroup*>(prefab)->findObject("Child"));
    CHECK(!session.client().findObject("Prefab"));
    const auto count = group->objects().size();
    CHECK(session.app.console().evaluate("ME::PasteFile(\"invalid.cs\");").value == "False");
    CHECK(group->objects().size() == count &&
          session.editor().selection().objects().front() == prefab);
    CHECK(session.run("ME::PasteFile(\"absent.cs\");") == "False");
    session.app.console().setVariable("missionFile", "");
    CHECK(session.run("ME::MissionLight(true);") == "False");
    CHECK(session.app.console().getVariable("ME::LightingStatus") == "Failed");
}

void installedEditorAcceptance() {
    const auto* assets = std::getenv("OPENT1_TEST_ASSETS");
    if (!assets || !*assets)
        return;
    const auto root = std::filesystem::temp_directory_path() /
                      std::format("opent1-editor-acceptance-{}",
                                  std::chrono::steady_clock::now().time_since_epoch().count());

    struct Cleanup {
        std::filesystem::path root;

        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } cleanup{root};

    game::FearGameApp::Config config;
    config.assetRoot = assets;
    config.writeRoot = root;
    game::FearGameApp app(config);
    CHECK(app.init());
    auto& console = app.console();
    console.setVariable("Server::Port", "0");
    CHECK(app.loadMission("Raindance"));
    (void)app.bindGameplayActions();
    const auto run = [&](std::string_view script) {
        const auto result = console.evaluate(script);
        if (!result.ok())
            for (const auto& diagnostic : result.diagnostics)
                std::fprintf(stderr, "%s\n", diagnostic.message.c_str());
        CHECK(result.ok());
        return result.value;
    };
    double time{};
    const auto frame = [&] {
        time += .032;
        CHECK(app.stepFrame(time, false));
    };
    CHECK(run("exec(\"editor.cs\");") == "True");
    frame();
    auto* view = dynamic_cast<gui::TSControl*>(app.guiRuntime()->findControl("EditorGui"));
    if (!view)
        std::fprintf(stderr, "Active GUI: %s\n", app.guiRuntime()->canvas.top()->getName().data());
    CHECK(view && view->sceneWorld && view->input && view->attached().objectId);
    CHECK(view->resize({}, {640, 480}));
    view->tick(0);
    CHECK(view->frame());
    const auto key = [&](UInt8 scanCode) {
        auto* manager = app.game().world(sim::WorldType::Client);
        auto* input =
            dynamic_cast<sim::SimInputManager*>(manager->manager().findObject(sim::InputManagerId));
        CHECK(input);
        sim::SimInputEvent event;
        event.objInst = scanCode;
        event.fValue = 1;
        CHECK(input->submit(event));
        event.action = sim::InputBreak;
        event.fValue = 0;
        (void)input->submit(event);
        frame();
    };
    key(60); // F2
    CHECK(app.guiRuntime()->findControl("Inspector")->visible());
    key(61); // F3
    CHECK(app.guiRuntime()->findControl("Creator")->visible());
    key(62); // F4
    auto model = app.fearPlugin().terrainEditor();
    CHECK(model->focused && app.guiRuntime()->findControl("TedBar")->visible());
    CHECK(model->settings().rightAction == world::TerrainEditor::actions().size());
    CHECK(run("TEDModeOne::onSelect(\"setHeight\", 0); Ted::setHeightVal(123);") == "True");
    auto* me = app.missionPlugin().editor();
    auto* camera = me->camera();
    CHECK(camera);
    // Point the camera down into terrain and use the production LOS picker.
    const auto document = model->snapshot();
    CHECK(document && !document->blocks.empty());
    const auto square = std::ldexp(1.f, document->grid.scale);
    const auto blockEntry = std::ranges::find(
        document->grid.blocks, document->grid.blockMap.front(), &world::TerrainGridBlockEntry::id);
    CHECK(blockEntry != document->grid.blocks.end());
    const auto blockIndex = std::size_t(blockEntry - document->grid.blocks.begin());
    const auto height =
        document->blocks[blockIndex].heights[10 * (document->blocks[blockIndex].width + 1) + 10];
    auto* terrainObject =
        dynamic_cast<world::SimMissionTerrain*>(me->targetManager()->findObject(sim::TerrainId));
    CHECK(terrainObject);
    camera->setPosition(terrainObject->state().position +
                            Point3F{10 * square, 10 * square - 20, height + 40},
                        {-1.1f, 0, 0});
    view->tick(0);
    gui::Event event;
    event.type = gui::EventType::Down;
    event.point = view->localToGlobal({view->fields().extent.x / 2, view->fields().extent.y / 2});
    auto& canvas = app.guiRuntime()->canvas;
    CHECK(canvas.dispatch(event));
    event.type = gui::EventType::Up;
    CHECK(canvas.dispatch(event));
    CHECK(model->canUndo());
    CHECK(model->snapshot()->blocks[blockIndex].heights != document->blocks[blockIndex].heights);
    if (const auto* screenshot = std::getenv("OPENT1_EDITOR_SCREENSHOT")) {
        const auto image = canvas.render(640, 480, app.defaultPalette().get());
        CHECK(image);
        FileWStream output(screenshot);
        CHECK(image->write(output, gfx::BitmapFormat::Microsoft));
    }
    // F2 reuses the same viewport with object editing focus.
    key(60);
    CHECK(!model->focused && me->focused());
    auto* server = app.game().world(sim::WorldType::Server);
    const auto find = [&](auto&& self, sim::SimObject* object) -> sim::SimObject* {
        if (!object)
            return nullptr;
        sim::SimObjectTransformQuery query;
        if (dynamic_cast<game::GameBase*>(object) && !object->isLocked() &&
            (object->getType() & me->grabMask()) && object->processQuery(query))
            return object;
        if (auto* group = dynamic_cast<sim::SimGroup*>(object))
            for (auto* child : group->objects())
                if (auto* found = self(self, child))
                    return found;
        return nullptr;
    };
    auto* object = find(find, server->manager().findObject("MissionGroup"));
    CHECK(object);
    sim::SimObjectTransformQuery transform;
    CHECK(object->processQuery(transform));
    camera->setPosition(transform.transform.p - Point3F{0, 1, 0}, {});
    view->tick(0);
    event.type = gui::EventType::Down;
    event.point = view->localToGlobal({view->fields().extent.x / 2, view->fields().extent.y / 2});
    CHECK(canvas.dispatch(event));
    CHECK(!me->selection().objects().empty());
    auto* picked = me->selection().objects().front();
    sim::SimObjectTransformQuery before;
    CHECK(picked->processQuery(before));
    event.type = gui::EventType::Drag;
    event.control = true;
    event.point.y -= 10;
    CHECK(canvas.dispatch(event));
    event.type = gui::EventType::Up;
    CHECK(canvas.dispatch(event));
    sim::SimObjectTransformQuery after;
    CHECK(picked->processQuery(after) && after.transform.p != before.transform.p);
    CHECK(run("ME::Undo();") == "True");
    CHECK(picked->processQuery(after) && after.transform.p == before.transform.p);
    console.setVariable("ME::InspectObject", std::to_string(picked->getId()));
    CHECK(run("ME::ObjectToSC();") == "True");
    CHECK(picked->processQuery(after) && after.transform.p != before.transform.p);
    CHECK(run("ME::CopySelection();") == "True");
    CHECK(run("ME::PasteSelection();") == "True");
    CHECK(run("ME::DeleteSelection();") == "True");
    const auto prefab = me->exportScript(*picked);
    CHECK(prefab);
    std::filesystem::create_directories(root / "temp");
    {
        std::ofstream output(root / "temp/editor-prefab.cs");
        output << *prefab;
    }
    console.setVariable("ConsoleWorld::DefaultSearchPath",
                        console.getVariable("ConsoleWorld::DefaultSearchPath"));
    CHECK(run("ME::PasteFile(\"editor-prefab.cs\");") == "True");
    CHECK(!me->selection().objects().empty());
    CHECK(run("ME::DeleteSelection();") == "True");
    CHECK(run("ME::Save();") == "True");
    CHECK(run("ME::MissionLight(true);") == "True");
    CHECK(console.getVariable("ME::LightingStatus") == "TerrainBaked");
    const auto saved = model->snapshot();
    const auto missionScript = me->exportScript(*server->manager().findObject("MissionGroup"));
    CHECK(missionScript);
    std::ifstream savedMission(console.getVariable("ME::LastSavedMission"), std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(savedMission),
                      std::istreambuf_iterator<char>()) == *missionScript);
    key(63); // F5: return to play
    CHECK(app.guiRuntime()->canvas.top()->getName() == "playGui");
    CHECK(!me->focused() && !camera->hasFocus());
    CHECK(run("MEMode();") == "True" || app.guiRuntime()->findControl("EditorGui"));
    frame();
    CHECK(app.loadMission("Raindance"));
    CHECK(!app.missionPlugin().editor());
    // Compare persistent state before dynamic items take another physics step.
    game::FearMissionEditor exporter;
    exporter.bindGame(&app.game());
    const auto reloadedScript = exporter.exportScript(
        *app.game().world(sim::WorldType::Server)->manager().findObject("MissionGroup"));
    CHECK(run("exec(\"editor.cs\");") == "True");
    frame();
    auto* second = app.missionPlugin().editor();
    CHECK(second &&
          second->targetManager() == &app.game().world(sim::WorldType::Server)->manager());
    if (reloadedScript != missionScript) {
        if (const auto* screenshot = std::getenv("OPENT1_EDITOR_SCREENSHOT")) {
            std::ofstream(std::filesystem::path(screenshot).parent_path() / "mission-before.cs")
                << *missionScript;
            std::ofstream(std::filesystem::path(screenshot).parent_path() / "mission-after.cs")
                << reloadedScript.value_or("");
        }
    }
    CHECK(reloadedScript);
    const auto normalizeItems = [](const std::string& script) {
        // Mission rules recompute Item::originalposition on load. Transform
        // matrices also incur a small Euler/float round trip through script.
        std::istringstream input(script);
        std::string result, line;
        bool item{};
        while (std::getline(input, line)) {
            const auto start = line.find_first_not_of(" \t");
            const auto text =
                start == line.npos ? std::string_view{} : std::string_view(line).substr(start);
            if (text.starts_with("instant ")) {
                item = text.starts_with("instant Item ");
            }
            if (item && text.starts_with("originalposition ="))
                continue;
            if (text.starts_with("position =") || text.starts_with("rotation =")) {
                const auto first = line.find('"'), last = line.rfind('"');
                std::istringstream values(line.substr(first + 1, last - first - 1));
                double x{}, y{}, z{};
                CHECK(bool(values >> x >> y >> z));
                const auto clean = [](double v) {
                    return std::abs(v) < .000005 ? 0. : v;
                };
                line = line.substr(0, first) +
                       std::format("\"{:.5f} {:.5f} {:.5f}\";", clean(x), clean(y), clean(z));
            }
            result += line + '\n';
        }
        return result;
    };
    CHECK(normalizeItems(*reloadedScript) == normalizeItems(*missionScript));
    auto reloaded = app.fearPlugin().terrainEditor()->snapshot();
    CHECK(reloaded && reloaded->blocks.size() == saved->blocks.size());
    for (std::size_t i = 0; i < saved->blocks.size(); ++i) {
        CHECK(reloaded->blocks[i].heights == saved->blocks[i].heights);
        CHECK(reloaded->blocks[i].lights == saved->blocks[i].lights);
    }
    key(62);
    CHECK(app.fearPlugin().terrainEditor()->focused);
    auto* secondView = dynamic_cast<gui::TSControl*>(app.guiRuntime()->findControl("EditorGui"));
    CHECK(secondView && secondView->frame());
    key(60);
    CHECK(second->focused());
}

int missionEditorOptions() {
    return test::run(consoleOptions);
}

int missionEditorContracts() {
    return test::run(consoleContracts);
}

int missionEditorUndo() {
    return test::run(undoAndSelectionLifetime);
}

int missionEditorFocus() {
    return test::run(focusAndCameraMap);
}

int missionEditorGui() {
    return test::run(guiPersistenceAndPopups);
}

int missionEditorTerrainStatus() {
    return test::run(terrainConsoleStatus);
}

int missionEditorSave() {
    return test::run(missionSaveRoundTrip);
}

int missionEditorViewport() {
    return test::run(viewportMouseEditing);
}

int missionEditorTerrainFocus() {
    return test::run(terrainFocusAndDisplay);
}

int missionEditorFilePaste() {
    return test::run(prefabPasteAndLighting);
}

int missionEditorInstalled() {
    return test::run(installedEditorAcceptance);
}
}

OPENT1_REGISTER_TEST("game", "mission_editor_options", missionEditorOptions);
OPENT1_REGISTER_TEST("game", "mission_editor_contracts", missionEditorContracts);
OPENT1_REGISTER_TEST("game", "mission_editor_undo_lifetimes", missionEditorUndo);
OPENT1_REGISTER_TEST("game", "mission_editor_focus", missionEditorFocus);
OPENT1_REGISTER_TEST("game", "mission_editor_gui", missionEditorGui);
OPENT1_REGISTER_TEST("game", "mission_editor_terrain_status", missionEditorTerrainStatus);
OPENT1_REGISTER_TEST("game", "mission_editor_save", missionEditorSave);
OPENT1_REGISTER_TEST("game", "mission_editor_viewport", missionEditorViewport);
OPENT1_REGISTER_TEST("game", "mission_editor_terrain_focus", missionEditorTerrainFocus);
OPENT1_REGISTER_TEST("game", "mission_editor_file_paste", missionEditorFilePaste);
OPENT1_REGISTER_TEST("editor_acceptance", "installed_editor_acceptance", missionEditorInstalled);
