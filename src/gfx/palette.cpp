#include <opent1/gfx/palette.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace opent1::gfx {
namespace {
bool mapped(PaletteType type) {
    return type == PaletteType::ShadeHaze || type == PaletteType::Translucent ||
           type == PaletteType::Additive || type == PaletteType::Subtractive;
}

UInt64 remapSize(PaletteType type, Int32 shift, Int32 haze) {
    if (type == PaletteType::ShadeHaze) {
        return UInt64{256} * (UInt64{1} << shift) * (UInt64(haze) + 1);
    }
    return mapped(type) ? 65536U : 0U;
}

bool headerValid(const Palette& p) {
    return !p.palettes.empty() && p.palettes.size() <= 16 && p.shadeShift >= 0 &&
           p.shadeShift <= 16 && p.hazeLevels >= 0 && p.hazeLevels <= 65536 && p.hazeColor >= 0 &&
           p.hazeColor < 256;
}

bool colors(StreamIO& s, SubPalette& p) {
    return std::ranges::all_of(p.colors, [&](auto& c) {
        return s.read(c.red) && s.read(c.green) && s.read(c.blue) && s.read(c.flags);
    });
}

bool writeColors(StreamIO& s, const SubPalette& p, std::size_t count = 256) {
    return std::ranges::all_of(std::span(p.colors).first(count), [&](auto c) {
        return s.write(c.red) && s.write(c.green) && s.write(c.blue) && s.write(c.flags);
    });
}

void generateMaps(SubPalette& p, bool alphaFlags) {
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{256}), [&](auto i) {
        p.identity[i] = static_cast<UInt8>(i);
        p.components[0][i] = Flt32(p.colors[i].red) / 255.f;
        p.components[1][i] = Flt32(p.colors[i].green) / 255.f;
        p.components[2][i] = Flt32(p.colors[i].blue) / 255.f;
        p.components[3][i] = alphaFlags ? Flt32(p.colors[i].flags) / 255.f : 1.f;
    });
}

bool readRiff(MemRWStream& s, Palette& p, UInt32 size) {
    UInt32 length{}, form{};
    if (!s.read(length) || length != size - 8 || !s.read(form) ||
        form != makeFourCC('P', 'A', 'L', ' ')) {
        return false;
    }
    bool found = false;
    // RIFF chunk traversal is stateful; bound every chunk before repositioning.
    while (UInt32(s.getPosition()) < size) {
        UInt32 id{}, bytes{};
        if (!s.read(id) || !s.read(bytes)) {
            return false;
        }
        const auto start = UInt32(s.getPosition());
        if (bytes > size - start || (bytes & 1U) > size - start - bytes) {
            return false;
        }
        if (id == makeFourCC('d', 'a', 't', 'a')) {
            UInt16 version{}, count{};
            if (found || bytes < 4 || !s.read(version) || !s.read(count) || version != 0x300 ||
                count == 0 || count > 256 || bytes != 4U + 4U * count) {
                return false;
            }
            auto& sub = p.palettes.front();
            sub.index = 0xffffffffU;
            if (!std::ranges::all_of(std::span(sub.colors).first(count), [&](auto& c) {
                    return s.read(c.red) && s.read(c.green) && s.read(c.blue) && s.read(c.flags);
                })) {
                return false;
            }
            generateMaps(sub, true);
            found = true;
        }
        if (!s.setPosition(static_cast<Int32>(start + bytes + (bytes & 1U)))) {
            return false;
        }
    }
    return found;
}

