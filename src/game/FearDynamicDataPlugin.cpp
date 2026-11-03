#include <opent1/core/filename.hpp>
#include <ranges>
#include "opent1/game/fear_dynamic_data_plugin.hpp"
#include <opent1/core/text.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace opent1::game {

namespace {

constexpr Flt32 kDefaultTerrainGravity = 20.0f;
constexpr Int32 kFirstPersonBit = 1;
constexpr Int32 kChaseCamBit = 2;
constexpr Int32 kThirdPersonBit = 4;

constexpr std::array<std::string_view, 15> kMatTypeNames{{
    "Default",
    "Concrete",
    "Carpet",
    "Metal",
    "Glass",
    "Plastic",
    "Wood",
    "Marble",
    "Snow",
    "Ice",
    "Sand",
    "Mud",
    "Stone",
    "SoftEarth",
    "PackedEarth",
}};

constexpr std::array<std::pair<std::string_view, UInt32>, 11> kSoundFlagTokens{{
    {"SFX_IS_2D", 0u},
    {"SFX_IS_3D", (1u << 4) | (1u << 5)},
    {"SFX_USE_HARDWARE_2D", 0x00000010u},
    {"SFX_USE_HARDWARE_3D", 0x00000020u},
    {"SFX_IS_LOOPING", 1u << 1},
    {"SFX_IS_STREAMING", 1u << 2},
    {"SFX_IS_PITCHING", 1u << 3},
    {"SFX_IS_SOFTWARE_3D", 1u << 4},
    {"SFX_IS_HARDWARE_3D", 1u << 5},
    {"SFX_STREAMING_2D", 1u << 2},
    {"SFX_STREAMING_3D", (1u << 2) | (1u << 4) | (1u << 5)},
}};

[[nodiscard]] std::size_t hashFieldName(std::string_view name, std::size_t tableSize) noexcept {
    if (tableSize == 0) {
        return 0;
    }
    std::uint32_t h = 2166136261u;
    for (char c : name) {
        h ^= static_cast<std::uint32_t>(std::tolower(static_cast<unsigned char>(c)));
        h *= 16777619u;
    }
    return static_cast<std::size_t>(h % tableSize);
}

[[nodiscard]] std::string formatInt(Int32 val) {
    return std::format("{}", val);
}

[[nodiscard]] std::string formatUInt(UInt32 val) {
    return std::format("{}", val);
}

[[nodiscard]] std::string formatFloat(Flt32 val) {
    return std::format("{:.6g}", static_cast<double>(val));
}

[[nodiscard]] std::string formatBool(bool val) {
    return val ? "True" : "False";
}

[[nodiscard]] Int32 parseIntValue(std::string_view str) noexcept {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front())) != 0) {
        str.remove_prefix(1);
    }
    Int32 out = 0;
    (void)std::from_chars(str.data(), str.data() + str.size(), out);
    return out;
}

[[nodiscard]] Flt32 parseFloatValue(std::string_view str) noexcept {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front())) != 0) {
        str.remove_prefix(1);
    }
    Flt32 out = 0.0f;
    (void)std::from_chars(str.data(), str.data() + str.size(), out);
    return out;
}

[[nodiscard]] bool parseBoolValue(std::string_view str) noexcept {
    if (equalIgnoreCase(str, "True") || equalIgnoreCase(str, "true")) {
        return true;
    }
    if (equalIgnoreCase(str, "False") || equalIgnoreCase(str, "false")) {
        return false;
    }
    return parseIntValue(str) != 0;
}

[[nodiscard]] std::string formatPoint3F(const Point3F& pt) {
    return std::format("{:.6g} {:.6g} {:.6g}",
                       static_cast<double>(pt.x),
                       static_cast<double>(pt.y),
                       static_cast<double>(pt.z));
}

[[nodiscard]] std::vector<std::string> splitWhitespace(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) ||
                                   text[i] == '|' || text[i] == ',')) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
               text[i] != '|' && text[i] != ',') {
            ++i;
        }
        tokens.emplace_back(text.substr(start, i - start));
    }
    return tokens;
}

[[nodiscard]] bool
parsePoint3FValues(std::span<const std::string> values, Point3F& out, bool normalize) noexcept {
    Flt32 x = 0.0f;
    Flt32 y = 0.0f;
    Flt32 z = 0.0f;
    if (values.size() == 3) {
        x = parseFloatValue(values[0]);
        y = parseFloatValue(values[1]);
        z = parseFloatValue(values[2]);
    } else if (values.size() == 1) {
        const auto parts = splitWhitespace(values[0]);
        if (parts.size() != 3) {
            return false;
        }
        x = parseFloatValue(parts[0]);
        y = parseFloatValue(parts[1]);
        z = parseFloatValue(parts[2]);
    } else {
        return false;
    }
    out = Point3F{x, y, z};
    if (normalize) {
        const Flt32 len = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
        if (len > 1e-6f) {
            out.x /= len;
            out.y /= len;
            out.z /= len;
        }
    }
    return true;
}

[[nodiscard]] std::string formatColorF(const ColorF& c) {
    return std::format("{:.6g} {:.6g} {:.6g}",
                       static_cast<double>(c.red),
                       static_cast<double>(c.green),
                       static_cast<double>(c.blue));
}

[[nodiscard]] bool parseColorFValues(std::span<const std::string> values, ColorF& out) noexcept {
    Point3F pt{};
    if (!parsePoint3FValues(values, pt, false)) {
        return false;
    }
    out.red = pt.x;
    out.green = pt.y;
    out.blue = pt.z;
    return true;
}

[[nodiscard]] UInt32 parseSoundFlags(std::span<const std::string> values) {
    UInt32 flags = 0;
    for (const auto& arg : values) {
        const auto tokens = splitWhitespace(arg);
        for (const auto& tok : tokens) {
            bool matched = false;
            for (const auto& [name, bit] : kSoundFlagTokens) {
                if (equalIgnoreCase(tok, name)) {
                    flags |= bit;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                std::string_view sv(tok);
                int base = 10;
                if (sv.size() > 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
                    sv.remove_prefix(2);
                    base = 16;
                }
                UInt32 v = 0;
                const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v, base);
                if (ec == std::errc{} && ptr != sv.data()) {
                    flags |= v;
                }
            }
        }
    }
    return flags;
}

[[nodiscard]] std::string
getBlockRef(const FearDynamicDataPlugin& plugin, Int32 blockId, Int32 groupId) {
    if (blockId < 0) {
        return {};
    }
    const auto* dbm = plugin.activeDataBlockManager();
    if (!dbm) {
        return {};
    }
    return std::string(dbm->lookupBlockName(blockId, groupId));
}

bool setBlockRef(FearDynamicDataPlugin& plugin,
                 Int32& targetId,
                 Int32 groupId,
                 std::span<const std::string> values) {
    if (values.size() != 1) {
        return false;
    }
    if (values[0].empty() || values[0] == "-1") {
        targetId = -1;
        return true;
    }
    auto* dbm = plugin.activeDataBlockManager();
    if (!dbm) {
        targetId = -1;
        return false;
    }
    Int32 resolved = -1;
    if (dbm->lookupDataBlock(values[0], groupId, &resolved)) {
        targetId = resolved;
        return true;
    }
    targetId = -1;
    return false;
}

