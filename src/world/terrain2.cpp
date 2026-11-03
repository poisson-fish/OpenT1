#include <opent1/world/terrain2.hpp>
#include <opent1/world/terrain_authoring.hpp>
#include <opent1/world/terrain_textures.hpp>
#include <opent1/math/trig.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace opent1::world {
namespace {
using GridSquare = Terrain2Square;

struct GridBlock {
    using Material = TerrainGridMaterial;
};

Flt32 fixedToFloat(UInt16 h) {
    return Flt32(h) * .03125f;
}

UInt16 floatToFixed(Flt32 h) {
    return UInt16(h * 32.f);
}

struct NativeBitmap {
    Int32 width{}, height{};
    std::vector<UInt16> data;

    NativeBitmap(Int32 w, Int32 h) : width(w), height(h), data(std::size_t(w) * h + 257) {}

    explicit NativeBitmap(const gfx::Bitmap& b) : NativeBitmap(b.width, b.height) {
        for (Int32 y = 0; y < b.height; ++y) {
            const auto row = b.row(y);
            for (Int32 x = 0; x < b.width; ++x) {
                data[std::size_t(y) * width + x] =
                    UInt16(std::to_integer<UInt8>(row[std::size_t(x) * 2]) |
                           (UInt16(std::to_integer<UInt8>(row[std::size_t(x) * 2 + 1])) << 8));
            }
        }
        for (UInt32 i = 0; i < 257; ++i) {
            data[std::size_t(width) * height + i] = data[i];
        }
    }

    void* getAddress(Int32 x, Int32 y) {
        return data.data() + std::size_t(y) * width + x;
    }

    Int32 getWidth() const {
        return width;
    }
};

struct NativeTerrainBlock {
    enum { BlockSize = 257, BlockShift = 8, BlockMask = 255, SquareMaxPoints = 1024 };

    UInt16* heightMap;
    GridSquare* gridMap[9]{};
    GridBlock::Material* materialMap;
    NativeBitmap low, dynamic;
    NativeBitmap *lightMap, *dynamicLightMap;

    explicit NativeTerrainBlock(Terrain2Block& b)
        : heightMap(b.heights.data()), materialMap(b.materials.data()), low(b.lightMap),
          dynamic(16, 16), lightMap(&low), dynamicLightMap(&dynamic) {
        for (UInt32 i = 0; i < 9; ++i) {
            gridMap[i] = b.squares[i].data();
        }
    }

    UInt16 getHeight(int x, int y) {
        return heightMap[x + y * 257];
    }

    GridSquare* findSquare(int level, Point2I p) {
        return gridMap[level] + (p.x >> level) + ((p.y >> level) << (8 - level));
    }

    GridBlock::Material* getMaterial(int x, int y) {
        return materialMap + x + y * 256;
    }

    void buildGridMap();
};

enum {
    ClampEdgeIndex = 0xffff,
    InvalidPointIndex = 0xfffe,
    MaxScanPoints = 65535,
    MaxScanEdges = 65535,
    ScanEdgeLimit = 65520,
    ScanPointLimit = 65520,
    MaxEmitSquares = 16384,
    NumClipPlanes = 6,
    DynamicLightMapSize = 8,
    DynamicLightMapShift = 3,
    MaxWaterStackSize = 512,
    EdgeStackSize = 1024
};

struct SquareStackNode {
    UInt16 level{}, clipFlags{};
    UInt32 lightMask{};
    int top{}, right{}, bottom{}, left{};
    Point2I pos;
};

struct WaterStackNode {
    Point2F position, extent;
    int clipFlags{};
    UInt32 lightMask{};
    int top{}, right{}, bottom{}, left{};
};

struct ScanEdge {
    UInt16 p1{}, p2{}, mp{}, firstSubEdge{};
};

struct ScanPoint {
    float pointHeight{}, avgHeight{}, minGrowFactor{};
    UInt16 p1{}, p2{};
};

struct EmitSquare {
    UInt16 edge[4]{}, x{}, y{};
    UInt8 level{}, clipFlags{}, flags{}, mipLevel{};
    UInt32 lightMask{};

    enum { Split45 = 1, Recache = 2, DynamicallyLit = 4, DynamicLightFlagShift = 2 };
};

struct Color {
    int r{}, g{}, b{};
    float z{};
};

struct TerrLightInfo {
    Point3F pos;
    float radius{}, radiusSquared{}, r{}, g{}, b{};
};

struct Projection {
    float focal{}, pixel{};

    float getPixelScale() const {
        return pixel;
    }

    float projectionDistance(float ps, float radius) const {
        return focal * radius / ps;
    }

