#include <opent1/game/version.hpp>
#include <ranges>
#include <array>
#include <bit>

namespace opent1::game {

std::optional<UInt32> getShapeCRC(ResourceManager& rm,
                                  std::string_view shapeFile,
                                  bool validateMaterials,
                                  const world::TsShape* preloadedShape,
                                  bool legacyBugCompatible) {
    UInt32 crc = 0xFFFFFFFFU;
    if (!rm.getCrc(shapeFile, crc, 0xFFFFFFFFU)) {
        return std::nullopt;
    }
    if (validateMaterials) {
        Resource<world::TsShape> loadedShape;
        const world::TsShape* shape = preloadedShape;
        if (!shape) {
            loadedShape = rm.load<world::TsShape>(shapeFile);
            shape = loadedShape.get();
        }
        if (shape && shape->materials) {
            const auto& ml = *shape->materials;
            const std::size_t count =
                ml.materialsPerDetail > 0
                    ? std::min<std::size_t>(static_cast<std::size_t>(ml.materialsPerDetail),
                                            ml.materials.size())
                    : ml.materials.size();
            const UInt32 mask = legacyBugCompatible ? 0x0F00U : 0x000FU;
            for (auto i :
                 std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                     return i < count;
                 })) {
                const auto& mat = ml.materials[i];
                if ((mat.flags & mask) == 0x03U && !mat.mapFile.empty()) {
                    if (!rm.getCrc(mat.mapFile, crc, crc)) {
                        return std::nullopt;
                    }
                }
            }
        }
    }
    return crc;
}

bool writeValidatedString(BitStream& stream,
                          std::string_view str,
                          bool validateShape,
                          UInt32 shapeCRC,
                          bool validateMaterials) {
    if (validateShape) {
        std::string buf;
        buf.reserve(str.size() + 1);
        buf.push_back(static_cast<char>(validateMaterials ? 0xFEU : 0xFFU));
        buf.append(str.substr(0, 254));
        if (!stream.writeString(buf)) {
            return false;
        }
        const auto leCrc = std::array<std::byte, 4>{
            static_cast<std::byte>(shapeCRC & 0xFFU),
            static_cast<std::byte>((shapeCRC >> 8) & 0xFFU),
            static_cast<std::byte>((shapeCRC >> 16) & 0xFFU),
            static_cast<std::byte>((shapeCRC >> 24) & 0xFFU),
        };
        return stream.writeBytes(leCrc) && stream.isValid();
    }
    return stream.writeString(str) && stream.isValid();
}

bool readValidatedString(BitStream& stream,
                         std::string& str,
                         bool& validateShape,
                         UInt32& shapeCRC,
                         bool& validateMaterials) {
    std::string buf;
    if (!stream.readString(buf) || !stream.isValid()) {
        return false;
    }
    if (!buf.empty()) {
        const auto marker = static_cast<UInt8>(buf[0]);
        if (marker == 0xFEU || marker == 0xFFU) {
            validateShape = true;
            validateMaterials = (marker == 0xFEU);
            str = buf.substr(1);
            std::array<std::byte, 4> leCrc{};
            if (!stream.readBytes(leCrc) || !stream.isValid()) {
                return false;
            }
            shapeCRC = static_cast<UInt32>(std::to_integer<UInt8>(leCrc[0])) |
                       (static_cast<UInt32>(std::to_integer<UInt8>(leCrc[1])) << 8) |
                       (static_cast<UInt32>(std::to_integer<UInt8>(leCrc[2])) << 16) |
                       (static_cast<UInt32>(std::to_integer<UInt8>(leCrc[3])) << 24);
            return true;
        }
    }
    validateShape = false;
    validateMaterials = false;
    shapeCRC = 0;
    str = std::move(buf);
    return true;
}

} // namespace opent1::game