template <typename Derived, typename FieldT>
FearDynamicDataPlugin::FieldEntry makeScalarField(std::string_view name,
                                                  FearDynamicDataPlugin::FieldTypeId type,
                                                  FieldT Derived::* member,
                                                  FearDynamicDataPlugin* pluginPtr) {
    FearDynamicDataPlugin::FieldEntry entry;
    entry.name = std::string(name);
    entry.type = type;
    entry.arraySize = 1;
    entry.getter = [=](const GameBaseData& base, std::size_t) -> std::string {
        const auto* d = dynamic_cast<const Derived*>(&base);
        if (!d) {
            return {};
        }
        const auto& val = d->*member;
        if constexpr (std::is_same_v<FieldT, Int32>) {
            switch (type) {
                case FearDynamicDataPlugin::TypeDamageSkinBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::DamageSkinDataType);
                case FearDynamicDataPlugin::TypeSoundProfileBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::SoundProfileDataType);
                case FearDynamicDataPlugin::TypeSoundBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::SoundDataType);
                case FearDynamicDataPlugin::TypeExplosionBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::ExplosionDataType);
                case FearDynamicDataPlugin::TypeDebrisBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::DebrisDataType);
                case FearDynamicDataPlugin::TypeItemImageBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::ItemImageDataType);
                case FearDynamicDataPlugin::TypeItemBlock:
                    return getBlockRef(*pluginPtr, val, DataBlockManager::ItemDataType);
                default:
                    return formatInt(val);
            }
        } else if constexpr (std::is_same_v<FieldT, UInt32>) {
            return formatUInt(val);
        } else if constexpr (std::is_same_v<FieldT, Flt32>) {
            if (type == FearDynamicDataPlugin::TypeGrenadeDist) {
                const Flt32 dist = (val * val) / kDefaultTerrainGravity;
                return formatFloat(dist);
            }
            return formatFloat(val);
        } else if constexpr (std::is_same_v<FieldT, bool>) {
            return formatBool(val);
        } else if constexpr (std::is_same_v<FieldT, std::string>) {
            return val;
        } else if constexpr (std::is_same_v<FieldT, Point3F>) {
            return formatPoint3F(val);
        } else if constexpr (std::is_same_v<FieldT, ColorF>) {
            return formatColorF(val);
        } else if constexpr (std::is_same_v<FieldT, StaticBaseData::Damage>) {
            return std::format("{:.6g} {:.6g}",
                               static_cast<double>(val.disabled),
                               static_cast<double>(val.destroyed));
        } else if constexpr (std::is_same_v<FieldT, ProjectileDataType>) {
            if (val.type < 0 || val.dataType < 0) {
                return {};
            }
            return getBlockRef(*pluginPtr, val.dataType, val.type);
        } else {
            return {};
        }
    };
    entry.setter = [=](GameBaseData& base,
                       std::size_t,
                       std::span<const std::string> values,
                       FearDynamicDataPlugin& plugin) -> bool {
        (void)plugin;
        auto* d = dynamic_cast<Derived*>(&base);
        if (!d || values.empty()) {
            return false;
        }
        auto& val = d->*member;
        if constexpr (std::is_same_v<FieldT, Int32>) {
            switch (type) {
                case FearDynamicDataPlugin::TypeDamageSkinBlock:
                    return setBlockRef(plugin, val, DataBlockManager::DamageSkinDataType, values);
                case FearDynamicDataPlugin::TypeSoundProfileBlock:
                    return setBlockRef(plugin, val, DataBlockManager::SoundProfileDataType, values);
                case FearDynamicDataPlugin::TypeSoundBlock:
                    return setBlockRef(plugin, val, DataBlockManager::SoundDataType, values);
                case FearDynamicDataPlugin::TypeExplosionBlock:
                    return setBlockRef(plugin, val, DataBlockManager::ExplosionDataType, values);
                case FearDynamicDataPlugin::TypeDebrisBlock:
                    return setBlockRef(plugin, val, DataBlockManager::DebrisDataType, values);
                case FearDynamicDataPlugin::TypeItemImageBlock:
                    return setBlockRef(plugin, val, DataBlockManager::ItemImageDataType, values);
                case FearDynamicDataPlugin::TypeItemBlock:
                    return setBlockRef(plugin, val, DataBlockManager::ItemDataType, values);
                default:
                    if (values.size() != 1) {
                        return false;
                    }
                    val = parseIntValue(values[0]);
                    return true;
            }
        } else if constexpr (std::is_same_v<FieldT, UInt32>) {
            if (type == FearDynamicDataPlugin::TypeSoundFlags) {
                val = parseSoundFlags(values);
                return true;
            }
            if (values.size() != 1) {
                return false;
            }
            val = static_cast<UInt32>(parseIntValue(values[0]));
            return true;
        } else if constexpr (std::is_same_v<FieldT, Flt32>) {
            if (values.size() != 1) {
                return false;
            }
            const Flt32 parsed = parseFloatValue(values[0]);
            if (type == FearDynamicDataPlugin::TypeGrenadeDist) {
                val = std::sqrt(std::abs(parsed) * kDefaultTerrainGravity);
            } else {
                val = parsed;
            }
            return true;
        } else if constexpr (std::is_same_v<FieldT, bool>) {
            if (values.size() != 1) {
                return false;
            }
            val = parseBoolValue(values[0]);
            return true;
        } else if constexpr (std::is_same_v<FieldT, std::string>) {
            if (values.size() != 1) {
                return false;
            }
            val = values[0];
            return true;
        } else if constexpr (std::is_same_v<FieldT, Point3F>) {
            return parsePoint3FValues(values, val, type == FearDynamicDataPlugin::TypeNormalVector);
        } else if constexpr (std::is_same_v<FieldT, ColorF>) {
            return parseColorFValues(values, val);
        } else if constexpr (std::is_same_v<FieldT, StaticBaseData::Damage>) {
            if (values.size() == 2) {
                val.disabled = parseFloatValue(values[0]);
                val.destroyed = parseFloatValue(values[1]);
                return true;
            }
            if (values.size() == 1) {
                const auto parts = splitWhitespace(values[0]);
                if (parts.size() == 2) {
                    val.disabled = parseFloatValue(parts[0]);
                    val.destroyed = parseFloatValue(parts[1]);
                    return true;
                }
            }
            return false;
        } else if constexpr (std::is_same_v<FieldT, ProjectileDataType>) {
            if (values.size() != 1) {
                return false;
            }
            auto* dbm = plugin.activeDataBlockManager();
            if (!dbm) {
                val.type = -1;
                val.dataType = -1;
                return false;
            }
            static constexpr std::array<Int32, 8> kProjectileGroups{{
                DataBlockManager::BulletDataType,
                DataBlockManager::GrenadeDataType,
                DataBlockManager::RocketDataType,
                DataBlockManager::LaserDataType,
                DataBlockManager::TargetLaserDataType,
                DataBlockManager::SeekingMissileDataType,
                DataBlockManager::LightningDataType,
                DataBlockManager::RepairEffectDataType,
            }};
            for (Int32 g : kProjectileGroups) {
                Int32 id = -1;
                if (dbm->lookupDataBlock(values[0], g, &id)) {
                    val.type = g;
                    val.dataType = id;
                    return true;
                }
            }
            val.type = -1;
            val.dataType = -1;
            return false;
        } else {
            return false;
        }
    };
    return entry;
}

template <typename Derived, typename ElemT, std::size_t N>
FearDynamicDataPlugin::FieldEntry makeArrayField(std::string_view name,
                                                 FearDynamicDataPlugin::FieldTypeId type,
                                                 std::array<ElemT, N> Derived::* member,
                                                 FearDynamicDataPlugin* pluginPtr) {
    FearDynamicDataPlugin::FieldEntry entry;
    entry.name = std::string(name);
    entry.type = type;
    entry.arraySize = N;
    entry.getter = [=](const GameBaseData& base, std::size_t idx) -> std::string {
        const auto* d = dynamic_cast<const Derived*>(&base);
        if (!d || idx >= N) {
            return {};
        }
        const auto& val = (d->*member)[idx];
        if constexpr (std::is_same_v<ElemT, Int32>) {
            if (type == FearDynamicDataPlugin::TypeSoundBlock) {
                return getBlockRef(*pluginPtr, val, DataBlockManager::SoundDataType);
            }
            return formatInt(val);
        } else if constexpr (std::is_same_v<ElemT, Flt32>) {
            return formatFloat(val);
        } else if constexpr (std::is_same_v<ElemT, std::string>) {
            return val;
        } else if constexpr (std::is_same_v<ElemT, Point3F>) {
            return formatPoint3F(val);
        } else if constexpr (std::is_same_v<ElemT, ColorF>) {
            return formatColorF(val);
        } else if constexpr (std::is_same_v<ElemT, ShapeBaseData::SequenceSound>) {
            if (val.sequence.empty() || val.sound == -1) {
                return {};
            }
            const std::string snd =
                getBlockRef(*pluginPtr, val.sound, DataBlockManager::SoundDataType);
            return val.sequence + " " + snd;
        } else if constexpr (std::is_same_v<ElemT, PlayerData::AnimData>) {
            if (val.name.empty()) {
                return {};
            }
            const std::string snd =
                getBlockRef(*pluginPtr, val.soundTag, DataBlockManager::SoundDataType);
            return std::format("{} {} {} {} {} {} {}",
                               val.name,
                               snd,
                               val.direction,
                               (val.viewFlags & kFirstPersonBit) ? 1 : 0,
                               (val.viewFlags & kChaseCamBit) ? 1 : 0,
                               (val.viewFlags & kThirdPersonBit) ? 1 : 0,
                               val.priority);
        } else if constexpr (std::is_same_v<ElemT, CarData::WheelInfo>) {
            return formatPoint3F(val.wheelBase);
        } else {
            return {};
        }
    };
    entry.setter = [=](GameBaseData& base,
                       std::size_t idx,
                       std::span<const std::string> values,
                       FearDynamicDataPlugin& plugin) -> bool {
        (void)plugin;
        auto* d = dynamic_cast<Derived*>(&base);
        if (!d || idx >= N || values.empty()) {
            return false;
        }
        auto& val = (d->*member)[idx];
        if constexpr (std::is_same_v<ElemT, Int32>) {
            if (type == FearDynamicDataPlugin::TypeSoundBlock) {
                return setBlockRef(plugin, val, DataBlockManager::SoundDataType, values);
            }
            if (values.size() != 1) {
                return false;
            }
            val = parseIntValue(values[0]);
            return true;
        } else if constexpr (std::is_same_v<ElemT, Flt32>) {
            if (values.size() != 1) {
                return false;
            }
            val = parseFloatValue(values[0]);
            return true;
        } else if constexpr (std::is_same_v<ElemT, std::string>) {
            if (values.size() != 1) {
                return false;
            }
            val = values[0];
            return true;
        } else if constexpr (std::is_same_v<ElemT, Point3F>) {
            return parsePoint3FValues(values, val, false);
        } else if constexpr (std::is_same_v<ElemT, ColorF>) {
            return parseColorFValues(values, val);
        } else if constexpr (std::is_same_v<ElemT, ShapeBaseData::SequenceSound>) {
            std::vector<std::string> args(values.begin(), values.end());
            if (args.size() == 1) {
                args = splitWhitespace(args[0]);
            }
            if (args.size() != 2) {
                return false;
            }
            auto* dbm = plugin.activeDataBlockManager();
            Int32 sndId = -1;
            if (dbm && dbm->lookupDataBlock(args[1], DataBlockManager::SoundDataType, &sndId)) {
                val.sequence = args[0];
                val.sound = sndId;
                return true;
            }
            val.sequence.clear();
            val.sound = -1;
            return false;
        } else if constexpr (std::is_same_v<ElemT, PlayerData::AnimData>) {
            std::vector<std::string> args(values.begin(), values.end());
            if (args.size() == 1) {
                args = splitWhitespace(args[0]);
            }
            if (args.size() < 7) {
                return false;
            }
            val.name = args[0];
            val.soundTag = -1;
            if (!equalIgnoreCase(args[1], "none") && !args[1].empty()) {
                if (auto* dbm = plugin.activeDataBlockManager()) {
                    Int32 sndId = -1;
                    if (dbm->lookupDataBlock(args[1], DataBlockManager::SoundDataType, &sndId)) {
                        val.soundTag = sndId;
                    }
                }
            }
            val.direction = parseIntValue(args[2]);
            val.viewFlags = 0;
            if (parseBoolValue(args[3])) {
                val.viewFlags |= kFirstPersonBit;
            }
            if (parseBoolValue(args[4])) {
                val.viewFlags |= kChaseCamBit;
            }
            if (parseBoolValue(args[5])) {
                val.viewFlags |= kThirdPersonBit;
            }
            if (args.size() >= 8) {
                if (parseBoolValue(args[6])) {
                    val.viewFlags |= (1 << 3);
                }
                val.priority = parseIntValue(args[7]);
            } else {
                val.priority = parseIntValue(args[6]);
            }
            return true;
        } else if constexpr (std::is_same_v<ElemT, CarData::WheelInfo>) {
            return parsePoint3FValues(values, val.wheelBase, false);
        } else {
            return false;
        }
    };
    return entry;
}

} // namespace