    float projectRadius(float distance, float radius) const {
        return focal * radius / distance;
    }
};

struct NativeTerrainRender {
    struct State {
        std::vector<ScanEdge> edges = std::vector<ScanEdge>(MaxScanEdges);
        std::vector<ScanPoint> points = std::vector<ScanPoint>(MaxScanPoints);
        std::vector<EmitSquare> squares = std::vector<EmitSquare>(MaxEmitSquares);
        std::vector<Point3F> emitPoints = std::vector<Point3F>(MaxScanPoints);
        std::vector<Point2F> waterTexCoords = std::vector<Point2F>(MaxScanPoints),
                             envTexCoords = std::vector<Point2F>(MaxScanPoints);
        NativeTerrainBlock* currentBlock{};
        Int32 squareSize{8};
        Flt32 screenSize{45}, currentGrowFactor{}, farDistance{500}, hazeDistance{250},
            pixelError{}, minWaterSquareSize{4};
        Point2F squareScale{8, 8}, blockPos, texScale;
        Point2I blockOffset;
        std::array<PlaneF, 6> clipPlane;
        Point3F camPos;
        UInt32 dynamicLightCount{};
        Projection* camera{};
        Int32 pointCount{}, edgeCount{}, squareCount{}, cameraSquare{};
        Int32 waveCount{};
        std::array<Point4F, 8> waveParameters;
        Color lmaBuffer[9][9]{};
    } trs;

    std::array<TerrLightInfo, 31> terrainLights;
    Flt32 currentR{}, currentG{}, currentB{}, currentRSq{}, screenSize{45};
    void setupColorAdder(float, float, float, float);
    void addColor(Color&, float);
    static UInt16 convertColorBack(Color&);
    static void convertColor(Color&, UInt16);
    void lightExpandHorz(float, float, float, int, int, int);
    void lightExpandVert(float, float, float, int, int, int);
    void lmExpandHorz(int, int, int);
    void lmExpandVert(int, int, int);
    void expandDynamicLight(TerrLightInfo&, int, int, int);
    void expandLightMap(int, int, int);
    void collapseLightMap();
    UInt32 TestSquareLights(GridSquare*, int, Point2I, UInt32);
    int TestSquareVisibility(Point3F&, Point3F&, int, float);
    int allocInitialPoint(Point3F);
    int allocWaterPoint(Point2F, int, int);
    int allocPoint(Point2I, int, int);
    int allocEdges(int);
    void clampEdge(int);
    void subdivideEdge(int, Point2I);
    void subdivideWaterEdge(int, Point2F);
    float getSquareDistance(Point3F&, Point3F&);
    void emitWaterSquare(WaterStackNode*);
    void emitSquare(SquareStackNode*, int, int, UInt32);
    void processCurrentBlock(int, int, int, int);
    float getWaterHeight(Point2F, int);
    void processWaterBlock(Point2F, float);
};

using std::max;

static int getPower(int n) {
    return int(std::bit_width(UInt32(n)));
}

static void m_sincos(float a, float* s, float* c) {
    opent1::m_sincos(a, *s, *c);
}
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4267 4018)
#endif
#include "terrain2_algorithms.inc"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
bool finite(Point3F p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::abs(p.x) <= 1e8f && std::abs(p.y) <= 1e8f && std::abs(p.z) <= 1e8f;
}

bool viewValid(const Terrain2View& v) {
    if (!finite(v.camera) || !std::isfinite(v.focalLength) || v.focalLength < .01f ||
        v.focalLength > 1e6f || !std::isfinite(v.pixelScale) || v.pixelScale < .01f ||
        v.pixelScale > 100 || !std::isfinite(v.screenSize) || v.screenSize < .01f ||
        v.screenSize > 1e6f || !std::isfinite(v.screenError) || v.screenError < 0 ||
        v.screenError > 1e6f || !std::isfinite(v.visibleDistance) || v.visibleDistance < 1 ||
        v.visibleDistance > 1e8f || !std::isfinite(v.hazeDistance) || v.hazeDistance < 0 ||
        v.hazeDistance >= v.visibleDistance || !v.maximumTriangles || v.maximumTriangles > 131072 ||
        v.lights.size() > 31) {
        return false;
    }
    if (v.planes) {
        for (auto p : *v.planes) {
            if (!finite(p) || !std::isfinite(p.d) || std::abs(p.d) > 1e9f) {
                return false;
            }
        }
    }
    return std::ranges::all_of(v.lights, [](const auto& l) {
        return finite(l.position) && std::isfinite(l.radius) && l.radius > 0 && l.radius <= 1e7f &&
               finite(l.color) && l.color.x >= 0 && l.color.y >= 0 && l.color.z >= 0 &&
               l.color.x <= 1 && l.color.y <= 1 && l.color.z <= 1;
    });
}

void configure(NativeTerrainRender& r, const Terrain2View& view, Projection& p) {
    p = {view.focalLength, view.pixelScale};
    auto& s = r.trs;
    s.camera = &p;
    s.camPos = view.camera;
    s.screenSize = r.screenSize = view.screenSize;
    s.farDistance = view.visibleDistance;
    s.hazeDistance = view.hazeDistance;
    s.pixelError =
        view.screenError >= .001f ? 1.f / p.projectRadius(view.screenError, 1) : .000001f;
    for (auto& plane : s.clipPlane) {
        plane = {0, 0, 0, 1e9f};
    }
    if (view.planes) {
        s.clipPlane = *view.planes;
    }
    s.dynamicLightCount = UInt32(view.lights.size());
    for (std::size_t i = 0; i < view.lights.size(); ++i) {
        const auto& l = view.lights[i];
        r.terrainLights[i] = {
            l.position, l.radius, l.radius * l.radius, l.color.x, l.color.y, l.color.z};
    }
}

