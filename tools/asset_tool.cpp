#include <opent1/core/file_stream.hpp>
#include <ranges>
#include <opent1/core/vol_stream.hpp>
#include <opent1/gfx/palette.hpp>
#include <opent1/world/ts_shape.hpp>
#include <opent1/world/interior_authoring.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace opent1;

namespace {
constexpr std::string_view help = R"(OpenT1 asset authoring
  palette INPUT|- OUTPUT [--set INDEX R G B FLAGS] [--matches FIRST LAST]
          [--tables SHADE_SHIFT HAZE_LEVELS] [--shade R G B AMOUNT] [--haze R G B AMOUNT]
  palette-info INPUT
  volume-create OUTPUT
  volume-list INPUT
  volume-put ARCHIVE INPUT ENTRY [none|rle|lzh] [ID]
  volume-remove ARCHIVE ENTRY
  volume-extract ARCHIVE ENTRY OUTPUT
  dml-make OUTPUT BITMAP...
  dts-convert INPUT OUTPUT
  dts-import TARGET SOURCE SEQUENCE OUTPUT
  interior-rewrite INPUT OUTPUT

Palette RGB editing uses 0..255 bytes; shade/haze colors and amounts use 0..1.
Use - for a new grayscale palette. Existing remap tables require --tables when
editing colors or matches. Output replacement happens only after a valid write.
)";

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <class T> T number(std::string_view text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
            "Invalid numeric argument");
    return result;
}

UInt8 byte(std::string_view text) {
    const auto value = number<unsigned>(text);
    require(value < 256, "Color/index must be in 0..255");
    return UInt8(value);
}

using Args = std::span<const std::string_view>;

template <class T> auto load(const std::filesystem::path& path) {
    FileRStream stream(path);
    require(stream() == STRM_OK, "Cannot open input");
    auto value = T::load(stream, stream.getSize());
    require(bool(value), "Invalid or unsupported input asset");
    return value;
}

