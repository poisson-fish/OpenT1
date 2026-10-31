// Interactive cube / loaded TS comparison using the software and SDL_GPU paths.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <opent1/gfx/sdl_gpu_surface.hpp>
#include <opent1/world/ts_static_shape.hpp>
#include <opent1/world/ts_shape_instance.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <opent1/sim/sim_software_environment.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <unordered_map>

namespace {
using namespace opent1;
using namespace opent1::gfx;
using namespace opent1::platform;

void require(bool ok, std::string_view message) {
    if (!ok) {
        throw std::runtime_error(std::string(message));
    }
}

Point3F add(Point3F a, Point3F b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Point3F sub(Point3F a, Point3F b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Point3F mul(Point3F a, Flt32 b) {
    return {a.x * b, a.y * b, a.z * b};
}

Flt32 dot(Point3F a, Point3F b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Point3F cross(Point3F a, Point3F b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Point3F normal(Point3F a) {
    return mul(a, 1 / std::sqrt(dot(a, a)));
}

Point3F rotate(Point3F a, Flt32 angle) {
    const auto c = std::cos(angle), s = std::sin(angle);
    return {c * a.x + s * a.z, a.y, -s * a.x + c * a.z};
}

struct ViewVertex {
    Point3F position, color;
    Point2F uv;
    Flt32 haze{};
};

ViewVertex lerp(const ViewVertex& a, const ViewVertex& b, Flt32 t) {
    return {add(a.position, mul(sub(b.position, a.position), t)),
            add(a.color, mul(sub(b.color, a.color), t)),
            {a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t},
            a.haze + (b.haze - a.haze) * t};
}

struct Triangle {
    std::array<PolygonVertex, 3> vertices;
    HardwareMaterial material;
    std::optional<PolygonMaterial> softwareMaterial;
};
enum class RenderMode : UInt8 { Hardware, Software, Split };

// A small original 5x7 ASCII atlas keeps the viewer independent of game fonts.
struct Glyph {
    char code;
    std::array<UInt8, 7> rows;
};

constexpr Glyph glyphs[]{{'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
                         {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
                         {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
                         {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
                         {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 2, 18, 12}},
                         {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
                         {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
                         {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
                         {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
                         {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
                         {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
                         {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
                         {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
                         {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
                         {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
                         {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
                         {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
                         {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
                         {':', {0, 4, 4, 0, 4, 4, 0}},        {'/', {1, 2, 2, 4, 8, 8, 16}},
                         {'-', {0, 0, 0, 31, 0, 0, 0}},       {'.', {0, 0, 0, 0, 0, 12, 12}}};

class RendererDemo : public SdlApplication {
public:
    RendererDemo() : SdlApplication(LoopConfig{false, 100, 60, 4096}) {}

private:
    std::unique_ptr<SdlGpuSurface> surface_;
    std::unique_ptr<Bitmap> cube_, floor_, shadow_, font_;
    std::unique_ptr<Bitmap> softwareCube_, softwareFloor_, softwareShadow_, softwareImage_;
    std::unique_ptr<SoftwareSurface> software_;
    Palette softwarePalette_;
    std::optional<world::TsShapeResources> model_;
    Resource<Palette> modelPalette_;
    std::optional<world::TsStaticShapeFrame> modelFrame_;
    std::optional<world::TsShapePose> modelPose_;
    std::optional<world::TsShapeInstance> modelInstance_;
    std::string modelName_, skin_, sequenceName_, transitionName_;
    Int32 sequence_{-1};
    Flt64 animationTime_{}, initialAnimationTime_{};
    bool animate_{}, automaticDetail_{}, animationSweep_{}, smokeAnimation_{},
        smokeAutomaticDetail_{};
    Int32 smokeSequence_{};
    bool rootMotion_{}, reverse_{};
    bool modelLighting_{true};
    bool smokeModelLighting_{};
    std::vector<std::string> utilityPrefixes_;
    bool smokeRootMotion_{}, smokeReverse_{};
    UInt64 triggerCount_{};
    std::vector<std::filesystem::path> assetRoots_;
    mutable std::unordered_map<UInt32, UInt8> modelColorCache_;
    bool drawModel_{true};
    RenderMode mode_{RenderMode::Split};
    RenderMode smokeMode_{RenderMode::Split};
    std::vector<Triangle> triangles_;
    WindowId window_{};
    Flt32 yaw_{.65f}, pitch_{.35f}, distance_{7.5f}, angle_{.35f};
    bool paused_{}, smoke_{}, shadows_{true}, sweep_{};
    UInt64 lastTime_{};
    unsigned frames_{};
    std::string snapshot_;
    std::vector<std::byte> firstFrame_;
    std::vector<std::byte> firstShadow_;
    Point3F light_{-4, 7, 5};
    Point3F eye_, right_, up_, forward_;
    Flt32 width_{}, height_{}, focal_{};

    UInt8 nearest(Point3F rgb) const {
        if (model_) {
            const auto byte = [](Flt32 v) {
                return UInt8(std::clamp(v, 0.f, 1.f) * 255);
            };
            const PaletteColor color{byte(rgb.x), byte(rgb.y), byte(rgb.z), 0};
            const auto key = UInt32(color.red) << 16 | UInt32(color.green) << 8 | color.blue;
            const auto found = modelColorCache_.find(key);
            if (found != modelColorCache_.end()) {
                return found->second;
            }
            const auto result = softwarePalette_.nearest(color);
            modelColorCache_.emplace(key, result);
            return result;
        }
        // The demo palette consists of three RGB ramps. Project onto each
        // ramp, then compare its adjacent entries, rather than searching 256
        // colors for every shade/blend-table entry during startup.
        constexpr std::array<Point3F, 3> ramps{{{.17f, .68f, .94f}, {.16f, .23f, .32f}, {1, 1, 1}}};
        constexpr std::array<unsigned, 3> starts{0, 128, 224}, counts{128, 96, 32};
        Flt32 best = 1000;
        UInt8 result{};
        for (std::size_t ramp = 0; ramp < ramps.size(); ++ramp) {
            const auto level =
                std::clamp(dot(rgb, ramps[ramp]) / dot(ramps[ramp], ramps[ramp]), 0.f, 1.f) *
                Flt32(counts[ramp] - 1);
            const auto first = unsigned(level);
            for (const auto index : {first, std::min(first + 1, counts[ramp] - 1)}) {
                const auto& c = softwarePalette_.palettes[0].colors[starts[ramp] + index];
                const auto delta =
                    sub(rgb, {Flt32(c.red) / 255, Flt32(c.green) / 255, Flt32(c.blue) / 255});
                const auto error = dot(delta, delta);
                if (error < best) {
                    best = error;
                    result = UInt8(starts[ramp] + index);
                }
            }
        }
        return result;
    }

    void softwareTextures() {
        if (model_) {
            auto palette = sim::softwareShadowPalette(*modelPalette_);
            require(bool(palette), "Unable to prepare model shadow palette");
            softwarePalette_ = std::move(*palette);
            softwareShadow_ = Bitmap::create(shadow_->width, shadow_->height);
            require(bool(softwareShadow_), "Unable to allocate indexed model shadow");
            softwareShadow_->paletteIndex = 0xfffffffeU;
            softwareFloor_ = Bitmap::create(floor_->width, floor_->height);
            require(bool(softwareFloor_), "Unable to allocate indexed model floor");
            std::ranges::for_each(std::views::iota(0, floor_->height), [&](auto y) {
                std::ranges::for_each(std::views::iota(0, floor_->width), [&](auto x) {
                    const auto row = floor_->row(y);
                    const auto at = std::size_t(x) * 3;
                    Point3F rgb{Flt32(std::to_integer<UInt8>(row[at + 2])) / 255,
                                Flt32(std::to_integer<UInt8>(row[at + 1])) / 255,
                                Flt32(std::to_integer<UInt8>(row[at])) / 255};
                    rgb = add(mul(rgb, .7f * .7f), mul({.07f, .11f, .16f}, .3f));
                    softwareFloor_->row(y)[std::size_t(x)] = std::byte(nearest(rgb));
                });
            });
            return;
        }
        softwarePalette_.shadeShift = 5;
        softwarePalette_.hazeLevels = 8;
        softwarePalette_.palettes.resize(2);
        auto& base = softwarePalette_.palettes[0];
        base.index = 0;
        base.type = PaletteType::ShadeHaze;
        for (unsigned i = 0; i < 256; ++i) {
            const Point3F ramp = i < 128   ? Point3F{.17f, .68f, .94f}
                                 : i < 224 ? Point3F{.16f, .23f, .32f}
                                           : Point3F{1, 1, 1};
            const auto level = i < 128   ? Flt32(i) / 127
                               : i < 224 ? Flt32(i - 128) / 95
                                         : Flt32(i - 224) / 31;
            base.colors[i] = {UInt8(ramp.x * level * 255),
                              UInt8(ramp.y * level * 255),
                              UInt8(ramp.z * level * 255),
                              0};
            base.identity[i] = UInt8(i);
        }
        base.remap.resize(32 * 9 * 256);
        const Point3F haze{.07f, .11f, .16f};
        for (unsigned h = 0; h < 9; ++h) {
            for (unsigned s = 0; s < 32; ++s) {
                for (unsigned i = 0; i < 256; ++i) {
                    const auto& c = base.colors[i];
                    Point3F rgb{Flt32(c.red) / 255, Flt32(c.green) / 255, Flt32(c.blue) / 255};
                    const auto amount = h == 8 ? Flt32(s) / 31 : Flt32(h) / 7;
                    if (h != 8) {
                        rgb = mul(rgb, Flt32(s) / 31);
                    }
                    base.remap[(h * 32 + s) * 256 + i] =
                        std::byte(nearest(add(mul(rgb, 1 - amount), mul(haze, amount))));
                }
            }
        }
        auto& blend = softwarePalette_.palettes[1];
        blend.index = 1;
        blend.type = PaletteType::Translucent;
        blend.colors = base.colors;
        blend.remap.resize(65536);
        for (unsigned alpha = 0; alpha < 256; ++alpha) {
            for (unsigned i = 0; i < 256; ++i) {
                const auto& c = base.colors[i];
                blend.remap[alpha * 256 + i] =
                    alpha == 0
                        ? std::byte(i)
                        : std::byte(nearest(
                              mul({Flt32(c.red) / 255, Flt32(c.green) / 255, Flt32(c.blue) / 255},
                                  1 - Flt32(alpha) / 255)));
            }
        }
        require(softwarePalette_.valid(), "Invalid software viewer palette");
        const auto convert = [&](const Bitmap& source, Point3F tint) {
            auto bitmap = Bitmap::create(source.width, source.height);
            require(bool(bitmap), "Unable to allocate indexed viewer texture");
            for (Int32 y = 0; y < source.height; ++y) {
                for (Int32 x = 0; x < source.width; ++x) {
                    const auto row = source.row(y);
                    const auto offset = std::size_t(x) * 3;
                    bitmap->row(y)[std::size_t(x)] = std::byte(
                        nearest({Flt32(std::to_integer<UInt8>(row[offset + 2])) / 255 * tint.x,
                                 Flt32(std::to_integer<UInt8>(row[offset + 1])) / 255 * tint.y,
                                 Flt32(std::to_integer<UInt8>(row[offset])) / 255 * tint.z}));
                }
            }
            return bitmap;
        };
        softwareCube_ = convert(*cube_, {.17f, .68f, .94f});
        softwareFloor_ = convert(*floor_, {1, 1, 1});
        softwareShadow_ = Bitmap::create(shadow_->width, shadow_->height);
        require(bool(softwareShadow_), "Unable to allocate indexed shadow");
        softwareShadow_->paletteIndex = 1;
    }

    void loadModel() {
        if (modelName_.empty()) {
            require(skin_.empty() && sequenceName_.empty() && transitionName_.empty() &&
                        !automaticDetail_ && !rootMotion_ && !reverse_ &&
                        utilityPrefixes_.empty() && modelLighting_,
                    "Model playback options require --shape");
            return;
        }
        if (assetRoots_.empty()) {
            assetRoots_ = {"PCRPG/RPG", "PCRPG/base"};
        }
        ResourceManager manager;
        require(world::registerTsLoaders(manager) && registerBitmapFontLoaders(manager) &&
                    registerPaletteLoaders(manager),
                "Model loader registration failed");
        require(manager.setSearchPaths(assetRoots_),
                "Cannot index the requested asset directories");
        const auto volumes = manager.findMatches("*.vol");
        std::ranges::for_each(volumes, [&](const auto& name) {
            require(manager.addVolume(name), "Cannot mount " + name);
        });
        auto shape = manager.load<world::TsShape>(modelName_);
        require(bool(shape), "Cannot decode DTS: " + modelName_);
        std::vector<std::string_view> prefixes;
        std::ranges::transform(utilityPrefixes_, std::back_inserter(prefixes), [](const auto& p) {
            return std::string_view(p);
        });
        require(bool(world::classifyTsUtilityDetails(*shape, prefixes)),
                "Invalid utility detail classification");
        require(bool(shape->materials), "DTS has no embedded material list: " + modelName_);
        auto list = *shape->materials;
        if (!skin_.empty()) {
            std::ranges::for_each(list.materials, [&](auto& material) {
                if (material.type() == 3 && material.mapFile.starts_with("base.")) {
                    material.mapFile = skin_ + material.mapFile.substr(4);
                }
            });
        }
        auto materials = world::TsMaterialResources::load(manager, list);
        require(bool(materials), "Cannot load a model texture; check --assets and --skin");
        model_ = world::TsShapeResources{std::move(shape), std::move(*materials)};
        modelPalette_ = manager.load<Palette>("lush.day.ppl");
        require(bool(modelPalette_), "Missing lush.day.ppl in the asset search paths");
        std::printf(
            "Loaded %s: %zu meshes, %zu sequences, %zu transitions, %zu triggers, skin %s.\n",
            modelName_.c_str(),
            model_->shape->meshes.size(),
            model_->shape->sequences.size(),
            model_->shape->transitions.size(),
            model_->shape->triggers.size(),
            skin_.empty() ? "embedded" : skin_.c_str());
        const auto& loaded = *model_->shape;
        std::ranges::for_each(
            std::views::iota(std::size_t{0}, loaded.sequences.size()), [&](auto i) {
                const auto& seq = loaded.sequences[i];
                const auto name =
                    seq.name >= 0 ? loaded.names[std::size_t(seq.name)] : std::to_string(i);
                std::printf("  %zu: %s (%.3f s%s)\n",
                            i,
                            name.c_str(),
                            double(seq.duration),
                            seq.cyclic ? ", cyclic" : "");
                if (sequence_ < 0 &&
                    (name == sequenceName_ || std::to_string(i) == sequenceName_)) {
                    sequence_ = Int32(i);
                }
            });
        require(sequenceName_.empty() || sequence_ >= 0,
                "Unknown --sequence; use a name or index printed above");
        animate_ = sequence_ >= 0;
        animationTime_ = initialAnimationTime_;
        require(transitionName_.empty() || sequence_ >= 0, "--transition-to requires --sequence");
        resetPlayback(true);
        updatePose();
    }

    void resetPlayback(bool requestTransition = false) {
        if (!model_) {
            return;
        }
        modelInstance_.emplace(model_->shape);
        triggerCount_ = 0;
        if (sequence_ < 0) {
            return;
        }
        const auto sample =
            world::TsAnimationSample::atTime(*model_->shape, sequence_, initialAnimationTime_);
        require(sample && modelInstance_->addThread(sequence_, sample->position).has_value(),
                "Invalid initial sequence time");
        require(modelInstance_->setTimeScale(0, reverse_ ? -1.f : 1.f), "Invalid playback speed");
        if (requestTransition && !transitionName_.empty()) {
            Int32 target = -1;
            const auto& shape = *model_->shape;
            for (std::size_t i = 0; i < shape.sequences.size(); ++i) {
                const auto name = shape.sequences[i].name;
                if (transitionName_ == std::to_string(i) ||
                    (name >= 0 && transitionName_ == shape.names[std::size_t(name)])) {
                    target = Int32(i);
                    break;
                }
            }
            require(target >= 0 && modelInstance_->transitionToSequence(0, target),
                    "No reachable authored --transition-to edge for the selected sequence/time");
        }
    }

    void chooseSequence(Int32 sequence) {
        sequence_ = sequence;
        animationTime_ = 0;
        animate_ = true;
        if (modelInstance_->threads().empty()) {
            require(modelInstance_->addThread(sequence_).has_value(),
                    "Cannot start playback thread");
        } else if (!modelInstance_->transitionToSequence(0, sequence_)) {
            require(modelInstance_->setSequence(0, sequence_), "Cannot select sequence");
        }
        require(modelInstance_->setTimeScale(0, reverse_ ? -1.f : 1.f), "Invalid playback speed");
    }

    void updatePose() {
        if (!model_) {
            return;
        }
        if (animate_ && sequence_ >= 0) {
            if (animationSweep_) {
                const auto sample =
                    world::TsAnimationSample::atTime(*model_->shape, sequence_, animationTime_);
                require(bool(sample), "Invalid sequence time");
                const std::array samples{*sample};
                modelPose_ = world::TsShapePose::sample(model_->shape, samples);
            } else {
                if (modelInstance_->threads().empty()) {
                    chooseSequence(sequence_);
                }
                modelPose_ = modelInstance_->pose();
            }
        } else {
            modelPose_ = world::TsShapePose::sample(model_->shape);
        }
        require(bool(modelPose_), "TS pose evaluation failed: invalid authored animation track");
    }

    void textures() {
        cube_ = Bitmap::create(64, 64, 24);
        floor_ = Bitmap::create(512, 512, 24);
        shadow_ = Bitmap::create(model_ ? 64 : 128, model_ ? 64 : 128, 16);
        font_ = Bitmap::create(128, 64);
        require(cube_ && floor_ && shadow_ && font_, "Unable to allocate viewer textures");
        for (Int32 y = 0; y < 64; ++y) {
            for (Int32 x = 0; x < 64; ++x) {
                const auto checker = ((x / 16 + y / 16) & 1) != 0;
                const auto edge = x < 2 || y < 2 || x >= 62 || y >= 62;
                const UInt8 value = edge ? 100 : checker ? 210 : 250;
                for (std::size_t c = 0; c < 3; ++c) {
                    cube_->row(y)[std::size_t(x) * 3 + c] = std::byte(value);
                }
            }
        }
        // Bake the complete floor grid into normalized UV space. The legacy
        // constant-lit wrapped U/V carry path is unsuitable for this large
        // arbitrary camera-projected polygon and can scramble one grid axis.
        for (Int32 y = 0; y < floor_->height; ++y) {
            for (Int32 x = 0; x < floor_->width; ++x) {
                const bool line = (x * 24 / 512) != (std::max(0, x - 1) * 24 / 512) ||
                                  (y * 24 / 512) != (std::max(0, y - 1) * 24 / 512);
                const std::array<UInt8, 3> ground =
                    line ? std::array<UInt8, 3>{73, 55, 39} : std::array<UInt8, 3>{43, 30, 22};
                for (std::size_t c = 0; c < 3; ++c) {
                    floor_->row(y)[std::size_t(x) * 3 + c] = std::byte(ground[c]);
                }
            }
        }
        shadow_->attributes |= BitmapAlpha88;
        shadow_->palette.emplace(); // black RGB, explicit per-texel alpha
        font_->palette.emplace();
        font_->palette->palettes[0].colors[1] = {255, 255, 255, 0};
        for (const auto& glyph : glyphs) {
            const auto slot = unsigned(glyph.code) - 32, ox = (slot % 16) * 8,
                       oy = (slot / 16) * 10;
            for (unsigned y = 0; y < 7; ++y) {
                for (unsigned x = 0; x < 5; ++x) {
                    if (glyph.rows[y] & (1U << (4 - x))) {
                        font_->row(Int32(oy + y))[ox + x] = std::byte{1};
                    }
                }
            }
        }
    }

    bool onInit(std::span<const std::string_view> args) override {
        GpuSurfaceConfig gpu;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--smoke") {
                smoke_ = true;
            } else if (args[i] == "--sweep") {
                smoke_ = true;
                sweep_ = true;
            } else if (args[i] == "--animation-sweep") {
                smoke_ = true;
                animationSweep_ = true;
            } else if (args[i] == "--root-motion") {
                rootMotion_ = true;
            } else if (args[i] == "--reverse") {
                reverse_ = true;
            } else if (args[i] == "--unlit") {
                modelLighting_ = false;
            } else if (args[i] == "--utility-prefix" && i + 1 < args.size()) {
                utilityPrefixes_.emplace_back(args[++i]);
            } else if (args[i] == "--transition-to" && i + 1 < args.size()) {
                transitionName_ = args[++i];
            } else if (args[i] == "--driver" && i + 1 < args.size()) {
                gpu.driver = args[++i];
            } else if (args[i] == "--snapshot" && i + 1 < args.size()) {
                snapshot_ = args[++i];
            } else if (args[i] == "--shape" && i + 1 < args.size()) {
                modelName_ = args[++i];
            } else if (args[i] == "--skin" && i + 1 < args.size()) {
                skin_ = args[++i];
            } else if (args[i] == "--sequence" && i + 1 < args.size()) {
                sequenceName_ = args[++i];
            } else if (args[i] == "--auto-detail") {
                automaticDetail_ = true;
            } else if (args[i] == "--time" && i + 1 < args.size()) {
                const auto value = args[++i];
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), initialAnimationTime_);
                require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                            std::isfinite(initialAnimationTime_),
                        "Invalid --time in seconds");
            } else if (args[i] == "--assets" && i + 1 < args.size()) {
                assetRoots_.emplace_back(args[++i]);
            } else if (args[i] == "--mode" && i + 1 < args.size()) {
                const auto value = args[++i];
                require(value == "hardware" || value == "software" || value == "split",
                        "Invalid renderer mode");
                mode_ = value == "hardware"   ? RenderMode::Hardware
                        : value == "software" ? RenderMode::Software
                                              : RenderMode::Split;
            } else {
                throw std::runtime_error(
                    "Usage: opent1_renderer_demo [--shape rpgmalehuman.dts] [--skin rpghuman] "
                    "[--sequence name|index] [--time seconds] [--transition-to name|index] "
                    "[--root-motion] [--reverse] [--auto-detail] [--unlit] [--utility-prefix "
                    "prefix ...] [--assets directory ...] [--driver vulkan|direct3d12] [--mode "
                    "hardware|software|split] [--smoke|--sweep|--animation-sweep] [--snapshot "
                    "image.bmp]");
            }
        }
        loadModel();
        require(!animationSweep_ || (model_ && !model_->shape->sequences.empty()),
                "--animation-sweep requires a shape with sequences");
        gpu.debug = smoke_;
        smokeMode_ = mode_;
        WindowConfig config;
        config.title = "OpenT1 - Renderer comparison / 1 Hardware / 2 Software / 3 Split";
        if (model_) {
            config.title += " / " + modelName_;
        }
        config.width = 1280;
        config.height = 720;
        config.minWidth = 960;
        config.minHeight = 480;
        config.maxWidth = 1920;
        config.maxHeight = 1200;
        config.hidden = smoke_;
        config.quitOnDestroy = true;
        auto& window = createWindow(config);
        window_ = window.id();
        surface_ = SdlGpuSurface::create(window, config.width, config.height, gpu);
        require(bool(surface_), SDL_GetError());
        require(surface_->setGamma(1.4f), "Invalid viewer gamma");
        textures();
        softwareTextures();
        lastTime_ = SDL_GetTicksNS();
        std::printf("OpenT1 viewer on %s. 1/2/3: renderers; Tab: cycle; drag: orbit; wheel: zoom; "
                    "space: pause; A: animation; N/P: sequence; M: root motion; B: reverse; D: "
                    "automatic detail; L: lighting; S: shadows; R: reset.\n",
                    std::string(surface_->driver()).c_str());
        return true;
    }

    void onEvent(const Event& event) override {
        if (event.window != window_) {
            return;
        }
        if (const auto* mouse = std::get_if<MouseMotionEvent>(&event.data);
            mouse && (mouse->buttons & SDL_BUTTON_LMASK)) {
            yaw_ -= mouse->dx * .008f;
            pitch_ = std::clamp(pitch_ + mouse->dy * .008f, .12f, 1.35f);
        }
        if (const auto* wheel = std::get_if<MouseWheelEvent>(&event.data)) {
            distance_ = std::clamp(distance_ * std::exp(-wheel->y * .12f), 4.f, 18.f);
        }
        if (const auto* key = std::get_if<KeyEvent>(&event.data); key && key->down) {
            if (key->key == SDLK_ESCAPE) {
                requestQuit();
            }
            if (key->key == SDLK_SPACE && !key->repeat) {
                paused_ = !paused_;
            }
            if (key->key == SDLK_S && !key->repeat) {
                shadows_ = !shadows_;
            }
            if (model_ && !key->repeat) {
                const auto count = Int32(model_->shape->sequences.size());
                if (key->key == SDLK_A && count) {
                    if (sequence_ < 0) {
                        sequence_ = 0;
                    }
                    animate_ = !animate_;
                }
                if ((key->key == SDLK_N || key->key == SDLK_P) && count) {
                    chooseSequence(sequence_ < 0
                                       ? 0
                                       : (sequence_ + count + (key->key == SDLK_N ? 1 : -1)) %
                                             count);
                }
                if (key->key == SDLK_M) {
                    rootMotion_ = !rootMotion_;
                }
                if (key->key == SDLK_B) {
                    reverse_ = !reverse_;
                    if (!modelInstance_->threads().empty()) {
                        require(modelInstance_->setTimeScale(0, reverse_ ? -1.f : 1.f),
                                "Invalid playback speed");
                    }
                }
                if (key->key == SDLK_D) {
                    automaticDetail_ = !automaticDetail_;
                }
                if (key->key == SDLK_L) {
                    modelLighting_ = !modelLighting_;
                }
            }
            if (key->key == SDLK_1) {
                mode_ = RenderMode::Hardware;
            }
            if (key->key == SDLK_2) {
                mode_ = RenderMode::Software;
            }
            if (key->key == SDLK_3) {
                mode_ = RenderMode::Split;
            }
            if (key->key == SDLK_TAB && !key->repeat) {
                mode_ = RenderMode((UInt8(mode_) + 1) % 3);
            }
            if (key->key == SDLK_R) {
                yaw_ = .65f;
                pitch_ = .35f;
                distance_ = 7.5f;
                angle_ = .35f;
                animationTime_ = initialAnimationTime_;
                paused_ = false;
                shadows_ = true;
                resetPlayback();
            }
            if (key->key == SDLK_LEFT) {
                yaw_ -= .08f;
            }
            if (key->key == SDLK_RIGHT) {
                yaw_ += .08f;
            }
            if (key->key == SDLK_UP) {
                pitch_ = std::min(pitch_ + .06f, 1.35f);
            }
            if (key->key == SDLK_DOWN) {
                pitch_ = std::max(pitch_ - .06f, .12f);
            }
        }
    }

    void screenQuad(Flt32 x,
                    Flt32 y,
                    Flt32 w,
                    Flt32 h,
                    HardwareMaterial material,
                    Point2F uv0 = {0, 0},
                    Point2F uv1 = {1, 1}) {
        const std::array<PolygonVertex, 4> v{{{{x, y}, uv0},
                                              {{x + w, y}, {uv1.x, uv0.y}},
                                              {{x + w, y + h}, uv1},
                                              {{x, y + h}, {uv0.x, uv1.y}}}};
        triangles_.push_back({{v[0], v[1], v[2]}, material, {}});
        triangles_.push_back({{v[0], v[2], v[3]}, material, {}});
    }

    void text(Flt32 x, Flt32 y, Flt32 scale, std::string_view message, Point3F color) {
        HardwareMaterial m;
        m.texture = font_.get();
        m.transparent = true;
        m.blend = HardwareBlend::Alpha;
        m.depth = HardwareDepth::Disabled;
        m.perspective = false;
        m.color = color;
        for (unsigned char character : message) {
            if (character >= 'a' && character <= 'z') {
                character = UInt8(character - 'a' + 'A');
            }
            if (character >= 32 && character < 96 && character != ' ') {
                const unsigned slot = character - 32;
                const Flt32 u = Flt32((slot % 16) * 8) / 128, v = Flt32((slot / 16) * 10) / 64;
                screenQuad(x, y, 5 * scale, 7 * scale, m, {u, v}, {u + 5.f / 128, v + 7.f / 64});
            }
            x += 6 * scale;
        }
    }

    void worldQuad(const std::array<Point3F, 4>& positions,
                   Point3F faceNormal,
                   HardwareMaterial material,
                   Flt32 textureScale = 1) {
        const std::array<Point2F, 4> uv{
            {{0, 0}, {textureScale, 0}, {textureScale, textureScale}, {0, textureScale}}};
        std::vector<ViewVertex> polygon;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto p = positions[i], view = sub(p, eye_);
            const auto light = normal(sub(light_, p));
            const auto half = normal(add(light, normal(sub(eye_, p))));
            const auto diffuse = .24f + .76f * std::max(0.f, dot(faceNormal, light));
            const auto specular = .28f * std::pow(std::max(0.f, dot(faceNormal, half)), 32.f);
            const auto intensity = std::min(1.f, diffuse + specular);
            const Point3F camera{dot(view, right_), dot(view, up_), dot(view, forward_)};
            polygon.push_back({camera,
                               {intensity, intensity, intensity},
                               uv[i],
                               std::clamp((camera.z - 9) / 24, 0.f, .94f)});
        }
        const Flt32 sx = width_ / (2 * focal_), sy = height_ / (2 * focal_);
        const std::array<Point3F, 6> planes{
            {{0, 0, 1}, {0, 0, -1}, {1, 0, sx}, {-1, 0, sx}, {0, 1, sy}, {0, -1, sy}}};
        // Clip before perspective division. This small demo only manufactures
        // convex quads; every clipped fan is submitted as individual triangles.
        for (std::size_t plane = 0; plane < planes.size() && !polygon.empty(); ++plane) {
            std::vector<ViewVertex> clipped;
            auto a = polygon.back();
            const auto distance = [&](const ViewVertex& v) {
                return dot(v.position, planes[plane]) + (plane == 0   ? -.1f
                                                         : plane == 1 ? 100.f
                                                                      : 0.f);
            };
            auto da = distance(a);
            for (const auto& b : polygon) {
                const auto db = distance(b);
                if ((da >= 0) != (db >= 0)) {
                    clipped.push_back(lerp(a, b, da / (da - db)));
                }
                if (db >= 0) {
                    clipped.push_back(b);
                }
                a = b;
                da = db;
            }
            polygon = std::move(clipped);
        }
        std::vector<PolygonVertex> projected;
        for (const auto& v : polygon) {
            const Point2F screen{width_ / 2 + focal_ * v.position.x / v.position.z,
                                 height_ / 2 - focal_ * v.position.y / v.position.z};
            projected.push_back({screen, v.uv, v.color, v.haze, 1 / v.position.z});
        }
        for (std::size_t i = 1; i + 1 < projected.size(); ++i) {
            const auto a = projected[0].position, b = projected[i].position,
                       c = projected[i + 1].position;
            const auto area = std::abs((double(b.x) - a.x) * (double(c.y) - a.y) -
                                       (double(b.y) - a.y) * (double(c.x) - a.x));
            const auto length = [](Point2F p, Point2F q) {
                return std::hypot(double(p.x) - q.x, double(p.y) - q.y);
            };
            const auto longest = std::max({length(a, b), length(b, c), length(c, a)});
            // Omit subpixel slivers on both paths. Nearly tangent faces can
            // give legacy fixed-point perspective spans undefined gradients.
            if (longest == 0 || area / longest < .75) {
                continue;
            }
            material.twoSided = true;
            triangles_.push_back({{projected[0], projected[i], projected[i + 1]}, material, {}});
        }
    }

    void castShadow(const std::array<Point3F, 8>& corners) {
        // Project the actual cube from the same point light used for shading
        // onto the floor, then form the union silhouette as a convex hull.
        std::array<Point2F, 8> points;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const auto p =
                add(light_, mul(sub(corners[i], light_), light_.y / (light_.y - corners[i].y)));
            points[i] = {p.x, p.z};
        }
        std::ranges::sort(points, [](auto a, auto b) {
            return a.x == b.x ? a.y < b.y : a.x < b.x;
        });
        const auto side = [](Point2F a, Point2F b, Point2F p) {
            return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        };
        std::vector<Point2F> hull;
        for (const auto p : points) {
            while (hull.size() >= 2 && side(hull[hull.size() - 2], hull.back(), p) <= 0) {
                hull.pop_back();
            }
            hull.push_back(p);
        }
        const auto lower = hull.size();
        for (Int32 i = Int32(points.size()) - 2; i >= 0; --i) {
            const auto p = points[std::size_t(i)];
            while (hull.size() > lower && side(hull[hull.size() - 2], hull.back(), p) <= 0) {
                hull.pop_back();
            }
            hull.push_back(p);
        }
        hull.pop_back();
        require(hull.size() >= 3, "Invalid cube shadow silhouette");
        constexpr Flt32 softness = .16f;
        Flt32 minX = points[0].x, maxX = points[0].x, minZ = points[0].y, maxZ = points[0].y;
        for (const auto p : points) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minZ = std::min(minZ, p.y);
            maxZ = std::max(maxZ, p.y);
        }
        minX -= softness * 2;
        maxX += softness * 2;
        minZ -= softness * 2;
        maxZ += softness * 2;
        for (Int32 y = 0; y < shadow_->height; ++y) {
            for (Int32 x = 0; x < shadow_->width; ++x) {
                const Point2F sample{
                    minX + (Flt32(x) + .5f) / Flt32(shadow_->width) * (maxX - minX),
                    maxZ - (Flt32(y) + .5f) / Flt32(shadow_->height) * (maxZ - minZ)};
                Flt32 distance = 1000;
                for (std::size_t i = 0; i < hull.size(); ++i) {
                    const auto a = hull[i], b = hull[(i + 1) % hull.size()];
                    const auto dx = b.x - a.x, dz = b.y - a.y;
                    distance =
                        std::min(distance, side(a, b, sample) / std::sqrt(dx * dx + dz * dz));
                }
                // Analytic soft edge around the geometric silhouette. This is a
                // planar demo shadow, rather than a world/TS shadow-map pipeline.
                const auto t = std::clamp(.5f + distance / (2 * softness), 0.f, 1.f);
                shadow_->row(y)[std::size_t(x) * 2 + 1] =
                    std::byte(UInt8(183 * t * t * (3 - 2 * t)));
            }
        }
        HardwareMaterial material;
        material.texture = shadow_.get();
        material.linear = true;
        material.depth = HardwareDepth::TestOnly;
        material.blend = HardwareBlend::Alpha;
        worldQuad(
            {{{minX, .008f, maxZ}, {maxX, .008f, maxZ}, {maxX, .008f, minZ}, {minX, .008f, minZ}}},
            {0, 1, 0},
            material);
    }

    void castModelShadow(const TMat3F& toWorld) {
        const auto posedBounds = modelPose_->bounds(modelFrame_->detail());
        if (!posedBounds) {
            return;
        }
        const auto& box = *posedBounds;
        std::array<Point2F, 8> projected;
        std::ranges::transform(std::views::iota(0U, 8U), projected.begin(), [&](auto i) {
            Point3F p{i & 1 ? box.fMax.x : box.fMin.x,
                      i & 2 ? box.fMax.y : box.fMin.y,
                      i & 4 ? box.fMax.z : box.fMin.z};
            m_mul(p, toWorld, p);
            require(light_.y - p.y > .1f, "Model shadow source crosses the light plane");
            const auto floor = add(light_, mul(sub(p, light_), light_.y / (light_.y - p.y)));
            return Point2F{floor.x, floor.z};
        });
        const auto [left, right] = std::ranges::minmax(projected, {}, &Point2F::x);
        const auto [bottom, top] = std::ranges::minmax(projected, {}, &Point2F::y);
        const auto centerX = (left.x + right.x) / 2, centerZ = (bottom.y + top.y) / 2;
        const auto radius = std::max(right.x - left.x, top.y - bottom.y) / 2 + .15f;
        // An off-axis light camera maps its perspective image directly onto
        // the floor window. The current pose sizes the window; actual mesh
        // triangles supply coverage, preserving gaps between limbs.
        const auto shearX = (light_.x - centerX) / light_.y,
                   shearZ = (light_.z - centerZ) / light_.y;
        TMat3F lightCamera;
        lightCamera.m = {{{1, 0, 0}, {-shearX, -1, -shearZ}, {0, 0, 1}}};
        lightCamera.p = {-centerX, light_.y, -centerZ};
        lightCamera.flags =
            RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasScale | RMat3F::Matrix_HasTranslation;
        constexpr Int32 sourceDimension = 132;
        world::TsStaticView view;
        m_mul(toWorld, lightCamera, view.shapeToCamera);
        view.width = sourceDimension;
        view.height = sourceDimension;
        view.center = {66, 66};
        const auto focal = 128 * light_.y / (2 * radius);
        view.focal = {focal, focal};
        view.farDistance = 100;
        view.twoSided = true;
        view.detail = modelFrame_->detail();
        auto shadowFrame = world::TsStaticShapeFrame::prepare(*model_, view, *modelPose_);
        require(bool(shadowFrame), "TS light-view preparation failed");
        auto mask = shadowFrame->silhouette();
        require(bool(mask), "TS silhouette rasterization failed");
        std::vector<UInt8> source;
        source.reserve(sourceDimension * sourceDimension);
        std::ranges::transform(mask->pixels, std::back_inserter(source), [](auto b) {
            return std::to_integer<UInt8>(b);
        });
        const auto filtered = sim::filterShadowBitmap(
            source, sourceDimension, 64, sim::ShadowBlur::Oversampled3, .65f);
        std::ranges::for_each(std::views::iota(std::size_t{0}, filtered.pixels.size()),
                              [&](auto i) {
                                  shadow_->pixels[i * 2 + 1] = std::byte(filtered.pixels[i]);
                              });
        HardwareMaterial material;
        material.texture = shadow_.get();
        material.linear = true;
        material.depth = HardwareDepth::TestOnly;
        material.blend = HardwareBlend::Alpha;
        worldQuad({{{centerX - radius, .008f, centerZ + radius},
                    {centerX + radius, .008f, centerZ + radius},
                    {centerX + radius, .008f, centerZ - radius},
                    {centerX - radius, .008f, centerZ - radius}}},
                  {0, 1, 0},
                  material);
    }

    void modelScene() {
        const auto& bounds = model_->shape->bounds;
        const auto height = bounds.fMax.z - bounds.fMin.z;
        require(std::isfinite(height) && height > 0, "Model has invalid bounds");
        const auto angle = angle_ + std::numbers::pi_v<Flt32>;
        const auto scale = 2.8f / height, c = std::cos(angle), s = std::sin(angle);
        TMat3F toWorld;
        toWorld.m = {{{c * scale, 0, -s * scale}, {-s * scale, 0, -c * scale}, {0, scale, 0}}};
        const Point3F origin{(bounds.fMin.x + bounds.fMax.x) / 2,
                             (bounds.fMin.y + bounds.fMax.y) / 2,
                             bounds.fMin.z};
        m_mul(-origin, static_cast<const RMat3F&>(toWorld), toWorld.p);
        toWorld.p.y += .05f;
        toWorld.flags =
            RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasScale | RMat3F::Matrix_HasTranslation;
        if (rootMotion_ && animate_ && !animationSweep_) {
            m_mul(modelInstance_->rootMotion(), toWorld, toWorld);
        }
        TMat3F camera;
        camera.m = {{{right_.x, forward_.x, up_.x},
                     {right_.y, forward_.y, up_.y},
                     {right_.z, forward_.z, up_.z}}};
        camera.p = {-dot(eye_, right_), -dot(eye_, forward_), -dot(eye_, up_)};
        camera.flags = RMat3F::Matrix_HasRotation | RMat3F::Matrix_HasTranslation;
        world::TsStaticView view;
        m_mul(toWorld, camera, view.shapeToCamera);
        view.width = Int32(width_);
        view.height = Int32(height_);
        view.center = {width_ / 2, height_ / 2};
        view.focal = {focal_, focal_};
        view.farDistance = 100;
        view.minimumProjectedAltitude = .75f;
        view.automaticDetail = automaticDetail_;
        if (modelLighting_) {
            auto inverse = toWorld;
            inverse.inverse();
            world::TsLight light;
            light.type = world::TsLight::Type::Point;
            m_mul(light_, inverse, light.position);
            light.range = 100 / scale;
            light.attenuation = {1, .08f * scale, .01f * scale * scale};
            light.intensity = {.85f, .85f, .85f};
            view.lighting = world::TsLighting{{.22f, .22f, .22f}, {light}};
        }
        modelFrame_ = world::TsStaticShapeFrame::prepare(*model_, view, *modelPose_);
        require(bool(modelFrame_), "TS pose preparation failed");
        auto hardware = modelFrame_->hardwarePolygons(modelPalette_.get());
        auto software = modelFrame_->softwarePolygons(&softwarePalette_);
        require(hardware && software && hardware->size() == software->size(),
                "TS material preparation failed");
        if (!drawModel_) {
            return;
        }
        if (shadows_ && modelFrame_->detail() >= 0) {
            castModelShadow(toWorld);
        }
        std::ranges::for_each(std::views::iota(std::size_t{0}, hardware->size()), [&](auto i) {
            const auto& polygon = (*hardware)[i];
            Triangle triangle;
            std::ranges::copy(polygon.vertices, triangle.vertices.begin());
            triangle.material = polygon.material;
            triangle.softwareMaterial = (*software)[i].material;
            triangles_.push_back(triangle);
        });
    }

    void scene(Int32 viewWidth, Int32 viewHeight) {
        width_ = Flt32(viewWidth);
        height_ = Flt32(viewHeight);
        focal_ = height_ * 1.07f;
        const Point3F target{0, 1.15f, 0};
        eye_ = add(target,
                   {distance_ * std::sin(yaw_) * std::cos(pitch_),
                    distance_ * std::sin(pitch_),
                    distance_ * std::cos(yaw_) * std::cos(pitch_)});
        forward_ = normal(sub(target, eye_));
        right_ = normal(cross(forward_, {0, 1, 0}));
        up_ = cross(right_, forward_);
        triangles_.clear();
        HardwareMaterial ground;
        ground.texture = floor_.get();
        ground.linear = true;
        // Both paths share the same normalized grid and constant floor lighting.
        ground.shadeSource = PolygonSource::Constant;
        ground.shade = .7f;
        ground.hazeSource = PolygonSource::Constant;
        ground.haze = .3f;
        worldQuad({{{-24, 0, 24}, {24, 0, 24}, {24, 0, -24}, {-24, 0, -24}}}, {0, 1, 0}, ground);
        if (model_) {
            modelScene();
            return;
        }
        constexpr std::array<Point3F, 8> corners{{{-1, -1, -1},
                                                  {1, -1, -1},
                                                  {1, 1, -1},
                                                  {-1, 1, -1},
                                                  {-1, -1, 1},
                                                  {1, -1, 1},
                                                  {1, 1, 1},
                                                  {-1, 1, 1}}};
        constexpr std::array<std::array<unsigned, 4>, 6> faces{
            {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3}, {1, 2, 6, 5}, {3, 7, 6, 2}, {0, 1, 5, 4}}};
        std::array<Point3F, 8> worldCorners;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            worldCorners[i] = add(rotate(corners[i], angle_), {0, 1.2f, 0});
        }
        if (shadows_) {
            castShadow(worldCorners);
        }
        for (const auto& indices : faces) {
            std::array<Point3F, 4> positions;
            for (std::size_t i = 0; i < 4; ++i) {
                positions[i] = worldCorners[indices[i]];
            }
            const auto n =
                normal(cross(sub(positions[1], positions[0]), sub(positions[2], positions[0])));
            const auto center =
                mul(add(add(positions[0], positions[1]), add(positions[2], positions[3])), .25f);
            if (dot(n, sub(eye_, center)) <= 0) {
                continue;
            }
            const auto light = normal(sub(light_, center)),
                       half = normal(add(light, normal(sub(eye_, center))));
            HardwareMaterial m;
            m.texture = cube_.get();
            m.linear = true;
            m.shadeSource = PolygonSource::Constant;
            // Flat faces share one light value on both renderers.
            m.shade = std::min(1.f,
                               .24f + .76f * std::max(0.f, dot(n, light)) +
                                   .28f * std::pow(std::max(0.f, dot(n, half)), 32.f));
            m.color = {.17f, .68f, .94f};
            worldQuad(positions, n, m);
        }
    }

    void softwareView() {
        const auto w = Int32(width_), h = Int32(height_);
        if (!software_ || software_->bitmap().width != w || software_->bitmap().height != h) {
            software_ = SoftwareSurface::create(w, h);
            softwareImage_ = Bitmap::create(w, h);
            require(software_ && softwareImage_ && software_->setPalette(softwarePalette_),
                    "Unable to allocate software view");
            softwareImage_->palette = softwarePalette_;
        }
        if (softwareShadow_) {
            for (Int32 y = 0; y < shadow_->height; ++y) {
                for (Int32 x = 0; x < shadow_->width; ++x) {
                    softwareShadow_->row(y)[std::size_t(x)] =
                        shadow_->row(y)[std::size_t(x) * 2 + 1];
                }
            }
        }
        std::vector<std::array<PolygonVertex, 3>> vertices;
        vertices.reserve(triangles_.size());
        std::vector<SortedPolygon> polygons;
        polygons.reserve(triangles_.size());
        for (const auto& triangle : triangles_) {
            auto v = triangle.vertices;
            const auto a = v[0].position, b = v[1].position, c = v[2].position;
            if ((double(b.x) - a.x) * (double(c.y) - a.y) -
                    (double(b.y) - a.y) * (double(c.x) - a.x) <
                0) {
                std::swap(v[1], v[2]);
            }
            vertices.push_back(v);
            if (triangle.softwareMaterial) {
                polygons.push_back({vertices.back(), *triangle.softwareMaterial});
                continue;
            }
            PolygonMaterial m;
            m.perspective = true;
            m.wrap = triangle.material.wrap;
            m.clampTexture = !m.wrap;
            m.shadeSource = triangle.material.shadeSource;
            m.shade = triangle.material.shade;
            m.hazeSource = triangle.material.hazeSource;
            m.haze = triangle.material.haze;
            if (triangle.material.texture == cube_.get()) {
                m.texture = softwareCube_.get();
            } else if (triangle.material.texture == floor_.get()) {
                m.texture = softwareFloor_.get();
                if (model_) {
                    m.shadeSource = PolygonSource::None;
                    m.hazeSource = PolygonSource::None;
                }
            } else if (triangle.material.texture == shadow_.get()) {
                m.texture = softwareShadow_.get();
                m.blend = true;
            } else {
                throw std::runtime_error("Unsupported demo software material");
            }
            polygons.push_back({vertices.back(), m});
        }
        software_->clear(nearest({.025f, .045f, .075f}));
        if (!software_->sortedPolygons(polygons)) {
            std::string detail;
            for (std::size_t i = 0; i < polygons.size(); ++i) {
                if (!software_->sortedPolygons(std::span(polygons).subspan(i, 1))) {
                    detail = std::format("; triangle {} (cube {}, floor {}, shadow {})",
                                         i,
                                         polygons[i].material.texture == softwareCube_.get(),
                                         polygons[i].material.texture == softwareFloor_.get(),
                                         polygons[i].material.texture == softwareShadow_.get());
                    break;
                }
            }
            throw std::runtime_error(std::format("Software scene rasterization failed at pose {}, "
                                                 "angle {} (yaw {}, pitch {}, distance {}){}",
                                                 frames_,
                                                 angle_,
                                                 yaw_,
                                                 pitch_,
                                                 distance_,
                                                 detail));
        }
        softwareImage_->pixels = software_->bitmap().pixels;
    }

    HardwareBatch frame() {
        const auto w = surface_->software().bitmap().width,
                   h = surface_->software().bitmap().height;
        const auto viewWidth = mode_ == RenderMode::Split ? w / 2 : w;
        scene(viewWidth, h);
        if (mode_ != RenderMode::Hardware) {
            softwareView();
            if (mode_ == RenderMode::Software) {
                triangles_.clear();
            }
            HardwareMaterial pixels;
            pixels.texture = softwareImage_.get();
            pixels.depth = HardwareDepth::Disabled;
            pixels.perspective = false;
            screenQuad(mode_ == RenderMode::Split ? Flt32(viewWidth) : 0,
                       0,
                       Flt32(w - (mode_ == RenderMode::Split ? viewWidth : 0)),
                       Flt32(h),
                       pixels);
        }
        // The HUD is ordinary textured geometry in the same hardware batch.
        HardwareMaterial panel;
        panel.depth = HardwareDepth::Disabled;
        panel.blend = HardwareBlend::Alpha;
        panel.color = {.018f, .028f, .045f};
        panel.opacity = .83f;
        panel.alphaSource = HardwareAlphaSource::Constant;
        screenQuad(0, Flt32(h) - 104, Flt32(w), 104, panel);
        text(28, 24, 2.5f, "OPENT1 / RENDERER VIEWER", {.8f, .9f, 1});
        if (mode_ != RenderMode::Software) {
            text(
                28, 58, 1.5f, std::format("HARDWARE / {}", surface_->driver()), {.35f, .75f, .95f});
        }
        if (mode_ != RenderMode::Hardware) {
            text(mode_ == RenderMode::Split ? Flt32(viewWidth) + 28 : 28,
                 58,
                 1.5f,
                 "SOFTWARE / 8 BIT CPU",
                 {.95f, .7f, .35f});
        }
        const auto activeSequence =
            modelInstance_ && !modelInstance_->threads().empty() && !animationSweep_
                ? modelInstance_->threads()[0].sequence
                : sequence_;
        const auto nameIndex = model_ && animate_ && activeSequence >= 0
                                   ? model_->shape->sequences[std::size_t(activeSequence)].name
                                   : -1;
        const auto poseName =
            nameIndex >= 0 ? model_->shape->names[std::size_t(nameIndex)] : "DEFAULT POSE";
        text(28,
             82,
             1.3f,
             model_ ? std::format("{} / {} / {} / LOD {} / SHADOW {}",
                                  paused_ ? "PAUSED" : "PLAYING",
                                  modelName_,
                                  poseName,
                                  modelFrame_->detail(),
                                  shadows_ ? "ON" : "OFF")
                    : std::format("{} / SHADOW {}",
                                  paused_ ? "PAUSED" : "ROTATING",
                                  shadows_ ? "ON" : "OFF"),
             {.4f, .65f, .8f});
        if (modelInstance_ && !modelInstance_->threads().empty()) {
            const auto p = modelInstance_->rootMotion().p;
            text(28,
                 103,
                 1.1f,
                 std::format("{} / ROOT {:.2f} {:.2f} {:.2f} / EVENTS {} / M: MOTION / B: REVERSE",
                             modelInstance_->threads()[0].state == world::TsThreadState::Transition
                                 ? "TRANSITION"
                                 : (reverse_ ? "REVERSE" : "FORWARD"),
                             p.x,
                             p.y,
                             p.z,
                             triggerCount_),
                 {.4f, .65f, .8f});
        }
        text(28,
             Flt32(h) - 82,
             1.5f,
             "1: HARDWARE   2: SOFTWARE   3: SPLIT   TAB: CYCLE",
             {.75f, .85f, .94f});
        text(28, Flt32(h) - 55, 1.4f, "DRAG: ORBIT   WHEEL: ZOOM   SPACE: PAUSE", {.4f, .6f, .75f});
        text(28,
             Flt32(h) - 29,
             1.4f,
             model_ ? "A: ANIMATE   N/P: SEQUENCE   D: LOD   L: LIGHT   S: SHADOW   R: RESET   "
                      "ESC: CLOSE"
                    : "ARROWS: ORBIT   S: SHADOWS   R: RESET   ESC: CLOSE",
             {.4f, .6f, .75f});
        if (mode_ == RenderMode::Split) {
            panel.color = {.35f, .55f, .7f};
            panel.opacity = .5f;
            screenQuad(Flt32(viewWidth), 112, 1, Flt32(h) - 216, panel);
        }
        std::vector<HardwarePolygon> polygons;
        polygons.reserve(triangles_.size());
        for (const auto& t : triangles_) {
            polygons.push_back({t.vertices, t.material});
        }
        auto batch = HardwareBatch::prepare(polygons, modelPalette_.get());
        require(bool(batch), "Viewer frame preparation failed");
        return std::move(*batch);
    }

    std::vector<std::byte> pixels() {
        std::vector<std::byte> result(std::size_t(surface_->software().bitmap().width) *
                                      std::size_t(surface_->software().bitmap().height) * 4);
        require(surface_->readback(result), SDL_GetError());
        return result;
    }

    void snapshot(const std::vector<std::byte>& rgba) {
        auto* image = SDL_CreateSurfaceFrom(surface_->software().bitmap().width,
                                            surface_->software().bitmap().height,
                                            SDL_PIXELFORMAT_RGBA32,
                                            const_cast<std::byte*>(rgba.data()),
                                            surface_->software().bitmap().width * 4);
        require(image != nullptr, SDL_GetError());
        const bool saved = SDL_SaveBMP(image, snapshot_.c_str());
        SDL_DestroySurface(image);
        require(saved, SDL_GetError());
    }

    void onIdle() override {
        auto* window = findWindow(window_);
        if (!window || !surface_->ready()) {
            requestQuit();
            return;
        }
        const auto state = window->state();
        if (state.minimized) {
            return;
        }
        require(surface_->resize(state.width, state.height), SDL_GetError());
        const auto now = SDL_GetTicksNS();
        const auto delta = std::min(.1f, Flt32(double(now - lastTime_) / 1e9));
        lastTime_ = now;
        if (!paused_ && (!smoke_ || frames_ > 0)) {
            angle_ += smoke_ ? .15f : delta * .32f;
            if (animate_) {
                const auto step = smoke_ ? .1 : Flt64(delta);
                animationTime_ += step;
                if (modelInstance_ && !modelInstance_->threads().empty() && !animationSweep_) {
                    require(modelInstance_->advance(step),
                            "TS playback failed: invalid animation, motion or trigger metadata");
                    triggerCount_ += modelInstance_->triggers().size();
                }
            }
        }
        if (animationSweep_) {
            sequence_ = Int32(frames_ / 5);
            animate_ = true;
            animationTime_ = Flt64(model_->shape->sequences[std::size_t(sequence_)].duration) *
                             Flt64(frames_ % 5) / 4;
        }
        updatePose();
        if (sweep_) {
            angle_ = Flt32(frames_) * .137f;
            yaw_ = .65f + Flt32(frames_) * .23f;
            pitch_ = .12f + Flt32(frames_ % 7) * .2f;
            distance_ = 4 + Flt32(frames_ % 5) * 3.5f;
            if (frames_ >= 96) {
                angle_ = frames_ == 96 ? 2.0770462f : Flt32(frames_ - 96) * 6.2831853f / 95;
                yaw_ = .65f;
                pitch_ = .35f;
                distance_ = 7.5f;
            }
        }
        GpuFrameConfig config;
        config.clearColor = {.025f, .045f, .075f};
        config.hazeColor = {.07f, .11f, .16f};
        require(surface_->render(frame(), config), SDL_GetError());
        if (animationSweep_) {
            if (++frames_ == model_->shape->sequences.size() * 5) {
                std::printf("Validated %u animated positions across all %zu sequences in the "
                            "selected renderer mode.\n",
                            frames_,
                            model_->shape->sequences.size());
                requestQuit();
            }
            return;
        }
        if (sweep_) {
            if (++frames_ == 192) {
                std::puts("Validated 192 camera/rotation/zoom poses.");
                requestQuit();
            }
            return;
        }
        if (frames_ == 0 && (smoke_ || !snapshot_.empty())) {
            firstFrame_ = pixels();
            if (!snapshot_.empty()) {
                snapshot(firstFrame_);
            }
            firstShadow_ = shadow_->pixels;
            const auto w = surface_->software().bitmap().width,
                       h = surface_->software().bitmap().height;
            const auto views = mode_ == RenderMode::Split ? 2 : 1, viewWidth = w / views;
            if (model_) {
                require(modelFrame_ && modelFrame_->polygonCount() > 0, "No visible TS polygons");
                drawModel_ = false;
                require(surface_->render(frame(), config), SDL_GetError());
                const auto background = pixels();
                drawModel_ = true;
                require(surface_->render(frame(), config), SDL_GetError());
                const auto litInputs = modelFrame_->hardwarePolygons(modelPalette_.get());
                require(bool(litInputs), "Invalid TS lighting inputs");
                const bool shaded = std::ranges::any_of(*litInputs, [](const auto& p) {
                    return p.material.shadeSource != PolygonSource::None;
                });
                modelLighting_ = !modelLighting_;
                require(surface_->render(frame(), config), SDL_GetError());
                const auto alternateLight = pixels();
                modelLighting_ = !modelLighting_;
                require(surface_->render(frame(), config), SDL_GetError());
                std::ranges::for_each(std::views::iota(0, views), [&](auto view) {
                    std::size_t changed{}, lightChanged{};
                    std::ranges::for_each(std::views::iota(h / 4, h * 3 / 4), [&](auto y) {
                        std::ranges::for_each(
                            std::views::iota(view * viewWidth + viewWidth / 4,
                                             view * viewWidth + viewWidth * 3 / 4),
                            [&](auto x) {
                                const auto at = std::size_t(y * w + x) * 4;
                                if (!std::ranges::equal(std::span(firstFrame_).subspan(at, 3),
                                                        std::span(background).subspan(at, 3))) {
                                    ++changed;
                                }
                                if (!std::ranges::equal(std::span(firstFrame_).subspan(at, 3),
                                                        std::span(alternateLight).subspan(at, 3))) {
                                    ++lightChanged;
                                }
                            });
                    });
                    require(changed > std::size_t(viewWidth * h) / 2000,
                            "Loaded model did not occupy the expected view region");
                    require(!shaded || lightChanged > 0,
                            "TS lighting did not change the displayed model");
                    std::printf("TS view %d: %zu visible triangles, %zu model pixels, %zu lighting "
                                "pixels.\n",
                                view,
                                modelFrame_->polygonCount(),
                                changed,
                                lightChanged);
                });
            } else {
                for (Int32 view = 0; view < views; ++view) {
                    std::size_t litPixels{};
                    for (Int32 y = h / 4; y < h * 3 / 4; ++y) {
                        for (Int32 x = view * viewWidth + viewWidth / 4;
                             x < view * viewWidth + viewWidth * 3 / 4;
                             ++x) {
                            const auto offset = std::size_t(y * w + x) * 4;
                            const auto r = std::to_integer<UInt8>(firstFrame_[offset]),
                                       g = std::to_integer<UInt8>(firstFrame_[offset + 1]),
                                       b = std::to_integer<UInt8>(firstFrame_[offset + 2]);
                            if (b > 60 && g > 40 && b > r * 2) {
                                ++litPixels;
                            }
                        }
                    }
                    require(litPixels > std::size_t(viewWidth * h) / 100,
                            "Lit cube did not occupy the expected view region");
                }
            }
        }
        if (smoke_) {
            ++frames_;
            // Route scripted controls through the same owned event queue as
            // interactive input. Keep the smoke run bounded and hidden.
            if (frames_ == 1) {
                SDL_Event event{};
                event.type = SDL_EVENT_MOUSE_MOTION;
                event.motion.windowID = window_;
                event.motion.state = SDL_BUTTON_LMASK;
                event.motion.xrel = 20;
                event.motion.yrel = 10;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 2) {
                require(std::abs(yaw_ - .49f) < .0001f && std::abs(pitch_ - .43f) < .0001f,
                        "Mouse orbit did not apply");
                require(shadow_->pixels != firstShadow_,
                        "Projected shadow did not follow object rotation");
                SDL_Event event{};
                event.type = SDL_EVENT_MOUSE_WHEEL;
                event.wheel.windowID = window_;
                event.wheel.y = 1;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 3 || frames_ == 4) {
                require(distance_ < 7.5f, "Wheel zoom did not apply");
                if (frames_ == 4) {
                    require(paused_, "Pause did not apply");
                }
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.key = frames_ == 3 ? SDLK_SPACE : SDLK_R;
                event.key.down = true;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 5) {
                require(!paused_ && distance_ == 7.5f && yaw_ == .65f, "Reset did not apply");
            }
            if (frames_ == 6) {
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.key = SDLK_S;
                event.key.down = true;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 7) {
                require(!shadows_, "Shadow toggle did not apply");
                const auto unshadowed = pixels();
                shadows_ = true;
                require(surface_->render(frame(), config), SDL_GetError());
                const auto shadowed = pixels();
                std::array<std::size_t, 2> darkened{};
                const auto w = surface_->software().bitmap().width,
                           h = surface_->software().bitmap().height;
                for (Int32 y = 112; y < h - 104; ++y) {
                    for (Int32 x = 0; x < w; ++x) {
                        const auto offset = std::size_t(y * w + x) * 4;
                        if (int(std::to_integer<UInt8>(unshadowed[offset])) -
                                int(std::to_integer<UInt8>(shadowed[offset])) >
                            3) {
                            ++darkened[mode_ == RenderMode::Split && x >= w / 2 ? 1U : 0U];
                        }
                    }
                }
                require(darkened[0] > 100 && (mode_ != RenderMode::Split || darkened[1] > 100),
                        "Projected shadow did not darken each renderer's floor");
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.key = SDLK_TAB;
                event.key.down = true;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 8) {
                require(mode_ == RenderMode((UInt8(smokeMode_) + 1) % 3),
                        "Renderer mode cycle did not apply");
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.key = smokeMode_ == RenderMode::Hardware   ? SDLK_1
                                : smokeMode_ == RenderMode::Software ? SDLK_2
                                                                     : SDLK_3;
                event.key.down = true;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 9) {
                require(mode_ == smokeMode_, "Renderer mode selection did not apply");
                require(pixels() != firstFrame_, "Object rotation did not change the frame");
                if (!model_ || model_->shape->sequences.empty()) {
                    std::puts("Validated geometry, orbit/zoom/pause/reset, moving shadows, shadow "
                              "on/off readback and renderer switching.");
                    requestQuit();
                } else {
                    smokeAnimation_ = animate_;
                    smokeAutomaticDetail_ = automaticDetail_;
                    smokeSequence_ = std::max(0, sequence_);
                    SDL_Event event{};
                    event.type = SDL_EVENT_KEY_DOWN;
                    event.key.windowID = window_;
                    event.key.key = SDLK_A;
                    event.key.down = true;
                    require(SDL_PushEvent(&event), SDL_GetError());
                }
            }
            if (frames_ == 10 || frames_ == 11 || frames_ == 12 || frames_ == 13) {
                const auto count = Int32(model_->shape->sequences.size());
                if (frames_ == 10) {
                    require(animate_ != smokeAnimation_, "Animation toggle did not apply");
                }
                if (frames_ == 11) {
                    require(animate_ && sequence_ == (smokeSequence_ + 1) % count,
                            "Next sequence did not apply");
                }
                if (frames_ == 12) {
                    require(animate_ && sequence_ == smokeSequence_,
                            "Previous sequence did not apply");
                }
                if (frames_ == 13) {
                    require(automaticDetail_ != smokeAutomaticDetail_,
                            "Automatic detail toggle did not apply");
                }
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.down = true;
                event.key.key = frames_ == 10 ? SDLK_N : frames_ == 11 ? SDLK_P : SDLK_D;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 14) {
                require(automaticDetail_ == smokeAutomaticDetail_,
                        "Explicit detail restore did not apply");
                smokeRootMotion_ = rootMotion_;
                smokeReverse_ = reverse_;
            }
            if (frames_ >= 14 && frames_ <= 17) {
                if (frames_ == 15) {
                    require(rootMotion_ != smokeRootMotion_, "Root-motion toggle did not apply");
                }
                if (frames_ == 16) {
                    require(rootMotion_ == smokeRootMotion_, "Root-motion restore did not apply");
                }
                if (frames_ == 17) {
                    require(reverse_ != smokeReverse_, "Reverse playback did not apply");
                    require(modelInstance_->threads()[0].timeScale == (reverse_ ? -1.f : 1.f),
                            "Thread playback direction did not apply");
                }
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.down = true;
                event.key.key = frames_ <= 15 ? SDLK_M : SDLK_B;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 18) {
                require(reverse_ == smokeReverse_, "Playback direction restore did not apply");
                smokeModelLighting_ = modelLighting_;
            }
            if (frames_ == 18 || frames_ == 19) {
                if (frames_ == 19) {
                    require(modelLighting_ != smokeModelLighting_, "Lighting toggle did not apply");
                }
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window_;
                event.key.down = true;
                event.key.key = SDLK_L;
                require(SDL_PushEvent(&event), SDL_GetError());
            }
            if (frames_ == 20) {
                require(modelLighting_ == smokeModelLighting_, "Lighting restore did not apply");
                std::puts("Validated loaded TS geometry/shadows/lighting, camera/render controls, "
                          "sequences, LOD, root-motion toggle and reverse playback.");
                requestQuit();
            }
        } else {
            ++frames_;
            require(surface_->presentFrame() != PresentResult::Error, SDL_GetError());
        }
    }

    void onExit(Int32 code) override {
        if (code != 0) {
            std::fprintf(stderr, "%s\n", error().c_str());
        }
        surface_.reset();
    }
};
}

SDL_AppResult SDL_AppInit(void** state, int argc, char** argv) {
    return initApplication<RendererDemo>(state, argc, argv);
}

SDL_AppResult SDL_AppIterate(void* state) {
    return iterateApplication(state);
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event) {
    return handleApplicationEvent(state, event);
}

void SDL_AppQuit(void* state, SDL_AppResult result) {
    quitApplication(state, result);
}