FearDynamicDataPlugin::FearDynamicDataPlugin(std::shared_ptr<ResourceManager> resources,
                                             std::filesystem::path writeRoot)
    : resources_(std::move(resources)), writeRoot_(std::move(writeRoot)),
      fallbackDbm_(std::make_shared<DataBlockManager>(true, resources_)) {
    registerClasses();
}

void FearDynamicDataPlugin::init(Console& console, sim::SimManager* manager) {
    console_ = &console;
    manager_ = manager;

    console.addCommand(0,
                       "setMaterialProperty",
                       [this](Console& con,
                              Int32,
                              std::span<const std::string> argv) -> std::optional<std::string> {
                           if (argv.size() != 4) {
                               con.print("setMaterialProperty <matType (Default, Concrete, etc)> "
                                         "<friction> <elasticity>");
                               return std::string("False");
                           }
                           Int32 typeIndex = -1;
                           for (auto i : std::views::iota(std::size_t{0}, kMatTypeNames.size())) {
                               if (equalIgnoreCase(argv[1], kMatTypeNames[i])) {
                                   typeIndex = static_cast<Int32>(i);
                                   break;
                               }
                           }
                           if (typeIndex < 0) {
                               return std::string("False");
                           }
                           MaterialPropertyOverride prop{};
                           prop.surfaceType = typeIndex;
                           prop.friction = parseFloatValue(argv[2]);
                           prop.elasticity = parseFloatValue(argv[3]);
                           materialProps_[typeIndex] = prop;
                           return std::string("True");
                       });

    console.addCommand(
        1,
        "exportObjectToScript",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::optional<std::string> {
            if (argv.size() < 3 || !manager_) {
                return std::string("False");
            }
            auto* obj = manager_->findObject(argv[1]);
            if (!obj) {
                return std::string("False");
            }
            const auto targetPath = resolveFilePath(writeRoot_, argv[2]);
            std::error_code ec;
            if (targetPath.has_parent_path()) {
                std::filesystem::create_directories(targetPath.parent_path(), ec);
            }
            std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::string("False");
            }
            out << "//--- export object begin ---//\r\n";
            if (!obj->getName().empty()) {
                out << "instant SimObject \"" << obj->getName() << "\";\r\n";
            } else {
                out << "instant SimObject;\r\n";
            }
            out << "//--- export object end ---//\r\n";
            for (const auto& line : exportLines_) {
                out << line << "\r\n";
            }
            return std::string("True");
        });

    console.addCommand(
        2,
        "setInstantGroup",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::optional<std::string> {
            if (argv.size() != 2) {
                return std::string("False");
            }
            instantGroupStr_ = argv[1];
            instantGroupId_ = parseIntValue(argv[1]);
            if (manager_) {
                if (auto* obj = manager_->findObject(argv[1])) {
                    if (dynamic_cast<sim::SimGroup*>(obj)) {
                        instantGroupId_ = static_cast<Int32>(obj->getId());
                        return std::string("True");
                    }
                    return std::string("False");
                }
            }
            return std::string("True");
        });

    console.addCommand(
        3,
        "flushExportText",
        [this](Console&, Int32, std::span<const std::string>) -> std::optional<std::string> {
            exportLines_.clear();
            return std::string("True");
        });

    console.addCommand(
        4,
        "addExportText",
        [this](Console&, Int32, std::span<const std::string> argv) -> std::optional<std::string> {
            if (argv.size() != 2) {
                return std::string("False");
            }
            exportLines_.push_back(argv[1]);
            return std::string("True");
        });
}

void FearDynamicDataPlugin::shutdown() noexcept {
    if (console_) {
        console_->removeCommand("setMaterialProperty");
        console_->removeCommand("exportObjectToScript");
        console_->removeCommand("setInstantGroup");
        console_->removeCommand("flushExportText");
        console_->removeCommand("addExportText");
    }
    transactionStack_.clear();
    console_ = nullptr;
    manager_ = nullptr;
}

void FearDynamicDataPlugin::setManager(sim::SimManager* manager) {
    manager_ = manager;
}

DataBlockManager* FearDynamicDataPlugin::activeDataBlockManager() const noexcept {
    if (manager_) {
        if (auto* obj = manager_->findObject(DataBlockManagerId)) {
            if (auto* dbm = dynamic_cast<DataBlockManager*>(obj)) {
                return dbm;
            }
        }
    }
    return fallbackDbm_.get();
}

DataBlockManager& FearDynamicDataPlugin::ensureDataBlockManager() {
    if (auto* dbm = activeDataBlockManager()) {
        return *dbm;
    }
    if (!fallbackDbm_) {
        fallbackDbm_ = std::make_shared<DataBlockManager>(true, resources_);
    }
    return *fallbackDbm_;
}

const FearDynamicDataPlugin::ClassSchema*
FearDynamicDataPlugin::findClassSchema(std::string_view className) const noexcept {
    for (const auto& schema : schemas_) {
        if (equalIgnoreCase(schema.name, className)) {
            return &schema;
        }
    }
    return nullptr;
}

const FearDynamicDataPlugin::FieldEntry*
FearDynamicDataPlugin::findClassField(std::string_view className,
                                      std::string_view fieldName) const noexcept {
    const auto* schema = findClassSchema(className);
    if (!schema || schema->hashTableSize == 0) {
        return nullptr;
    }
    const std::size_t bucket = hashFieldName(fieldName, schema->hashTableSize);
    for (std::size_t idx : schema->hashBuckets[bucket]) {
        if (equalIgnoreCase(schema->fields[idx].name, fieldName)) {
            return &schema->fields[idx];
        }
    }
    return nullptr;
}

bool FearDynamicDataPlugin::contains(std::string_view name) const {
    if (manager_ && manager_->findObject(name)) {
        return true;
    }
    if (const auto* dbm = activeDataBlockManager()) {
        return dbm->lookupDataBlock(name) != nullptr;
    }
    return false;
}

void FearDynamicDataPlugin::declareDataBlock(std::string_view className, std::string_view name) {
    auto& dbm = ensureDataBlockManager();
    PendingTransaction tx;
    tx.dbm = &dbm;
    tx.snapshot = dbm.captureSnapshot();
    tx.className = std::string(className);
    tx.blockName = std::string(name);

    if (dbm.lookupDataBlock(name) != nullptr) {
        if (console_) {
            console_->print("warning: data block " + std::string(name) + " already declared");
        }
        tx.wasExisting = true;
        transactionStack_.push_back(std::move(tx));
        return;
    }

    const auto* schema = findClassSchema(className);
    if (!schema || schema->groupId < 0) {
        if (console_) {
            console_->print("Unknown class " + std::string(className));
        }
        transactionStack_.push_back(std::move(tx));
        return;
    }

    if (auto* created = dbm.createDataBlock(
            console_ ? console_->internString(name) : std::string(name), schema->groupId)) {
        created->pluginClassName = schema->name;
        tx.groupId = created->dbmGroup;
        tx.blockId = created->dbmIndex;
    }
    transactionStack_.push_back(std::move(tx));
}

