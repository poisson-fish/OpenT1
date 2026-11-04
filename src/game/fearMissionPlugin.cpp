#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/core/filename.hpp>
#include <ranges>

#include <charconv>
#include <format>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif

namespace opent1::game {

namespace {

enum MECommandId : Int32 {
    MissionCreateObject = 1,
    MECreate = 2,
    MERegisterType = 3,
    MEGetConsoleOptions = 4,
    MEDeleteSelection = 5,
    MECopySelection = 6,
    MECutSelection = 7,
    MEPasteSelection = 8,
    MEDuplicateSelection = 9,
    MEDropSelection = 10,
    MESave = 11,
    MEPlaceBookmark = 12,
    MEGotoBookmark = 13,
    MEObjectsToCamera = 14,
    MECameraToObjects = 15,
    MEClearSelection = 16,
    MEAddObject = 17,
    MESelectObject = 18,
    MEUnselectObject = 19,
    MECreateGroup = 20,
    MESetGrabMask = 21,
    MESetPlaceMask = 22,
    MESetDefaultPlaceMask = 23,
    MESetHandCursor = 24,
    MEUndo = 25,
    MERebuildCommandMap = 26,
    MEOnSelected,
    MEOnUnselected,
    MEObjectToCamera,
    MECameraToObject,
    MEOnUseTerrainGrid,
    MERedo,
    MEObjectToSC,
    MEPasteFile,
    MEMissionLight,
};

[[nodiscard]] Int32 parseIntArg(std::string_view sv) noexcept {
    Int32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

[[nodiscard]] UInt32 parseUIntArg(std::string_view sv) noexcept {
    // The shipped scripts pass decimal complements (e.g. -68). Convert the
    // signed value modulo 2^32, as vanilla's atoi-to-DWORD assignment does.
    if (sv.starts_with('-')) {
        Int32 val{};
        const auto result = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        return result.ec == std::errc{} && result.ptr == sv.data() + sv.size()
                   ? static_cast<UInt32>(val)
                   : 0;
    }
    UInt32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

std::string missionWriteName(Console& console, sim::SimManager& manager, const std::string& name) {
    auto* service = sim::SimResource::find(manager);
    const auto assetRoot = console.getVariable("OpenT1::AssetRoot");
    const auto source =
        service ? service->get().getFullPath(FileName(name).getTitle()) : std::nullopt;
    if (!assetRoot.empty() && source) {
        for (const auto& root : {std::filesystem::path(assetRoot), console.filePath("")}) {
            const auto relative = source->lexically_relative(root);
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                return relative.generic_string();
        }
    }
    return name;
}

} // namespace

void FearMissionPlugin::init(Console& console, sim::SimManager* manager) {
    console_ = &console;
    manager_ = manager;

    auto bindCmd = [this, &console](Int32 id, std::string_view name) {
        console.addCommand(
            id, name, [this](Console& c, Int32 cmdId, std::span<const std::string> argv) {
                return handleCommand(c, cmdId, argv);
            });
    };

    bindCmd(MissionCreateObject, "MissionCreateObject");
    bindCmd(MECreate, "ME::Create");
    bindCmd(MERegisterType, "ME::RegisterType");
    bindCmd(MEGetConsoleOptions, "ME::GetConsoleOptions");
    bindCmd(MEDeleteSelection, "ME::DeleteSelection");
    bindCmd(MECopySelection, "ME::CopySelection");
    bindCmd(MECutSelection, "ME::CutSelection");
    bindCmd(MEPasteSelection, "ME::PasteSelection");
    bindCmd(MEDuplicateSelection, "ME::DuplicateSelection");
    bindCmd(MEDropSelection, "ME::DropSelection");
    bindCmd(MESave, "ME::Save");
    bindCmd(MEPlaceBookmark, "ME::PlaceBookmark");
    bindCmd(MEGotoBookmark, "ME::GotoBookmark");
    bindCmd(MEObjectsToCamera, "ME::ObjectsToCamera");
    bindCmd(MECameraToObjects, "ME::CameraToObjects");
    bindCmd(MEClearSelection, "ME::ClearSelection");
    bindCmd(MEAddObject, "ME::AddObject");
    bindCmd(MESelectObject, "ME::SelectObject");
    bindCmd(MEUnselectObject, "ME::UnselectObject");
    bindCmd(MECreateGroup, "ME::CreateGroup");
    bindCmd(MESetGrabMask, "ME::SetGrabMask");
    bindCmd(MESetPlaceMask, "ME::SetPlaceMask");
    bindCmd(MESetDefaultPlaceMask, "ME::SetDefaultPlaceMask");
    bindCmd(MESetHandCursor, "ME::SetHandCursor");
    bindCmd(MEUndo, "ME::Undo");
    bindCmd(MERebuildCommandMap, "ME::RebuildCommandMap");
    bindCmd(MEOnSelected, "ME::onSelected");
    bindCmd(MEOnUnselected, "ME::onUnselected");
    bindCmd(MEClearSelection, "ME::onSelectionCleared");
    bindCmd(MEObjectToCamera, "ME::ObjectToCamera");
    bindCmd(MECameraToObject, "ME::CameraToObject");
    bindCmd(MEOnUseTerrainGrid, "ME::onUseTerrainGrid");
    bindCmd(MERedo, "ME::Redo");
    bindCmd(MEObjectToSC, "ME::ObjectToSC");
    bindCmd(MEPasteFile, "ME::PasteFile");
    bindCmd(MEMissionLight, "ME::MissionLight");
}

void FearMissionPlugin::endFrame() {}

FearMissionEditor* FearMissionPlugin::editor() const noexcept {
    auto* owner = manager_;
    if (game_) {
        auto* client = game_->world(sim::WorldType::Client);
        owner = client ? &client->manager() : nullptr;
    }
    return owner ? dynamic_cast<FearMissionEditor*>(owner->findObject(sim::MissionEditorId))
                 : nullptr;
}

std::optional<std::string>
FearMissionPlugin::handleCommand(Console& console, Int32 id, std::span<const std::string> argv) {
    me_ = editor();
    auto* target = me_ ? me_->targetManager() : nullptr;
    if (!manager_ && !game_) {
        return "False";
    }
    if (id != MECreate && !target) {
        return "False";
    }

    switch (id) {
        case MECreate: {
            auto* client = game_ ? game_->world(sim::WorldType::Client) : nullptr;
            auto* server = game_ ? game_->world(sim::WorldType::Server) : nullptr;
            auto* owner = game_ ? (client ? &client->manager() : nullptr) : manager_;
            auto* editTarget = game_ ? (server ? &server->manager() : nullptr) : manager_;
            if (argv.size() != 2 || !owner || !editTarget ||
                !dynamic_cast<sim::SimCanvas*>(owner->findObject(argv[1]))) {
                return "False";
            }
            if (me_) {
                me_->deleteObject();
                me_ = nullptr;
            }
            {
                auto created = std::make_unique<FearMissionEditor>();
                created->bindGame(game_);
                me_ = created.get();
                if (owner->addObject(std::move(created))) {
                    if (!owner->assignId(*me_, sim::MissionEditorId)) {
                        me_->deleteObject();
                        me_ = nullptr;
                        return "False";
                    }
                    me_->assignName("MissionEditor");
                    me_->setTargetManager(editTarget);
                    if (!me_->init()) {
                        me_->deleteObject();
                        me_ = nullptr;
                        return "False";
                    }
                } else {
                    me_ = nullptr;
                    return "False";
                }
            }
            return "True";
        }

        case MissionCreateObject: {
            if (argv.size() < 3 || !me_) {
                return "False";
            }
            std::string name;
            for (auto index :
                 std::views::iota(UInt32{1}) | std::views::take_while([&](UInt32 index) {
                     return index != 0;
                 })) {
                name = std::format("{}{}", argv[1], index);
                if (!target->findObject(name) && !me_->getAddGroup()->findObject(name)) {
                    break;
                }
            }
            // newObject uses the registered game factory and rolls back on failed
            // processArguments. Passing arguments directly preserves quoted values.
            std::vector<std::string> args{name, argv[2]};
            args.insert(args.end(), argv.begin() + 3, argv.end());
            script::ExecutionResult result;
            const auto create = [&] {
                result = console.execute("newObject", args);
            };
            if (game_) {
                game_->withWorld(sim::WorldType::Server, create);
            } else {
                create();
            }
            auto* object = result.ok() ? target->findObject(result.value) : nullptr;
            if (!object) {
                return "False";
            }
            if (!me_->addObject(object)) {
                object->deleteObject();
                return "False";
            }
            me_->selection().calcCentroid();
            me_->onDrop(object);
            if (me_->flags() & FearMissionEditor::DropToGround) {
                me_->dropObjectDown(object);
            }
            if (console.isFunction("MissionObjectList::ClearSelection")) {
                (void)console.execute("MissionObjectList::ClearSelection");
            }
            me_->selection().clearSet();
            const std::array<std::string, 2> selected{"1", std::to_string(object->getId())};
            if (console.isFunction("MissionObjectList::SelectObject")) {
                (void)console.execute("MissionObjectList::SelectObject", selected);
            }
            me_->selection().addObject(*object);
            if (console.isFunction("MissionObjectList::Inspect")) {
                (void)console.execute("MissionObjectList::Inspect", selected);
            }
            return std::to_string(object->getId());
        }

        case MERegisterType: {
            if (argv.size() < 3 || !me_) {
                return "False";
            }
            const UInt32 mask = parseUIntArg(argv[2]);
            const std::string_view desc = argv[1];
            const std::string_view lockedBmp =
                (argv.size() >= 4) ? std::string_view(argv[3]) : std::string_view{};
            const std::string_view unlockedBmp =
                (argv.size() >= 5) ? std::string_view(argv[4]) : std::string_view{};
            return me_->registerClassType(desc, mask, lockedBmp, unlockedBmp) ? "True" : "False";
        }

        case MEGetConsoleOptions:
            if (!me_) {
                return "False";
            }
            me_->getConsoleOptions();
            return "True";

        case MEDeleteSelection:
            if (!me_) {
                return "False";
            }
            me_->deleteSelection();
            return "True";

        case MECopySelection:
            if (!me_) {
                return "False";
            }
            return me_->copySelection() ? "True" : "False";

        case MECutSelection:
            if (!me_) {
                return "False";
            }
            if (me_->copySelection()) {
                me_->deleteSelection();
                return "True";
            }
            return "False";

        case MEPasteSelection:
            if (!me_) {
                return "False";
            }
            return me_->pasteSelection() ? "True" : "False";

        case MEPasteFile:
            return argv.size() == 2 && me_->pasteFile(argv[1]) ? "True" : "False";

        case MEMissionLight: {
            if (argv.size() > 2)
                return "False";
            console.setVariable("ME::LightingStatus", "Failed");
            if (handleCommand(console, MESave, {}).value_or("False") != "True")
                return "False";
            const bool quick = argv.size() == 2 && script::toBool(argv[1]);
            const auto result = console.execute(
                "Ted::relight", std::array<std::string, 1>{quick ? "false" : "true"});
            if (!result.ok() || !script::toBool(result.value))
                return "False";
            if (handleCommand(console, MESave, {}).value_or("False") != "True") {
                (void)console.execute("Ted::undo");
                return "False";
            }
            console.setVariable("ME::LightingStatus", "TerrainBaked");
            console.print("Mission lighting: terrain lightmaps baked and saved; authored interior "
                          "lightmaps retained.");
            return "True";
        }

        case MEDuplicateSelection:
            if (!me_) {
                return "False";
            }
            return me_->duplicateSelection() ? "True" : "False";

        case MEDropSelection:
            if (!me_) {
                return "False";
            }
            me_->saveInfo();
            me_->dropObjectDown(&me_->selection());
            return "True";

        case MESave: {
            if (!me_) {
                return "False";
            }
            const std::string mission(console.getVariable("missionFile"));
            auto* group = target->findObject("MissionGroup");
            if (mission.empty() || !group) {
                return "False";
            }
            const auto script = me_->exportScript(*group);
            if (!script) {
                return "False";
            }
            const std::string terrain(console.getVariable("Ted::currFile"));
            if (!terrain.empty()) {
                if (!console.isFunction("Ted::save")) {
                    return "False";
                }
                console.setVariable("TED::success", "False");
                const auto result =
                    console.execute("Ted::save", std::array<std::string, 1>{terrain});
                if (!result.ok() || !script::toBool(console.getVariable("TED::success"))) {
                    return "False";
                }
            }
            // Scripts omit the active mod prefix (e.g. base/missions). Mirror
            // the resource's selected directory in the write overlay so a
            // subsequent mission/volume load can discover the saved files.
            const auto destination = console.filePath(missionWriteName(console, *target, mission));
            static std::atomic<UInt64> serial{};
            auto temporary = destination;
            temporary +=
                ".saving-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(serial.fetch_add(1));
            std::error_code ec;
            if (destination.has_parent_path()) {
                std::filesystem::create_directories(destination.parent_path(), ec);
                if (ec)
                    return "False";
            }
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << *script;
                output.flush();
                if (!output) {
                    output.close();
                    std::filesystem::remove(temporary, ec);
                    return "False";
                }
            }
            // Retain the previous mission until a complete replacement exists.
            // A failed replacement is a failure, never a successful save.
#ifdef _WIN32
            const bool replaced =
                MoveFileExW(temporary.c_str(),
                            destination.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            std::filesystem::rename(temporary, destination, ec);
            const bool replaced = !ec;
#endif
            if (!replaced) {
                std::filesystem::remove(temporary, ec);
                return "False";
            }
            console.setVariable("ME::LastSavedMission", destination.generic_string());
            if (console.hasVariable("ConsoleWorld::DefaultSearchPath"))
                console.setVariable("ConsoleWorld::DefaultSearchPath",
                                    console.getVariable("ConsoleWorld::DefaultSearchPath"));
            return "True";
        }

        case MEPlaceBookmark:
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            me_->placeBookmark(parseIntArg(argv[1]));
            return "True";

        case MEGotoBookmark:
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            me_->gotoBookmark(parseIntArg(argv[1]));
            return "True";

        case MEObjectsToCamera:
            if (!me_) {
                return "False";
            }
            me_->saveInfo();
            me_->objectsToCamera();
            return "True";

        case MECameraToObjects:
            if (!me_) {
                return "False";
            }
            me_->cameraToObjects();
            return "True";

        case MEClearSelection:
            if (!me_) {
                return "False";
            }
            me_->selection().clearSet();
            return "True";

        case MEAddObject: {
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            if (auto* obj = target->findObject(argv[1])) {
                return me_->addObject(obj) ? "True" : "False";
            }
            return "False";
        }

        case MESelectObject:
        case MEUnselectObject:
        case MEOnSelected:
        case MEOnUnselected: {
            const bool callback = id == MEOnSelected || id == MEOnUnselected;
            if (argv.size() != (callback ? 3U : 2U) || !me_ || (callback && argv[1] != "1")) {
                return "False";
            }
            if (auto* obj = target->findObject(argv[callback ? 2 : 1])) {
                if (id == MESelectObject || id == MEOnSelected) {
                    me_->selection().addObject(*obj);
                } else {
                    me_->selection().removeObject(*obj);
                }
                return "True";
            }
            return "False";
        }

        case MECreateGroup: {
            if (argv.size() > 2 || !me_) {
                return "False";
            }
            auto grp = std::make_unique<sim::SimGroup>();
            auto* raw = grp.get();
            if (target->addObject(std::move(grp))) {
                raw->assignName(argv.size() == 2 ? argv[1] : "SimGroup");
                if (!me_->addObject(raw)) {
                    raw->deleteObject();
                    return "False";
                }
                return argv.size() == 2 ? std::to_string(raw->getId()) : "True";
            }
            return "False";
        }

        case MESetGrabMask:
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            me_->setGrabMask(parseUIntArg(argv[1]));
            return "True";

        case MESetPlaceMask:
            if (argv.size() < 2 || argv.size() > 3 || !me_) {
                return "False";
            }
            return me_->setClassPlaceMask(argv[1],
                                          argv.size() == 3
                                              ? std::optional<UInt32>(parseUIntArg(argv[2]))
                                              : std::nullopt)
                       ? "True"
                       : "False";

        case MESetDefaultPlaceMask:
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            me_->setDefaultPlaceMask(parseUIntArg(argv[1]));
            return "True";

        case MESetHandCursor:
            return "True";

        case MEUndo:
            if (!me_) {
                return "False";
            }
            me_->restoreInfo();
            return "True";

        case MERedo:
            // Vanilla registers this name but has no redo implementation.
            return "False";

        case MEObjectToCamera:
        case MEObjectToSC:
        case MECameraToObject: {
            auto* obj = target->findObject(console.getVariable("ME::InspectObject"));
            auto* cam = me_->camera();
            if (!obj || !cam || (id != MECameraToObject && obj->isLocked())) {
                return "False";
            }
            sim::SimObjectTransformQuery query;
            if (!obj->processQuery(query)) {
                return "False";
            }
            if (id != MECameraToObject) {
                me_->saveInfo(obj);
                query.transform.p = {};
                sim::SimObjectTransformEvent event;
                event.transform = query.transform;
                obj->processEvent(event);
                me_->onDrop(obj,
                            id == MEObjectToSC ? FearMissionEditor::DropToScreenCenter
                                               : FearMissionEditor::DropAtCamera);
            } else {
                me_->saveInfo(cam);
                cam->setPos(query.transform.p);
            }
            return "True";
        }

        case MEOnUseTerrainGrid: {
            auto* terrain =
                dynamic_cast<world::SimMissionTerrain*>(target->findObject(sim::TerrainId));
            const auto resources = terrain ? terrain->editableResources() : std::nullopt;
            if (!resources || !resources->grid) {
                return "False";
            }
            const auto snap = std::to_string(std::ldexp(1.0, resources->grid->scale));
            console.setVariable("ME::XGridSnap", snap);
            console.setVariable("ME::YGridSnap", snap);
            return "True";
        }

        case MERebuildCommandMap:
            if (!me_) {
                return "False";
            }
            me_->rebuildCommandMap();
            return "True";

        default:
            return "False";
    }
}

} // namespace opent1::game
