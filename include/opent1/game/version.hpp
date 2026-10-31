#pragma once
#include <opent1/core/bit_stream.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/world/ts_shape.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace opent1::game {

inline constexpr std::string_view FearVersion = "1.11";
inline constexpr std::string_view FearDebugVersion = "1.11d";
inline constexpr std::string_view BuildDateStamp = "Jan 18 2000";
inline constexpr std::string_view BuildTimeStamp = "15:18:10";

// Computes the legacy shape CRC from ResourceManager::getCrc(shapeFile, crc, 0xFFFFFFFF).
// Material validation uses (flags & 0x0f) == 3 and includes each texture's CRC.
// Explicit legacyBugCompatible=true selects the alternate 0xf00 mask.
[[nodiscard]] std::optional<UInt32> getShapeCRC(ResourceManager& rm,
                                                std::string_view shapeFile,
                                                bool validateMaterials,
                                                const world::TsShape* preloadedShape = nullptr,
                                                bool legacyBugCompatible = false);

// Legacy validated shape string wire codec (0xFE = validateShape + validateMaterials,
// 0xFF = validateShape only, followed by string and 32-bit shapeCRC).
bool writeValidatedString(BitStream& stream,
                          std::string_view str,
                          bool validateShape,
                          UInt32 shapeCRC,
                          bool validateMaterials);

bool readValidatedString(BitStream& stream,
                         std::string& str,
                         bool& validateShape,
                         UInt32& shapeCRC,
                         bool& validateMaterials);

} // namespace opent1::game
