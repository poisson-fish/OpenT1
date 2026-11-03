#include "gfx_test_support.hpp"
#include <ranges>
#include <opent1/gfx/hardware_batch.hpp>
#include <future>
#include <limits>
#include <opent1/gfx/render_limits.hpp>

namespace {
using namespace gfx_test;

std::array<PolygonVertex, 4> quad() {
    return {{{{1, 2}, {0, 0}}, {{11, 2}, {1, 0}}, {{11, 12}, {1, 1}}, {{1, 12}, {0, 1}}}};
}

std::optional<HardwareBatch> prepare(std::span<const PolygonVertex> vertices,
                                     HardwareMaterial material = {},
                                     const Palette* p = nullptr) {
    return HardwareBatch::prepare(std::array<HardwarePolygon, 1>{{{vertices, material}}}, p);
}

void geometryAndLifetime() {
    auto v = quad();
    HardwareMaterial m;
    m.shadeSource = PolygonSource::Vertex;
    m.hazeSource = PolygonSource::Vertex;
    v[1].color = {.1f, .2f, .3f};
    v[1].haze = .75f;
    v[2].inverseDepth = .5f;
    auto batch = prepare(v, m);
    CHECK(batch && batch->draws().size() == 1 && batch->vertices().size() == 6);
    CHECK(batch->vertices()[1].color == v[1].color && batch->vertices()[1].haze == .75f);
    CHECK(batch->vertices()[2].inverseDepth == .5f);
    const auto saved = *batch;
    batch.reset();
    v = {};
    auto copied = saved;
    auto moved = std::move(copied);
    CHECK(moved.vertices().size() == 6 && moved.vertices()[1].haze == .75f);
    m.shadeSource = PolygonSource::Constant;
    m.shade = .25f;
    m.hazeSource = PolygonSource::Constant;
    m.haze = .5f;
    v = quad();
    batch = prepare(v, m);
    CHECK(batch && batch->vertices()[0].color == Point3F(.25f, .25f, .25f) &&
          batch->vertices()[0].haze == .5f);
    batch = prepare(v);
    CHECK(batch && batch->vertices()[0].color == Point3F(1, 1, 1) &&
          batch->vertices()[0].haze == 0);
    std::ranges::reverse(v);
    batch = prepare(v);
    CHECK(batch && batch->draws().empty());
    m.twoSided = true;
    CHECK(prepare(v, m)->draws().size() == 1);
    const auto empty = HardwareBatch::prepare({});
    CHECK(empty && empty->draws().empty() && empty->textures().size() == 1);
    auto a = std::async(std::launch::async, [&] {
        return prepare(quad())->vertices().size();
    });
    auto b = std::async(std::launch::async, [&] {
        return prepare(quad())->vertices().size();
    });
    CHECK(a.get() == 6 && b.get() == 6);
}

void textureFormats() {
    auto p = palette(1);
    p.palettes[0].colors[1] = {12, 34, 56, 77};
    auto b = Bitmap::create(3, 2);
    CHECK(b);
    b->pixels.assign(b->pixels.size(), std::byte{1});
    b->pixels[0] = std::byte{0};
    HardwareMaterial m;
    m.texture = b.get();
    m.transparent = true;
    CHECK(!prepare(quad(), m));
    auto batch = prepare(quad(), m, &p);
    CHECK(batch && batch->textures().size() == 2);
    const auto textureView = batch->textures();
    const auto& rgba = textureView[1].rgba;
    CHECK(rgba.size() == 24 && rgba[3] == std::byte{0});
    CHECK(rgba[4] == std::byte{12} && rgba[5] == std::byte{34} && rgba[6] == std::byte{56} &&
          rgba[7] == std::byte{255});
    b->palette = p;
    b->palette->palettes[0].colors[1].red = 99;
    CHECK(prepare(quad(), m, &p)->textures()[1].rgba[4] == std::byte{99});
    m.useSurfacePalette = true;
    CHECK(prepare(quad(), m, &p)->textures()[1].rgba[4] == std::byte{12});
    CHECK(prepare(quad(), m)->textures()[1].rgba[4] == std::byte{99});
    const auto mixedVertices = quad();
    auto embedded = m;
    embedded.useSurfacePalette = false;
    const std::array<HardwarePolygon, 2> mixed{{{mixedVertices, m}, {mixedVertices, embedded}}};
    const auto mixedBatch = HardwareBatch::prepare(mixed, &p);
    CHECK(mixedBatch && mixedBatch->textures().size() == 3);
    CHECK(mixedBatch->textures()[1].rgba[4] == std::byte{12});
    CHECK(mixedBatch->textures()[2].rgba[4] == std::byte{99});
    m.useSurfacePalette = false;
    auto extra = p.palettes[0];
    extra.index = 7;
    extra.colors[1].green = 90;
    b->palette->palettes.push_back(extra);
    b->paletteIndex = 7;
    CHECK(prepare(quad(), m, &p)->textures()[1].rgba[5] == std::byte{90});
    // A texture is deduplicated by source and zero-transparency policy, without
    // sharing source storage. Different source palettes remain independent.
    const auto vertices = quad();
    const std::array<HardwarePolygon, 2> stable{{{vertices, m}, {vertices, m}}};
    batch = HardwareBatch::prepare(stable, &p);
    CHECK(batch && batch->textures().size() == 2);
    const auto bytes = batch->textures()[1].rgba;
    b.reset();
    p = {};
    CHECK(batch->textures()[1].rgba == bytes);
    b = Bitmap::create(2, 1, 24);
    CHECK(b);
    b->pixels[0] = std::byte{3};
    b->pixels[1] = std::byte{2};
    b->pixels[2] = std::byte{1};
    m = {};
    m.texture = b.get();
    batch = prepare(vertices, m);
    CHECK(batch && batch->textures()[1].rgba[0] == std::byte{1});
    m.transparent = true;
    CHECK(!prepare(vertices, m));
    b = Bitmap::create(1, 1, 16);
    CHECK(b);
    b->pixels[0] = std::byte{0xe0};
    b->pixels[1] = std::byte{0x07};
    m = {};
    m.texture = b.get();
    batch = prepare(vertices, m);
    CHECK(batch &&
          batch->textures()[1].rgba ==
              std::vector<std::byte>({std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}}));
    b->attributes |= BitmapAlpha88;
    b->pixels[0] = std::byte{1};
    b->pixels[1] = std::byte{128};
    b->palette = palette(0);
    b->palette->palettes[0].colors[1] = {12, 34, 56, 0};
    batch = prepare(vertices, m);
    CHECK(batch && batch->textures()[1].rgba ==
                       std::vector<std::byte>(
                           {std::byte{12}, std::byte{34}, std::byte{56}, std::byte{128}}));
}

