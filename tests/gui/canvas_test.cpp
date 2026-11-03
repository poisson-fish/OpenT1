#include "test_support.hpp"
#include <ranges>
#include <opent1/gui/runtime.hpp>
#include <opent1/core/file_stream.hpp>
#include <cmath>
#include "gui_retained_gpu_support.hpp"
using namespace opent1;
using namespace opent1::gui;

namespace {
struct Entry {
    UInt32 kind, a, b, c;
    bool operator==(const Entry&) const = default;
};

std::vector<Entry> trace;

UInt32 word(StreamIO& in) {
    UInt32 n{};
    CHECK(in.read(n));
    return n;
}

struct State {
    std::array<UInt32, 18> values;
    std::vector<Entry> entries;
    std::vector<std::byte> pixels;
};

State read(StreamIO& in) {
    State s;
    for (auto& v : s.values) {
        v = word(in);
    }
    const auto n = word(in);
    CHECK(n < 100);
    for ([[maybe_unused]] auto i :
         std::views::iota(UInt32{0}) | std::views::take_while([&](UInt32 i) {
             return i < n;
         })) {
        s.entries.push_back({word(in), word(in), word(in), word(in)});
    }
    const auto size = word(in);
    CHECK(size == 64 * 48);
    s.pixels.resize(size);
    CHECK(in.readBytes(s.pixels));
    return s;
}

struct Probe : Control {
    bool failTick{}, failDraw{}, failKey{}, failMouse{};
    int mutations{};

    bool wantsTab() const override {
        return true;
    }

    bool becomeFirstResponder() const override {
        return true;
    }

    bool event(const Event& e) override {
        if (e.type == EventType::KeyDown) {
            trace.push_back({e.repeat ? 3U : 1U, e.repeat && e.key == Key::Tab ? 1U : 0U, 0, 0});
            if (e.repeat && failKey) {
                ++mutations;
                canvas()->setCursorPosition({41, 42});
                return false;
            }
        }
        if (e.type == EventType::KeyUp) {
            trace.push_back({2, 0, 0, 0});
        }
        if (e.type == EventType::Repeat) {
            trace.push_back({4, UInt32(e.point.x), UInt32(e.point.y), 0});
            if (failMouse) {
                ++mutations;
                canvas()->unlockMouse();
                return false;
            }
        }
        if (e.type == EventType::Down) {
            return canvas()->lockMouse(this);
        }
        if (e.type == EventType::Up) {
            canvas()->unlockMouse();
        }
        return true;
    }

    Int32 helpTag(double elapsed) const override {
        return elapsed > 500 ? fields().helpTag : 0;
    }

    std::optional<std::string> helpText(double elapsed) const override {
        return elapsed > 750 ? std::optional<std::string>("help") : std::nullopt;
    }

    void tick(double) override {
        if (failTick) {
            ++mutations;
            throw std::runtime_error("prepare");
        }
    }

    bool draw(gfx::SoftwareSurface& s, Point2I p, RectI c) const override {
        if (failDraw) {
            return false;
        }
        return Control::draw(s, p, c);
    }

