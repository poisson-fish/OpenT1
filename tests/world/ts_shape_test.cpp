#include "ts_test_support.hpp"
#include <opent1/world/ts_shape_pose.hpp>
#include <opent1/world/ts_shape_instance.hpp>
#include <opent1/world/ts_collision.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/vol_stream.hpp>
#include <cstdio>

namespace {
using namespace opent1;
using namespace opent1::world;
using namespace opent1::test::ts;

void skins() {
    ResourceManager manager;
    for (const auto name :
         {"base.body.bmp", "base.trim.bmp", "metal.bmp", "storm.body.bmp", "storm.model.bmp"}) {
        CHECK(manager.add(name, gfx::Bitmap::create(4, 4)));
    }
    TsMaterialList original;
    original.details = 2;
    original.materialsPerDetail = 4;
    original.materials = {{0x103, .5f, 0, {}, "base.body.bmp"},
                          {0x1103, 1, 0, {}, "metal.bmp"},
                          {0x2103, 1, 0, {}, "base.trim.bmp"},
                          {0x102, 1, 0, {12, 34, 56, 0}, "base.solid.bmp"}};
    const auto detail = original.materials;
    original.materials.insert(original.materials.end(), detail.begin(), detail.end());
    original.materials[0].surfaceType = 7;
    original.materials[0].friction = .25f;
    const auto source = TsMaterialResources::load(manager, original);
    CHECK(source);
    const auto skinned = TsMaterialResources::loadSkin(manager, original, "storm", "model.dts");
    CHECK(skinned && skinned->list.details == 2 && skinned->list.materialsPerDetail == 4);
    for (std::size_t offset : {0U, 4U}) {
        CHECK(skinned->list.materials[offset].mapFile == "storm.body.bmp");
        CHECK(skinned->textures[offset].get() == manager.load<gfx::Bitmap>("storm.body.bmp").get());
        // Each base.* slot keeps its own suffix and falls back independently.
        CHECK(skinned->list.materials[offset + 2].mapFile == "base.trim.bmp");
        for (std::size_t slot = offset + 1; slot < offset + 4; ++slot) {
            CHECK(skinned->list.materials[slot].mapFile == original.materials[slot].mapFile);
            CHECK(skinned->textures[slot].get() == source->textures[slot].get());
        }
    }
    CHECK(skinned->list.materials[0].alpha == .5f && skinned->list.materials[0].surfaceType == 7 &&
          skinned->list.materials[0].friction == .25f);
    CHECK(hash(original) == hash(source->list)); // the shared shape list stays unchanged
    for (const auto skin : {"missing", "base", ""}) {
        const auto fallback = TsMaterialResources::loadSkin(manager, original, skin, "model.dts");
        CHECK(fallback && hash(fallback->list) == hash(original));
    }
    // An explicit DML may intentionally replace every slot, including custom materials.
    auto replacement = std::make_unique<TsMaterialList>(original);
    for (auto& material : replacement->materials) {
        if (material.type() == 3) {
            material.mapFile = "metal.bmp";
        }
    }
    const auto replacementHash = hash(*replacement);
    CHECK(manager.add("listed.model.dml", std::move(replacement)));
    const auto listed = TsMaterialResources::loadSkin(manager, original, "listed", "model.dts");
    CHECK(listed && hash(listed->list) == replacementHash);
    // An unusable DML falls back to the individual original slots.
    auto invalid = std::make_unique<TsMaterialList>(original);
    invalid->materials[0].mapFile = "absent.bmp";
    CHECK(manager.add("broken.model.dml", std::move(invalid)));
    const auto fallback = TsMaterialResources::loadSkin(manager, original, "broken", "model");
    CHECK(fallback && hash(fallback->list) == hash(original));
    auto missing = original;
    missing.materials[1].mapFile = "absent.bmp";
    CHECK(!TsMaterialResources::loadSkin(manager, missing, "storm", "model"));
}

void damageSkins() {
    ResourceManager manager;
    auto base = gfx::Bitmap::create(8, 8);
    base->detailLevels = 2;
    base->pixels.assign(80, std::byte{1});
    base->paletteIndex = 7;
    CHECK(base->valid());
    CHECK(manager.add("armor.bmp", std::move(base)));
    auto decal = gfx::Bitmap::create(2, 2);
    decal->row(0)[0] = std::byte{};
    decal->row(0)[1] = std::byte{2};
    decal->row(1)[0] = std::byte{3};
    decal->row(1)[1] = std::byte{4};
    CHECK(manager.add("scar.bmp", std::move(decal)));
    CHECK(manager.add("oversized.bmp", gfx::Bitmap::create(8, 8)));
    TsMaterialList list;
    list.details = 1;
    list.materialsPerDetail = 3;
    list.materials = {{0x103, 1, 0, {}, "armor.bmp"},
                      {0x1103, 1, 0, {}, "armor.bmp"},
                      {0x2103, 1, 0, {}, "armor.bmp"}};
    const auto source = TsMaterialResources::load(manager, list);
    CHECK(source);
    const std::array names{std::string("scar")};
    // Independent expected stamps from the legacy DWORD coordinate sequence.
    // Index zero leaves the destination unchanged; lower mip levels are copied.
    const std::array<std::array<std::string_view, 8>, 3> expected{{{{"11121111",
                                                                     "11342111",
                                                                     "11134211",
                                                                     "11113421",
                                                                     "11111341",
                                                                     "11111111",
                                                                     "11111111",
                                                                     "11111111"}},
                                                                   {{"11121211",
                                                                     "11343421",
                                                                     "12134341",
                                                                     "34213421",
                                                                     "13411341",
                                                                     "11111111",
                                                                     "11111111",
                                                                     "11111111"}},
                                                                   {{"12121211",
                                                                     "34243421",
                                                                     "13424341",
                                                                     "34343421",
                                                                     "13411341",
                                                                     "11112111",
                                                                     "11134111",
                                                                     "11111111"}}}};
    for (Int32 level = 1; level <= 3; ++level) {
        const auto damaged = TsMaterialResources::withDamageSkin(manager, *source, level, names);
        CHECK(damaged);
        const auto& bitmap = *damaged->textures[0];
        CHECK(bitmap.valid() && bitmap.paletteIndex == 7 && bitmap.detailLevels == 2);
        for (Int32 y = 0; y < 8; ++y) {
            for (Int32 x = 0; x < 8; ++x) {
                CHECK(std::to_integer<int>(bitmap.row(y)[x]) == expected[level - 1][y][x] - '0');
            }
        }
        CHECK(std::ranges::all_of(bitmap.level(1), [](auto pixel) {
            return pixel == std::byte{1};
        }));
        CHECK(damaged->textures[1].get() == source->textures[1].get());
        CHECK(damaged->textures[2].get() == source->textures[2].get());
        const auto cached = TsMaterialResources::withDamageSkin(manager, *source, level, names);
        CHECK(cached && cached->textures[0].get() == damaged->textures[0].get());
    }
    CHECK(std::ranges::all_of(source->textures[0]->pixels, [](auto pixel) {
        return pixel == std::byte{1};
    }));
    const auto clean = TsMaterialResources::withDamageSkin(manager, *source, 0, names);
    CHECK(clean && clean->textures[0].get() == source->textures[0].get());
    CHECK(!TsMaterialResources::withDamageSkin(manager, *source, 4, names));
    // An unavailable first bitmap ends the legacy usable prefix.
    const std::array missing{std::string("missing"), std::string("scar")};
    const auto absent = TsMaterialResources::withDamageSkin(manager, *source, 1, missing);
    CHECK(absent && absent->textures[0].get() == source->textures[0].get());
    ResourceManager oversizeManager;
    CHECK(oversizeManager.add("armor.bmp", std::make_unique<gfx::Bitmap>(*source->textures[0])));
    CHECK(oversizeManager.add("oversized.bmp", gfx::Bitmap::create(8, 8)));
    const auto oversizedSource = TsMaterialResources::load(oversizeManager, list);
    CHECK(oversizedSource);
    const std::array oversized{std::string("oversized")};
    const auto skipped =
        TsMaterialResources::withDamageSkin(oversizeManager, *oversizedSource, 3, oversized);
    CHECK(skipped && skipped->textures[0]->pixels == source->textures[0]->pixels);
}

void unit() {
    skins();
    damageSkins();
    for (Int32 version = 0; version <= 8; ++version) {
        const auto bytes = shape(version, 3, 4, 0, true);
        const auto historical = load(bytes);
        CHECK(historical && historical->version == version);
        CHECK(historical->defaultMaterials == (version < 5 ? 0 : 1) &&
              historical->alwaysNode == -1);
        CHECK(historical->bounds.fMin == Point3F(-1, 0, -1));
        CHECK(historical->nodes.size() == 2 && historical->objects.size() == 2);
        CHECK(historical->sequences.size() == 1 && historical->subSequences.size() == 1);
        CHECK(historical->sequences[0].duration == 1.25f && historical->sequences[0].priority == 7);
        CHECK(historical->sequences[0].firstIfl == 0 && historical->sequences[0].triggerCount == 0);
        CHECK(historical->keyframes.size() == 1 && historical->keyframes[0].position == .25f);
        CHECK(historical->keyframes[0].value == 1 &&
              historical->keyframes[0].materialFlags == 0xd000);
        std::vector<std::byte> storage(8192);
        MemRWStream output{std::span<std::byte>(storage)};
        CHECK(historical->write(output));
        Bytes encoded;
        encoded.data.assign(storage.begin(), storage.begin() + output.getPosition());
        const auto restored = load(encoded);
        CHECK(restored && restored->version == 8);
        CHECK(restored->sequences[0].duration == 1.25f &&
              restored->keyframes[0].materialFlags == 0xd000);
        CHECK(hash(restored->meshes[0]) == hash(historical->meshes[0]));
        CHECK(hash(*restored->materials) == hash(*historical->materials));
        historical->names[0] = std::string(25, 'x');
        output.setPosition(0);
        CHECK(!historical->write(output) && output.getPosition() == 0);
        auto truncated = bytes;
        truncated.data.pop_back();
        CHECK(!load(truncated));
    }
    {
        const auto list = loadMaterial(material());
        CHECK(list);
        std::array<std::byte, 1024> storage{};
        MemRWStream output{std::span(storage)};
        CHECK(list->write(output));
        // Independent hand-authored native layout, including every output byte.
        CHECK(std::ranges::equal(std::span(storage).first(output.getPosition()), material().data));
        list->materials[0].mapFile = std::string(33, 'x');
        output.setPosition(0);
        CHECK(!list->write(output) && output.getPosition() == 0);
    }
    {
        const auto source = load(shape(8, 3, 4, 0, true));
        const auto target = load(shape());
        CHECK(source && target);
        source->names[1] = "DETAIL"; // native import maps names without case
        source->radius = 5;
        source->bounds.fMax.z = 4;
        source->triggers.push_back({.75f, 42});
        source->sequences[0].triggerCount = 1;
        // Add an object frame track alongside the node transform track.
        source->objects[1].firstSubSequence = 1;
        source->objects[1].subSequenceCount = 1;
        source->subSequences.push_back({0, 1, 1});
        source->keyframes.push_back({.5f, 0, 0xf003});
        const auto oldMeshSize = target->meshes[0].vertices.size();
        CHECK(target->importSequence(*source, 0));
        CHECK(target->sequences.size() == 1 && target->names.back() == "DETAIL");
        CHECK(target->radius == 5 && target->bounds.fMax.z == 4);
        CHECK(target->triggers.size() == 1 && target->triggers[0].value == 42);
        const auto& nodeSub = target->subSequences[target->nodes[1].firstSubSequence];
        const auto& nodeKey = target->keyframes[nodeSub.firstKey];
        CHECK(nodeKey.materialFlags == 0x8000 &&
              target->transforms[nodeKey.value].translation == Point3F{});
        const auto& objectSub = target->subSequences[target->objects[1].firstSubSequence];
        const auto& objectKey = target->keyframes[objectSub.firstKey];
        CHECK(objectKey.materialFlags == 0xf000 && objectKey.value == 1);
        CHECK(target->meshes[0].vertices.size() ==
              oldMeshSize + source->meshes[0].verticesPerFrame);
        CHECK(target->meshes[0].point(2, 1) == source->meshes[0].point(2, 0));
        CHECK(!target->importSequence(*source, -1) && !target->importSequence(*source, 1));
        source->names[0] = "missing";
        CHECK(!target->importSequence(*source, 0) && target->sequences.size() == 1);
        std::vector<std::byte> storage(8192);
        MemRWStream output{std::span<std::byte>(storage)};
        CHECK(target->write(output));
        MemRWStream input{std::span<const std::byte>(storage).first(output.getPosition())};
        CHECK(TsShape::load(input, output.getPosition()));
    }
    std::ranges::for_each(std::views::iota(0, 36), [&](auto test) {
        const auto version = 5 + test / 9, meshVersion = 1 + (test / 3) % 3,
                   matVersion = 2 + test % 3;
        auto s = load(shape(version, meshVersion, matVersion));
        CHECK(s);
        CHECK(s->version == version && s->meshes[0].version == meshVersion &&
              s->materials->version == matVersion);
        CHECK(s->nodes.size() == 2 && s->objects.size() == 2 && s->details[0].root == 1);
        CHECK(s->meshes[0].point(2) == Point3F(-1, 0, -1));
        CHECK(!s->meshes[0].point(5));
        CHECK(!s->meshes[0].point(0, -1));
        CHECK(s->dependencies() == std::vector<std::string>{"test.bmp"});
        CHECK(s->materials->materials[0].rgb[0] == 16 && s->transforms[0].translation.x == 5);
        CHECK(s->materials->materials[0].friction == (matVersion == 2 ? 1 : .25f));
    });
    {
        FileRStream nativeV1(std::filesystem::path(OPENT1_TS_FORMAT_FIXTURE).parent_path() /
                             "material-v1.dml");
        CHECK(nativeV1() == STRM_OK);
        const auto old = TsMaterialList::load(nativeV1, nativeV1.getSize());
        CHECK(old && old->version == 1 && old->materials.size() == 1);
        CHECK(old->materials[0].mapFile == "123456789012345678901234567.bmp" &&
              old->materials[0].alpha == .5f);
        CHECK(old->materials[0].surfaceType == 0 && old->materials[0].elasticity == 1 &&
              old->materials[0].friction == 1 && old->materials[0].useDefaultProperties);
        CHECK(loadMaterial(material(1)) && load(shape(8, 3, 1)));
        auto shortV1 = material(1);
        shortV1.data.resize(shortV1.data.size() - 16);
        CHECK(!loadMaterial(shortV1));
    }
    FileRStream formats(OPENT1_TS_FORMAT_FIXTURE);
    CHECK(formats() == STRM_OK);
    const auto word = [&] {
        UInt32 v{};
        CHECK(formats.read(v));
        return v;
    };
    CHECK(word() == 0x4654314f && word() == 1 && word() == 37);
    for (Int32 test = 0; test < 36; ++test) {
        const auto s = load(shape(5 + test / 9, 1 + (test / 3) % 3, 2 + test % 3));
        CHECK(s);
        CHECK(hash(s->meshes[0]) == word());
        CHECK(hash(*s->materials) == word());
    }
    const auto skipped = load(shape(8, 3, 4, -1));
    CHECK(skipped && skipped->meshes[0].faces[0].material == -1);
    CHECK(hash(skipped->meshes[0]) == word());
    CHECK(hash(*skipped->materials) == word());
    CHECK(UInt32(formats.getPosition()) == formats.getSize());
    const auto good = shape();
    std::ranges::for_each(std::views::iota(std::size_t{0}, good.data.size()), [&](auto size) {
        auto truncated = good;
        truncated.data.resize(size);
        CHECK(!load(truncated));
    });
    const auto corrupt = [&](std::size_t at, UInt32 value) {
        auto b = good;
        Bytes n;
        n.put(value);
        std::ranges::copy(n.data, b.data.begin() + std::ptrdiff_t(at));
        CHECK(!load(b));
    };
    corrupt(20, 99); // unsupported DTS version
    corrupt(24, 0xffffffffU);
    corrupt(24, 0x7fffffffU); // negative/oversized node count
    corrupt(68, 0x7fc00000U); // nonfinite radius
    auto trailing = good;
    trailing.data.push_back(std::byte{});
    CHECK(load(trailing));
    auto wrong = good;
    wrong.data[10] = std::byte{'X'};
    CHECK(!load(wrong));
    auto cycle = good;
    cycle.data[110] = std::byte{1};
    cycle.data[111] = std::byte{0};
    CHECK(!load(cycle));
    auto m = material();
    m.data[26] = std::byte{99};
    CHECK(!loadMaterial(m));
    const auto dml = material();
    std::ranges::for_each(std::views::iota(std::size_t{0}, dml.data.size()), [&](auto size) {
        auto truncated = dml;
        truncated.data.resize(size);
        CHECK(!loadMaterial(truncated));
    });
    auto v6 = shape(6);
    Bytes fraction;
    fraction.real(.123456f);
    std::ranges::copy(fraction.data, v6.data.begin() + 124);
    auto upgraded = load(v6);
    CHECK(upgraded);
    CHECK(upgraded->transforms[0].rotation.x == Flt32(Int16(.123456f * 32767)) / 32767);
    auto valid = load(good);
    ResourceManager manager;
    CHECK(registerTsLoaders(manager));
    CHECK(gfx::registerBitmapFontLoaders(manager));
    CHECK(manager.add<TsShape>("model.dts", std::move(valid)));
    CHECK(!TsShapeResources::load(manager, "model.dts")); // missing bitmap must fail as a unit
    CHECK(manager.add<gfx::Bitmap>("test.bmp", gfx::Bitmap::create(4, 4)));
    auto retained = TsShapeResources::load(manager, "MODEL.DTS");
    CHECK(retained);
    CHECK(retained->materials.textures[0] && retained->shape->nodes.size() == 2);
}

void assets(const std::filesystem::path& directory) {
    FileRStream fixture(OPENT1_TS_ASSET_FIXTURE);
    CHECK(fixture() == STRM_OK);
    const auto word = [&] {
        UInt32 v{};
        CHECK(fixture.read(v));
        return v;
    };
    const auto text = [&] {
        const auto n = word();
        CHECK(n <= 1024);
        std::string s(n, '\0');
        CHECK(fixture.readBytes(std::as_writable_bytes(std::span(s))));
        return s;
    };
    CHECK(word() == 0x5354314fU && word() == 1);
    const auto count = word();
    CHECK(count == 475);
    UInt32 failures{}, sampled{}, rejectedTracks{}, advanced{}, rejectedPlayback{},
        transitionSamples{}, collisionSnapshots{};
    std::ranges::for_each(std::views::iota(0U, count), [&](auto) {
        const auto archive = text(), name = text();
        const auto size = word(), checksum = word();
        VolumeRStream volume;
        CHECK(volume.openVolume(directory / archive));
        CHECK(volume.open(name));
        CHECK(volume.data().size() == size);
        Hash raw;
        std::ranges::for_each(volume.data(), [&](auto b) {
            raw.put(std::to_integer<UInt8>(b));
        });
        CHECK(raw.value == checksum);
        const bool isShape = name.ends_with(".DTS") || name.ends_with(".dts");
        const auto wordCount = isShape ? 18U : 4U;
        std::vector<UInt32> expected(wordCount);
        std::ranges::generate(expected, word);
        auto s = isShape ? TsShape::load(volume, size) : nullptr;
        auto m = isShape ? nullptr : TsMaterialList::load(volume, size);
        const bool decoded = isShape ? bool(s) : bool(m);
        if (!decoded) {
            std::fprintf(stderr, "Rejected original asset: %s/%s\n", archive.c_str(), name.c_str());
            ++failures;
        }
        if (s) {
            const std::array<UInt32, 14> actual{UInt32(s->version),
                                                UInt32(s->nodes.size()),
                                                UInt32(s->sequences.size()),
                                                UInt32(s->subSequences.size()),
                                                UInt32(s->keyframes.size()),
                                                UInt32(s->transforms.size()),
                                                UInt32(s->names.size()),
                                                UInt32(s->objects.size()),
                                                UInt32(s->details.size()),
                                                UInt32(s->meshes.size()),
                                                UInt32(s->transitions.size()),
                                                UInt32(s->triggers.size()),
                                                UInt32(s->defaultMaterials),
                                                UInt32(s->alwaysNode)};
            CHECK(std::ranges::equal(actual, std::span(expected).first(14)));
        }
        const auto* list = s ? (s->materials ? &*s->materials : nullptr) : m.get();
        const auto matAt = isShape ? 14U : 0U;
        if (list) {
            CHECK(UInt32(list->version) == expected[matAt] &&
                  UInt32(list->details) == expected[matAt + 1] &&
                  UInt32(list->materialsPerDetail) == expected[matAt + 2] &&
                  hash(*list) == expected[matAt + 3]);
        }
        if (isShape) {
            std::ranges::for_each(std::views::iota(0U, expected[9]), [&](auto i) {
                std::array<UInt32, 9> values{};
                std::ranges::generate(values, word);
                if (!s) {
                    return;
                }
                const auto& mesh = s->meshes[i];
                const std::array<UInt32, 9> actual{UInt32(mesh.version),
                                                   UInt32(mesh.vertices.size()),
                                                   UInt32(mesh.verticesPerFrame),
                                                   UInt32(mesh.textureVertices.size()),
                                                   UInt32(mesh.textureVerticesPerFrame),
                                                   UInt32(mesh.faces.size()),
                                                   UInt32(mesh.frames.size()),
                                                   std::bit_cast<UInt32>(mesh.radius),
                                                   hash(mesh)};
                CHECK(values == actual);
            });
        }
        const auto deps = word();
        std::vector<std::string> dependencies(deps);
        std::ranges::generate(dependencies, text);
        if (decoded) {
            CHECK((s ? s->dependencies() : m->dependencies()) == dependencies);
        }
        if (s) {
            const auto nonfinite = std::ranges::any_of(s->keyframes, [](const auto& k) {
                return !std::isfinite(k.position);
            });
            ResourceManager manager;
            CHECK(manager.add<TsShape>("sample.dts", std::move(s)));
            auto resource = manager.load<TsShape>("sample.dts");
            const auto defaultPose = TsShapePose::sample(resource);
            CHECK(defaultPose);
            TsShapeInstance evaluated(resource);
            const auto cached = evaluated.pose(), repeated = evaluated.pose();
            CHECK(cached && repeated && cached->revision() == repeated->revision());
            CHECK(cached->nodeTransforms().data() == repeated->nodeTransforms().data());
            CHECK(evaluated.advance(0) && evaluated.pose()->revision() == cached->revision());
            CHECK(!evaluated.addThread(-1) && evaluated.pose()->revision() == cached->revision());
            if (!resource->details.empty()) {
                const auto collision = TsShapeCollision::shared(*cached, 0);
                CHECK(collision && collision == TsShapeCollision::shared(*repeated, 0));
                CHECK(!TsShapeCollision::shared(*cached, -1));
                CHECK(collision == TsShapeCollision::shared(*cached, 0));
            }
            const auto child = std::ranges::find_if(resource->nodes, [](const auto& node) {
                return node.parent >= 0;
            });
            if (child != resource->nodes.end()) {
                auto overridden = *cached;
                auto parent = cached->nodeTransforms()[child->parent];
                parent.p.x += 1;
                CHECK(overridden.overrideParentTransform(Int32(child - resource->nodes.begin()),
                                                         parent));
                CHECK(overridden.revision() != cached->revision());
                CHECK(overridden.nodeTransforms().data() != cached->nodeTransforms().data());
                CHECK(evaluated.pose()->revision() == cached->revision());
            }
            for (std::size_t detail = 0; detail < resource->details.size(); ++detail) {
                if (!TsShapeCollision::prepare(*defaultPose, Int32(detail))) {
                    std::fprintf(stderr,
                                 "Rejected original collision detail: %s/%s detail %zu\n",
                                 archive.c_str(),
                                 name.c_str(),
                                 detail);
                    CHECK(false);
                }
                ++collisionSnapshots;
            }
            std::ranges::for_each(
                std::views::iota(std::size_t{0}, resource->sequences.size()), [&](auto sequence) {
                    TsShapeInstance instance(resource);
                    CHECK(instance.addThread(Int32(sequence)));
                    for (const auto position : {0.f, .25f, .5f, .75f, 1.f}) {
                        const std::array samples{TsAnimationSample{Int32(sequence), position}};
                        const auto pose = TsShapePose::sample(resource, samples);
                        ++sampled;
                        if (!pose) {
                            if (!nonfinite) {
                                std::fprintf(
                                    stderr,
                                    "Rejected original pose: %s/%s sequence %zu position %.2f\n",
                                    archive.c_str(),
                                    name.c_str(),
                                    sequence,
                                    double(position));
                            }
                            CHECK(nonfinite);
                            ++rejectedTracks;
                        }
                        if (pose && !resource->details.empty()) {
                            CHECK(TsShapeCollision::prepare(*pose, 0));
                            ++collisionSnapshots;
                        }
                        const auto seconds =
                            position == 0 ? 0.
                                          : Flt64(resource->sequences[sequence].duration) * .25;
                        ++advanced;
                        if (!instance.advance(seconds)) {
                            if (!nonfinite) {
                                std::fprintf(stderr,
                                             "Rejected original playback: %s/%s sequence %zu "
                                             "position %.2f\n",
                                             archive.c_str(),
                                             name.c_str(),
                                             sequence,
                                             double(position));
                            }
                            CHECK(nonfinite);
                            ++rejectedPlayback;
                        }
                    }
                });
            for (std::size_t index = 0; index < resource->transitions.size(); ++index) {
                const auto& edge = resource->transitions[index];
                TsShapeInstance instance(resource);
                CHECK(instance.addThread(edge.startSequence, edge.startPosition));
                CHECK(instance.setTransition(0, Int32(index)));
                for (int step = 0; step < 5; ++step) {
                    CHECK(instance.advance(Flt64(edge.duration) * .3));
                    ++transitionSamples;
                }
            }
        }
    });
    CHECK(failures == 0);
    UInt8 end{};
    CHECK(!fixture.read(end) && fixture() == STRM_EOS);
    CHECK(sampled == 2290 && rejectedTracks == 5);
    CHECK(advanced == 2290 && rejectedPlayback == 5);
    std::printf("Sampled %u original sequence positions and %u stateful advances; %u/%u safely "
                "rejected authored nonfinite tracks. %u authored transition steps.\n",
                sampled,
                advanced,
                rejectedTracks,
                rejectedPlayback,
                transitionSamples);
    std::printf("Prepared %u original default-detail/animated collision snapshots.\n",
                collisionSnapshots);
}
}

int opent1_test_ts_shape(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--formats") {
        std::filesystem::create_directories(argv[2]);
        for (Int32 test = 0; test < 37; ++test) {
            const auto bytes = test == 36 ? shape(8, 3, 4, -1)
                                          : shape(5 + test / 9, 1 + (test / 3) % 3, 2 + test % 3);
            FileWStream file(std::filesystem::path(argv[2]) / std::format("format-{}.dts", test));
            CHECK(file.writeBytes(bytes.data));
        }
        return 0;
    }
    return opent1::test::run([&] {
        if (argc == 2) {
            assets(argv[1]);
        } else {
            unit();
        }
    });
}

namespace {
int opent1_test_ts_shape_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_ts_shape(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("world", "ts_shape", opent1_test_ts_shape_default);