std::vector<UInt32> fan(const NativeTerrainRender::State& s, const EmitSquare& square) {
    std::vector<UInt32> result;
    const auto append = [&](auto&& self, UInt32 index, bool reverse, UInt32 depth) -> void {
        if (index >= UInt32(s.edgeCount) || depth > 9) {
            throw std::runtime_error("Terrain2 edge overflow");
        }
        const auto& edge = s.edges[index];
        if (edge.mp < InvalidPointIndex) {
            self(self, edge.firstSubEdge + (reverse ? 1 : 0), reverse, depth + 1);
            self(self, edge.firstSubEdge + (reverse ? 0 : 1), reverse, depth + 1);
        } else {
            const auto p = reverse ? edge.p2 : edge.p1;
            if (p >= s.pointCount || result.size() >= 1024) {
                throw std::runtime_error("Terrain2 fan overflow");
            }
            result.push_back(p);
        }
    };
    for (UInt32 i = 0; i < 4; ++i) {
        const auto edge = (i + UInt32((square.flags & 1) != 0)) & 3;
        append(append, square.edge[edge], edge >= 2, 0);
    }
    return result;
}

void morph(NativeTerrainRender::State& s, bool water) {
    for (Int32 i = 0; i < s.pointCount; ++i) {
        auto& point = s.points[std::size_t(i)];
        const auto avg = (s.points[point.p1].avgHeight + s.points[point.p2].avgHeight) * .5f;
        point.avgHeight = point.pointHeight + point.minGrowFactor * (avg - point.pointHeight);
        s.emitPoints[std::size_t(i)].z = point.avgHeight;
        if (water) {
            auto& uv = s.envTexCoords[std::size_t(i)];
            const auto a = (s.envTexCoords[point.p1].x + s.envTexCoords[point.p2].x) * .5f,
                       b = (s.envTexCoords[point.p1].y + s.envTexCoords[point.p2].y) * .5f;
            uv.x += point.minGrowFactor * (a - uv.x);
            uv.y += point.minGrowFactor * (b - uv.y);
        }
    }
}

std::shared_ptr<const gfx::LightMap> lightmap(const NativeBitmap& bitmap) {
    auto map = std::make_shared<gfx::LightMap>();
    map->size = {bitmap.width, bitmap.height};
    map->format = gfx::LightMap::Format::Rgb565;
    map->samples.assign(bitmap.data.begin(),
                        bitmap.data.begin() + std::ptrdiff_t(bitmap.width * bitmap.height));
    return map;
}

Point2F oriented(Point2F p, UInt8 flags) {
    switch (flags & 7U) {
        case 1:
            return {p.y, 1 - p.x};
        case 2:
            return {1 - p.x, p.y};
        case 3:
            return {1 - p.y, 1 - p.x};
        case 4:
            return {p.x, 1 - p.y};
        case 5:
            return {p.y, p.x};
        case 6:
            return {1 - p.x, 1 - p.y};
        case 7:
            return {1 - p.y, p.x};
        default:
            return p;
    }
}

bool location(Point2I p) {
    return p.x >= 0 && p.y >= 0 && p.x < 768 && p.y < 768;
}
} // namespace

std::optional<Terrain2> Terrain2::convert(const TerrainGridResources& resources) {
    if (!resources.grid || resources.grid->blockMap.empty() ||
        resources.blocks.size() != resources.grid->blocks.size() ||
        resources.materials.textures().size() < 2 || !resources.materials.textures()[1]) {
        return {};
    }
    const auto id = resources.grid->blockMap[0];
    const auto at = std::ranges::find(resources.grid->blocks, id, &TerrainGridBlockEntry::id);
    if (at == resources.grid->blocks.end()) {
        return {};
    }
    const auto& input = resources.blocks[std::size_t(at - resources.grid->blocks.begin())];
    if (!input || !writeTerrainBlock(*input) || input->lightScale != 0 ||
        !std::isfinite(resources.grid->minimumHeight)) {
        return {};
    }
    auto b = std::make_shared<Terrain2Block>();
    b->heights.resize(257 * 257);
    b->materials.resize(256 * 256);
    for (UInt32 i = 0; i < 9; ++i) {
        const auto side = 256U >> i;
        b->squares[i].resize(std::size_t(side) * side);
    }
    auto base = gfx::Bitmap::create(256, 256), light = gfx::Bitmap::create(256, 256, 16);
    if (!base || !light) {
        return {};
    }
    b->baseTexture = std::move(*base);
    b->lightMap = std::move(*light);
    const auto palette = resources.materials.textures()[1]->paletteIndex;
    b->baseTexture.paletteIndex = palette;
    std::vector<UInt8> texels(resources.materials.textures().size());
    for (std::size_t i = 0; i < texels.size(); ++i) {
        const auto& t = resources.materials.textures()[i];
        if (t) {
            if (!t->valid() || t->bitDepth != 8) {
                return {};
            }
            texels[i] = std::to_integer<UInt8>(t->level(t->detailLevels - 1)[0]);
        }
    }
    const auto add = resources.grid->minimumHeight < 0 ? -resources.grid->minimumHeight : 0;
    for (UInt32 y = 0; y <= 256; ++y) {
        for (UInt32 x = 0; x <= 256; ++x) {
            const auto sx = x % input->width, sy = y % input->width;
            const auto h = input->heights[std::size_t(sy) * (input->width + 1) + sx] + add;
            b->heights[std::size_t(y) * 257 + x] = floatToFixed(std::clamp(h, 0.f, 2047.f));
            if (x < 256 && y < 256) {
                auto m = input->materials[std::size_t(sy) * input->width + sx];
                if (m.index >= texels.size()) {
                    m.index = 0;
                }
                b->materials[std::size_t(y) * 256 + x] = m;
                b->baseTexture.row(Int32(y))[x] = std::byte(texels[m.index]);
                const auto value = input->lights[std::size_t(sy) * (input->width + 1) + sx];
                const UInt32 r = (value >> 8) & 15, g = (value >> 4) & 15, blue = value & 15;
                const auto rgb = UInt16((r << 12) | ((r << 8) & 8) | (g << 7) | ((g << 3) & 12) |
                                        (blue << 1) | (blue >> 3));
                auto row = b->lightMap.row(Int32(y));
                row[x * 2] = std::byte(rgb & 255);
                row[x * 2 + 1] = std::byte(rgb >> 8);
            }
        }
    }
    NativeTerrainBlock native(*b);
    native.buildGridMap();
    Terrain2 result;
    result.block_ = std::move(b);
    result.materials_ = resources.materials;
    return result;
}

