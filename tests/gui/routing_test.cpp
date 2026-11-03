#include "test_support.hpp"
#include <ranges>
#include <opent1/gui/controls.hpp>
#include <opent1/core/file_stream.hpp>
using namespace opent1;
using namespace opent1::gui;

namespace {
UInt32 word(StreamIO& s) {
    UInt32 n{};
    CHECK(s.read(n));
    return n;
}
}

int opent1_test_gui_routing() {
    return test::run([] {
        FileRStream in(OPENT1_GUI_ROUTING_FIXTURE);
        CHECK(word(in) == 0x48573742 && word(in) == 1 && word(in) == 256);
        Canvas canvas;
        Control root, behind;
        TextWrap wrap;
        CHECK(root.resize({}, {200, 160}) && root.addObject(behind) && root.addObject(wrap) &&
              canvas.content(&root));
        unsigned direct = 0, routed = 0;
        for (auto history : std::views::iota(0, 256)) {
            const int x = 5 + history % 13, y = 7 + history % 17, w = 30 + history % 19,
                      h = 20 + history % 11;
            CHECK(behind.resize({x, y}, {w, h}) && wrap.resize({x, y}, {w, h}));
            wrap.ignoreClick = (history & 1) != 0;
            canvas.editMode = (history & 2) != 0;
            wrap.visible((history & 4) == 0);
            const std::array<Point2I, 7> points{{{x, y},
                                                 {x + w - 1, y + h - 1},
                                                 {x - 1, y},
                                                 {x + w, y},
                                                 {x, y - 1},
                                                 {x, y + h},
                                                 {x + w / 2, y + h / 2}}};
            CHECK(word(in) == points.size());
            for (auto p : points) {
                CHECK(word(in) == UInt32(wrap.pointInControl({p.x - x, p.y - y})));
                ++direct;
                auto* hit = root.hit(p);
                CHECK(word(in) == UInt32(hit == &wrap ? 2 : hit == &behind ? 1 : 0));
                ++routed;
            }
        }
        CHECK(in.getPosition() == Int32(in.getSize()) && direct == 1792 && routed == 1792);
        std::printf("256 native TextWrap edit/ignore/visibility histories; %u direct and %u "
                    "hierarchy hit comparisons\n",
                    direct,
                    routed);
    });
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("gui", "gui_routing", opent1_test_gui_routing);
