#pragma once
#include "test_support.hpp"
#include <opent1/gui/runtime.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>

namespace opent1::test {
inline std::vector<gfx::Bitmap> nativeEditorFrames() {
    using namespace gui;
    FileRStream in(OPENT1_GUI_EDITOR_FIXTURE);
    const auto word = [&]() {
        UInt32 n{};
        CHECK(in.read(n));
        return n;
    };
    CHECK(word() == 0x45473742 && word() == 2 && word() == 64);
    Persistent::ClassRegistry registry;
    CHECK(registerClasses(registry) && registerExtendedClasses(registry));
    std::vector<gfx::Bitmap> retained;
    for (unsigned h = 0; h < 64; ++h) {
        Control root;
        Canvas canvas;
        EditControl editor;
        std::array<Control, 4> controls;
        CHECK(root.resize({}, {160, 100}));
        root.tag(100);
        const std::array<Point2I, 4> positions = {Point2I{12, 12}, {65, 18}, {10, 60}, {8, 4}};
        const std::array<Point2I, 4> extents = {Point2I{30, 20}, {28, 22}, {110, 32}, {20, 16}};
        for (unsigned i = 0; i < 4; ++i) {
            CHECK(controls[i].resize(positions[i], extents[i]));
            controls[i].tag(i + 1);
            CHECK((i == 3 ? controls[2] : root).addObject(controls[i]));
        }
        CHECK(editor.resize({}, {160, 100}) && root.addObject(editor) && canvas.content(&root) &&
              editor.addSet(root));
        editor.grid = {std::max(1, Int32(word())), std::max(1, Int32(word()))};
        std::vector<std::byte> saved;
        const auto count = word();
        CHECK(count == 88);
        for (unsigned s = 0; s < count; ++s) {
            const auto kind = word();
            const Int32 x = Int32(word()), y = Int32(word());
            const auto modifier = word();
            Event e;
            e.point = {x, y};
            e.shift = (modifier & 3) != 0;
            e.control = (modifier & 12) != 0;
            const auto run = [&] {
                switch (kind) {
                    case 1:
                        e.type = EventType::Down;
                        CHECK(editor.event(e));
                        break;
                    case 2:
                        e.type = EventType::Drag;
                        CHECK(editor.event(e));
                        break;
                    case 3:
                        e.type = EventType::Up;
                        CHECK(editor.event(e));
                        break;
                    case 4:
                        e.type = EventType::Down;
                        e.button = 3;
                        CHECK(editor.event(e));
                        break;
                    case 5: {
                        auto* c = root.findTag(UInt32(x));
                        CHECK(c && editor.select(*c));
                        break;
                    }
                    case 6:
                        CHECK(editor.addSet(root));
                        break;
                    case 7:
                        e.type = EventType::KeyDown;
                        switch (x) {
                            case 0xcd:
                                e.key = Key::Right;
                                break;
                            case 0xd0:
                                e.key = Key::Down;
                                break;
                            case 0xcb:
                                e.key = Key::Left;
                                break;
                            case 0xc8:
                                e.key = Key::Up;
                                break;
                            case 0x1e:
                                e.key = Key::A;
                                break;
                            case 0x43:
                                e.key = Key::F9;
                                break;
                            case 0x44:
                                e.key = Key::F10;
                                break;
                            default:
                                CHECK(x >= 0x3b && x <= 0x41);
                                e.key = Key(Int32(Key::F1) + x - 0x3b);
                                break;
                        }
                        editor.event(e);
                        break;
                    case 8: {
                        saved.resize(65536);
                        MemRWStream out{std::span<std::byte>(saved)};
                        CHECK(editor.saveSelection(out, registry));
                        saved.resize(out.getPosition());
                        break;
                    }
                    case 9:
                        CHECK(editor.erase());
                        break;
                    case 10: {
                        MemRWStream stream{std::span<const std::byte>(saved)};
                        CHECK(editor.loadSelection(stream, registry));
                        break;
                    }
                    case 11:
                        CHECK(controls[0].resize(positions[0], extents[0]));
                        break;
                    default:
                        CHECK(false);
                }
            };
            try {
                run();
                CHECK(word() == editor.currentAddSet()->fields().tag);
                // Native Canvas holds edit-mode capture between gestures. The
                // modern owned overlay captures individual gestures instead.
                CHECK(word() == 1);
                std::vector<Control*> all;
                const auto collect = [&](auto&& self, Control& p) -> void {
                    for (auto* o : p.objects()) {
                        if (o == &editor) {
                            continue;
                        }
                        auto* c = static_cast<Control*>(o);
                        all.push_back(c);
                        self(self, *c);
                    }
                };
                collect(collect, root);
                CHECK(word() == all.size());
                const auto selected = editor.selection();
                for (auto* c : all) {
                    CHECK(word() == c->fields().tag);
                    CHECK(word() == c->parent()->fields().tag);
                    const auto px = Int32(word()), py = Int32(word()), ex = Int32(word()),
                               ey = Int32(word());
                    CHECK(c->fields().position == Point2I(px, py) &&
                          c->fields().extent == Point2I(ex, ey));
                    CHECK(bool(word()) == (std::ranges::find(selected, c) != selected.end()));
                }
                std::vector<std::byte> expected;
                const auto runs = word();
                CHECK(runs <= 16000);
                for (unsigned r = 0; r < runs; ++r) {
                    const auto n = word(), color = word();
                    CHECK(n && expected.size() + n <= 16000 && color <= 255);
                    expected.insert(expected.end(), n, std::byte(color));
                }
                CHECK(expected.size() == 16000);
                auto surface = gfx::SoftwareSurface::create(160, 100);
                CHECK(surface);
                surface->clear(19);
                CHECK(editor.draw(*surface, {}, surface->clip()));
                if (surface->bitmap().pixels != expected) {
                    auto i = std::mismatch(
                                 expected.begin(), expected.end(), surface->bitmap().pixels.begin())
                                 .first -
                             expected.begin();
                    throw std::runtime_error(std::format("pixel {},{}", i % 160, i / 160));
                }
                if (s + 1 == count) {
                    retained.push_back(surface->bitmap());
                }
            } catch (const std::exception& ex) {
                throw std::runtime_error(
                    std::format("editor history {} step {} kind {}: {}", h, s, kind, ex.what()));
            }
        }
    }
    CHECK(UInt64(in.getPosition()) == UInt64(in.getSize()));
    return retained;
}
}