void FearDynamicDataPlugin::endDataBlock() {
    if (!transactionStack_.empty()) {
        transactionStack_.pop_back();
    }
}

void FearDynamicDataPlugin::abortDataBlock() {
    if (transactionStack_.empty()) {
        return;
    }
    auto tx = std::move(transactionStack_.back());
    transactionStack_.pop_back();
    if (tx.dbm) {
        tx.dbm->restoreSnapshot(tx.snapshot);
    }
}

GameBaseData* FearDynamicDataPlugin::resolveTargetBlock(
    std::optional<std::string_view> objectName) const noexcept {
    auto* dbm = activeDataBlockManager();
    if (!dbm) {
        return nullptr;
    }
    if (objectName.has_value() && !objectName->empty()) {
        return dbm->lookupDataBlock(*objectName);
    }
    if (!transactionStack_.empty()) {
        const auto& tx = transactionStack_.back();
        if (!tx.wasExisting && tx.groupId >= 0 && tx.blockId >= 0) {
            return dbm->lookupDataBlock(tx.blockId, tx.groupId);
        }
    }
    return nullptr;
}

std::string FearDynamicDataPlugin::getDataField(std::string_view objectName,
                                                std::string_view fieldName,
                                                std::optional<std::string_view> arrayIndex) {
    if (manager_ && !objectName.empty()) {
        if (auto* obj = manager_->findObject(objectName)) {
            if (!arrayIndex.has_value() || arrayIndex->empty()) {
                const auto val = obj->getFieldValue(fieldName);
                return val ? std::string(*val) : std::string{};
            }
            std::string combined(fieldName);
            combined.append(*arrayIndex);
            const auto val = obj->getFieldValue(combined);
            return val ? std::string(*val) : std::string{};
        }
    }

    auto* dbm = activeDataBlockManager();
    if (!dbm) {
        return "False";
    }
    auto* dat = dbm->lookupDataBlock(objectName);
    if (!dat) {
        return "False";
    }
    const auto* fld = findClassField(dat->pluginClassName, fieldName);
    if (!fld) {
        if (console_) {
            console_->print("No such field: " + std::string(fieldName) + " in block type " +
                            std::string(objectName));
        }
        return "False";
    }

    const Int32 idx =
        (arrayIndex.has_value() && !arrayIndex->empty()) ? parseIntValue(*arrayIndex) : -1;
    if (idx == -1 && fld->arraySize == 1) {
        const auto value = fld->getter(*dat, 0);
        return console_ && fld->type == TypeString ? console_->internString(value) : value;
    }
    if (idx >= 0 && static_cast<std::size_t>(idx) < fld->arraySize) {
        const auto value = fld->getter(*dat, static_cast<std::size_t>(idx));
        return console_ && fld->type == TypeString ? console_->internString(value) : value;
    }
    return "False";
}

void FearDynamicDataPlugin::setDataField(std::optional<std::string_view> objectName,
                                         std::string_view fieldName,
                                         std::optional<std::string_view> arrayIndex,
                                         std::span<const std::string> values) {
    if (values.empty()) {
        return;
    }
    if (manager_ && objectName.has_value() && !objectName->empty()) {
        if (auto* obj = manager_->findObject(*objectName)) {
            if (!arrayIndex.has_value() || arrayIndex->empty()) {
                obj->setFieldValue(fieldName, values[0]);
            } else {
                std::string combined(fieldName);
                combined.append(*arrayIndex);
                obj->setFieldValue(combined, values[0]);
            }
            return;
        }
    }

    auto* dat = resolveTargetBlock(objectName);
    if (!dat) {
        return;
    }
    const auto* fld = findClassField(dat->pluginClassName, fieldName);
    if (!fld) {
        return;
    }

    std::vector<std::string> strings;
    if (console_ && fld->type == TypeString) {
        for (const auto& value : values) {
            strings.push_back(console_->internString(value));
        }
        values = strings;
    }

    const Int32 idx =
        (arrayIndex.has_value() && !arrayIndex->empty()) ? parseIntValue(*arrayIndex) : -1;
    if (idx == -1 && fld->arraySize == 1) {
        fld->setter(*dat, 0, values, *this);
    } else if (idx >= 0 && static_cast<std::size_t>(idx) < fld->arraySize && fld->arraySize > 1) {
        fld->setter(*dat, static_cast<std::size_t>(idx), values, *this);
    } else if (idx == -1 && values.size() == fld->arraySize) {
        for (auto i : std::views::iota(std::size_t{0}, values.size())) {
            fld->setter(*dat, i, values.subspan(i, 1), *this);
        }
    } else if (console_) {
        console_->print("setDataField: array set access out of range.");
    }
}

FearDynamicDataPlugin::ClassSchema& FearDynamicDataPlugin::registerClass(
    std::string_view className,
    std::string_view superClassName,
    Int32 groupId,
    const std::function<void(std::vector<FieldEntry>&)>& buildFields) {
    ClassSchema schema;
    schema.name = std::string(className);
    schema.superClassName = std::string(superClassName);
    schema.groupId = groupId;

    if (!superClassName.empty()) {
        if (const auto* superSchema = findClassSchema(superClassName)) {
            schema.fields = superSchema->fields;
        }
    }
    std::vector<FieldEntry> ownFields;
    buildFields(ownFields);
    for (auto& f : ownFields) {
        auto it = std::find_if(
            schema.fields.begin(), schema.fields.end(), [&](const FieldEntry& existing) {
                return equalIgnoreCase(existing.name, f.name);
            });
        if (it != schema.fields.end()) {
            *it = std::move(f);
        } else {
            schema.fields.push_back(std::move(f));
        }
    }

    schema.hashTableSize = schema.fields.size() * 2 + 1;
    schema.hashBuckets.resize(schema.hashTableSize);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < schema.fields.size();
                  })) {
        const std::size_t bucket = hashFieldName(schema.fields[i].name, schema.hashTableSize);
        schema.hashBuckets[bucket].push_back(i);
    }

    schemas_.push_back(std::move(schema));
    return schemas_.back();
}

