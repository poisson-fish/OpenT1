#pragma once
#include <opent1/world/terrain_grid.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <opent1/math/plane.hpp>

namespace opent1::world {
struct Terrain2Square {
    UInt16 minimum{}, maximum{}, deviance{}, flags{};

    enum : UInt16 { Split45 = 1, Empty = 2 };
};

struct Terrain2Block {
    std::vector<UInt16> heights;                // 257x257, native 11.5 fixed heights
    std::vector<TerrainGridMaterial> materials; // 256x256
    std::array<std::vector<Terrain2Square>, 9> squares;
    gfx::Bitmap baseTexture, lightMap; // 256x256, indexed and RGB565
};

struct Terrain2Light {
    Point3F position;
    Flt32 radius{};
    Point3F color;
};

struct Terrain2View {
    Point3F camera;
    Flt32 focalLength{320}, pixelScale{1}, screenSize{45}, screenError{9};
    Flt32 visibleDistance{500}, hazeDistance{250};
    // Exact native six plane convention. Empty optional uses distance culling.
    std::optional<std::array<PlaneF, 6>> planes;
    std::vector<Terrain2Light> lights; // original cap is 31
    UInt32 maximumTriangles{32768};
};

struct Terrain2Draw {
    UInt32 x{}, y{}, level{}, mip{}, lightMask{};
    std::vector<StaticSurfaceVertex> vertices; // original ordered edge fan
    Resource<gfx::Bitmap> texture;
    std::shared_ptr<const gfx::LightMap> lightMap;
};

struct Terrain2Frame {
    std::shared_ptr<const Terrain2Block> block;
    std::vector<Point3F> points;
    std::vector<Terrain2Draw> draws;
    TsMaterialResources materials;
    std::vector<StaticSurfaceTriangle> triangles;
    [[nodiscard]] std::optional<TsStaticShapeFrame> prepare(const TsStaticView&) const;
};

struct Terrain2WaterView {
    Terrain2View view;
    Point2F extent{256, 256}, textureScale{1, 1};
    Flt32 height{1}, minimumSquare{4}, timeSeconds{};
    std::vector<Point4F> waves; // native authored wave parameters, cap 8
    bool underwater{};          // force underwater in addition to native camera detection
};

struct Terrain2WaterFrame {
    std::vector<Point3F> points;
    std::vector<Point2F> textureCoordinates, environmentCoordinates;
    std::vector<std::array<UInt32, 3>> triangles;
    Point3F camera;
    Flt32 visibleDistance{}, hazeDistance{};
    bool underwater{};
    [[nodiscard]] std::optional<gfx::HardwareBatch>
    hardware(const TsStaticView&,
             const gfx::Bitmap* waterTexture = nullptr,
             const gfx::Bitmap* environmentTexture = nullptr,
             const gfx::Palette* palette = nullptr,
             Point3F waterColor = {0, 0, 1},
             Flt32 alpha = .5f) const;
};

// Explicitly optional alternative to GridRender. Every 3x3 placement retains
// the same owned converted block, as in the active 1.11 conversion routine.
class Terrain2 {
public:
    [[nodiscard]] static std::optional<Terrain2> convert(const TerrainGridResources&);

    [[nodiscard]] std::shared_ptr<const Terrain2Block> block() const {
        return block_;
    }

    bool setHeight(Point2I, Flt32); // native outward-only edit bounds/error growth
    bool setMaterial(Point2I, TerrainGridMaterial);
    bool setLighting(Point2I, UInt16 rgb565);
    [[nodiscard]] std::optional<Terrain2Frame> render(const Terrain2View&) const;
    [[nodiscard]] std::shared_ptr<const gfx::LightMap> lighting(Point2I,
                                                                UInt32 level,
                                                                std::span<const Terrain2Light>,
                                                                UInt32 mask = 0xffffffffU) const;
    [[nodiscard]] static std::optional<Terrain2WaterFrame> water(const Terrain2WaterView&);

private:
    std::shared_ptr<const Terrain2Block> block_;
    TsMaterialResources materials_;
};
} // namespace opent1::world