bool Terrain2::setHeight(Point2I p, Flt32 h) {
    if (!block_ || !location(p) || !std::isfinite(h) || h < 0 || h > 2047) {
        return false;
    }
    auto b = std::make_shared<Terrain2Block>(*block_);
    p.x &= 255;
    p.y &= 255;
    NativeTerrainBlock native(*b);
    native.heightMap[p.x + p.y * 257] = floatToFixed(h);
    const Point3F p1{0, 0, Flt32(native.getHeight(p.x, p.y))},
        p2{0, 1, Flt32(native.getHeight(p.x, p.y + 1))},
        p3{1, 1, Flt32(native.getHeight(p.x + 1, p.y + 1))},
        p4{1, 0, Flt32(native.getHeight(p.x + 1, p.y))};
    PlaneF a, c;
    a.set(p1, p2, p3);
    c.set(p1, p3, p4);
    auto* square = native.findSquare(0, p);
    for (Int32 y = 0; y <= 1; ++y) {
        for (Int32 x = 0; x <= 1; ++x) {
            const auto value = native.getHeight(p.x + x, p.y + y);
            square->minimum = std::min(square->minimum, value);
            square->maximum = std::max(square->maximum, value);
            Point3F point{Flt32(x), Flt32(y), fixedToFloat(value)};
            const auto dev = x < y   ? calcDev(a, point)
                             : x > y ? calcDev(c, point)
                                     : Umax(calcDev(a, point), calcDev(c, point));
            square->deviance = std::max(square->deviance, dev);
        }
    }
    for (Int32 i = 1; i <= 8; ++i) {
        auto* parent = native.findSquare(i, p);
        parent->minimum = std::min(parent->minimum, square->minimum);
        parent->maximum = std::max(parent->maximum, square->maximum);
        parent->deviance = std::max(parent->deviance, square->deviance);
    }
    block_ = std::move(b);
    return true;
}

bool Terrain2::setMaterial(Point2I p, TerrainGridMaterial m) {
    if (!block_ || !location(p) || m.index >= materials_.textures().size()) {
        return false;
    }
    auto b = std::make_shared<Terrain2Block>(*block_);
    p.x &= 255;
    p.y &= 255;
    b->materials[std::size_t(p.y) * 256 + p.x] = m;
    NativeTerrainBlock native(*b);
    if (bool(native.findSquare(0, p)->flags & GridSquare::Empty) != m.hole()) {
        native.buildGridMap();
    }
    block_ = std::move(b);
    return true;
}

bool Terrain2::setLighting(Point2I p, UInt16 rgb) {
    if (!block_ || !location(p)) {
        return false;
    }
    auto b = std::make_shared<Terrain2Block>(*block_);
    p.x &= 255;
    p.y &= 255;
    auto row = b->lightMap.row(p.y);
    row[std::size_t(p.x) * 2] = std::byte(rgb & 255);
    row[std::size_t(p.x) * 2 + 1] = std::byte(rgb >> 8);
    block_ = std::move(b);
    return true;
}

std::shared_ptr<const gfx::LightMap> Terrain2::lighting(Point2I p,
                                                        UInt32 level,
                                                        std::span<const Terrain2Light> lights,
                                                        UInt32 mask) const {
    Terrain2View view;
    view.lights.assign(lights.begin(), lights.end());
    if (!block_ || !location(p) || level > 8 || (UInt32(p.x) & ((1U << level) - 1)) ||
        (UInt32(p.y) & ((1U << level) - 1)) || !viewValid(view)) {
        return {};
    }
    auto copy = *block_;
    NativeTerrainBlock block(copy);
    auto renderer = std::make_unique<NativeTerrainRender>();
    Projection projection;
    configure(*renderer, view, projection);
    renderer->trs.currentBlock = &block;
    renderer->expandLightMap(p.x, p.y, Int32(level));
    mask &= (1U << lights.size()) - 1;
    UInt32 i{};
    while (mask) {
        renderer->expandDynamicLight(renderer->terrainLights[i++], p.x, p.y, Int32(level));
        mask >>= 1;
    }
    renderer->collapseLightMap();
    return lightmap(block.dynamic);
}