void malformedAndBudgets() {
    const auto largeQuad = quad();
    std::vector<HardwarePolygon> large(5000, {largeQuad, {}});
    CHECK(HardwareBatch::prepare(large));
    ScopedRenderLimits budget{RenderLimits{}};
    const auto original = quad();
    auto v = original;
    HardwareMaterial m;
    CHECK(!prepare(std::span(v).first(2)));
    v[2].position = {2, 3};
    CHECK(!prepare(v));
    v = original;
    v[1] = v[0];
    CHECK(!prepare(v));
    v = original;
    v[1].position.x = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!prepare(v));
    v = original;
    v[1].inverseDepth = 0;
    CHECK(!prepare(v));
    v[1].inverseDepth = 65537;
    CHECK(!prepare(v));
    v = original;
    v[1].haze = 2;
    CHECK(!prepare(v));
    v = original;
    v[1].color.y = -1;
    CHECK(!prepare(v));
    m.opacity = -1;
    CHECK(!prepare(original, m));
    m = {};
    m.depth = HardwareDepth(99);
    CHECK(!prepare(original, m));
    m = {};
    m.shadeSource = PolygonSource(99);
    CHECK(!prepare(original, m));
    m = {};
    m.wrap = true;
    CHECK(!prepare(original, m));
    std::vector<HardwarePolygon> polygons(4096, {original, {}});
    CHECK(HardwareBatch::prepare(polygons));
    polygons.push_back({original, {}});
    CHECK(!HardwareBatch::prepare(polygons));
    auto texture = Bitmap::create(4096, 4096);
    CHECK(texture);
    m = {};
    m.texture = texture.get();
    auto p = palette(0);
    CHECK(!prepare(original, m, &p)); // white texel counts toward 64 MiB
    texture.reset();
    std::vector<Bitmap> textures;
    textures.reserve(257);
    polygons.clear();
    std::ranges::for_each(std::views::iota(0U, 257U), [&](auto) {
        textures.push_back(std::move(*Bitmap::create(1, 1)));
    });
    std::ranges::for_each(textures, [&](const auto& t) {
        HardwareMaterial material;
        material.texture = &t;
        polygons.push_back({original, material});
    });
    CHECK(HardwareBatch::prepare(std::span(polygons).first(256), &p));
    CHECK(HardwareBatch::prepare(polygons, &p));
    LightMap map{{2, 2}, {}, {0, 0, 0, 0}};
    const std::array<Point2F, 4> uv{};
    polygons[0].material.lightMap = &map;
    polygons[0].lightMapCoordinates = uv;
    CHECK(HardwareBatch::prepare(std::span(polygons).first(255), &p));
    CHECK(HardwareBatch::prepare(std::span(polygons).first(256),
                                 &p)); // native lightmaps have their own bounded budget
    // Many convex vertices cross the aggregate input budget before publication.
    std::vector<PolygonVertex> circle;
    std::ranges::for_each(std::views::iota(0U, 100U), [&](auto i) {
        const double angle = double(i) * 6.283185307179586 / 100;
        PolygonVertex vertex;
        vertex.position = {Flt32(20 + 10 * std::cos(angle)), Flt32(20 + 10 * std::sin(angle))};
        circle.push_back(vertex);
    });
    polygons.assign(656, {circle, {}});
    CHECK(!HardwareBatch::prepare(polygons));
    polygons.resize(655);
    CHECK(HardwareBatch::prepare(polygons));
}

