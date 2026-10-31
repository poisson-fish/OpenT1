#pragma once
#include <opent1/world/landscape.hpp>
#include <opent1/math/random.hpp>

namespace opent1::world {
struct LandscapeDistribution {
    Flt32 minimum{}, maximum{1}, mean{.5f}, deviation{.5f};
};

struct LandscapeRule {
    Int32 group{};
    LandscapeDistribution altitude;
    Flt32 altitudeWeight{1};
    bool adjustHeights{};
    LandscapeDistribution slope;
    Flt32 slopeWeight{1};
    bool adjustSlopes{};
};

[[nodiscard]] std::optional<std::vector<LandscapeRule>> readLandscapeRules(std::string_view plr);
[[nodiscard]] std::optional<std::string> writeLandscapeRules(std::span<const LandscapeRule>);

struct LandscapeTextureTable {
    Int32 version{3};
    UInt32 types{};
    UInt32 pickCount{}; // legacy informational counter; offsets own byte extents
    std::vector<std::string> descriptions;
    std::vector<std::array<UInt8, 4>> corners; // ul,ur,lr,ll
    std::vector<UInt32> combinations, offsets;
    std::vector<UInt8> picks;
    [[nodiscard]] static std::optional<LandscapeTextureTable> load(std::span<const std::byte>);
    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::optional<std::vector<std::byte>> encode() const;
};

struct LandscapeCorner {
    Point2I point;
    UInt8 type{};
};

struct LandscapeTextureDefinition {
    TsMaterial material;
    std::array<UInt8, 4> corners{};
    UInt8 rotations{255}, percentage{100};
};

struct LandscapeTextureRecipe {
    std::vector<std::pair<char, std::string>> types;
    std::vector<LandscapeTextureDefinition> textures;
    bool addType(char, std::string);
    bool addTexture(std::string_view file,
                    std::string_view sides,
                    UInt8 rotations,
                    UInt8 percent,
                    std::string_view physicalType,
                    Flt32 elasticity,
                    Flt32 friction);
    [[nodiscard]] std::optional<LandscapeTextureTable> build() const;
    [[nodiscard]] std::optional<std::vector<std::byte>> materialList() const;
};

struct LandscapeStampTexture {
    Point2I point;
    UInt8 flags{}, material{};
};

struct LandscapeStamp {
    std::vector<Point2I> flat;
    std::vector<LandscapeStampTexture> textures;
    UInt32 clampDetail{};
};

// All application is staged. Random state and authored terrain publish together.
class LandscapeMapper {
public:
    bool setRules(std::vector<LandscapeRule>);
    bool setTextures(LandscapeTextureTable);

    void seed(UInt32 value) {
        random_.setSeed(value);
    }

    bool apply(TerrainDocument&, std::span<const LandscapeCorner> forced = {});
    bool applyLocal(TerrainDocument&, gfx::RectI, std::span<const LandscapeCorner> forced = {});
    bool
    stamp(TerrainDocument&, UInt32 block, Point2I origin, UInt32 rotation, const LandscapeStamp&);

    [[nodiscard]] const std::vector<LandscapeRule>& rules() const {
        return rules_;
    }

    [[nodiscard]] const LandscapeTextureTable& textures() const {
        return textures_;
    }

    void clearRules() {
        rules_.clear();
    }

private:
    bool map(UInt32 width,
             Int32 scale,
             std::span<const Flt32> heights,
             std::vector<TerrainGridMaterial>&,
             std::span<const LandscapeCorner>,
             bool local,
             Random&) const;
    std::vector<LandscapeRule> rules_;
    LandscapeTextureTable textures_;
    Random random_;
};
} // namespace opent1::world
