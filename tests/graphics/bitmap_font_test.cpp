#include "gfx_test_support.hpp"
#include <opent1/core/file_stream.hpp>
#include <opent1/core/vol_stream.hpp>
#include <filesystem>
#include <random>
using namespace gfx_test;

namespace {
template <class T, class... Args>
void truncation(const std::vector<std::byte>& bytes, Args... args) {
    std::ranges::for_each(std::views::iota(std::size_t{0}, bytes.size()), [&](auto n) {
        CHECK(!decode<T>(std::span(bytes).first(n), args...));
    });
    auto trailing = bytes;
    trailing.push_back(std::byte{});
    CHECK(!decode<T>(trailing, args...));
}

void bitmaps() {
    CHECK(!Bitmap::create(0, 1) && !Bitmap::create(-1, 1) && !Bitmap::create(1, 0) &&
          !Bitmap::create(1, 1, 7));
    CHECK(!Bitmap::create(16384, 16384, 24) && !Bitmap::create(16385, 1));
    std::ranges::for_each(std::views::iota(0U, corpus::BitmapCases), [&](auto test) {
        auto b = bitmap(test);
        const auto bytes = encode(b);
        truncation<Bitmap>(bytes);
        auto noPalette = decode<Bitmap>(bytes, false);
        CHECK(noPalette && !noPalette->palette && noPalette->pixels == b.pixels);
        const auto omitted = encode(b, BitmapFormat::Darkstar, false);
        CHECK(decode<Bitmap>(omitted) && !decode<Bitmap>(omitted)->palette);
        CHECK(b.level(-1).empty() && b.level(b.detailLevels).empty() && b.row(-1).empty() &&
              b.row(b.height).empty());
        CHECK(b.crop({0, 0, 0, 0}) && !b.crop({-1, 0, 0, 0}) &&
              !b.crop({0, 0, b.width, b.height}) && !b.crop({1, 1, 0, 0}));
        auto crop = b.crop({b.width - 1, b.height - 1, b.width - 1, b.height - 1});
        CHECK(crop);
        CHECK(std::ranges::equal(crop->row(0),
                                 b.row(b.height - 1).last(std::size_t(b.bitDepth / 8))));
        const auto before = b.pixels;
        crop->row(0)[0] ^= std::byte{255};
        CHECK(b.pixels == before);
        if (b.bitDepth != 16) {
            const auto bmp = encode(b, BitmapFormat::Microsoft);
            truncation<Bitmap>(bmp);
            // Top-down BMP with an explicit gap before pixels; offsets are relative to the asset, not the parent stream.
            auto topDown = bmp;
            const std::size_t offset = b.bitDepth == 8 ? 1078 : 54;
            auto base = b.crop(b.bounds());
            CHECK(base);
            std::ranges::copy(base->pixels, topDown.begin() + std::ptrdiff_t(offset));
            setWord(topDown, 22, std::bit_cast<UInt32>(-b.height));
            topDown.insert(topDown.begin() + std::ptrdiff_t(offset), 7, std::byte{0xaa});
            setWord(topDown, 2, UInt32(topDown.size()));
            setWord(topDown, 10, UInt32(offset + 7));
            auto read = decode<Bitmap>(topDown);
            CHECK(read && read->pixels == base->pixels);
            for (const auto field : {std::size_t{18},
                                     std::size_t{22},
                                     std::size_t{30},
                                     std::size_t{46},
                                     std::size_t{10}}) {
                auto bad = bmp;
                setWord(bad, field, field == 22 ? 0x80000000U : 0xffffffffU);
                CHECK(!decode<Bitmap>(bad));
            }
        }
    });
    auto b = bitmap(18);
    const auto source = b.pixels;
    b.detailLevels = 10;
    CHECK(!b.valid() && !b.flipVertical() && b.pixels == source);
    b.detailLevels = std::numeric_limits<Int32>::min();
    CHECK(!b.valid() && !b.flipVertical());
    b = bitmap(18);
    b.width = 7;
    CHECK(!b.valid());
    auto bytes = encode(bitmap(1));
    for (const auto field :
         {std::size_t{12}, std::size_t{16}, std::size_t{20}, std::size_t{24}, std::size_t{28}}) {
        auto bad = bytes;
        setWord(bad, field, 0xffffffffU);
        CHECK(!decode<Bitmap>(bad));
    }
    b = bitmap(18);
    b.pixels.push_back(std::byte{0x17});
    CHECK(b.valid() && decode<Bitmap>(encode(b))->pixels == b.pixels);
}

void arraysAndFonts() {
    BitmapArray a;
    a.bitmaps = {bitmap(1), bitmap(18)};
    a.indexRemap = {4, 2000000000};
    a.userDataSize = 2;
    a.userData = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto bytes = encode(a, true);
    truncation<BitmapArray>(bytes, true);
    auto loaded = decode<BitmapArray>(bytes, true);
    CHECK(loaded && loaded->get(4) && loaded->get(2000000000));
    CHECK(!loaded->get(-1) && !loaded->get(0) && !loaded->get(5) && loaded->data(5).empty());
    CHECK(loaded->data(2000000000)[0] == std::byte{3});
    a.indexRemap = {4, 4};
    CHECK(!a.valid());
    a.indexRemap = {5, 4};
    CHECK(!a.valid());
    a.indexRemap.clear();
    a.userDataSize = 3;
    CHECK(!a.valid());
    auto empty = decode<BitmapArray>(encode(BitmapArray{}));
    CHECK(empty && empty->bitmaps.empty() && !empty->get(0));
    std::ranges::for_each(std::views::iota(0U, corpus::FontCases), [&](auto test) {
        auto f = font(test);
        truncation<BitmapFont>(encode(f));
        CHECK(f.glyph(100).sheet == nullptr && f.glyph(99).sheet &&
              f.glyph(0xffffffffU).sheet == nullptr);
        CHECK(f.glyph('A').bounds == RectI(4, 1, 5, 4));
        CHECK(f.textWidth(std::string_view("A\0B", 3)) == f.textWidth("A"));
        CHECK(f.textWidth(std::u16string_view(u"A\0B", 3)) == f.textWidth(u"A"));
        std::array<UInt8, 256> mapping{};
        std::ranges::for_each(std::views::iota(0U, 256U), [&](auto i) {
            mapping[i] = UInt8(255 - i);
        });
        const auto before = f.sheets.bitmaps[0].pixels;
        f.remapColors(mapping);
        CHECK(f.info.backColor == 0 &&
              f.sheets.bitmaps[0].pixels[0] == std::byte(255 - std::to_integer<UInt8>(before[0])));
        f.lookup[0] = 500;
        CHECK(!f.valid());
    });
    auto f = font(0);
    f.characters[1].bitmap = 20;
    CHECK(!f.valid());
    f = font(0);
    f.characters[0].x = 255;
    CHECK(!f.valid());
    f = font(0);
    f.info.scale.x = -1;
    CHECK(!f.valid());
    f = font(0);
    f.info.flags &= ~FontMono;
    CHECK(!f.setForeColor(2) && !f.setBackColor(2));
    auto bad = encode(font(0));
    setWord(bad, 8, 1);
    CHECK(!decode<BitmapFont>(bad));
    bad = encode(font(0));
    setWord(bad, 4, 0xffffffffU);
    CHECK(!decode<BitmapFont>(bad));
    // Bounded mutations exercise hostile lengths, chunk counts, glyph tables and offsets.
    std::mt19937 random(123);
    const std::array sources{encode(bitmap(1)), bytes, encode(font(8))};
    std::ranges::for_each(std::views::iota(0U, 2000U), [&](auto i) {
        auto fuzz = sources[i % 3];
        const auto offset = random() % fuzz.size();
        fuzz[offset] ^= std::byte((random() % 255) + 1);
        if (i % 3 == 0) {
            const auto value = decode<Bitmap>(fuzz);
            CHECK(!value || value->valid());
        } else if (i % 3 == 1) {
            const auto value = decode<BitmapArray>(fuzz, true);
            CHECK(!value || value->valid());
        } else {
            const auto value = decode<BitmapFont>(fuzz);
            CHECK(!value || value->valid());
        }
    });
}

void clipping() {
    const RectI clip{0, 0, 7, 7};
    CHECK(clip.contains({7, 7}) && !clip.contains({8, 7}));
    CHECK(intersect(clip, {7, 7, 9, 9}) == RectI(7, 7, 7, 7) && !intersect(clip, {8, 8, 9, 9}));
    RectI source{-2, 1, 5, 4}, dest{0, -1, 7, 2};
    CHECK(clipBlit(source, clip, dest, clip));
    CHECK(source == RectI(0, 2, 5, 4) && dest == RectI(2, 0, 7, 2));
    const auto before = source;
    CHECK(!clipBlit(source, clip, dest, {99, 99, 100, 100}) && source == before);
    Point2I first{std::numeric_limits<Int32>::min(), std::numeric_limits<Int32>::min()},
        second{std::numeric_limits<Int32>::max(), std::numeric_limits<Int32>::max()};
    CHECK(clipLine(first, second, clip) && clip.contains(first) && clip.contains(second));
    first = {-1, -1};
    second = {-2, -2};
    CHECK(!clipLine(first, second, clip) && first == Point2I(-1, -1));
}

void resources() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("opent1-gfx-asset-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};

    const auto bmp = encode(bitmap(1)), pft = encode(font(0));
    {
        FileWStream stream(root / "asset.pbm");
        CHECK(stream.writeBytes(bmp));
    }
    {
        FileWStream stream(root / "font.pft");
        CHECK(stream.writeBytes(pft));
    }
    Resource<Bitmap> retained;
    {
        ResourceManager manager;
        const std::array paths{root};
        CHECK(manager.setSearchPaths(paths) && registerBitmapFontLoaders(manager));
        CHECK(!registerBitmapFontLoaders(manager));
        retained = manager.load<Bitmap>("ASSET.PBM");
        CHECK(retained && retained->width == 2);
        const auto font = manager.load<BitmapFont>("font.pft");
        CHECK(font && font->glyph('A').sheet);
        CHECK(!manager.load<Bitmap>("font.pft"));
        CHECK(manager.purge(true) == 0);
    }
    CHECK(retained && retained->row(0).size() == 2);
}

void assets(const std::filesystem::path& root) {
    ResourceManager manager;
    const std::array paths{root};
    CHECK(manager.setSearchPaths(paths) && registerBitmapFontLoaders(manager));
    const auto volumes = manager.findMatches("*.vol");
    std::size_t count{};
    std::ranges::for_each(volumes, [&](const auto& name) {
        CHECK(manager.addVolume(name));
    });
    std::ranges::for_each(manager.findMatches("*"), [&](const auto& name) {
        const auto type = resourceType(name);
        bool supported = true, ok = false;
        if (type == resourceType(".bmp") || type == resourceType(".pbm") ||
            type == resourceType(".dib")) {
            const auto b = manager.load<Bitmap>(name);
            ok = b && b->valid();
        } else if (type == resourceType(".pba")) {
            const auto a = manager.load<BitmapArray>(name);
            ok = a && a->valid();
        } else if (type == resourceType(".pft")) {
            const auto f = manager.load<BitmapFont>(name);
            ok = f && f->valid();
        } else {
            supported = false;
        }
        if (supported) {
            if (!ok) {
                std::fprintf(stderr, "Asset failed: %s\n", name.c_str());
            }
            CHECK(ok);
            ++count;
        }
    });
    CHECK(count > 0);
    std::printf("Loaded %zu graphics assets from %zu volumes\n", count, volumes.size());
}
} // namespace

int opent1_test_bitmap_font(int argc, char** argv) {
    return opent1::test::run([&] {
        if (argc == 2) {
            assets(argv[1]);
        } else {
            bitmaps();
            arraysAndFonts();
            clipping();
            resources();
        }
    });
}

namespace {
int opent1_test_bitmap_font_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_bitmap_font(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("graphics", "bitmap_font", opent1_test_bitmap_font_default);
