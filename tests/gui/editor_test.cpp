#include "gui_editor_support.hpp"
#include <ranges>
#include "gui_retained_gpu_support.hpp"
using namespace opent1;
using namespace opent1::gui;

namespace {
void histories() {
    for (auto h : std::views::iota(unsigned{0}, unsigned{64})) {
        Control root, a, b;
        EditControl editor;
        Canvas canvas;
        CHECK(root.resize({}, {160, 100}) && a.resize({12, 12}, {30, 20}) &&
              b.resize({65, 18}, {28, 22}) && editor.resize({}, {160, 100}) && root.addObject(a) &&
              root.addObject(b) && root.addObject(editor) && canvas.content(&root));
        bool acquire = false, release = false, physical = false, inspect = false;
        canvas.captureMouse = [&](bool capture) {
            if (capture ? !acquire : !release) {
                return false;
            }
            physical = capture;
            return true;
        };
        editor.inspect = [&](Control*) {
            return inspect;
        };
        Event e;
        e.type = EventType::Down;
        e.point = {25, 23};
        CHECK(!canvas.dispatch(e) && editor.selection().empty() && !canvas.capture() && !physical);
        acquire = true;
        CHECK(canvas.dispatch(e) && editor.selection() == std::vector<Control*>{&a} && physical);
        e.type = EventType::Drag;
        e.point = {35 + Int32(h % 5), 31};
        CHECK(canvas.dispatch(e));
        const auto moved = a.fields().position;
        const auto retained = canvas.render(160, 100);
        CHECK(retained);
        e.type = EventType::Up;
        CHECK(!canvas.dispatch(e) && canvas.capture() == &editor && a.fields().position == moved &&
              physical);
        release = true;
        CHECK(!canvas.dispatch(e) && canvas.capture() == &editor && a.fields().position == moved &&
              physical);
        inspect = true;
        CHECK(canvas.dispatch(e) && !canvas.capture() && !physical && editor.undo() &&
              a.fields().position == Point2I(12, 12) && editor.redo() &&
              a.fields().position == moved);
        CHECK(editor.undo());
        e.type = EventType::Down;
        e.point = {25, 23};
        CHECK(canvas.dispatch(e));
        e.type = EventType::Drag;
        e.point = {40, 30};
        CHECK(canvas.dispatch(e));
        e.button = 3;
        e.point = {90, 90};
        CHECK(!canvas.dispatch(e) && a.fields().position == Point2I(27, 19));
        e.type = EventType::Up;
        CHECK(!canvas.dispatch(e) && canvas.capture() == &editor);
        canvas.cancel();
        CHECK(a.fields().position == Point2I(12, 12));
        CHECK(editor.select(a) && editor.move({1, 2}) && editor.undo() && editor.move({2, 3}) &&
              !editor.redo());
        auto properties = a.fields();
        properties.extent = {-1, 30};
        CHECK(!editor.properties(a, properties) && a.fields().extent == Point2I(30, 20));
        properties = a.fields();
        properties.tag = 71;
        properties.position = {23, 24};
        CHECK(editor.properties(a, properties) && editor.undo() &&
              a.fields().position == Point2I(14, 15) && a.fields().tag == 0 && editor.redo() &&
              a.fields().tag == 71);
        CHECK(root.removeObject(a) && !editor.undo() && editor.selection().empty() &&
              retained->valid());
    }
}

void preparations() {
    Persistent::ClassRegistry registry;
    CHECK(registerClasses(registry) && registerExtendedClasses(registry));
    Control source, a, b;
    EditControl writer;
    CHECK(source.addObject(a) && source.addObject(b) && source.addObject(writer) &&
          writer.select(a) && writer.select(b, true));
    std::vector<std::byte> bytes(65536);
    MemRWStream out{std::span<std::byte>(bytes)};
    CHECK(writer.saveSelection(out, registry));
    bytes.resize(out.getPosition());

    struct Refuses : Control {
        unsigned admissions{};
        bool refuse = true, throws = false;

        bool addObject(sim::SimObject& c) override {
            if (dynamic_cast<EditControl*>(&c)) {
                return Control::addObject(c);
            }
            if (++admissions % 2 == 0 && refuse) {
                if (throws) {
                    throw std::runtime_error("admission");
                }
                return false;
            }
            return Control::addObject(c);
        }
    } root;

    EditControl editor;
    CHECK(root.addObject(editor));
    for (bool throws : {false, true}) {
        root.admissions = 0;
        root.throws = throws;
        MemRWStream in{std::span<const std::byte>(bytes)};
        CHECK(!editor.loadSelection(in, registry) && root.size() == 1 &&
              editor.selection().empty() && editor.ownedObjectCount() == 0);
    }
    int bindings = 0;
    MemRWStream missing{std::span<const std::byte>(bytes)};
    CHECK(!editor.loadSelection(missing,
                                registry,
                                [&](Control&) {
                                    return ++bindings < 2;
                                }) &&
          root.size() == 1 && editor.selection().empty());
    root.refuse = false;
    MemRWStream retry{std::span<const std::byte>(bytes)};
    CHECK(editor.loadSelection(retry, registry) && editor.selection().size() == 2 &&
          root.size() == 3);
    const auto first = editor.selection().front()->lifetime();
    CHECK(editor.erase() && first.expired() && root.size() == 1);
    // Cut prepares the clipboard before retirement; failed deletion preserves
    // its previous bytes and selection. A later paste still decodes that copy.
    Console console;
    auto runtime = std::make_shared<Runtime>(console, registry);
    CHECK(runtime->command("GuiNewContentCtrl", std::array<std::string, 1>{"SimGui::Control"}) ==
              "True" &&
          runtime->command("GuiEditNewControl", std::array<std::string, 1>{"SimGui::Control"}) ==
              "True");
    auto* edit = runtime->editor();
    auto* target = edit->selection().front();
    CHECK(edit->copySelection(false));
    const auto destroy = edit->destroySelection;
    edit->destroySelection = [](auto) {
        return false;
    };
    CHECK(!edit->copySelection(true) && edit->selection().front() == target);
    edit->destroySelection = destroy;
    CHECK(edit->pasteSelection() && runtime->canvas.top()->size() == 3);
    runtime->toolbar = [](auto&) -> bool {
        throw std::runtime_error("toolbar preparation");
    };
    runtime->inspect = [](auto*) {
        return false;
    };
    CHECK(runtime->editing(false));
    TextEdit previous;
    CHECK(runtime->canvas.top()->addObject(previous) &&
          runtime->canvas.makeFirstResponder(&previous) && runtime->canvas.lockMouse(&previous));
    CHECK(runtime->command("GuiToolbar", {}) == "False" && !runtime->editor() &&
          runtime->canvas.focus() == &previous && runtime->canvas.capture() == &previous);
    CHECK(runtime->command("GuiInspect", {}) == "False" && !runtime->editor() &&
          runtime->canvas.focus() == &previous && runtime->canvas.capture() == &previous);
    // Relative layout rounding is not invertible by resizing the parent back.
    // History restores every recorded descendant, and stale add sets fall back
    // to the live root rather than accepting paste into a detached hierarchy.
    Control layoutRoot, group, child;
    EditControl layout;
    CHECK(layoutRoot.addObject(group) && group.addObject(child) && layoutRoot.addObject(layout) &&
          group.resize({10, 10}, {31, 29}));
    auto cf = child.fields();
    cf.position = {3, 5};
    cf.extent = {7, 9};
    cf.horizontal = cf.vertical = 4;
    CHECK(child.configure(cf) && layout.select(group));
    const auto before = child.fields();
    CHECK(layout.size({7, 11}));
    const auto after = child.fields();
    CHECK(layout.undo() && child.fields().position == before.position &&
          child.fields().extent == before.extent && layout.redo() &&
          child.fields().position == after.position && child.fields().extent == after.extent);
    CHECK(layout.addSet(group) && layoutRoot.removeObject(group) &&
          layout.currentAddSet() == &layoutRoot && !layout.undo());
    Control boundedRoot, boundedTarget;
    EditControl bounded;
    CHECK(boundedRoot.addObject(boundedTarget) && boundedRoot.addObject(bounded) &&
          bounded.select(boundedTarget));
    for ([[maybe_unused]] auto i : std::views::iota(unsigned{0}, unsigned{140})) {
        CHECK(bounded.move({1, 0}));
    }
    for ([[maybe_unused]] auto i : std::views::iota(unsigned{0}, unsigned{128})) {
        CHECK(bounded.undo());
    }
    CHECK(!bounded.undo() && boundedTarget.fields().position.x == 12);
    for ([[maybe_unused]] auto i : std::views::iota(unsigned{0}, unsigned{128})) {
        CHECK(bounded.redo());
    }
    CHECK(!bounded.redo() && boundedTarget.fields().position.x == 140);
}

void nativeFailures() {
    FileRStream in(OPENT1_GUI_EDITOR_FAILURES_FIXTURE);
    const auto word = [&]() {
        UInt32 n{};
        CHECK(in.read(n));
        return n;
    };
    CHECK(word() == 0x46453742 && word() == 1 && word() == 4);
    for (auto i : std::views::iota(UInt32{0}, UInt32{4})) {
        CHECK(word() == i && word() == (i < 2 ? 0U : 0xc0000005U) && word() == (i < 2 ? 24U : 16U));
        CHECK(word() == 0x50473742 && word() == 1 && word() == 2 && word() == 2);
        if (i < 2) {
            CHECK(word() == (i == 0 ? 2U : 1U) && word() == (i == 0 ? 2U : 0U));
        }
    }
    CHECK(UInt64(in.getPosition()) == UInt64(in.getSize()));
    // Original malformed paste crashes after its explicit pre-state only. The
    // modern transaction refuses without changing objects, selection or history.
    Persistent::ClassRegistry registry;
    CHECK(registerClasses(registry) && registerExtendedClasses(registry));
    Control root, target;
    EditControl editor;
    CHECK(root.addObject(target) && root.addObject(editor) && editor.select(target) &&
          editor.move({4, 5}));
    for (const auto& words : {std::vector<UInt32>{1}, std::vector<UInt32>{1, 0xdecafbad, 0}}) {
        MemRWStream broken{std::as_bytes(std::span(words))};
        CHECK(!editor.loadSelection(broken, registry) && root.size() == 2 &&
              editor.selection().front() == &target && target.fields().position == Point2I(4, 5));
    }
    CHECK(editor.undo() && target.fields().position == Point2I{});
}
}

int opent1_test_gui_editor(int argc, char** argv) {
    return test::run([&] {
        auto frames = test::nativeEditorFrames();
        CHECK(frames.size() == 64);
        histories();
        preparations();
        nativeFailures();
        std::vector<std::shared_ptr<const gfx::Bitmap>> retained;
        for (auto& frame : frames) {
            retained.push_back(std::make_shared<const gfx::Bitmap>(std::move(frame)));
        }
        test::retainedGuiGpu(retained, argc > 1 ? argv[1] : "");
        std::puts(
            "Original EditControl: 64 histories, 5632 exact states/frames and four bounded file "
            "failures; modern history/capture/preparation rollback, retry and 64 retained frames");
    });
}

namespace {
int opent1_test_gui_editor_default() {
    char program[] = "opent1_tests";
    char* args[] = {program, nullptr};
    return opent1_test_gui_editor(1, args);
}
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("gui", "gui_editor", opent1_test_gui_editor_default);