std::optional<Terrain2Frame> Terrain2::render(const Terrain2View& view) const {
    if (!block_ || !viewValid(view)) {
        return {};
    }
    try {
        auto r = std::make_unique<NativeTerrainRender>();
        Projection projection;
        configure(*r, view, projection);
        auto local = *block_;
        NativeTerrainBlock block(local);
        auto& s = r->trs;
        s.currentBlock = &block;
        Int32 initial[4][4]{}, horizontal[4][3]{}, vertical[4][3]{};
        for (Int32 i = 0; i < 4; ++i) {
            for (Int32 j = 0; j < 4; ++j) {
                initial[i][j] = r->allocInitialPoint(
                    {Flt32(i * 2048), Flt32(j * 2048), fixedToFloat(block.heightMap[0])});
            }
        }
        for (Int32 i = 0; i < 4; ++i) {
            for (Int32 j = 0; j < 3; ++j) {
                auto e = r->allocEdges(1);
                s.edges[std::size_t(e)].p1 = UInt16(initial[j][i]);
                s.edges[std::size_t(e)].p2 = UInt16(initial[j + 1][i]);
                horizontal[i][j] = e;
                e = r->allocEdges(1);
                s.edges[std::size_t(e)].p1 = UInt16(initial[i][j + 1]);
                s.edges[std::size_t(e)].p2 = UInt16(initial[i][j]);
                vertical[i][j] = e;
            }
        }
        for (Int32 i = 0; i < 3; ++i) {
            for (Int32 j = 0; j < 3; ++j) {
                s.blockOffset = {i * 256, j * 256};
                s.blockPos = {Flt32(i * 2048), Flt32(j * 2048)};
                r->processCurrentBlock(
                    horizontal[j + 1][i], vertical[i + 1][j], horizontal[j][i], vertical[i][j]);
            }
        }
        if (s.pointCount >= MaxScanPoints || s.edgeCount >= MaxScanEdges ||
            s.squareCount >= MaxEmitSquares) {
            return {};
        }
        morph(s, false);
        Terrain2Frame result;
        result.block = block_;
        result.points.assign(s.emitPoints.begin(), s.emitPoints.begin() + s.pointCount);
        // Cache entries use the authored material/mip blitter, with this edited
        // Terrain2 block's owned material map. No old cache key can alias an edit.
        ResourceManager owner;
        auto grid = std::make_unique<TerrainGridFile>();
        grid->detailCount = 9;
        grid->size = {1, 1};
        grid->scale = 3;
        grid->blockMap = {0};
        grid->blocks = {{0, "terrain2"}};
        auto source = createTerrainBlock(256);
        if (!source) {
            return {};
        }
        source->materials = block_->materials;
        for (std::size_t i = 0; i < source->heights.size(); ++i) {
            source->heights[i] = fixedToFloat(block_->heights[i]);
        }
        if (!rebuildTerrainHeightRange(*source)) {
            return {};
        }
        if (!owner.add("terrain2.dtf", std::move(grid)) ||
            !owner.add("terrain2#0.dtb", std::make_unique<TerrainGridBlock>(std::move(*source)))) {
            return {};
        }
        TerrainGridResources cacheResources;
        cacheResources.grid = owner.load<TerrainGridFile>("terrain2.dtf");
        cacheResources.blocks = {owner.load<TerrainGridBlock>("terrain2#0.dtb")};
        cacheResources.materials = materials_;
        TerrainTextureCache cache(cacheResources);
        auto staticMap = lightmap(block.low);
        auto base = std::make_unique<gfx::Bitmap>(block_->baseTexture);
        if (!owner.add("terrain2-base.bmp", std::move(base))) {
            return {};
        }
        const auto baseHandle = owner.load<gfx::Bitmap>("terrain2-base.bmp");
        std::map<std::tuple<UInt32, UInt32, UInt32, UInt32>, UInt32> materialIds;
        TsMaterialResources::Data materialData;
        materialData.list.details = 1;
        materialData.list.version = 4;
        for (Int32 square = 0; square < s.squareCount; ++square) {
            const auto& q = s.squares[std::size_t(square)];
            const auto indices = fan(s, q);
            if (indices.size() < 3 ||
                result.triangles.size() + indices.size() - 2 > view.maximumTriangles) {
                return {};
            }
            Terrain2Draw draw;
            draw.x = q.x;
            draw.y = q.y;
            draw.level = q.level;
            draw.mip = q.mipLevel;
            draw.lightMask = q.lightMask;
            const auto cells = 1U << q.level, x = UInt32(q.x) & 255, y = UInt32(q.y) & 255;
            const auto key =
                !q.level ? std::tuple{UInt32(block_->materials[std::size_t(y) * 256 + x].index),
                                      0U,
                                      0U,
                                      0U}
                : q.mipLevel == 255 ? std::tuple{0U, 0U, 9U, 255U}
                                    : std::tuple{x, y, UInt32(q.level), UInt32(q.mipLevel)};
            auto id = materialIds.find(key);
            UInt32 slot{};
            if (id == materialIds.end()) {
                Resource<gfx::Bitmap> texture;
                TsMaterial material;
                material.flags = 3; // native terrain is opaque texture shaded
                if (!q.level) {
                    const auto m = block_->materials[std::size_t(y) * 256 + x];
                    if (m.index >= materials_.textures().size() || !materials_.textures()[m.index]) {
                        return {};
                    }
                    texture = materials_.textures()[m.index];
                } else if (q.mipLevel == 255) {
                    texture = baseHandle;
                } else {
                    const auto tile = cache.tile(0, x, y, cells, 1U << q.mipLevel, false, false);
                    if (!tile) {
                        return {};
                    }
                    auto mipmap = std::make_unique<gfx::Bitmap>(*tile->texture);
                    mipmap->detailLevels = q.mipLevel + 1;
                    std::size_t bytes{};
                    for (Int32 mip = 0; mip < mipmap->detailLevels; ++mip) {
                        bytes += std::size_t(std::max(1, mipmap->width >> mip)) *
                                 std::max(1, mipmap->height >> mip);
                    }
                    mipmap->pixels.resize(bytes);
                    for (Int32 mip = 1; mip < mipmap->detailLevels; ++mip) {
                        const auto side = mipmap->width >> mip;
                        for (Int32 row = 0; row < side; ++row) {
                            auto dest = mipmap->row(row, mip);
                            const auto src = mipmap->row(row * 2, mip - 1);
                            for (Int32 col = 0; col < side; ++col) {
                                dest[std::size_t(col)] = src[std::size_t(col) * 2];
                            }
                        }
                    }
                    const auto name = std::format("terrain2-composite-{}.bmp", materialIds.size());
                    if (!mipmap->valid() || !owner.add(name, std::move(mipmap))) {
                        return {};
                    }
                    texture = owner.load<gfx::Bitmap>(name);
                }
                slot = UInt32(materialData.textures.size());
                if (slot >= 4096) {
                    return {};
                }
                materialData.textures.push_back(texture);
                materialData.list.materials.push_back(material);
                materialIds.emplace(key, slot);
            } else {
                slot = id->second;
            }
            draw.texture = materialData.textures[slot];
            Point2F lightOffset{Flt32(x) + .5f, Flt32(y) + .5f};
            Flt32 lightScale = 1.f / 256;
            if (q.lightMask) {
                r->expandLightMap(q.x, q.y, q.level);
                UInt32 mask = q.lightMask, index{};
                // Original prepareLighting's !mask & 1 precedence applies all
                // lights through the highest surviving bit, including gaps.
                while (mask) {
                    r->expandDynamicLight(r->terrainLights[index++], q.x, q.y, q.level);
                    mask >>= 1;
                }
                r->collapseLightMap();
                draw.lightMap = lightmap(block.dynamic);
                lightScale = .5f / Flt32(cells);
                lightOffset = {.0625f * cells, .0625f * cells};
            } else {
                draw.lightMap = staticMap;
            }
            for (auto i : indices) {
                const auto point = s.emitPoints[i];
                const auto distance = (point - view.camera).len();
                Flt32 haze{};
                if (distance >= view.hazeDistance) {
                    const auto factor =
                        (std::min(distance, view.visibleDistance) - view.hazeDistance) /
                            (view.visibleDistance - view.hazeDistance) -
                        1;
                    haze = 1 - factor * factor;
                }
                Point2F uv{(point.x / 8 - q.x) / Flt32(cells),
                           1 - (point.y / 8 - q.y) / Flt32(cells)};
                const Point2F lightUv{(lightOffset.x + (point.x / 8 - q.x)) * lightScale,
                                      (lightOffset.y + (point.y / 8 - q.y)) * lightScale};
                if (!q.level) {
                    uv = oriented(uv, block_->materials[std::size_t(y) * 256 + x].flags);
                } else if (q.mipLevel == 255) {
                    uv = lightUv;
                }
                draw.vertices.push_back({point, uv, {1, 1, 1}, lightUv, distance, haze});
            }
            for (std::size_t i = 1; i + 1 < draw.vertices.size(); ++i) {
                result.triangles.push_back(
                    {{draw.vertices[0], draw.vertices[i], draw.vertices[i + 1]},
                     slot,
                     false,
                     false,
                     draw.lightMap,
                     {},
                     true});
            }
            result.draws.push_back(std::move(draw));
        }
        materialData.list.materialsPerDetail = Int32(materialData.textures.size());
        result.materials = TsMaterialResources(std::move(materialData));
        return result;
    } catch (const AssertException&) {
        return {};
    } catch (const std::runtime_error&) {
        return {};
    }
}

