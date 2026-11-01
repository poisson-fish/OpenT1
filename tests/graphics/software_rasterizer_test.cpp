#include "gfx_test_support.hpp"
#include "software_corpus.hpp"
#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/core/file_stream.hpp>
using namespace gfx_test;

namespace {
void parity() {
    namespace c = software_corpus;
    FileRStream fixture(OPENT1_SOFTWARE_FIXTURE);
    CHECK(fixture() == STRM_OK);
    const auto word = [&] {
        UInt32 value{};
        CHECK(fixture.read(value));
        return value;
    };
    const auto record = [&] {
        std::vector<std::byte> bytes(word());
        CHECK(bytes.size() == c::Stride * c::Height);
        CHECK(fixture.readBytes(bytes));
        return bytes;
    };
    CHECK(word() == 0x53574631 && word() == 1);
    auto surface = SoftwareSurface::create(c::Width, c::Height);
    CHECK(surface);
    surface->clear(37);
    CHECK(std::ranges::equal(surface->pixels(), record()));
    surface->setClip({1, 1, c::Width - 2, c::Height - 2});
    std::ranges::for_each(std::views::iota(0U, c::LineCases), [&](auto test) {
        const auto points = c::line(test);
        surface->clear(19);
        surface->line({points[0], points[1]}, {points[2], points[3]}, 87);
        const auto expected = record();
        if (!std::ranges::equal(surface->pixels(), expected)) {
            std::fprintf(stderr,
                         "Line %u: %d,%d -> %d,%d\n",
                         test,
                         points[0],
                         points[1],
                         points[2],
                         points[3]);
        }
        CHECK(std::ranges::equal(surface->pixels(), expected));
    });
    Palette p;
    p.palettes.resize(2);
    p.palettes[0].index = 1;
    p.palettes[1].index = 2;
    p.palettes[1].type = PaletteType::Translucent;
    p.palettes[1].remap.resize(65536);
    std::ranges::for_each(std::views::iota(0U, 256U), [&](auto i) {
        p.palettes[0].identity[i] = c::identity(i);
    });
    std::ranges::for_each(std::views::iota(0U, 65536U), [&](auto i) {
        p.palettes[1].remap[i] = std::byte(c::blend(i / 256, i % 256));
    });
    CHECK(surface->setPalette(p));
    auto source = Bitmap::create(11, 9);
    CHECK(source);
    std::ranges::for_each(std::views::iota(std::size_t{0}, source->pixels.size()), [&](auto i) {
        source->pixels[i] = std::byte(c::pixel(unsigned(i)));
    });
    std::ranges::for_each(std::views::iota(0U, c::BlitCases), [&](auto test) {
        surface->clear(19);
        source->attributes = c::flags(test);
        source->paletteIndex = test % 4 == 2 ? 1 : test % 4 == 3 ? 2 : 0xffffffffU;
        const auto at = c::at(test);
        const RectI region = test & 16 ? RectI{2, 1, 8, 6} : source->bounds();
        CHECK(surface->blit(*source, region, {at[0], at[1]}, Flip((test / 4) % 4)));
        const auto expected = record();
        if (!std::ranges::equal(surface->pixels(), expected)) {
            const auto mismatch = std::ranges::mismatch(surface->pixels(), expected);
            std::fprintf(stderr,
                         "Blit %u differs at %zu (actual %u, expected %u)\n",
                         test,
                         std::size_t(mismatch.in1 - surface->pixels().begin()),
                         std::to_integer<unsigned>(*mismatch.in1),
                         std::to_integer<unsigned>(*mismatch.in2));
        }
        CHECK(std::ranges::equal(surface->pixels(), expected));
    });
    UInt8 end{};
    CHECK(!fixture.read(end));
}

void drawing() {
    auto surface = SoftwareSurface::create(9, 7);
    CHECK(surface && surface->bitmap().stride() == 12);
    surface->clear(11);
    surface->setClip({1, 1, 7, 5});
    surface->fill({-2, -2, 4, 3}, 17);
    CHECK(surface->bitmap().row(0)[0] == std::byte{11} &&
          surface->bitmap().row(1)[1] == std::byte{17});
    CHECK(surface->pixels()[9] == std::byte{11});
    surface->point({0, 0}, 2);
    CHECK(surface->pixels()[0] == std::byte{11});
    surface->rectangle({0, 0, 5, 5}, 30);
    CHECK(surface->bitmap().row(5)[1] == std::byte{30});
    surface->setClip({99, 99, 100, 100});
    const auto before = surface->bitmap().pixels;
    surface->fill({0, 0, 8, 6}, 0);
    CHECK(surface->bitmap().pixels == before);
    surface->resetClip();
    auto source = Bitmap::create(4, 4);
    CHECK(source);
    source->attributes = BitmapTranslucent;
    CHECK(!surface->blit(*source, {0, 0}) && surface->bitmap().pixels == before);
    source->attributes = BitmapTransparent;
    source->row(0)[1] = std::byte{42};
    CHECK(surface->blit(*source, {0, 0}));
    CHECK(surface->bitmap().row(0)[0] == std::byte{11} &&
          surface->bitmap().row(0)[1] == std::byte{42});
    CHECK(!surface->blit(*source, {0, 0}, Flip(9)));
    CHECK(!surface->blit(*Bitmap::create(2, 2, 24), {0, 0}));
    CHECK(!surface->blit(*source, {std::numeric_limits<Int32>::max(), 0}));
    const auto snapshot = surface->bitmap();
    CHECK(surface->blit(surface->bitmap(), {1, 1}, Flip::Both));
    CHECK(surface->bitmap().row(1)[1] ==
          snapshot.row(6)[8]); // independent snapshot despite overlap
    auto p = palette(0);
    CHECK(surface->setPalette(p));
    p.palettes[0].colors[0].red = 123;
    surface->clear(0);
    std::vector<std::byte> rgba(9 * 7 * 4);
    CHECK(surface->rgba(rgba) && rgba[0] == std::byte{0} && rgba[3] == std::byte{255});
    CHECK(!surface->rgba(std::span(rgba).first(4)) && !surface->rgba(rgba, 0) &&
          !surface->rgba(rgba, std::numeric_limits<Flt32>::quiet_NaN()));
    surface->clear(128);
    CHECK(surface->rgba(rgba, 2));
    CHECK(std::to_integer<UInt8>(rgba[0]) >= 180 && std::to_integer<UInt8>(rgba[0]) <= 181);
    surface->clearPalette();
    CHECK(!surface->rgba(rgba));
    auto fontSurface = SoftwareSurface::create(32, 20);
    auto f = font(2);
    f.info.scale = {65536, 65536};
    CHECK(fontSurface && fontSurface->text(f, {0, 0}, "ABC"));
    const auto ascii = fontSurface->bitmap().pixels;
    fontSurface->clear(0);
    CHECK(fontSurface->text(f, {0, 0}, u"ABC") && fontSurface->bitmap().pixels == ascii);
    fontSurface->clear(0);
    CHECK(fontSurface->text(f, {0, 0}, "?") &&
          std::ranges::find(fontSurface->pixels(), std::byte{253}) != fontSurface->pixels().end());
    f.info.scale.x = 32768;
    CHECK(fontSurface->text(f, {0, 0}, "A"));
}
} // namespace