void FearDynamicDataPlugin::registerClasses() {
    schemas_.clear();
    schemas_.reserve(34);

    registerClass("GameBaseData", "", -1, [this](std::vector<FieldEntry>& f) {
        f.push_back(makeScalarField("className", TypeString, &GameBaseData::className, this));
        f.push_back(makeScalarField("mapFilter", TypeInt, &GameBaseData::mapFilter, this));
        f.push_back(makeScalarField("mapIcon", TypeString, &GameBaseData::mapIcon, this));
        f.push_back(
            makeScalarField("visibleToSensor", TypeBool, &GameBaseData::visibleToSensor, this));
        f.push_back(makeScalarField("validateShape", TypeBool, &GameBaseData::validateShape, this));
        f.push_back(
            makeScalarField("validateMaterials", TypeBool, &GameBaseData::validateMaterials, this));
        f.push_back(makeScalarField("description", TypeString, &GameBaseData::description, this));
    });

    registerClass(
        "SoundProfileData",
        "",
        DataBlockManager::SoundProfileDataType,
        [](std::vector<FieldEntry>& f) {
            {
                FieldEntry e{"baseVolume",
                             TypeFloat,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatFloat(d->profile.baseVolume) : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d || v.size() != 1)
                                     return false;
                                 d->profile.baseVolume = parseFloatValue(v[0]);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"minDistance",
                             TypeFloat,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatFloat(d->profile.minDistance) : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d || v.size() != 1)
                                     return false;
                                 d->profile.minDistance = parseFloatValue(v[0]);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"maxDistance",
                             TypeFloat,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatFloat(d->profile.maxDistance) : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d || v.size() != 1)
                                     return false;
                                 d->profile.maxDistance = parseFloatValue(v[0]);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"coneInsideAngle",
                             TypeFloat,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatFloat(d->profile.coneInsideAngle) : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d || v.size() != 1)
                                     return false;
                                 d->profile.coneInsideAngle = parseFloatValue(v[0]);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"coneOutsideAngle",
                             TypeFloat,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatFloat(d->profile.coneOutsideAngle)
                                          : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d || v.size() != 1)
                                     return false;
                                 d->profile.coneOutsideAngle = parseFloatValue(v[0]);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"coneVector",
                             TypeNormalVector,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatPoint3F(d->profile.coneVector) : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d)
                                     return false;
                                 return parsePoint3FValues(v, d->profile.coneVector, true);
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"coneOutsideVolume",
                             TypeFloat,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatFloat(d->profile.coneOutsideVolume)
                                          : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d || v.size() != 1)
                                     return false;
                                 d->profile.coneOutsideVolume = parseFloatValue(v[0]);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
            {
                FieldEntry e{"flags",
                             TypeSoundFlags,
                             1,
                             [](const GameBaseData& b, std::size_t) {
                                 const auto* d = dynamic_cast<const SoundProfileData*>(&b);
                                 return d ? formatUInt(d->profile.flags) : std::string{};
                             },
                             [](GameBaseData& b,
                                std::size_t,
                                std::span<const std::string> v,
                                FearDynamicDataPlugin&) {
                                 auto* d = dynamic_cast<SoundProfileData*>(&b);
                                 if (!d)
                                     return false;
                                 d->profile.flags = parseSoundFlags(v);
                                 return true;
                             }};
                f.push_back(std::move(e));
            }
        });

    registerClass(
        "SoundData", "", DataBlockManager::SoundDataType, [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("wavFileName", TypeString, &SoundData::wavFileName, this));
            f.push_back(makeScalarField("priority", TypeFloat, &SoundData::priority, this));
            f.push_back(
                makeScalarField("profile", TypeSoundProfileBlock, &SoundData::profileIndex, this));
        });

    registerClass("DamageSkinData",
                  "GameBaseData",
                  DataBlockManager::DamageSkinDataType,
                  [this](std::vector<FieldEntry>& f) {
                      f.push_back(
                          makeArrayField("bmpName", TypeString, &DamageSkinData::bmpName, this));
                  });

    registerClass("ShapeBaseData", "GameBaseData", -1, [this](std::vector<FieldEntry>& f) {
        f.push_back(makeScalarField("shapeFile", TypeString, &ShapeBaseData::fileName, this));
        f.push_back(makeScalarField(
            "damageSkinData", TypeDamageSkinBlock, &ShapeBaseData::damageSkinId, this));
        f.push_back(makeArrayField(
            "sequenceSound", TypeSequenceSound, &ShapeBaseData::sequenceSound, this));
        f.push_back(
            makeScalarField("shieldShapeName", TypeString, &ShapeBaseData::shieldShapeName, this));
        f.push_back(
            makeScalarField("shieldSoundId", TypeSoundBlock, &ShapeBaseData::sfxShield, this));
        f.push_back(makeScalarField("maxEnergy", TypeFloat, &ShapeBaseData::maxEnergy, this));
        f.push_back(makeScalarField("maxDamage", TypeFloat, &ShapeBaseData::maxDamage, this));
        f.push_back(makeScalarField("repairRate", TypeFloat, &ShapeBaseData::repairRate, this));
        f.push_back(
            makeScalarField("explosionId", TypeExplosionBlock, &ShapeBaseData::explosionId, this));
        f.push_back(makeScalarField("debrisId", TypeDebrisBlock, &ShapeBaseData::debrisId, this));
        f.push_back(
            makeScalarField("shadowDetailMask", TypeInt, &ShapeBaseData::shadowDetailMask, this));
    });

    registerClass("StaticBaseData", "ShapeBaseData", -1, [this](std::vector<FieldEntry>& f) {
        f.push_back(makeScalarField("maxDamage", TypeFloat, &StaticBaseData::maxDamage, this));
        f.push_back(
            makeScalarField("damageLevel", TypeStaticDamage, &StaticBaseData::damageLevel, this));
        f.push_back(
            makeScalarField("isTranslucent", TypeBool, &StaticBaseData::isTranslucent, this));
        f.push_back(makeScalarField(
            "disableCollisions", TypeBool, &StaticBaseData::disableCollision, this));
        f.push_back(
            makeScalarField("sfxAmbient", TypeSoundBlock, &StaticBaseData::sfxAmbient, this));
        f.push_back(makeScalarField("outerRadius", TypeFloat, &StaticBaseData::outerRadius, this));
    });

    registerClass("StaticShapeData",
                  "StaticBaseData",
                  DataBlockManager::StaticShapeDataType,
                  [this](std::vector<FieldEntry>& f) {
                      f.push_back(makeScalarField(
                          "sfxAmbient", TypeSoundBlock, &StaticShapeData::sfxAmbient, this));
                  });

    registerClass(
        "MoveableData",
        "StaticBaseData",
        DataBlockManager::MoveableDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("sfxStart", TypeSoundBlock, &MoveableData::sfxStart, this));
            f.push_back(makeScalarField("sfxStop", TypeSoundBlock, &MoveableData::sfxStop, this));
            f.push_back(makeScalarField("sfxRun", TypeSoundBlock, &MoveableData::sfxRun, this));
            f.push_back(
                makeScalarField("sfxBlocked", TypeSoundBlock, &MoveableData::sfxBlocked, this));
            f.push_back(makeScalarField("speed", TypeFloat, &MoveableData::speed, this));
            f.push_back(makeScalarField("side", TypeString, &MoveableData::side, this));
            f.push_back(makeScalarField("displace", TypeBool, &MoveableData::displace, this));
            f.push_back(
                makeScalarField("blockForward", TypeBool, &MoveableData::blockForward, this));
            f.push_back(
                makeScalarField("blockBackward", TypeBool, &MoveableData::blockBackward, this));
        });

    registerClass("DoorData",
                  "MoveableData",
                  DataBlockManager::MoveableDataType,
                  [](std::vector<FieldEntry>&) {});
    registerClass("ElevatorData",
                  "MoveableData",
                  DataBlockManager::MoveableDataType,
                  [](std::vector<FieldEntry>&) {});

    registerClass(
        "ItemImageData",
        "GameBaseData",
        DataBlockManager::ItemImageDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("shapeFile", TypeString, &ItemImageData::shapeFile, this));
            f.push_back(makeScalarField("mountPoint", TypeInt, &ItemImageData::mountPoint, this));
            f.push_back(
                makeScalarField("mountOffset", TypePoint3F, &ItemImageData::mountOffset, this));
            f.push_back(
                makeScalarField("mountRotation", TypePoint3F, &ItemImageData::mountRotation, this));
            f.push_back(makeScalarField("weaponType", TypeInt, &ItemImageData::weaponType, this));
            f.push_back(makeScalarField("ammoType", TypeItemBlock, &ItemImageData::ammoType, this));
            f.push_back(
                makeScalarField("activateTime", TypeFloat, &ItemImageData::activateTime, this));
            f.push_back(makeScalarField("reloadTime", TypeFloat, &ItemImageData::reloadTime, this));
            f.push_back(makeScalarField("fireTime", TypeFloat, &ItemImageData::fireTime, this));
            f.push_back(makeScalarField("spinUpTime", TypeFloat, &ItemImageData::spinUpTime, this));
            f.push_back(
                makeScalarField("spinDownTime", TypeFloat, &ItemImageData::spinDownTime, this));
            f.push_back(makeScalarField(
                "projectileType", TypeProjectileBlock, &ItemImageData::projectile, this));
            f.push_back(makeScalarField("minEnergy", TypeFloat, &ItemImageData::minEnergy, this));
            f.push_back(makeScalarField("maxEnergy", TypeFloat, &ItemImageData::maxEnergy, this));
            f.push_back(makeScalarField("mass", TypeFloat, &ItemImageData::mass, this));
            f.push_back(makeScalarField("lightType", TypeInt, &ItemImageData::lightType, this));
            f.push_back(
                makeScalarField("lightRadius", TypeFloat, &ItemImageData::lightRadius, this));
            f.push_back(makeScalarField("lightTime", TypeFloat, &ItemImageData::lightTime, this));
            f.push_back(
                makeScalarField("lightColor", TypePoint3F, &ItemImageData::lightColor, this));
            f.push_back(makeScalarField(
                "sfxActivate", TypeSoundBlock, &ItemImageData::sfxActivateTag, this));
            f.push_back(
                makeScalarField("sfxFire", TypeSoundBlock, &ItemImageData::sfxFireTag, this));
            f.push_back(
                makeScalarField("sfxReady", TypeSoundBlock, &ItemImageData::sfxReadyTag, this));
            f.push_back(
                makeScalarField("sfxReload", TypeSoundBlock, &ItemImageData::sfxReloadTag, this));
            f.push_back(
                makeScalarField("sfxSpinUp", TypeSoundBlock, &ItemImageData::sfxSpinUpTag, this));
            f.push_back(makeScalarField(
                "sfxSpinDown", TypeSoundBlock, &ItemImageData::sfxSpinDownTag, this));
            f.push_back(
                makeScalarField("firstPerson", TypeBool, &ItemImageData::firstPerson, this));
            f.push_back(makeScalarField("accuFire", TypeBool, &ItemImageData::accuFire, this));
        });

    registerClass(
        "ItemData",
        "StaticBaseData",
        DataBlockManager::ItemDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("typeString", TypeString, &ItemData::typeString, this));
            f.push_back(makeScalarField("heading", TypeString, &ItemData::typeString, this));
            f.push_back(
                makeScalarField("shadowDetailMask", TypeInt, &ItemData::shadowDetailMask, this));
            f.push_back(makeScalarField("imageId", TypeItemImageBlock, &ItemData::imageId, this));
            f.push_back(makeScalarField("imageType", TypeItemImageBlock, &ItemData::imageId, this));
            f.push_back(makeScalarField("price", TypeInt, &ItemData::price, this));
            f.push_back(makeScalarField("showInventory", TypeBool, &ItemData::showInventory, this));
            f.push_back(makeScalarField("showWeaponBar", TypeBool, &ItemData::showWeaponBar, this));
            f.push_back(
                makeScalarField("hiliteOnActive", TypeBool, &ItemData::hiliteOnActive, this));
            f.push_back(makeScalarField("mass", TypeFloat, &ItemData::mass, this));
            f.push_back(makeScalarField("drag", TypeFloat, &ItemData::drag, this));
            f.push_back(makeScalarField("density", TypeFloat, &ItemData::density, this));
            f.push_back(makeScalarField("elasticity", TypeFloat, &ItemData::elasticity, this));
            f.push_back(makeScalarField("friction", TypeFloat, &ItemData::friction, this));
            f.push_back(makeScalarField("lightType", TypeInt, &ItemData::lightType, this));
            f.push_back(makeScalarField("lightRadius", TypeFloat, &ItemData::lightRadius, this));
            f.push_back(makeScalarField("lightTime", TypeFloat, &ItemData::lightTime, this));
            f.push_back(makeScalarField("lightColor", TypePoint3F, &ItemData::lightColor, this));
            f.push_back(makeScalarField("hudIcon", TypeString, &ItemData::hudIcon, this));
        });

    registerClass(
        "PlayerData",
        "ShapeBaseData",
        DataBlockManager::PlayerDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(
                makeScalarField("flameShapeName", TypeString, &PlayerData::flameShapeName, this));
            f.push_back(
                makeScalarField("minJetEnergy", TypeFloat, &PlayerData::minJetEnergy, this));
            f.push_back(
                makeScalarField("jetEnergyDrain", TypeFloat, &PlayerData::jetEnergyDrain, this));
            f.push_back(makeScalarField("groundForce", TypeFloat, &PlayerData::groundForce, this));
            f.push_back(
                makeScalarField("groundTraction", TypeFloat, &PlayerData::groundTraction, this));
            f.push_back(makeScalarField(
                "maxJetSideForceFactor", TypeFloat, &PlayerData::maxJetSideForceFactor, this));
            f.push_back(makeScalarField(
                "maxJetForwardVelocity", TypeFloat, &PlayerData::maxJetForwardVelocity, this));
            f.push_back(makeScalarField("jetForce", TypeFloat, &PlayerData::jetForce, this));
            f.push_back(makeScalarField("mass", TypeFloat, &PlayerData::mass, this));
            f.push_back(makeScalarField("drag", TypeFloat, &PlayerData::drag, this));
            f.push_back(makeScalarField("density", TypeFloat, &PlayerData::density, this));
            f.push_back(makeScalarField("canCrouch", TypeBool, &PlayerData::canCrouch, this));
            f.push_back(
                makeScalarField("maxForwardSpeed", TypeFloat, &PlayerData::maxForwardSpeed, this));
            f.push_back(makeScalarField(
                "maxBackwardSpeed", TypeFloat, &PlayerData::maxBackwardSpeed, this));
            f.push_back(
                makeScalarField("maxSideSpeed", TypeFloat, &PlayerData::maxSideSpeed, this));
            f.push_back(makeScalarField("jumpImpulse", TypeFloat, &PlayerData::jumpImpulse, this));
            f.push_back(makeScalarField(
                "jumpSurfaceMinDot", TypeFloat, &PlayerData::jumpSurfaceMinDot, this));
            f.push_back(
                makeScalarField("boxNormalHeight", TypeFloat, &PlayerData::boxNormalHeight, this));
            f.push_back(
                makeScalarField("boxCrouchHeight", TypeFloat, &PlayerData::boxCrouchHeight, this));
            f.push_back(makeScalarField(
                "boxNormalHeadPercentage", TypeFloat, &PlayerData::boxNormalHeadPercentage, this));
            f.push_back(makeScalarField("boxNormalTorsoPercentage",
                                        TypeFloat,
                                        &PlayerData::boxNormalTorsoPercentage,
                                        this));
            f.push_back(makeScalarField(
                "boxCrouchHeadPercentage", TypeFloat, &PlayerData::boxCrouchHeadPercentage, this));
            f.push_back(makeScalarField("boxCrouchTorsoPercentage",
                                        TypeFloat,
                                        &PlayerData::boxCrouchTorsoPercentage,
                                        this));
            f.push_back(makeScalarField(
                "boxHeadLeftPercentage", TypeFloat, &PlayerData::boxHeadLeftPercentage, this));
            f.push_back(makeScalarField(
                "boxHeadRightPercentage", TypeFloat, &PlayerData::boxHeadRightPercentage, this));
            f.push_back(makeScalarField(
                "boxHeadBackPercentage", TypeFloat, &PlayerData::boxHeadBackPercentage, this));
            f.push_back(makeScalarField(
                "boxHeadFrontPercentage", TypeFloat, &PlayerData::boxHeadFrontPercentage, this));
            f.push_back(makeScalarField("boxWidth", TypeFloat, &PlayerData::boxWidth, this));
            f.push_back(makeScalarField("boxDepth", TypeFloat, &PlayerData::boxDepth, this));
            f.push_back(
                makeScalarField("minDamageSpeed", TypeFloat, &PlayerData::minDamageSpeed, this));
            f.push_back(makeScalarField("damageScale", TypeFloat, &PlayerData::damageScale, this));
            f.push_back(makeScalarField("sfxJet", TypeSoundBlock, &PlayerData::jetSound, this));
            f.push_back(makeArrayField("animData", TypeAnimData, &PlayerData::animData, this));
            f.push_back(
                makeArrayField("rFootSounds", TypeSoundBlock, &PlayerData::rFootSounds, this));
            f.push_back(
                makeArrayField("lFootSounds", TypeSoundBlock, &PlayerData::lFootSounds, this));
            f.push_back(makeArrayField("footPrints", TypeInt, &PlayerData::footPrints, this));
        });

    registerClass(
        "SensorData",
        "StaticBaseData",
        DataBlockManager::SensorDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("range", TypeInt, &SensorData::iRange, this));
            f.push_back(
                makeScalarField("dopplerVelocity", TypeInt, &SensorData::iDopplerVelocity, this));
            f.push_back(makeScalarField("castLOS", TypeBool, &SensorData::fCastLOS, this));
            f.push_back(makeScalarField("supression", TypeBool, &SensorData::supression, this));
            f.push_back(makeScalarField("supressable", TypeBool, &SensorData::supressable, this));
            f.push_back(makeScalarField("pinger", TypeBool, &SensorData::pinger, this));
        });

    registerClass(
        "TurretData",
        "SensorData",
        DataBlockManager::TurretDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField(
                "projectileType", TypeProjectileBlock, &TurretData::projectile, this));
            f.push_back(makeScalarField("gunRange", TypeFloat, &TurretData::gunRange, this));
            f.push_back(makeScalarField("FOV", TypeFloat, &TurretData::FOV, this));
            f.push_back(
                makeScalarField("minGunEnergy", TypeFloat, &TurretData::minGunEnergy, this));
            f.push_back(
                makeScalarField("maxGunEnergy", TypeFloat, &TurretData::maxGunEnergy, this));
            f.push_back(makeScalarField("reloadDelay", TypeFloat, &TurretData::reloadDelay, this));
            f.push_back(makeScalarField("speed", TypeFloat, &TurretData::speed, this));
            f.push_back(
                makeScalarField("speedModifier", TypeFloat, &TurretData::speedModifier, this));
            f.push_back(
                makeScalarField("sfxActivate", TypeSoundBlock, &TurretData::activationSound, this));
            f.push_back(makeScalarField(
                "sfxDeactivate", TypeSoundBlock, &TurretData::deactivateSound, this));
            f.push_back(makeScalarField("sfxFire", TypeSoundBlock, &TurretData::fireSound, this));
            f.push_back(makeScalarField("sfxWhir", TypeSoundBlock, &TurretData::whirSound, this));
            f.push_back(makeScalarField("deflection", TypeFloat, &TurretData::deflection, this));
            f.push_back(makeScalarField(
                "targetableFovRatio", TypeFloat, &TurretData::targetableFovRatio, this));
            f.push_back(makeScalarField("isSustained", TypeBool, &TurretData::isSustained, this));
            f.push_back(makeScalarField("firingTimeMS", TypeInt, &TurretData::firingTime, this));
            f.push_back(makeScalarField("energyRate", TypeFloat, &TurretData::energyRate, this));
        });

    registerClass(
        "VehicleData",
        "StaticBaseData",
        DataBlockManager::VehicleDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("ramDamage", TypeFloat, &VehicleData::ramDamage, this));
            f.push_back(
                makeScalarField("ramDamageType", TypeInt, &VehicleData::ramDamageType, this));
            f.push_back(makeScalarField("mass", TypeFloat, &VehicleData::mass, this));
            f.push_back(makeScalarField("drag", TypeFloat, &VehicleData::drag, this));
            f.push_back(makeScalarField("density", TypeFloat, &VehicleData::density, this));
            f.push_back(makeScalarField("maxSpeed", TypeFloat, &VehicleData::maxSpeed, this));
            f.push_back(makeScalarField("minSpeed", TypeFloat, &VehicleData::minSpeed, this));
            f.push_back(makeArrayField("suspension", TypePoint3F, &VehicleData::suspension, this));
            f.push_back(
                makeScalarField("idleSound", TypeSoundBlock, &VehicleData::idleSound, this));
            f.push_back(
                makeScalarField("moveSound", TypeSoundBlock, &VehicleData::moveSound, this));
            f.push_back(
                makeScalarField("mountSound", TypeSoundBlock, &VehicleData::mountSound, this));
            f.push_back(makeScalarField(
                "dismountSound", TypeSoundBlock, &VehicleData::dismountSound, this));
            f.push_back(
                makeScalarField("damageSound", TypeSoundBlock, &VehicleData::damageSound, this));
            f.push_back(makeScalarField(
                "projectileType", TypeProjectileBlock, &VehicleData::projectile, this));
            f.push_back(makeScalarField("reloadDelay", TypeFloat, &VehicleData::reloadDelay, this));
            f.push_back(
                makeScalarField("fireSound", TypeSoundBlock, &VehicleData::fireSound, this));
            f.push_back(
                makeScalarField("visibleDriver", TypeBool, &VehicleData::visibleDriver, this));
            f.push_back(makeScalarField("driverPose", TypeInt, &VehicleData::driverPose, this));
        });

    registerClass(
        "FlierData",
        "VehicleData",
        DataBlockManager::FlierDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("maxBank", TypeFloat, &FlierData::maxBank, this));
            f.push_back(makeScalarField("maxPitch", TypeFloat, &FlierData::maxPitch, this));
            f.push_back(makeScalarField("lift", TypeFloat, &FlierData::lift, this));
            f.push_back(makeScalarField("maxAlt", TypeFloat, &FlierData::maxAlt, this));
            f.push_back(makeScalarField("maxVertical", TypeFloat, &FlierData::maxVertical, this));
            f.push_back(makeScalarField("accel", TypeFloat, &FlierData::accel, this));
            f.push_back(makeScalarField(
                "groundDamageScale", TypeFloat, &FlierData::groundDamageScale, this));
        });

    registerClass("TankData",
                  "VehicleData",
                  DataBlockManager::TankDataType,
                  [this](std::vector<FieldEntry>& f) {
                      f.push_back(
                          makeScalarField("maxGrade", TypeFloat, &TankData::maxGrade, this));
                  });

    registerClass("HoverData",
                  "VehicleData",
                  DataBlockManager::HoverDataType,
                  [](std::vector<FieldEntry>&) {});

    registerClass(
        "CarData",
        "VehicleData",
        DataBlockManager::CarDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("wheelCount", TypeInt, &CarData::wheelCount, this));
            f.push_back(makeArrayField("wheel", TypeWheel, &CarData::wheels, this));
            f.push_back(makeScalarField("wheelRadius", TypeFloat, &CarData::wheelRadius, this));
            f.push_back(makeScalarField("wheelRestDist", TypeFloat, &CarData::wheelRestDist, this));
            f.push_back(makeScalarField("wheelOneGDist", TypeFloat, &CarData::wheelOneGDist, this));
            f.push_back(makeScalarField(
                "wheelSideBrakeForce", TypeFloat, &CarData::wheelSideBrakeForce, this));
            f.push_back(makeScalarField("turnAngle", TypeFloat, &CarData::turnAngle, this));
            f.push_back(
                makeScalarField("wheelShapeName", TypeString, &CarData::wheelShapeName, this));
        });

    registerClass(
        "ProjectileData",
        "GameBaseData",
        DataBlockManager::ProjectileDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField(
                "bulletShapeName", TypeString, &ProjectileData::bulletShapeName, this));
            f.push_back(makeScalarField(
                "explosionTag", TypeExplosionBlock, &ProjectileData::explosionTag, this));
            f.push_back(
                makeScalarField("expRandCycle", TypeInt, &ProjectileData::expRandCycle, this));
            f.push_back(makeScalarField(
                "collideWithOwner", TypeBool, &ProjectileData::collideWithOwner, this));
            f.push_back(
                makeScalarField("ownerGraceMS", TypeInt, &ProjectileData::ownerGraceMS, this));
            f.push_back(makeScalarField(
                "collisionRadius", TypeFloat, &ProjectileData::collisionRadius, this));
            f.push_back(makeScalarField("mass", TypeFloat, &ProjectileData::mass, this));
            f.push_back(
                makeScalarField("damageClass", TypeInt, &ProjectileData::damageClass, this));
            f.push_back(
                makeScalarField("damageValue", TypeFloat, &ProjectileData::damageValue, this));
            f.push_back(makeScalarField("damageType", TypeInt, &ProjectileData::damageType, this));
            f.push_back(makeScalarField(
                "explosionRadius", TypeFloat, &ProjectileData::explosionRadius, this));
            f.push_back(makeScalarField(
                "kickBackStrength", TypeFloat, &ProjectileData::kickBackStrength, this));
            f.push_back(
                makeScalarField("aimDeflection", TypeFloat, &ProjectileData::aimDeflection, this));
            f.push_back(makeScalarField(
                "muzzleVelocity", TypeFloat, &ProjectileData::muzzleVelocity, this));
            f.push_back(makeScalarField(
                "terminalVelocity", TypeFloat, &ProjectileData::terminalVelocity, this));
            f.push_back(
                makeScalarField("acceleration", TypeFloat, &ProjectileData::acceleration, this));
            f.push_back(makeScalarField("totalTime", TypeFloat, &ProjectileData::totalTime, this));
            f.push_back(makeScalarField("liveTime", TypeFloat, &ProjectileData::liveTime, this));
            f.push_back(makeScalarField(
                "projSpecialTime", TypeFloat, &ProjectileData::projSpecialTime, this));
            f.push_back(makeScalarField(
                "projSpecialBool", TypeBool, &ProjectileData::projSpecialBool, this));
            f.push_back(
                makeScalarField("lightRange", TypeFloat, &ProjectileData::lightRange, this));
            f.push_back(
                makeScalarField("lightColor", TypeColorF, &ProjectileData::lightColor, this));
            f.push_back(makeScalarField("inheritedVelocityScale",
                                        TypeFloat,
                                        &ProjectileData::inheritedVelocityScale,
                                        this));
            f.push_back(makeScalarField("soundId", TypeSoundBlock, &ProjectileData::soundId, this));
        });

    registerClass(
        "BulletData",
        "ProjectileData",
        DataBlockManager::BulletDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("isVisible", TypeBool, &BulletData::projSpecialBool, this));
            f.push_back(
                makeScalarField("tracerPercentage", TypeFloat, &BulletData::projSpecialTime, this));
            f.push_back(
                makeScalarField("rotationPeriod", TypeFloat, &BulletData::rotationPeriod, this));
            f.push_back(
                makeScalarField("tracerLength", TypeFloat, &BulletData::tracerLength, this));
            f.push_back(
                makeScalarField("bulletHoleIndex", TypeInt, &BulletData::bulletHoleIndex, this));
        });

    registerClass(
        "GrenadeData",
        "ProjectileData",
        DataBlockManager::GrenadeDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("elasticity", TypeFloat, &GrenadeData::elasticity, this));
            f.push_back(makeScalarField("smokeName", TypeString, &GrenadeData::pSmokeName, this));
            f.push_back(makeScalarField(
                "maxThrowDist", TypeGrenadeDist, &GrenadeData::muzzleVelocity, this));
            f.push_back(makeScalarField(
                "maxLevelFlightDist", TypeGrenadeDist, &GrenadeData::muzzleVelocity, this));
        });

    registerClass(
        "RocketData",
        "ProjectileData",
        DataBlockManager::RocketDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(
                makeScalarField("smokeDist", TypeFloat, &RocketData::projSpecialTime, this));
            f.push_back(makeScalarField("trailType", TypeInt, &RocketData::trailType, this));
            f.push_back(makeScalarField("trailString", TypeString, &RocketData::trailString, this));
            f.push_back(makeScalarField("trailLength", TypeFloat, &RocketData::trailLength, this));
            f.push_back(makeScalarField("trailWidth", TypeFloat, &RocketData::trailWidth, this));
        });

    registerClass(
        "LaserData",
        "ProjectileData",
        DataBlockManager::LaserDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(
                makeScalarField("laserBitmapName", TypeString, &LaserData::pBitmapName, this));
            f.push_back(makeScalarField("hitName", TypeString, &LaserData::pHitShapeName, this));
            f.push_back(
                makeScalarField("damageConversion", TypeFloat, &LaserData::damageValue, this));
            f.push_back(makeScalarField("baseDamageType", TypeInt, &LaserData::damageType, this));
            f.push_back(makeScalarField("beamTime", TypeFloat, &LaserData::liveTime, this));
            f.push_back(makeScalarField("hitSoundId", TypeSoundBlock, &LaserData::soundId, this));
            f.push_back(makeScalarField("bitmapName", TypeString, &LaserData::pBitmapName, this));
            f.push_back(
                makeScalarField("hitConversion", TypeString, &LaserData::pHitShapeName, this));
            f.push_back(makeScalarField(
                "detachFromShooter", TypeBool, &LaserData::detachFromShooter, this));
        });

    registerClass("TargetLaserData",
                  "LaserData",
                  DataBlockManager::TargetLaserDataType,
                  [this](std::vector<FieldEntry>& f) {
                      f.push_back(makeScalarField(
                          "hitShapeName", TypeString, &TargetLaserData::pHitShapeName, this));
                  });

    registerClass(
        "SeekingMissileData",
        "ProjectileData",
        DataBlockManager::SeekingMissileDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("nonSeekingTurningRadius",
                                        TypeFloat,
                                        &SeekingMissileData::nonSeekingTurningRadius,
                                        this));
            f.push_back(makeScalarField("seekingTurningRadius",
                                        TypeFloat,
                                        &SeekingMissileData::seekingTurningRadius,
                                        this));
            f.push_back(makeScalarField(
                "smokeDist", TypeFloat, &SeekingMissileData::projSpecialTime, this));
            f.push_back(makeScalarField("nonSeekingTurningDesc",
                                        TypeFloat,
                                        &SeekingMissileData::nonSeekingTurningRadius,
                                        this));
            f.push_back(makeScalarField(
                "seekingTurningDesc", TypeFloat, &SeekingMissileData::seekingTurningRadius, this));
            f.push_back(makeScalarField(
                "proximityDist", TypeFloat, &SeekingMissileData::proximityDist, this));
        });

    registerClass(
        "LightningData",
        "ProjectileData",
        DataBlockManager::LightningDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(
                makeScalarField("bitmapName", TypeString, &LightningData::pBitmapName, this));
            f.push_back(makeScalarField("boltLength", TypeFloat, &LightningData::boltLength, this));
            f.push_back(makeScalarField("coneAngle", TypeFloat, &LightningData::coneAngle, this));
            f.push_back(
                makeScalarField("damagePerSec", TypeFloat, &LightningData::damagePerSec, this));
            f.push_back(makeScalarField(
                "energyDrainPerSec", TypeFloat, &LightningData::energyDrainPerSec, this));
            f.push_back(makeScalarField(
                "segmentDivisions", TypeInt, &LightningData::segmentDivisions, this));
            f.push_back(makeScalarField("numSegments", TypeInt, &LightningData::numSegments, this));
            f.push_back(makeScalarField("beamWidth", TypeFloat, &LightningData::beamWidth, this));
            f.push_back(makeScalarField("updateTime", TypeInt, &LightningData::updateTime, this));
            f.push_back(
                makeScalarField("skipPercent", TypeFloat, &LightningData::skipPercent, this));
            f.push_back(
                makeScalarField("displaceBias", TypeFloat, &LightningData::displaceBias, this));
        });

    registerClass("RepairEffectData",
                  "LightningData",
                  DataBlockManager::RepairEffectDataType,
                  [](std::vector<FieldEntry>&) {});

    registerClass("InteriorShapeData",
                  "GameBaseData",
                  DataBlockManager::InteriorShapeType,
                  [](std::vector<FieldEntry>&) {});

    registerClass(
        "ExplosionData",
        "GameBaseData",
        DataBlockManager::ExplosionDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("shapeName", TypeString, &ExplosionData::pShapeName, this));
            f.push_back(makeScalarField("soundId", TypeSoundBlock, &ExplosionData::soundId, this));
            f.push_back(makeScalarField("faceCamera", TypeBool, &ExplosionData::faceCamera, this));
            f.push_back(makeScalarField("randomSpin", TypeBool, &ExplosionData::randomSpin, this));
            f.push_back(makeScalarField("hasLight", TypeBool, &ExplosionData::hasLight, this));
            f.push_back(makeScalarField("lightRange", TypeFloat, &ExplosionData::lightRange, this));
            f.push_back(makeScalarField("timeZero", TypeFloat, &ExplosionData::timeZero, this));
            f.push_back(makeScalarField("timeOne", TypeFloat, &ExplosionData::timeOne, this));
            f.push_back(makeScalarField("timeScale", TypeFloat, &ExplosionData::timeScale, this));
            f.push_back(makeArrayField("colors", TypeColorF, &ExplosionData::colors, this));
            f.push_back(makeArrayField("radFactors", TypeFloat, &ExplosionData::radFactors, this));
            f.push_back(
                makeScalarField("shiftPosition", TypeBool, &ExplosionData::shiftPosition, this));
        });

    registerClass(
        "DebrisData",
        "GameBaseData",
        DataBlockManager::DebrisDataType,
        [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("mass", TypeFloat, &DebrisData::mass, this));
            f.push_back(makeScalarField("elasticity", TypeFloat, &DebrisData::elasticity, this));
            f.push_back(makeScalarField("friction", TypeFloat, &DebrisData::friction, this));
            f.push_back(makeScalarField("type", TypeInt, &DebrisData::type, this));
            f.push_back(makeScalarField("shapeName", TypeString, &DebrisData::pShapeName, this));
            f.push_back(makeScalarField("imageType", TypeInt, &DebrisData::imageType, this));
            f.push_back(
                makeScalarField("imageSortValue", TypeInt, &DebrisData::imageSortValue, this));
            f.push_back(makeArrayField("center", TypeFloat, &DebrisData::center, this));
            f.push_back(
                makeScalarField("collisionMask", TypeInt, &DebrisData::collisionMask, this));
            f.push_back(makeScalarField("knockMask", TypeInt, &DebrisData::knockMask, this));
            f.push_back(makeScalarField(
                "animationSequence", TypeInt, &DebrisData::animationSequence, this));
            f.push_back(
                makeScalarField("animRestartTime", TypeFloat, &DebrisData::animRestartTime, this));
            f.push_back(makeScalarField("minTimeout", TypeFloat, &DebrisData::minTimeout, this));
            f.push_back(makeScalarField("maxTimeout", TypeFloat, &DebrisData::maxTimeout, this));
            f.push_back(
                makeScalarField("explodeOnBounce", TypeFloat, &DebrisData::explodeOnBounce, this));
            f.push_back(makeScalarField("damage", TypeFloat, &DebrisData::damage, this));
            f.push_back(
                makeScalarField("damageThreshold", TypeFloat, &DebrisData::damageThreshold, this));
            f.push_back(makeScalarField(
                "spawnedDebrisMask", TypeInt, &DebrisData::spawnedDebrisMask, this));
            f.push_back(makeScalarField(
                "spawnedDebrisStrength", TypeFloat, &DebrisData::spawnedDebrisStrength, this));
            f.push_back(makeScalarField(
                "spawnedDebrisRadius", TypeFloat, &DebrisData::spawnedDebrisRadius, this));
            f.push_back(makeScalarField(
                "spawnedExplosionID", TypeExplosionBlock, &DebrisData::spawnedExplosionID, this));
            f.push_back(makeScalarField("p", TypeFloat, &DebrisData::p, this));
            f.push_back(
                makeScalarField("explodeOnRest", TypeBool, &DebrisData::explodeOnRest, this));
            f.push_back(
                makeScalarField("collisionDetail", TypeInt, &DebrisData::collisionDetail, this));
        });

    registerClass(
        "MineData", "ItemData", DataBlockManager::MineDataType, [this](std::vector<FieldEntry>& f) {
            f.push_back(
                makeScalarField("explosionRadius", TypeFloat, &MineData::explosionRadius, this));
            f.push_back(makeScalarField("damageValue", TypeFloat, &MineData::damageValue, this));
            f.push_back(makeScalarField("damageType", TypeInt, &MineData::damageType, this));
            f.push_back(
                makeScalarField("kickBackStrength", TypeFloat, &MineData::kickBackStrength, this));
        });

    registerClass("MarkerData",
                  "GameBaseData",
                  DataBlockManager::MarkerDataType,
                  [this](std::vector<FieldEntry>& f) {
                      f.push_back(
                          makeScalarField("shapeFile", TypeString, &MarkerData::fileName, this));
                  });

    registerClass("TriggerData",
                  "GameBaseData",
                  DataBlockManager::TriggerDataType,
                  [this](std::vector<FieldEntry>& f) {
                      f.push_back(makeScalarField("rate", TypeFloat, &TriggerData::rate, this));
                  });

    registerClass(
        "IRCChannel", "", DataBlockManager::IRCChannelType, [this](std::vector<FieldEntry>& f) {
            f.push_back(makeScalarField("name", TypeString, &IRCChannelData::name, this));
            f.push_back(makeScalarField("topic", TypeString, &IRCChannelData::topic, this));
        });
}

} // namespace opent1::game