std::optional<TsStaticShapeFrame> Terrain2Frame::prepare(const TsStaticView& view) const {
    return TsStaticShapeFrame::prepareSurfaces(materials, triangles, view);
}

std::optional<Terrain2WaterFrame> Terrain2::water(const Terrain2WaterView& view) {
    if (!viewValid(view.view) || !std::isfinite(view.extent.x) || !std::isfinite(view.extent.y) ||
        view.extent.x <= 0 || view.extent.y <= 0 || view.extent.x > 1e6f || view.extent.y > 1e6f ||
        !std::isfinite(view.minimumSquare) || view.minimumSquare < .01f ||
        view.minimumSquare > 1e6f || !std::isfinite(view.height) || view.height < 0 ||
        view.height > 1e6f || !std::isfinite(view.timeSeconds) || view.timeSeconds < 0 ||
        view.timeSeconds > 1e9f || view.waves.size() > 8 || !std::isfinite(view.textureScale.x) ||
        !std::isfinite(view.textureScale.y) || std::abs(view.textureScale.x) > 100 ||
        std::abs(view.textureScale.y) > 100) {
        return {};
    }
    for (auto w : view.waves) {
        if (!finite({w.x, w.y, w.z}) || std::abs(w.x) > 100 || std::abs(w.y) > 100 ||
            !std::isfinite(w.w) || std::abs(w.w) > 10000) {
            return {};
        }
    }
    try {
        auto r = std::make_unique<NativeTerrainRender>();
        Projection p;
        configure(*r, view.view, p);
        auto& s = r->trs;
        s.minWaterSquareSize = view.minimumSquare;
        s.texScale = view.textureScale;
        s.waveCount = Int32(view.waves.size());
        std::ranges::copy(view.waves, s.waveParameters.begin());
        const auto scale = .05f * Flt32(3.14159265358979323846),
                   frequency = Flt32(double(view.timeSeconds) * 6.28318530717958647692);
        for (auto& wave : s.waveParameters) {
            wave.w *= view.height;
            wave.x *= scale;
            wave.y *= scale;
            wave.z *= frequency;
        }
        r->processWaterBlock(view.extent, std::abs(view.height));
        if (s.pointCount >= MaxScanPoints || s.edgeCount >= MaxScanEdges ||
            s.squareCount >= MaxEmitSquares) {
            return {};
        }
        morph(s, true);
        Terrain2WaterFrame result;
        result.points.assign(s.emitPoints.begin(), s.emitPoints.begin() + s.pointCount);
        result.camera = view.view.camera;
        result.visibleDistance = view.view.visibleDistance;
        result.hazeDistance = view.view.hazeDistance;
        result.underwater =
            view.underwater ||
            (result.camera.x >= 0 && result.camera.y >= 0 && result.camera.z <= -view.height &&
             result.camera.x <= view.extent.x && result.camera.y <= view.extent.y);
        if (!result.underwater && s.cameraSquare >= 0 && s.cameraSquare < s.squareCount) {
            const auto& square = s.squares[std::size_t(s.cameraSquare)];
            const auto tl = s.emitPoints[s.edges[square.edge[0]].p1],
                       tr = s.emitPoints[s.edges[square.edge[0]].p2],
                       bl = s.emitPoints[s.edges[square.edge[2]].p1],
                       br = s.emitPoints[s.edges[square.edge[2]].p2];
            Point3F normal;
            if ((result.camera.x - tl.x) / (br.x - tl.x) >
                (result.camera.y - tl.y) / (br.y - tl.y)) {
                m_cross(tl - tr, br - tr, normal);
            } else {
                m_cross(br - bl, tl - bl, normal);
            }
            result.underwater = m_dot(result.camera - tl, normal) < 0;
        }
        result.textureCoordinates.assign(s.waterTexCoords.begin(),
                                         s.waterTexCoords.begin() + s.pointCount);
        result.environmentCoordinates.assign(s.envTexCoords.begin(),
                                             s.envTexCoords.begin() + s.pointCount);
        for (Int32 i = 0; i < s.squareCount; ++i) {
            const auto indices = fan(s, s.squares[std::size_t(i)]);
            if (result.triangles.size() + indices.size() - 2 > view.view.maximumTriangles) {
                return {};
            }
            for (std::size_t j = 1; j + 1 < indices.size(); ++j) {
                result.triangles.push_back(
                    result.underwater
                        ? std::array<UInt32, 3>{indices[0], indices[j + 1], indices[j]}
                        : std::array<UInt32, 3>{indices[0], indices[j], indices[j + 1]});
            }
        }
        return result;
    } catch (const AssertException&) {
        return {};
    } catch (const std::runtime_error&) {
        return {};
    }
}