    std::function<void()> checkpoint() override {
        auto base = Control::checkpoint();
        const auto old = mutations;
        return [this, base = std::move(base), old] {
            base();
            mutations = old;
        };
    }
};

std::shared_ptr<const gfx::BitmapArray> frames() {
    auto a = std::make_shared<gfx::BitmapArray>();
    for (auto i : std::views::iota(0, 3)) {
        gfx::Bitmap b;
        b.width = 5;
        b.height = 3;
        b.pixels.resize(24);
        for (auto y : std::views::iota(0, 3)) {
            for (auto x : std::views::iota(0, 5)) {
                b.pixels[std::size_t(y * 8 + x)] = std::byte((x + y) % 3 ? 180 + i : 0);
            }
        }
        a->bitmaps.push_back(std::move(b));
    }
    CHECK(a->valid());
    return a;
}

void histories(std::string_view driver) {
    FileRStream input(OPENT1_GUI_CANVAS_FIXTURE);
    CHECK(input.getStatus() == STRM_OK);
    CHECK(word(input) == 0x43473742 && word(input) == 1 && word(input) == 64 && word(input) == 48);
    std::vector<std::shared_ptr<const gfx::Bitmap>> retained;
    constexpr std::array<UInt32, 48> times = {
        1000, 1300, 1301, 1351, 1352, 1400, 1700, 1701, 1702, 1703, 2203, 2204,
        2454, 2500, 2900, 2901, 3000, 3001, 3002, 3003, 3100, 3101, 3102, 3200,
        3201, 3300, 3400, 3401, 3500, 3801, 3802, 3803, 3804, 3805, 3900, 3901,
        4000, 4001, 4100, 4101, 4200, 4326, 4327, 4400, 4502, 4600, 4801, 4802};
    for (auto history : std::views::iota(UInt32{0}, UInt32{64})) {
        Probe p;
        auto f = p.fields();
        f.extent = {64, 48};
        f.helpTag = 400 + Int32(history);
        f.style.opaque = 1;
        f.style.fill = UInt8(31 + history % 29);
        CHECK(p.configure(f));
        Canvas c;
        c.synthesizeKeyRepeat = true;
        c.cursorVisible = true;
        CHECK(c.content(&p) && c.makeFirstResponder(&p));
        CHECK(c.setCursor({{}, frames(), {Int32(history % 3), Int32(history % 2)}, .2}));
        CHECK(c.setCursorPosition({11, 13}));
        c.controlCursor = [](Int32 tag) -> std::optional<Canvas::Cursor> {
            if (tag == 140102) {
                return {};
            }
            auto images = frames();
            if (tag == 140103) {
                return Canvas::Cursor{
                    std::make_shared<const gfx::Bitmap>(images->bitmaps[0]), {}, {2, 1}, .5};
            }
            return Canvas::Cursor{{}, images, {1, 2}, .125};
        };
        c.onHelp = [](const Canvas::HelpState& h) {
            const auto elapsed = UInt32(std::llround(h.elapsed));
            for (UInt32 kind : {5, 6, 7, 8}) {
                trace.push_back({kind,
                                 kind % 2 ? UInt32(h.tag) : UInt32(h.text.has_value()),
                                 elapsed,
                                 UInt32(h.clicked)});
            }
        };
        trace.clear();
        const auto key = [&](bool down, double t, bool tab = false) {
            Event e;
            e.type = down ? EventType::KeyDown : EventType::KeyUp;
            e.key = tab ? Key::Tab : Key::None;
            CHECK(c.dispatch(e, t));
        };
        for (auto step :
             std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t step) {
                 return step < times.size();
             })) {
            const auto native = read(input);
            const double seconds = double(times[step] - 1000) / 1000;
            bool refused = false;
            if (step == 0) {
                Event e;
                e.type = EventType::Move;
                e.point = {11, 13};
                CHECK(c.dispatch(e, seconds));
                key(true, seconds);
            }
            if (step == 5 || step == 9) {
                if (step == 9) {
                    key(false, seconds);
                }
                Event e;
                e.type = step == 5 ? EventType::Down : EventType::Up;
                e.point = step == 5 ? Point2I{11, 13} : Point2I{19, 17};
                CHECK(c.dispatch(e, seconds));
            }
            if (step == 8) {
                CHECK(c.setCursorPosition({19, 17}));
            }
            if (step == 13) {
                key(true, seconds, true);
            }
            if (step == 15) {
                key(false, seconds, true);
            }
            if (step == 16) {
                c.cursorVisible = false;
                CHECK(c.addUpdateRegion({7, 9}, {11, 13}));
            }
            p.failTick = step == 23;
            p.failDraw = step == 24;
            if (step == 24 || step == 25) {
                c.resetUpdateRegions();
            }
            if (step == 25) {
                c.cursorVisible = true;
            }
            if (step == 26) {
                c.cursorVisible = false;
            }
            if (step == 27) {
                CHECK(c.setCursorPosition({32, 24}));
                c.resetUpdateRegions();
            }
            if (step == 28) {
                c.cursorVisible = true;
                key(true, seconds);
                Event e;
                e.type = EventType::Down;
                e.point = {32, 24};
                CHECK(c.dispatch(e, seconds));
            }
            if (step == 33 || step == 34 || step == 35) {
                if (step == 33) {
                    key(false, seconds);
                }
                Event e;
                e.type = step == 34 ? EventType::Down : EventType::Up;
                e.point = {32, 24};
                CHECK(c.dispatch(e, seconds));
            }
            p.failKey = step == 29;
            p.failMouse = step == 31;
            if (step == 36) {
                c.resetUpdateRegions();
            }
            if (step == 37) {
                c.cursorEnabled = false;
                CHECK(c.setCursorPosition({5, 7}));
            }
            if (step == 38) {
                c.cursorVisible = false;
            }
            if (step == 39) {
                c.cursorEnabled = true;
            }
            if (step == 40) {
                c.cursorVisible = true;
                p.mouseCursorTag = 140101;
            }
            if (step == 42) {
                p.mouseCursorTag = 140102;
            }
            if (step == 43) {
                p.mouseCursorTag = 0;
                CHECK(c.setCursor(
                    {{}, c.cursor.frames, {Int32(history % 3), Int32(history % 2)}, .2}));
            }
            if (step == 44) {
                p.mouseCursorTag = 140103;
            }
            if (step == 45) {
                p.mouseCursorTag = 140101;
            }
            if (step == 46) {
                p.mouseCursorTag = 0;
            }
            auto previousHelp = c.helpState();
            auto previousUpdate = c.updateRegion();
            std::shared_ptr<const gfx::Bitmap> output;
            if (step < 20 || step > 22) {
                if (c.advance(seconds)) {
                    output = c.render(64, 48);
                    refused = !output;
                } else {
                    refused = true;
                }
            }
            CHECK(refused == (step == 23 || step == 24 || step == 29 || step == 31 || step == 42));
            CHECK(p.mutations == 0);
            if (refused) {
                CHECK(c.helpState().tag == previousHelp.tag &&
                      c.helpState().text == previousHelp.text &&
                      c.updateRegion() == previousUpdate);
            }
            CHECK((c.capture() != nullptr) == bool(native.values[3]));
            if (step == 42) {
                CHECK(trace.empty() && native.entries == std::vector<Entry>{{5, 0, 426, 1},
                                                                            {6, 0, 426, 1},
                                                                            {7, 0, 426, 1},
                                                                            {8, 0, 426, 1}});
            } else if (trace != native.entries) {
                std::fprintf(stderr,
                             "history %u step %zu traces modern %zu native %zu\n",
                             history,
                             step,
                             trace.size(),
                             native.entries.size());
                for (auto x : trace) {
                    std::fprintf(stderr, "m %u %u %u %u\n", x.kind, x.a, x.b, x.c);
                }
                for (auto x : native.entries) {
                    std::fprintf(stderr, "n %u %u %u %u\n", x.kind, x.a, x.b, x.c);
                }
                CHECK(false);
            }
            trace.clear();
            if (output) {
                if (output->pixels != native.pixels) {
                    std::fprintf(stderr, "frame history %u step %zu\n", history, step);
                    CHECK(false);
                }
                if (step < 20) {
                    auto r = c.updateRegion();
                    CHECK(r);
                    CHECK(r->left == Int32(native.values[10]) &&
                          r->top == Int32(native.values[11]) &&
                          r->right + 1 == Int32(native.values[12]) &&
                          r->bottom + 1 == Int32(native.values[13]));
                }
                if (step == 25) {
                    retained.push_back(output);
                }
            }
        }
        CHECK(c.content(nullptr));
    }
    CHECK(word(input) == 16);
    for (auto i : std::views::iota(UInt32{0}, UInt32{16})) {
        const std::array<UInt32, 8> expected = {1, 1, 0, 1, 1, 1, 1, 1};
        for (auto n : expected) {
            CHECK(word(input) == n);
        }
        CHECK(word(input) == 64 * 48);
        std::vector<std::byte> frame(64 * 48);
        CHECK(input.readBytes(frame));
        CHECK(std::ranges::all_of(frame, [&](auto p) {
            return p == std::byte(61 + i);
        }));
    }
    CHECK(Int64(input.getPosition()) == input.getSize());
    CHECK(retained.size() == 64);
    for (auto& frame : retained) {
        CHECK(frame->valid());
    }
    test::retainedGuiGpu(retained, driver);
}