bool readPl98(MemRWStream& s, Palette& p, UInt32 size) {
    Int32 count{};
    if (!s.read(count) || count < 1 || count > 16 || !s.read(p.shadeShift) ||
        !s.read(p.hazeLevels) || !s.read(p.hazeColor)) {
        return false;
    }
    p.palettes.resize(static_cast<std::size_t>(count));
    if (!headerValid(p) || !std::ranges::all_of(p.allowedMatches, [&](auto& v) {
            return s.read(v);
        })) {
        return false;
    }
    UInt64 total{};
    if (!std::ranges::all_of(p.palettes, [&](auto& sub) {
            UInt32 type{};
            if (!colors(s, sub) || !s.read(sub.index) || !s.read(type) || type > 8) {
                return false;
            }
            sub.type = static_cast<PaletteType>(type);
            const auto bytes = remapSize(sub.type, p.shadeShift, p.hazeLevels);
            total += bytes + (mapped(sub.type) ? 4352U : 0U);
            if (total > size || total > Palette::MaxFileBytes) {
                return false;
            }
            sub.remap.resize(static_cast<std::size_t>(bytes));
            return true;
        })) {
        return false;
    }
    if (total > size - UInt32(s.getPosition())) {
        return false;
    }
    if (!std::ranges::all_of(p.palettes, [&](auto& sub) {
            return s.readBytes(sub.remap);
        })) {
        return false;
    }
    // Legacy writes no auxiliary maps for NoRemap/quantization palettes. Its
    // reader incorrectly reads extra NoRemap bytes before generating them.
    if (!std::ranges::all_of(p.palettes, [&](auto& sub) {
            if (!mapped(sub.type)) {
                generateMaps(sub, false);
                return true;
            }
            return std::ranges::all_of(sub.identity,
                                       [&](auto& v) {
                                           return s.read(v);
                                       }) &&
                   std::ranges::all_of(sub.components, [&](auto& channel) {
                       return std::ranges::all_of(channel, [&](auto& v) {
                           return s.read(v) && std::isfinite(v);
                       });
                   });
        })) {
        return false;
    }
    UInt8 quant{};
    if (!s.read(quant) || quant > 1) {
        return false;
    }
    if (quant) {
        p.weights.emplace();
        if (!std::ranges::all_of(*p.weights,
                                 [&](auto& v) {
                                     return s.read(v) && std::isfinite(v) && v >= 0;
                                 }) ||
            !s.read(p.usedRangeStart) || !s.read(p.usedRangeEnd) ||
            p.usedRangeStart > p.usedRangeEnd || p.usedRangeEnd > 255) {
            return false;
        }
    }
    UInt32 reserved{};
    return s.read(reserved) && reserved == 0 && UInt32(s.getPosition()) == size;
}
}

std::unique_ptr<Palette> Palette::load(StreamIO& stream, UInt32 size) {
    if (size < 12 || size > MaxFileBytes) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!stream.readBytes(bytes)) {
        return {};
    }
    MemRWStream input{std::span<const std::byte>(bytes)};
    UInt32 magic{};
    if (!input.read(magic)) {
        return {};
    }
    auto result = std::make_unique<Palette>();
    const auto ok = magic == makeFourCC('R', 'I', 'F', 'F')
                        ? readRiff(input, *result, size)
                        : magic == makeFourCC('P', 'L', '9', '8') && readPl98(input, *result, size);
    return ok ? std::move(result) : nullptr;
}

bool Palette::valid() const {
    if (!headerValid(*this)) {
        return false;
    }
    UInt64 total = 57U + 1032U * palettes.size();
    const auto good = std::ranges::all_of(palettes, [&](const auto& p) {
        if (UInt32(p.type) > 8 || p.remap.size() != remapSize(p.type, shadeShift, hazeLevels)) {
            return false;
        }
        total += p.remap.size() + (mapped(p.type) ? 4352U : 0U);
        return !mapped(p.type) || std::ranges::all_of(p.components, [](const auto& channel) {
            return std::ranges::all_of(channel, [](auto v) {
                return std::isfinite(v);
            });
        });
    });
    if (weights) {
        total += 1032;
    }
    return good && total <= MaxFileBytes &&
           (!weights || (usedRangeStart <= usedRangeEnd && usedRangeEnd <= 255 &&
                         std::ranges::all_of(*weights, [](auto v) {
                             return std::isfinite(v) && v >= 0;
                         })));
}