void mipChainsAndLightMaps() {
    auto bitmap = Bitmap::create(8, 2);
    CHECK(bitmap);
    bitmap->detailLevels = 4;
    bitmap->pixels.resize(23);
    auto p = palette(0);
    for (auto i : std::views::iota(Int32{0}, Int32{4})) {
        std::ranges::fill(bitmap->level(i), std::byte(i + 1));
        p.palettes[0].colors[std::size_t(i + 1)] = {UInt8(10 + i), UInt8(20 + i), UInt8(30 + i), 0};
    }
    CHECK(bitmap->valid());
    HardwareMaterial m;
    m.texture = bitmap.get();
    m.mipFilter = HardwareMipFilter::Linear;
    m.lodBias = -.5f;
    const auto v = quad();
    auto batch = prepare(v, m, &p);
    CHECK(batch);
    CHECK(batch->textures()[1].rgba.size() == 64 && batch->textures()[1].mips.size() == 3);
    CHECK(batch->textures()[1].mips[0].width == 4 && batch->textures()[1].mips[0].height == 1);
    CHECK(batch->textures()[1].mips[2].width == 1 && batch->textures()[1].mips[2].height == 1);
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
        CHECK(batch->textures()[1].mips[i].rgba[0] == std::byte(11 + i));
    }
    CHECK(batch->draws()[0].mipFilter == HardwareMipFilter::Linear &&
          batch->draws()[0].lodBias == -.5f);
    CHECK(!batch->draws()[0].magnificationLinear);
    m.linear = true;
    batch = prepare(v, m, &p);
    CHECK(batch && batch->draws()[0].magnificationLinear);
    m.magnificationLinear = false;
    batch = prepare(v, m, &p);
    CHECK(batch && batch->draws()[0].linear && !batch->draws()[0].magnificationLinear);
    std::ranges::fill(bitmap->level(2), std::byte{0});
    m.transparent = true;
    CHECK(prepare(v, m, &p)->textures()[1].mips[1].rgba[3] == std::byte{0});
    bitmap.reset();
    p = {};
    CHECK(batch->textures()[1].mips[2].rgba[0] == std::byte{13});
    m = {};
    m.mipFilter = HardwareMipFilter(99);
    CHECK(!prepare(v, m));
    m = {};
    m.lodBias = 1;
    CHECK(!prepare(v, m));
    m = {};
    m.magnificationLinear = false;
    CHECK(!prepare(v, m));
    m = {};
    m.magnificationLinear = true;
    CHECK(!prepare(v, m));
    m = {};
    bitmap = Bitmap::create(4, 4);
    CHECK(bitmap);
    m.texture = bitmap.get();
    m.lodBias = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!prepare(v, m, &p));
    m.lodBias = 16.01f;
    CHECK(!prepare(v, m, &p));
    // Exhaust the original RGB444 input domain. The alpha nibble has no effect.
    LightMap map{{64, 64}, {7, 9}, std::vector<UInt16>(4096)};
    for (auto i : std::views::iota(std::size_t{0}, map.samples.size())) {
        map.samples[i] = UInt16(i | ((i & 15) << 12));
    }
    const std::array<Point2F, 4> uv{{{-.5f, 0}, {.5f, 0}, {.5f, 1}, {-.5f, 1}}};
    m = {};
    m.lightMap = &map;
    const std::array<HardwarePolygon, 2> polygons{{{v, m, uv}, {v, m, uv}}};
    batch = HardwareBatch::prepare(polygons);
    CHECK(batch && batch->textures().size() == 2);
    CHECK(batch->draws()[0].lightMap == 1 && batch->draws()[1].lightMap == 1);
    CHECK(batch->vertices()[0].lightMap == uv[0] && batch->vertices()[4].lightMap == uv[2]);
    constexpr std::array<UInt8, 16> expanded{
        15, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 207, 223, 239, 255};
    const auto textureView = batch->textures();
    const auto& rgba = textureView[1].rgba;
    for (auto i : std::views::iota(std::size_t{0}, std::size_t{4096})) {
        CHECK(rgba[i * 4] == std::byte(expanded[i / 256]) &&
              rgba[i * 4 + 1] == std::byte(expanded[(i / 16) % 16]));
        CHECK(rgba[i * 4 + 2] == std::byte(expanded[i % 16]) && rgba[i * 4 + 3] == std::byte{255});
    }
    map.samples.clear();
    CHECK(rgba[0] == std::byte{15} && !HardwareBatch::prepare(polygons));
    map = {{2, 2}, {}, {0, 0, 0, 0}};
    CHECK(!prepare(v, m)); // missing independent coordinates
    auto invalid = uv;
    invalid[0].x = std::numeric_limits<Flt32>::infinity();
    CHECK(!HardwareBatch::prepare(std::array<HardwarePolygon, 1>{{{v, m, invalid}}}));
    m = {};
    CHECK(!HardwareBatch::prepare(std::array<HardwarePolygon, 1>{{{v, m, uv}}}));
    // All supplied mip bytes count, even when the draw disables mip sampling.
    auto large = Bitmap::create(4096, 2048);
    CHECK(large);
    large->detailLevels = 2;
    large->pixels.resize(4096U * 2048U + 2048U * 1024U);
    CHECK(large->valid());
    auto other = *large;
    m.texture = large.get();
    auto n = m;
    n.texture = &other;
    CHECK(prepare(v, m, &p));
    const ScopedRenderLimits limits{RenderLimits{}};
    CHECK(!HardwareBatch::prepare(std::array<HardwarePolygon, 2>{{{v, m}, {v, n}}}, &p));
}