void rollback() {
    Probe p;
    Canvas c;
    c.synthesizeKeyRepeat = true;
    CHECK(c.content(&p) && c.makeFirstResponder(&p));
    c.cursorVisible = true;
    CHECK(c.setCursor({{}, frames(), {}, .2}));
    Event e;
    e.type = EventType::KeyDown;
    CHECK(c.dispatch(e));
    e.type = EventType::Down;
    e.point = {11, 13};
    CHECK(c.dispatch(e));
    CHECK(c.advance(.3));
    const auto old = c.render(64, 48);
    CHECK(old);
    p.failMouse = true;
    CHECK(!c.advance(.301) && c.time() == .3 && c.capture() == &p && p.mutations == 0);
    p.failMouse = false;
    p.failKey = true;
    CHECK(!c.advance(.301) && c.time() == .3 && c.capture() == &p && p.mutations == 0);
    p.failKey = false;
    trace.clear();
    CHECK(c.advance(.301));
    CHECK(trace.size() == 2 && trace[0].kind == 3 && trace[1].kind == 4 && trace[1].a == 11);
    // Any key release cancels the native repeat, including another key.
    e.type = EventType::KeyUp;
    e.key = Key::A;
    CHECK(c.dispatch(e));
    trace.clear();
    CHECK(c.advance(.4));
    CHECK(trace.size() == 1 && trace[0].kind == 4);
    CHECK(!c.advance(.39));
    CHECK(!c.setCursor({{}, frames(), {}, 0}));
    CHECK(!c.addUpdateRegion({}, {-1, 3}));
    auto broken = std::make_shared<gfx::Bitmap>();
    c.cursor.bitmap = broken;
    const auto update = c.updateRegion();
    CHECK(!c.render(64, 48) && c.updateRegion() == update);
    c.cursor.bitmap.reset();
    CHECK(c.render(64, 48));
    CHECK(old->valid());
    CHECK(c.content(nullptr));
}