bool Palette::write(StreamIO& s, bool microsoft) const {
    if (!valid()) {
        return false;
    }
    if (microsoft) {
        return s.write(makeFourCC('R', 'I', 'F', 'F')) && s.write(UInt32{1040}) &&
               s.write(makeFourCC('P', 'A', 'L', ' ')) && s.write(makeFourCC('d', 'a', 't', 'a')) &&
               s.write(UInt32{1028}) && s.write(UInt16{0x300}) && s.write(UInt16{256}) &&
               writeColors(s, palettes.front());
    }
    if (!s.write(makeFourCC('P', 'L', '9', '8')) || !s.write(static_cast<Int32>(palettes.size())) ||
        !s.write(shadeShift) || !s.write(hazeLevels) || !s.write(hazeColor) ||
        !std::ranges::all_of(allowedMatches,
                             [&](auto v) {
                                 return s.write(v);
                             }) ||
        !std::ranges::all_of(palettes,
                             [&](const auto& p) {
                                 return writeColors(s, p) && s.write(p.index) &&
                                        s.write(UInt32(p.type));
                             }) ||
        !std::ranges::all_of(palettes,
                             [&](const auto& p) {
                                 return s.writeBytes(p.remap);
                             }) ||
        !std::ranges::all_of(palettes,
                             [&](const auto& p) {
                                 return !mapped(p.type) ||
                                        (std::ranges::all_of(p.identity,
                                                             [&](auto v) {
                                                                 return s.write(v);
                                                             }) &&
                                         std::ranges::all_of(
                                             p.components, [&](const auto& channel) {
                                                 return std::ranges::all_of(channel, [&](auto v) {
                                                     return s.write(v);
                                                 });
                                             }));
                             }) ||
        !s.write(bool(weights))) {
        return false;
    }
    if (weights && (!std::ranges::all_of(*weights,
                                         [&](auto v) {
                                             return s.write(v);
                                         }) ||
                    !s.write(usedRangeStart) || !s.write(usedRangeEnd))) {
        return false;
    }
    return s.write(UInt32{0});
}