std::optional<gfx::HardwareBatch> Terrain2WaterFrame::hardware(const TsStaticView& view,
                                                               const gfx::Bitmap* water,
                                                               const gfx::Bitmap* environment,
                                                               const gfx::Palette* palette,
                                                               Point3F waterColor,
                                                               Flt32 alpha) const {
    if (!finite(waterColor) || waterColor.x < 0 || waterColor.y < 0 || waterColor.z < 0 ||
        waterColor.x > 1 || waterColor.y > 1 || waterColor.z > 1 || !std::isfinite(alpha) ||
        alpha < 0 || alpha > 1 || points.size() != textureCoordinates.size() ||
        points.size() != environmentCoordinates.size() || triangles.size() > 4096 ||
        visibleDistance <= hazeDistance) {
        return {};
    }
    ResourceManager owner;
    TsMaterialResources::Data materials;
    materials.list.details = 1;
    materials.list.version = 4;
    materials.list.materialsPerDetail = 1;
    TsMaterial m;
    if (water) {
        if (!water->valid() || !owner.add("water.bmp", std::make_unique<gfx::Bitmap>(*water))) {
            return {};
        }
        m.flags = 0x103;
        materials.textures.push_back(owner.load<gfx::Bitmap>("water.bmp"));
    } else {
        m.flags = 0x102;
        m.rgb = {0, 0, 255, 0};
        materials.textures.resize(1);
    }
    materials.list.materials.push_back(m);
    std::shared_ptr<gfx::LightMap> env;
    if (!underwater && water && environment) {
        if (!environment->valid() || environment->width < 2 || environment->height < 2 ||
            environment->width > 256 || environment->height > 256) {
            return {};
        }
        env = std::make_shared<gfx::LightMap>();
        env->format = gfx::LightMap::Format::Rgb565;
        env->size = {environment->width, environment->height};
        env->samples.resize(std::size_t(env->size.x) * env->size.y);
        const auto* colors = environment->palette ? &*environment->palette : palette;
        if (environment->bitDepth == 8 && (!colors || !colors->valid())) {
            return {};
        }
        for (Int32 y = 0; y < environment->height; ++y) {
            const auto row = environment->row(y);
            for (Int32 x = 0; x < environment->width; ++x) {
                UInt16 rgb{};
                if (environment->bitDepth == 16) {
                    rgb =
                        UInt16(std::to_integer<UInt8>(row[std::size_t(x) * 2]) |
                               (UInt16(std::to_integer<UInt8>(row[std::size_t(x) * 2 + 1])) << 8));
                } else {
                    UInt32 red{}, green{}, blue{};
                    if (environment->bitDepth == 8) {
                        const auto& c = colors->find(environment->paletteIndex)
                                            .colors[std::to_integer<UInt8>(row[std::size_t(x)])];
                        red = c.red;
                        green = c.green;
                        blue = c.blue;
                    } else {
                        blue = std::to_integer<UInt8>(row[std::size_t(x) * 3]);
                        green = std::to_integer<UInt8>(row[std::size_t(x) * 3 + 1]);
                        red = std::to_integer<UInt8>(row[std::size_t(x) * 3 + 2]);
                    }
                    rgb = UInt16((red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3));
                }
                env->samples[std::size_t(y) * environment->width + x] = rgb;
            }
        }
    }
    std::vector<StaticSurfaceTriangle> surfaces;
    surfaces.reserve(triangles.size());
    for (const auto& tri : triangles) {
        StaticSurfaceTriangle t;
        t.repeatTexture = true;
        t.nativeFarSphere = true;
        t.lightMap = env;
        for (UInt32 i = 0; i < 3; ++i) {
            const auto index = tri[i];
            if (index >= points.size()) {
                return {};
            }
            const auto distance = (points[index] - camera).len();
            float haze{};
            if (distance >= hazeDistance) {
                const auto f = (std::min(distance, visibleDistance) - hazeDistance) /
                                   (visibleDistance - hazeDistance) -
                               1;
                haze = 1 - f * f;
            }
            t.vertices[i] = {points[index],
                             textureCoordinates[index],
                             {1, 1, 1},
                             environmentCoordinates[index],
                             distance,
                             haze};
        }
        surfaces.push_back(std::move(t));
    }
    auto prepared = TsStaticShapeFrame::prepareSurfaces(std::move(materials), surfaces, view);
    if (!prepared) {
        return {};
    }
    auto polygons = prepared->hardwarePolygons(palette);
    if (!polygons) {
        return {};
    }
    for (auto& p : *polygons) {
        p.material.perspective = false;
        p.material.wrap = true;
        p.material.deferredHaze = false;
        if (!underwater || !water) {
            p.material.alphaSource = gfx::HardwareAlphaSource::Constant;
            p.material.opacity = water ? alpha : .5f;
            p.material.blend = gfx::HardwareBlend::Alpha;
            p.material.depth = gfx::HardwareDepth::TestOnly;
        }
    }
    const std::array<gfx::PolygonVertex, 4> overlay{{{{0, 0}},
                                                     {{Flt32(view.width), 0}},
                                                     {{Flt32(view.width), Flt32(view.height)}},
                                                     {{0, Flt32(view.height)}}}};
    if (underwater) {
        gfx::HardwareMaterial color;
        color.color = waterColor;
        color.opacity = .5f;
        color.alphaSource = gfx::HardwareAlphaSource::Constant;
        color.blend = gfx::HardwareBlend::Alpha;
        color.depth = gfx::HardwareDepth::Disabled;
        polygons->push_back({overlay, color, {}, {}});
    }
    return gfx::HardwareBatch::prepare(*polygons, palette);
}
} // namespace opent1::world