void viewportSizing() {
    Probe root, dialog;
    Control centered, edge;
    Canvas canvas;
    const auto render = [&](Int32 w, Int32 h) {
        return canvas.render(w, h, nullptr, nullptr, {}, true);
    };
    CHECK(root.resize({7, 9}, {512, 384}) && dialog.resize({3, 5}, {512, 384}));
    auto fields = centered.fields();
    fields.position = {136, 330};
    fields.extent = {240, 20};
    fields.horizontal = 3;
    fields.vertical = 2;
    CHECK(centered.configure(fields) && root.addObject(centered));
    fields = edge.fields();
    fields.position = {480, 350};
    fields.extent = {20, 20};
    fields.horizontal = 2;
    fields.vertical = 2;
    CHECK(edge.configure(fields) && dialog.addObject(edge) && canvas.content(&root) &&
          canvas.pushDialog(dialog));
    dialog.failDraw = true;
    CHECK(!render(640, 480) && root.fields().position == Point2I{7, 9} &&
          root.fields().extent == Point2I{512, 384});
    CHECK(dialog.fields().position == Point2I{3, 5} &&
          centered.fields().position == Point2I{136, 330} &&
          edge.fields().position == Point2I{480, 350});
    dialog.failDraw = false;
    for (const auto size : {Point2I{640, 480}, Point2I{1280, 720}, Point2I{640, 480}}) {
        CHECK(render(size.x, size.y) && root.fields().position == Point2I{} &&
              dialog.fields().position == Point2I{});
        CHECK(root.fields().extent == size && dialog.fields().extent == size);
        CHECK(centered.fields().position == Point2I{(size.x - 240) / 2, size.y - 54} &&
              edge.fields().position == Point2I{size.x - 32, size.y - 34});
    }
}