void alphaControls() {
    const auto vertices = quad();
    HardwareMaterial m;
    m.alphaSource = HardwareAlphaSource::Vertex;
    std::array<Flt32, 4> opacity{0, .25f, .75f, 1};
    const auto prepareAlpha = [&] {
        return HardwareBatch::prepare(std::array<HardwarePolygon, 1>{{{vertices, m, {}, opacity}}});
    };
    CHECK(!prepare(vertices, m));
    auto batch = prepareAlpha();
    CHECK(batch && batch->vertices()[1].opacity == .25f && batch->vertices()[5].opacity == 1);
    opacity.fill(.5f);
    CHECK(batch->vertices()[1].opacity == .25f); // owns the fan's alpha
    opacity[0] = -1;
    CHECK(!prepareAlpha());
    opacity[0] = 1.01f;
    CHECK(!prepareAlpha());
    opacity[0] = std::numeric_limits<Flt32>::quiet_NaN();
    CHECK(!prepareAlpha());
    opacity[0] = 0;
    m.alphaSource = HardwareAlphaSource::Constant;
    CHECK(!prepareAlpha());
    m.alphaSource = HardwareAlphaSource(99);
    CHECK(!prepare(vertices, m));
    m = {};
    m.alphaCutoff = -1;
    CHECK(!prepare(vertices, m));
    m.alphaCutoff = 1.01f;
    CHECK(!prepare(vertices, m));
    m.alphaCutoff = std::numeric_limits<Flt32>::infinity();
    CHECK(!prepare(vertices, m));
    m.alphaCutoff = 1;
    m.blend = HardwareBlend::Subtractive;
    m.depth = HardwareDepth::WriteOnly;
    batch = prepare(vertices, m);
    CHECK(batch && batch->draws()[0].alphaCutoff == 1);
    m.blend = HardwareBlend(99);
    CHECK(!prepare(vertices, m));
    m = {};
    m.paletteAlpha = true;
    CHECK(!prepare(vertices, m));
    auto bitmap = Bitmap::create(4, 1);
    CHECK(bitmap);
    bitmap->detailLevels = 3;
    bitmap->pixels.resize(7);
    std::ranges::fill(bitmap->pixels, std::byte{1});
    bitmap->pixels[0] = std::byte{0};
    auto p = palette(0);
    p.palettes[0].colors[1] = {30, 60, 90, 128};
    p.palettes[0].colors[0].flags = 77;
    m.texture = bitmap.get();
    batch = prepare(vertices, m, &p);
    CHECK(batch);
    CHECK(batch->textures()[1].rgba[3] == std::byte{77} &&
          batch->textures()[1].rgba[7] == std::byte{128});
    CHECK(batch->textures()[1].mips[1].rgba[3] == std::byte{128});
    auto opaque = m;
    opaque.paletteAlpha = false;
    auto cutout = m;
    cutout.transparent = true;
    batch = HardwareBatch::prepare(
        std::array<HardwarePolygon, 3>{{{vertices, m}, {vertices, opaque}, {vertices, cutout}}},
        &p);
    CHECK(batch && batch->textures().size() == 4);
    CHECK(batch->textures()[2].rgba[7] == std::byte{255} &&
          batch->textures()[3].rgba[3] == std::byte{0});
    CHECK(batch->textures()[3].rgba[7] == std::byte{128});
    p.palettes[0].colors[1].flags = 99;
    CHECK(batch->textures()[1].rgba[7] == std::byte{128});
    bitmap = Bitmap::create(1, 1, 24);
    m.texture = bitmap.get();
    CHECK(!prepare(vertices, m, &p));
    bitmap = Bitmap::create(1, 1, 16);
    bitmap->attributes |= BitmapAlpha88;
    m.texture = bitmap.get();
    CHECK(!prepare(vertices, m, &p));
    // All flag bytes are valid constant fill opacities, without software tables.
    std::ranges::for_each(std::views::iota(0U, 256U), [&](auto i) {
        p.palettes[0].colors[i] = {UInt8(i), UInt8(255 - i), 127, UInt8(i)};
        const auto fill = hardwarePaletteFill(p, UInt8(i));
        CHECK(fill);
        CHECK(fill->color == Point3F(Flt32(i) / 255, Flt32(255 - i) / 255, 127.f / 255) &&
              fill->opacity == Flt32(i) / 255);
        CHECK(fill->alphaSource == HardwareAlphaSource::Constant &&
              fill->blend == HardwareBlend::Alpha && !fill->texture);
    });
    auto sub = p.palettes[0];
    sub.index = 9;
    sub.colors[2] = {1, 2, 3, 4};
    p.palettes.push_back(sub);
    CHECK(hardwarePaletteFill(p, 2, 9)->opacity == 4.f / 255);
    CHECK(hardwarePaletteFill(p, 2, 77)->opacity == 2.f / 255); // established fallback
    p.palettes.clear();
    CHECK(!hardwarePaletteFill(p, 2));
}