void save(const std::filesystem::path& path, const std::function<bool(StreamIO&)>& writer) {
    static unsigned serial{};
    auto temporary = path;
    temporary += L".opent1-saving-" +
                 std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 L"-" + std::to_wstring(serial++);
    try {
        FileWStream stream(temporary);
        require(stream() == STRM_OK, "Cannot create output beside destination");
        require(writer(stream) && stream.flush(),
                "Asset serialization failed; destination preserved");
        stream.close();
#if defined(_WIN32)
        require(MoveFileExW(temporary.c_str(),
                            path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "Cannot replace destination");
#else
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        require(!ec, "Cannot replace destination");
#endif
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        throw;
    }
}

void palette(Args args) {
    require(args.size() >= 3, "palette requires input and output");
    auto p = args[1] == "-" ? std::make_unique<gfx::Palette>()
                            : load<gfx::Palette>(std::filesystem::path(args[1]));
    if (args[1] == "-") {
        for (auto i : std::views::iota(unsigned{0}, unsigned{256})) {
            p->palettes[0].colors[i] = {UInt8(i), UInt8(i), UInt8(i), 0};
        }
    }
    gfx::PaletteBuildOptions options;
    bool build = false, edit = false;
    for (std::size_t at = 3; at < args.size();) {
        const auto option = args[at++];
        const auto take = [&]() {
            require(at < args.size(), "Missing palette option argument");
            return args[at++];
        };
        if (option == "--set") {
            const auto index = byte(take());
            const auto r = byte(take()), g = byte(take()), b = byte(take()), flags = byte(take());
            p->palettes[0].colors[index] = {r, g, b, flags};
            edit = true;
        } else if (option == "--matches") {
            const auto first = byte(take()), last = byte(take());
            require(first <= last, "Invalid matching range");
            p->allowedMatches.fill(0);
            for (auto i : std::views::iota(static_cast<unsigned>(first)) |
                              std::views::take_while([&](unsigned i) {
                                  return i <= last;
                              })) {
                p->allowedMatches[i / 32] |= 0x80000000U >> (i % 32);
            }
            edit = true;
        } else if (option == "--tables") {
            options.shadeShift = number<Int32>(take());
            options.hazeLevels = number<Int32>(take());
            build = true;
        } else if (option == "--shade" || option == "--haze") {
            auto& color = option == "--shade" ? options.shadeColor : options.hazeColor;
            for (auto& component : color) {
                component = number<float>(take());
            }
            (option == "--shade" ? options.shadePercentage : options.hazePercentage) =
                number<float>(take());
            build = true;
        } else {
            throw std::runtime_error("Unknown palette option");
        }
    }
    require(!edit || build || p->palettes[0].type == gfx::PaletteType::NoRemap,
            "Editing mapped colors requires --tables");
    if (build) {
        require(p->buildRemapTables(options), "Palette table generation failed");
    }
    save(std::filesystem::path(args[2]), [&](StreamIO& out) {
        return p->write(out);
    });
}

void run(Args args) {
    require(!args.empty(), "Missing command; use --help");
    const auto command = args[0];
    if (command == "--help" || command == "help") {
        std::cout << help;
        return;
    }
    if (command == "palette") {
        palette(args);
        return;
    }
    if (command == "palette-info") {
        require(args.size() == 2, "palette-info requires one input");
        const auto p = load<gfx::Palette>(std::filesystem::path(args[1]));
        std::cout << "palettes " << p->palettes.size() << " shadeShift " << p->shadeShift
                  << " hazeLevels " << p->hazeLevels << '\n';
        for (auto sub : std::views::iota(std::size_t{0}, p->palettes.size())) {
            const auto& s = p->palettes[sub];
            std::cout << "palette " << sub << " id " << s.index << " type " << UInt32(s.type)
                      << " remapBytes " << s.remap.size() << '\n';
            for (auto i : std::views::iota(unsigned{0}, unsigned{256})) {
                const auto c = s.colors[i];
                std::cout << i << ' ' << unsigned(c.red) << ' ' << unsigned(c.green) << ' '
                          << unsigned(c.blue) << ' ' << unsigned(c.flags) << '\n';
            }
        }
        return;
    }
    if (command == "volume-create") {
        require(args.size() == 2, "volume-create requires output");
        VolumeRWStream volume;
        require(volume.save(std::filesystem::path(args[1])), "Cannot create volume");
        return;
    }
    if (command == "volume-list" || command == "volume-extract") {
        require(args.size() == (command == "volume-list" ? 2U : 4U),
                "Invalid volume argument count");
        VolumeRStream volume;
        require(volume.openVolume(std::filesystem::path(args[1])), "Cannot read volume");
        if (command == "volume-list") {
            for (const auto& e : volume.getVolumeItems()) {
                std::cout << e.id << '\t' << e.size << '\t' << unsigned(e.compression) << '\t'
                          << e.name.value_or("") << '\n';
            }
        } else {
            require(volume.open(args[2]), "Volume entry missing or invalid");
            save(std::filesystem::path(args[3]), [&](StreamIO& out) {
                return out.writeBytes(volume.data());
            });
        }
        return;
    }
    if (command == "volume-put" || command == "volume-remove") {
        require(command == "volume-remove" ? args.size() == 3
                                           : args.size() >= 4 && args.size() <= 6,
                "Invalid volume edit arguments");
        VolumeRWStream volume;
        require(volume.openVolume(std::filesystem::path(args[1])),
                "Cannot open volume for editing");
        if (command == "volume-remove") {
            require(volume.remove(args[2]), "Volume entry does not exist");
        } else {
            FileRStream input{std::filesystem::path(args[2])};
            require(input() == STRM_OK && input.getSize() <= VolumeRWStream::MaxArchiveBytes,
                    "Cannot read volume input");
            std::vector<std::byte> bytes(input.getSize());
            require(input.readBytes(bytes), "Cannot read complete input");
            auto mode = STRM_COMPRESS_NONE;
            if (args.size() > 4) {
                if (args[4] == "rle") {
                    mode = STRM_COMPRESS_RLE;
                } else if (args[4] == "lzh") {
                    mode = STRM_COMPRESS_LZH;
                } else {
                    require(args[4] == "none", "Unknown compression");
                }
            }
            require(volume.put(args[3], bytes, mode, args.size() > 5 ? number<UInt32>(args[5]) : 0),
                    "Cannot store volume entry");
        }
        require(volume.save(std::filesystem::path(args[1])), "Cannot publish volume edit");
        return;
    }
    if (command == "dml-make") {
        require(args.size() > 2, "dml-make requires output and bitmap inputs");
        world::TsMaterialList list;
        list.details = 1;
        list.materialsPerDetail = Int32(args.size() - 2);
        for (const auto filename : args.subspan(2)) {
            FileRStream input{std::filesystem::path(filename)};
            require(input() == STRM_OK, "Cannot open bitmap input");
            world::TsMaterial material;
            material.flags = 0x103;
            material.alpha = 0;
            material.mapFile = std::filesystem::path(filename).filename().string();
            list.materials.push_back(std::move(material));
        }
        save(std::filesystem::path(args[1]), [&](StreamIO& out) {
            return list.write(out);
        });
        return;
    }
    if (command == "dts-convert" || command == "dts-import") {
        require(args.size() == (command == "dts-convert" ? 3U : 5U), "Invalid DTS arguments");
        auto shape = load<world::TsShape>(std::filesystem::path(args[1]));
        if (command == "dts-import") {
            auto source = load<world::TsShape>(std::filesystem::path(args[2]));
            require(shape->importSequence(*source, number<Int32>(args[3])),
                    "Sequence import failed");
        }
        save(std::filesystem::path(args.back()), [&](StreamIO& out) {
            return shape->write(out);
        });
        return;
    }
    if (command == "interior-rewrite") {
        require(args.size() == 3, "interior-rewrite requires input and output");
        const auto path = std::filesystem::path(args[1]);
        auto extension = path.extension().string();
        for (char& c : extension) {
            if (c >= 'A' && c <= 'Z') {
                c = char(c - 'A' + 'a');
            }
        }
        std::optional<std::vector<std::byte>> bytes;
        if (extension == ".dig") {
            bytes = world::writeInteriorGeometry(*load<world::InteriorGeometry>(path));
        } else if (extension == ".dis") {
            bytes = world::writeInteriorShape(*load<world::InteriorShape>(path));
        } else if (extension == ".dil") {
            bytes = world::writeInteriorLighting(*load<world::InteriorLighting>(path));
        } else {
            throw std::runtime_error("Interior input must be DIG, DIS or DIL");
        }
        require(bool(bytes), "Interior serialization failed");
        save(std::filesystem::path(args[2]), [&](StreamIO& out) {
            return out.writeBytes(*bytes);
        });
        return;
    }
    throw std::runtime_error("Unknown command; use --help");
}
}

int main(int argc, char** argv) {
    try {
        std::vector<std::string_view> args;
        for (auto i : std::views::iota(1) | std::views::take_while([&](int i) {
                          return i < argc;
                      })) {
            args.emplace_back(argv[i]);
        }
        run(args);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OpenT1 asset tool: " << error.what() << '\n';
        return 1;
    }
}