void taggedCursors() {
    Console console;
    Persistent::ClassRegistry registry;
    Resources resources;
    resources.string = [](Int32 tag) -> std::optional<std::string> {
        if (tag == 101) {
            return "cursor.pba, 1, 2, 0.125 ";
        }
        if (tag == 102) {
            return "cursor.bmp,2,1";
        }
        return "cursor.pba,broken";
    };
    Runtime runtime(console, registry, resources);
    int loads = 0;
    runtime.loadCursor = [&](std::string_view name) -> std::optional<Canvas::Cursor> {
        ++loads;
        auto images = frames();
        if (name == "cursor.bmp") {
            return Canvas::Cursor{
                std::make_shared<const gfx::Bitmap>(images->bitmaps[0]), {}, {}, .5};
        }
        CHECK(name == "cursor.pba");
        return Canvas::Cursor{{}, images, {}, .5};
    };
    auto root = std::make_unique<Probe>();
    root->mouseCursorTag = 101;
    auto* p = root.get();
    auto graph = Graph::create(std::move(root));
    CHECK(runtime.show(graph));
    auto& c = runtime.canvas;
    c.cursorVisible = true;
    Event e;
    e.type = EventType::Move;
    e.point = {11, 13};
    CHECK(c.dispatch(e));
    CHECK(c.render(64, 48) && loads == 1);
    CHECK(c.render(64, 48) && loads == 1);
    p->mouseCursorTag = 103;
    const auto region = c.updateRegion();
    CHECK(!c.render(64, 48) && c.updateRegion() == region);
    p->mouseCursorTag = 102;
    CHECK(c.render(64, 48) && loads == 3);
    p->mouseCursorTag = 0;
    CHECK(c.render(64, 48));
    p->mouseCursorTag = 102;
    CHECK(c.render(64, 48) && loads == 3);
    c.cursorEnabled = false;
    e.type = EventType::Down;
    CHECK(!c.dispatch(e));
    CHECK(runtime.clear());
}

void retiredActionSenders() {
    for (const bool named : {false, true}) {
        for (const bool fail : {false, true}) {
            Console console;
            Persistent::ClassRegistry registry;
            Runtime runtime(console, registry);
            auto button = std::make_unique<Active>();
            auto* sender = button.get();
            const auto life = sender->lifetime();
            sender->assignName("RetiredActionSender");
            auto fields = sender->fields();
            fields.variable = "ActionTest::LongVariableOwnedByTheRetiredControl";
            fields.command = named ? "" : "ReplaceActionScreen(); $ActionScriptFinished = true;";
            const auto variable = fields.variable;
            CHECK(sender->configure(fields));
            sender->message = 123;
            CHECK(runtime.show(Graph::create(std::move(button))));
            console.setVariable(variable, "previous value");
            console.addCommand(
                0, "ReplaceActionScreen",
                [&](Console&, Int32, std::span<const std::string>) -> std::string {
                    CHECK(runtime.show(Graph::create(std::make_unique<Control>())));
                    CHECK(life.expired());
                    if (fail) {
                        throw std::runtime_error("screen transition failure");
                    }
                    return "True";
                });
            CHECK(console.evaluate(named
                                       ? "function RetiredActionSender::onAction() { "
                                         "ReplaceActionScreen(); $NamedActionFinished = true; }"
                                       : "function RetiredActionSender::onAction() { "
                                         "$StaleActionWasCalled = true; }").ok());
            // Intro movie completion invokes actions after Canvas dispatch has
            // ended, so replacement retires the sender immediately.
            CHECK(!runtime.canvas.dispatching());
            const bool result = runtime.canvas.onAction(*sender, sender->message,
                                                       sender->fields().command);
            CHECK(result == !fail && life.expired());
            CHECK(!console.hasVariable("StaleActionWasCalled"));
            CHECK(console.getVariable(variable) == (fail ? "previous value" : ""));
            CHECK(runtime.canvas.top() && runtime.graphCount() == 1);
        }
    }
}
}