bool Palette::buildRemapTables(const PaletteBuildOptions& options) {
    const auto unit = [](float v) {
        return std::isfinite(v) && v >= 0 && v <= 1;
    };
    if (palettes.empty() || palettes.size() > 16 || options.shadeShift < 0 ||
        options.shadeShift > 16 || options.hazeLevels < 1 || !unit(options.shadePercentage) ||
        !unit(options.hazePercentage) || !std::ranges::all_of(options.shadeColor, unit) ||
        !std::ranges::all_of(options.hazeColor, unit) ||
        std::ranges::none_of(
            allowedMatches,
            [](auto value) {
                return value != 0;
            })) {
        return false;
    }
    try {
        Palette next = *this;
        next.shadeShift = options.shadeShift;
        next.hazeLevels = options.hazeLevels;
        if (next.palettes[0].type == PaletteType::NoRemap) {
            next.palettes[0].type = PaletteType::ShadeHaze;
        }
        UInt64 total = 57 + 1032 * next.palettes.size() + (next.weights ? 1032 : 0);
        for (auto& sub : next.palettes) {
            switch (sub.type) {
                case PaletteType::ColorQuant:
                    sub.type = PaletteType::ShadeHaze;
                    break;
                case PaletteType::AlphaQuant:
                    sub.type = PaletteType::Translucent;
                    break;
                case PaletteType::AdditiveQuant:
                    sub.type = PaletteType::Additive;
                    break;
                case PaletteType::SubtractiveQuant:
                    sub.type = PaletteType::Subtractive;
                    break;
                default:
                    break;
            }
            total += remapSize(sub.type, next.shadeShift, next.hazeLevels) +
                     (mapped(sub.type) ? 4352U : 0U);
            if (total > MaxFileBytes) {
                return false;
            }
        }
        std::unordered_map<UInt32, UInt8> matches;
        const auto match = [&](PaletteColor color) {
            const UInt32 key = (UInt32(color.red) << 16) | (UInt32(color.green) << 8) | color.blue;
            if (!key) {
                return UInt8{0};
            }
            if (const auto found = matches.find(key); found != matches.end()) {
                return found->second;
            }
            Int32 best = std::numeric_limits<Int32>::max();
            UInt8 result{};
            for (UInt32 i = 0; i < 256; ++i) {
                if (!(next.allowedMatches[i / 32] & (0x80000000U >> (i % 32)))) {
                    continue;
                }
                const auto& c = next.palettes[0].colors[i];
                const Int32 r = Int32(color.red) - c.red, g = Int32(color.green) - c.green,
                            b = Int32(color.blue) - c.blue;
                const auto distance = r * r + g * g + b * b;
                if (distance < best) {
                    best = distance;
                    result = static_cast<UInt8>(i);
                }
            }
            matches.emplace(key, result);
            return result;
        };
        const UInt32 shades = 1U << next.shadeShift;
        const auto interpolate =
            [](std::array<float, 3> color, const std::array<float, 3>& target, float amount) {
                const float inverse = 1.f - amount;
                for (std::size_t i = 0; i < 3; ++i) {
                    color[i] = color[i] * inverse + target[i] * amount;
                }
                return color;
            };
        const auto asColor = [](const std::array<float, 3>& c) {
            return PaletteColor{
                UInt8(c[0] * 255.f + .5f), UInt8(c[1] * 255.f + .5f), UInt8(c[2] * 255.f + .5f), 0};
        };
        for (auto& sub : next.palettes) {
            sub.remap.resize(
                static_cast<std::size_t>(remapSize(sub.type, next.shadeShift, next.hazeLevels)));
            if (!mapped(sub.type)) {
                generateMaps(sub, false);
                continue;
            }
            if (sub.type == PaletteType::ShadeHaze) {
                for (UInt32 h = 0; h < UInt32(next.hazeLevels); ++h) {
                    for (UInt32 s = 0; s < shades; ++s) {
                        for (UInt32 i = 0; i < 256; ++i) {
                            const auto& c = sub.colors[i];
                            std::array<float, 3> color{float(c.red) / 255.f,
                                                       float(c.green) / 255.f,
                                                       float(c.blue) / 255.f};
                            float amount = options.shadePercentage;
                            if (shades > 1) {
                                amount *= float(s) / float(shades - 1);
                            }
                            color = interpolate(color, options.shadeColor, 1.f - amount);
                            if (h) {
                                amount = options.hazePercentage;
                                if (next.hazeLevels > 1) {
                                    amount *= float(h) / float(next.hazeLevels - 1);
                                }
                                color = interpolate(color, options.hazeColor, amount);
                            }
                            sub.remap[256 * (std::size_t(h) * shades + s) + i] =
                                std::byte(match(asColor(color)));
                        }
                    }
                }
                const auto hazeStart = 256 * std::size_t(shades) * next.hazeLevels;
                for (UInt32 h = 0; h < shades; ++h) {
                    for (UInt32 i = 0; i < 256; ++i) {
                        const auto& c = sub.colors[i];
                        std::array<float, 3> color{
                            float(c.red) / 255.f, float(c.green) / 255.f, float(c.blue) / 255.f};
                        float amount = options.hazePercentage;
                        if (shades > 1) {
                            amount *= float(h) / float(shades - 1);
                        }
                        sub.remap[hazeStart + 256 * std::size_t(h) + i] = std::byte(
                            match(asColor(interpolate(color, options.hazeColor, amount))));
                    }
                }
            } else {
                for (UInt32 dst = 0; dst < 256; ++dst) {
                    sub.remap[dst] = std::byte(dst);
                }
                for (UInt32 src = 1; src < 256; ++src) {
                    for (UInt32 dst = 0; dst < 256; ++dst) {
                        const auto& s = sub.colors[src];
                        const auto& d = next.palettes[0].colors[dst];
                        const auto blend = [&](UInt8 source, UInt8 destination) {
                            if (sub.type == PaletteType::Translucent) {
                                const float alpha = float(s.flags) / 255.f;
                                return UInt8(float(source) * alpha +
                                             float(destination) * (1.f - alpha));
                            }
                            if (sub.type == PaletteType::Additive) {
                                return UInt8(std::min(255.f, float(source) + float(destination)));
                            }
                            return UInt8(float(destination) * ((255.f - float(source)) / 255.f));
                        };
                        sub.remap[256 * src + dst] = std::byte(match({blend(s.red, d.red),
                                                                      blend(s.green, d.green),
                                                                      blend(s.blue, d.blue),
                                                                      0}));
                    }
                }
            }
            for (UInt32 i = 0; i < 256; ++i) {
                sub.identity[i] = &sub == &next.palettes[0] ? UInt8(i) : match(sub.colors[i]);
                sub.components[0][i] = float(sub.colors[i].red) / 255.f;
                sub.components[1][i] = float(sub.colors[i].green) / 255.f;
                sub.components[2][i] = float(sub.colors[i].blue) / 255.f;
                sub.components[3][i] = sub.type == PaletteType::Translucent
                                           ? float(sub.colors[i].flags) / 255.f
                                           : (i == 0 ? 0.f : 1.f);
            }
        }
        if (!next.valid()) {
            return false;
        }
        *this = std::move(next);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

const SubPalette& Palette::find(UInt32 index) const {
    if (palettes.empty()) {
        throw std::logic_error("empty palette");
    }
    const auto it = std::ranges::find(palettes, index, &SubPalette::index);
    return index == 0xffffffffU || it == palettes.end() ? palettes.front() : *it;
}

UInt8 Palette::nearest(PaletteColor color) const {
    const auto& entries = find().colors;
    Int32 bestError = std::numeric_limits<Int32>::max();
    UInt8 result = 255;
    const auto error = [&](auto i) {
        const auto& c = entries[i];
        const Int32 r = Int32(color.red) - c.red, g = Int32(color.green) - c.green,
                    b = Int32(color.blue) - c.blue;
        return r * r + g * g + b * b;
    };
    std::ranges::for_each(std::views::iota(UInt32{0}, UInt32{256}), [&](auto i) {
        if ((allowedMatches[i / 32] & (UInt32{0x80000000} >> (i % 32))) && error(i) < bestError) {
            bestError = error(i);
            result = static_cast<UInt8>(i);
        }
    });
    if (error(0) < bestError) {
        result = 0;
    }
    return result;
}

std::optional<UInt8> Palette::mapColor(UInt8 color, UInt32 shade, UInt32 haze, UInt32 index) const {
    if (shadeShift < 0 || shadeShift > 16 || hazeLevels <= 0 || haze >= UInt32(hazeLevels) ||
        shade >= (UInt32{1} << shadeShift)) {
        return {};
    }
    const auto& p = find(index);
    const UInt64 offset = (UInt64{256} * shade + color) + (UInt64{256} << shadeShift) * haze;
    if (p.type != PaletteType::ShadeHaze || offset >= p.remap.size()) {
        return {};
    }
    return std::to_integer<UInt8>(p.remap[static_cast<std::size_t>(offset)]);
}

bool registerPaletteLoaders(ResourceManager& resources) {
    const auto load = [](StreamIO& s, UInt32 size) {
        return Palette::load(s, size);
    };
    const bool pal = resources.registerLoader<Palette>(".pal", load),
               ppl = resources.registerLoader<Palette>(".ppl", load),
               dpl = resources.registerLoader<Palette>(".dpl", load);
    if (pal)
        resources.setSourceOnlyLoader(".pal");
    if (ppl)
        resources.setSourceOnlyLoader(".ppl");
    if (dpl)
        resources.setSourceOnlyLoader(".dpl");
    return pal && ppl && dpl;
}
} // namespace opent1::gfx