void sharedTexturesAcrossFrames() {
    const auto vertices = quad();
    auto p = palette(0);
    auto bitmap = Bitmap::create(4, 4);
    CHECK(bitmap);
    bitmap->detailLevels = 2;
    bitmap->pixels.resize(20, std::byte{1});
    std::ranges::fill(bitmap->pixels, std::byte{1});
    p.palettes[0].colors[1] = {64, 96, 128, 200};
    p.palettes[0].colors[2] = {32, 48, 64, 100};
    HardwareMaterial material;
    material.texture = bitmap.get();
    material.paletteMode = HardwarePaletteMode::OpenGL;
    auto first = prepare(vertices, material, &p);
    auto next = prepare(vertices, material, &p);
    CHECK(first && next);
    CHECK(&first->textures()[1] == &next->textures()[1]);
    CHECK(first->textures()[1].rgba.data() == next->textures()[1].rgba.data());
    CHECK(first->textureHashes()[1] == next->textureHashes()[1]);
    material.dynamicTexture = true;
    const auto dynamic = prepare(vertices, material, &p);
    CHECK(dynamic && dynamic->textures()[1].dynamic && !first->textures()[1].dynamic);
    CHECK(dynamic->textures()[1].rgba == first->textures()[1].rgba);
    material.dynamicTexture = false;
    auto combined = HardwareBatch::compose(std::array<const HardwareBatch*, 2>{&*first, &*next});
    CHECK(combined);
    CHECK(&combined->textures()[1] == &first->textures()[1] &&
          &combined->textures()[3] == &first->textures()[1]);
    // Reallocation and draw-only policy changes do not invalidate conversion.
    bitmap->pixels = std::vector<std::byte>(bitmap->pixels);
    material.linear = true;
    material.wrap = true;
    material.mipFilter = HardwareMipFilter::Linear;
    next = prepare(vertices, material, &p);
    CHECK(next && &next->textures()[1] == &first->textures()[1]);
    const auto original = first->textures()[1].rgba;
    material.paletteGamma = 2;
    next = prepare(vertices, material, &p);
    CHECK(next && &next->textures()[1] != &first->textures()[1] &&
          next->textures()[1].rgba != original);
    material.paletteGamma = 1;
    p.palettes[0].colors[1].red = 180;
    next = prepare(vertices, material, &p);
    CHECK(next && next->textures()[1].rgba[0] == std::byte{180});
    CHECK(first->textures()[1].rgba == original);
    const auto changedPalette = *next;
    bitmap->level(1)[0] = std::byte{2};
    next = prepare(vertices, material, &p);
    CHECK(next && &next->textures()[1] != &changedPalette.textures()[1]);
    CHECK(next->textures()[1].rgba == changedPalette.textures()[1].rgba &&
          next->textures()[1].mips[0].rgba != changedPalette.textures()[1].mips[0].rgba);
    const auto changedMip = *next;
    bitmap->pixels[0] = std::byte{2};
    next = prepare(vertices, material, &p);
    CHECK(next && next->textures()[1].rgba != changedMip.textures()[1].rgba);
    bitmap->detailLevels = 1;
    bitmap->pixels.resize(16);
    next = prepare(vertices, material, &p);
    CHECK(next && next->textures()[1].mips.empty());
    material.transparent = true;
    bitmap->pixels[0] = std::byte{};
    next = prepare(vertices, material, &p);
    CHECK(next && next->textures()[1].rgba[3] == std::byte{});
    bitmap->pixels.clear();
    CHECK(!prepare(vertices, material, &p)); // warm cache cannot admit invalid input
    // Lightmaps use the same ownership policy, including encoding and size.
    LightMap map{{2, 2}, {}, {0x123, 0x456, 0x789, 0xabc}};
    const std::array<Point2F, 4> uv{};
    material = {};
    material.lightMap = &map;
    const auto lit = [&] {
        return HardwareBatch::prepare(std::array<HardwarePolygon, 1>{{{vertices, material, uv}}});
    };
    auto light = lit();
    next = lit();
    CHECK(light && next && &light->textures()[1] == &next->textures()[1]);
    map.samples[0] = 0xfff;
    next = lit();
    CHECK(next && next->textures()[1].rgba != light->textures()[1].rgba);
    const auto changedSamples = *next;
    material.lightMapMode = HardwareLightMapMode::OpenGLPacked;
    next = lit();
    CHECK(next && next->textures()[1].rgba != changedSamples.textures()[1].rgba);
    map.format = LightMap::Format::Rgb565;
    next = lit();
    CHECK(next && next->textures()[1].rgba[3] == std::byte{255});
    map.size = {2, 3};
    map.samples.resize(6);
    next = lit();
    CHECK(next && next->textures()[1].width == 2 && next->textures()[1].height == 3);
    // Cache eviction never changes snapshots retained by copied/composed batches.
    auto tiny = Bitmap::create(1, 1, 24);
    CHECK(tiny);
    material = {};
    material.texture = tiny.get();
    std::vector<Bitmap> sources(4100, *tiny);
    for (auto& source : sources) {
        material.texture = &source;
        CHECK(prepare(vertices, material));
    }
    bitmap.reset();
    p = {};
    map = {};
    CHECK(first->textures()[1].rgba == original && combined->textures()[3].rgba == original);
}
}