namespace {
void retainedRendering() {
    struct Paint : Control {
        mutable int draws{};
        UInt8 ink{3};
        bool fail{};

        bool draw(gfx::SoftwareSurface& s, Point2I at, RectI) const override {
            ++draws;
            s.fill({at.x, at.y, at.x + fields().extent.x - 1, at.y + fields().extent.y - 1}, ink);
            return !fail;
        }
    } root;

    struct Layout : Control {
        mutable int calls{};

        void
        layoutInParent(Point2I oldExtent, Point2I next, Point2I& p, Point2I& e) const override {
            ++calls;
            Control::layoutInParent(oldExtent, next, p, e);
        }
    } child;

    CHECK(root.addObject(child));
    Canvas canvas;
    CHECK(canvas.content(&root));
    auto palette = std::make_shared<const gfx::Palette>();
    CHECK(canvas.fitContent({32, 24}, true));
    const auto layouts = child.calls;
    CHECK(canvas.fitContent({32, 24}, true) && child.calls == layouts);
    auto fields = child.fields();
    fields.horizontal = 3;
    CHECK(child.configure(fields));
    CHECK(canvas.fitContent({32, 24}, true) && child.calls == layouts + 1);
    CHECK(child.fields().position.x == (32 - child.fields().extent.x) / 2);
    const auto snapshot = canvas.render(32, 24, palette.get());
    CHECK(snapshot && snapshot->palette);
    for ([[maybe_unused]] auto i : std::views::iota(0, 5)) {
        CHECK(canvas.renderRetained(32, 24, palette));
    }
    CHECK(!canvas.updateRegion());
    const auto draws = root.draws;
    CHECK(canvas.renderRetained(32, 24, palette) && root.draws == draws);
    root.ink = 9;
    CHECK(canvas.addUpdateRegion({2, 3}, {4, 5}));
    const auto* changed = canvas.renderRetained(32, 24, palette);
    CHECK(changed && changed->row(3)[2] == std::byte{9} && changed->row(0)[0] == std::byte{3});
    CHECK(!changed->palette && snapshot->row(3)[2] == std::byte{3});
    const auto region = canvas.updateRegion();
    root.fail = true;
    root.ink = 7;
    CHECK(canvas.addUpdateRegion({2, 3}, {4, 5}));
    CHECK(!canvas.renderRetained(32, 24, palette) && canvas.updateRegion() == region);
    root.fail = false;
    const auto* recovered = canvas.renderRetained(32, 24, palette);
    CHECK(recovered && recovered->row(3)[2] == std::byte{7} &&
          recovered->row(0)[0] == std::byte{3});
    root.fail = true;
    CHECK(!canvas.renderRetained(64, 48, palette, nullptr, {}, true));
    CHECK(root.fields().extent == Point2I(32, 24));
    root.fail = false;
    CHECK(canvas.renderRetained(64, 48, palette, nullptr, {}, true));
    CHECK(root.fields().extent == Point2I(64, 48));
    CHECK(canvas.content(nullptr));
    CHECK(root.removeObject(child));
}
}

int opent1_test_gui_canvas(int argc, char** argv) {
    return test::run([&] {
        histories(argc > 1 ? argv[1] : "");
        rollback();
        viewportSizing();
        taggedCursors();
        retiredActionSenders();
        retainedRendering();
        std::puts("64 original Canvas histories: repeat/cursor/update/help/render failures and "
                  "retained rollback");
    });
}

namespace {
int opent1_test_gui_canvas_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_gui_canvas(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("gui", "gui_canvas", opent1_test_gui_canvas_default);