void transferFramebuffer() {
    auto surface = SoftwareSurface::create(9, 7);
    CHECK(surface);
    surface->clear(42);
    const auto* pixels = surface->pixels().data();
    const auto size = surface->pixels().size();
    auto bitmap = std::move(*surface).releaseBitmap();
    surface.reset();
    CHECK(bitmap.valid() && bitmap.width == 9 && bitmap.height == 7);
    CHECK(bitmap.pixels.data() == pixels && bitmap.pixels.size() == size);
    CHECK(std::ranges::all_of(bitmap.pixels, [](auto pixel) {
        return pixel == std::byte{42};
    }));
}

int opent1_test_software_rasterizer() {
    return opent1::test::run([] {
        parity();
        drawing();
        transferFramebuffer();
        auto surface = SoftwareSurface::create(4, 4);
        auto original = palette(0);
        original.palettes[0].colors[1].red = 12;
        CHECK(surface && surface->setPalette(original));
        const auto copy = *surface;
        original.palettes[0].colors[1].red = 99;
        CHECK(surface->setPalette(original));
        CHECK(copy.palette()->palettes[0].colors[1].red == 12);
        CHECK(surface->palette()->palettes[0].colors[1].red == 99);
        auto shared = std::make_shared<const Palette>(original);
        CHECK(surface->setSharedPalette(shared) && surface->palette() == shared.get());
        CHECK(!surface->setSharedPalette({}) && surface->palette() == shared.get());
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("graphics", "software_rasterizer", opent1_test_software_rasterizer);