int opent1_test_hardware_batch() {
    return opent1::test::run([] {
        geometryAndLifetime();
        textureFormats();
        malformedAndBudgets();
        mipChainsAndLightMaps();
        alphaControls();
        sharedTexturesAcrossFrames();
        const auto vertices = quad();
        HardwareMaterial material;
        auto first = prepare(vertices, material);
        CHECK(first);
        material.color = {0, 1, 0};
        material.depth = HardwareDepth::Disabled;
        auto second = prepare(vertices, material);
        CHECK(second);
        auto combined =
            HardwareBatch::compose(std::array<const HardwareBatch*, 2>{&*first, &*second});
        CHECK(combined);
        CHECK(combined->draws().size() == 2 && combined->layerEnds().size() == 2);
        CHECK(combined->draws()[1].firstVertex == first->vertices().size());
        CHECK(combined->draws()[1].texture == first->textures().size());
        first.reset();
        second.reset();
        CHECK(combined->draws()[1].color == Point3F(0, 1, 0));
        HardwareBatch invalid;
        CHECK(!HardwareBatch::compose(std::array<const HardwareBatch*, 1>{&invalid}));
        CHECK(!HardwareBatch::compose(std::array<const HardwareBatch*, 1>{nullptr}));
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("graphics", "hardware_batch", opent1_test_hardware_batch);
