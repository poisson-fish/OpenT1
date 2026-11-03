#include <opent1/game/fear_mission_editor.hpp>
#include <ranges>

#include <charconv>
#include <format>
#include <filesystem>
#include <fstream>
#include <chrono>
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
};

[[nodiscard]] Int32 parseIntArg(std::string_view sv) noexcept {
    Int32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
}

[[nodiscard]] UInt32 parseUIntArg(std::string_view sv) noexcept {
    UInt32 val = 0;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return val;
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
}

void FearMissionPlugin::endFrame() {}

std::optional<std::string>
FearMissionPlugin::handleCommand(Console& console, Int32 id, std::span<const std::string> argv) {
    if (!manager_) {
        return "False";
    }
    if (!me_) {
        me_ = dynamic_cast<FearMissionEditor*>(manager_->findObject(sim::MissionEditorId));
    }

    switch (id) {
        case MECreate: {
            if (!me_) {
                auto created = std::make_unique<FearMissionEditor>();
                created->bindGame(game_);
                me_ = created.get();
                if (manager_->addObject(std::move(created))) {
                    manager_->assignId(*me_, sim::MissionEditorId);
                    me_->assignName("MissionEditor");
                    me_->init();
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
                if (!manager_->findObject(name) && !me_->getAddGroup()->findObject(name)) {
                    break;
                }
            }
            // newObject uses the registered game factory and rolls back on failed
            // processArguments. Passing arguments directly preserves quoted values.
            std::vector<std::string> args{name, argv[2]};
            args.insert(args.end(), argv.begin() + 3, argv.end());
            const auto result = console.execute("newObject", args);
            auto* object = result.ok() ? manager_->findObject(result.value) : nullptr;
            if (!object) {
                return "False";
            }
            if (!me_->addObject(object)) {
                object->deleteObject();
                return "False";
            }
            me_->selection().clearSet();
            me_->selection().addObject(*object);
            me_->onDrop(object);
            if (me_->flags() & FearMissionEditor::DropToGround) {
                me_->dropObjectDown(object);
            }
            return std::to_string(object->getId());
        }

        case MERegisterType: {
            if (argv.size() < 3 || !me_) {
                return "False";
            }
            const UInt32 mask = parseUIntArg(argv[1]);
            const std::string_view desc = argv[2];
            const std::string_view lockedBmp =
                (argv.size() >= 4) ? std::string_view(argv[3]) : std::string_view{};
            const std::string_view unlockedBmp =
                (argv.size() >= 5) ? std::string_view(argv[4]) : std::string_view{};
            return me_->registerType(mask, desc, lockedBmp, unlockedBmp) ? "True" : "False";
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
            auto* group = manager_->findObject("MissionGroup");
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
            const std::filesystem::path destination(mission);
            static std::atomic<UInt64> serial{};
            auto temporary = destination;
            temporary +=
                ".saving-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(serial.fetch_add(1));
            std::error_code ec;
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
            if (auto* obj = manager_->findObject(argv[1])) {
                me_->addObject(obj);
                return "True";
            }
            return "False";
        }

        case MESelectObject:
        case MEUnselectObject: {
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            if (auto* obj = manager_->findObject(argv[1])) {
                if (id == MESelectObject) {
                    me_->selection().addObject(*obj);
                } else {
                    me_->selection().removeObject(*obj);
                }
                return "True";
            }
            return "False";
        }

        case MECreateGroup: {
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            auto grp = std::make_unique<sim::SimGroup>();
            auto* raw = grp.get();
            if (manager_->addObject(std::move(grp))) {
                raw->assignName(argv[1]);
                me_->addObject(raw);
                return std::format("{}", raw->getId());
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
            if (argv.size() < 2 || !me_) {
                return "False";
            }
            me_->setPlaceMask(parseUIntArg(argv[1]));
            return "True";

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
