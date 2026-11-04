#include <opent1/gui/containers.hpp>
#include <cmath>

namespace opent1::gui {
namespace {
using Error = Persistent::Error;

bool inside(Point2I p, RectI r) {
    return r.valid() && p.x >= r.left && p.y >= r.top && p.x <= r.right && p.y <= r.bottom;
}

bool coordinate(Int64 n) {
    return n >= -Control::MaxCoordinate && n <= Control::MaxCoordinate;
}

bool dimensions(Point2I p) {
    return p.x >= 0 && p.y >= 0 && p.x <= 32768 && p.y <= 32768;
}

RectI translated(RectI r, Point2I p) {
    return {r.left + p.x, r.top + p.y, r.right + p.x, r.bottom + p.y};
}
}

Scroll::Scroll() {
    fields_.extent = {200, 200};
}

ScrollContent* Scroll::content() const {
    return objects().size() == 1 ? dynamic_cast<ScrollContent*>(objects().front()) : nullptr;
}

bool Scroll::ensureContent() {
    if (content()) {
        return true;
    }
    if (!objects().empty()) {
        return false;
    }
    auto next = std::make_unique<ScrollContent>();
    if (!Control::addObject(*next)) {
        return false;
    }
    ownedContent_ = std::move(next);
    return true;
}

bool Scroll::addObject(sim::SimObject& object) {
    if (dynamic_cast<ScrollContent*>(&object)) {
        return objects().empty() && Control::addObject(object);
    }
    return ensureContent() && content()->addObject(object);
}

bool Scroll::removeObject(sim::SimObject& object) {
    if (object.getGroup() == this) {
        return Control::removeObject(object);
    }
    auto* c = content();
    return c && c->removeObject(object);
}

bool Scroll::thumbs(Point2I p, Point2I e, Geometry& g) const {
    g.childPosition = p;
    g.childExtent = e;
    if (g.horizontalEnabled) {
        if (e.x <= g.viewport.x || e.x <= 0) {
            return false;
        }
        g.x = -float(p.x) / float(e.x - g.viewport.x);
        g.width = float(g.viewport.x) / float(e.x);
        const auto length = g.horizontal ? g.horizontalTrack.right - g.horizontalTrack.left : 0;
        g.horizontalSize =
            constantThumb ? baseThumbSize : std::max(baseThumbSize, Int32(g.width * float(length)));
        g.horizontalSize = std::min(length, g.horizontalSize);
        g.horizontalThumb =
            Int32(float(g.horizontalTrack.left) + g.x * float(length - g.horizontalSize));
    } else {
        g.x = 0;
        g.width = 1;
    }
    if (g.verticalEnabled) {
        if (e.y <= g.viewport.y || e.y <= 0) {
            return false;
        }
        g.y = -float(p.y) / float(e.y - g.viewport.y);
        g.height = float(g.viewport.y) / float(e.y);
        const auto length = g.vertical ? g.verticalTrack.bottom - g.verticalTrack.top : 0;
        g.verticalSize = constantThumb ? baseThumbSize
                                       : std::max(baseThumbSize, Int32(g.height * float(length)));
        g.verticalSize = std::min(length, g.verticalSize);
        g.verticalThumb = Int32(float(g.verticalTrack.top) + g.y * float(length - g.verticalSize));
    } else {
        g.y = 0;
        g.height = 1;
    }
    return true;
}

bool Scroll::moveChildren(Point2I delta) {
    auto* c = content();
    if (!c) {
        return false;
    }
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& child) -> void {
        undo.push_back(child.checkpoint());
        for (auto* o : child.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    for (auto* object : c->objects()) {
        save(save, *static_cast<Control*>(object));
    }
    for (auto* object : c->objects()) {
        auto* child = static_cast<Control*>(object);
        const auto p = child->fields().position;
        if (!coordinate(Int64(p.x) + delta.x) || !coordinate(Int64(p.y) + delta.y) ||
            !child->resize({p.x + delta.x, p.y + delta.y}, child->fields().extent)) {
            for (auto& restore : undo) {
                restore();
            }
            return false;
        }
    }
    return true;
}

bool Scroll::computeSizes() {
    const auto header = headerSize();
    if (horizontalPolicy < AlwaysOn || horizontalPolicy > Dynamic || verticalPolicy < AlwaysOn ||
        verticalPolicy > Dynamic || borderThickness < 0 || borderThickness > 1024 ||
        barThickness < 1 || barThickness > 1024 || arrowLength < 1 || arrowLength > 1024 ||
        baseThumbSize < 1 || baseThumbSize > 2048 || !dimensions(header)) {
        return false;
    }
    if (!ensureContent()) {
        return false;
    }
    auto* c = content();
    Geometry g;
    g.horizontal = horizontalPolicy == AlwaysOn;
    g.vertical = verticalPolicy == AlwaysOn;
    Int32 thickness = barThickness, arrow = arrowLength, base = baseThumbSize;
    if (frames) {
        if (frames->bitmaps.size() < 25 || !frames->valid()) {
            return false;
        }
        thickness = frames->bitmaps[23].width;
        arrow = frames->bitmaps[2].height;
        base = frames->bitmaps[17].height + frames->bitmaps[16].height;
        if (thickness < 1 || thickness > 1024 || arrow < 1 || arrow > 1024 || base < 1) {
            return false;
        }
    }
    Point2I p{}, e{};
    const bool children = !c->objects().empty();
    if (children) {
        auto* child = static_cast<Control*>(c->objects().front());
        p = child->fields().position;
        e = child->fields().extent;
        // Preserve the original ordered extent accumulation (including the left
        // edge update before the right edge comparison).
        for (auto* object : c->objects().subspan(1)) {
            const auto& f = static_cast<Control*>(object)->fields();
            p.x = std::min(p.x, f.position.x);
            p.y = std::min(p.y, f.position.y);
            e.x = std::max(e.x, f.position.x + f.extent.x - p.x);
            e.y = std::max(e.y, f.position.y + f.extent.y - p.y);
        }
    }
    Point2I ext{fields_.extent.x - 2 * borderThickness, fields_.extent.y - 2 * borderThickness};
    if (g.vertical) {
        ext.x -= thickness;
    }
    if (g.horizontal) {
        ext.y -= thickness;
    }
    if (children && e.x > ext.x && horizontalPolicy == Dynamic) {
        g.horizontal = true;
        ext.y -= thickness;
    }
    if (children && e.y > ext.y && verticalPolicy == Dynamic) {
        g.vertical = true;
        ext.x -= thickness;
        if (e.x > ext.x && !g.horizontal && horizontalPolicy == Dynamic) {
            g.horizontal = true;
            ext.y -= thickness;
        }
    }
    if (!dimensions(ext) || header.x > ext.x || header.y > ext.y) {
        return false;
    }
    g.viewport = ext;
    const auto b = borderThickness, w = fields_.extent.x, h = fields_.extent.y;
    if (g.horizontal) {
        g.left = {b + header.x, h - b - thickness, b + header.x + arrow, h - b};
        g.right = {w - b - (g.vertical ? thickness : 0) - arrow,
                   h - b - thickness,
                   w - b - (g.vertical ? thickness : 0),
                   h - b};
        g.horizontalTrack = {g.left.right, g.left.top, g.right.left, g.right.bottom};
        if (g.horizontalTrack.right < g.horizontalTrack.left) {
            return false;
        }
    }
    if (g.vertical) {
        g.up = {w - b - thickness, b + header.y, w - b, b + header.y + arrow};
        g.down = {w - b - thickness,
                  h - b - (g.horizontal ? thickness : 0) - arrow,
                  w - b,
                  h - b - (g.horizontal ? thickness : 0)};
        g.verticalTrack = {g.up.left, g.up.bottom, g.down.right, g.down.top};
        if (g.verticalTrack.bottom < g.verticalTrack.top) {
            return false;
        }
    }
    Point2I delta;
    if (children) {
        delta.x = p.x > 0 ? -p.x : ext.x > p.x + e.x ? std::min(-p.x, ext.x - p.x - e.x) : 0;
        delta.y = p.y > 0 ? -p.y : ext.y > p.y + e.y ? std::min(-p.y, ext.y - p.y - e.y) : 0;
    }
    const auto old = c->checkpoint();
    if (!c->resize({b, b}, ext) || !moveChildren(delta)) {
        old();
        return false;
    }
    p += delta;
    e.x = std::max(e.x, ext.x);
    e.y = std::max(e.y, ext.y);
    g.horizontalEnabled = e.x > ext.x;
    g.verticalEnabled = e.y > ext.y;
    barThickness = thickness;
    arrowLength = arrow;
    baseThumbSize = base;
    if (!thumbs(p, e, g)) {
        return false;
    }
    if (!c->objects().empty()) {
        if (auto* array = dynamic_cast<Array*>(c->objects().front())) {
            array->header = header;
        }
    }
    geometry_ = g;
    return true;
}

void Scroll::wake() {
    if (!computeSizes()) {
        throw std::runtime_error("GUI scroll layout");
    }
}

void Scroll::tick(double) {
    wake();
}

bool Scroll::scrollTo(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y) || !computeSizes()) {
        return false;
    }
    x = std::clamp(x, 0.f, 1.f);
    y = std::clamp(y, 0.f, 1.f);
    auto next = geometry_;
    Point2I p{next.horizontalEnabled ? -Int32(x * float(next.childExtent.x - next.viewport.x)) : 0,
              next.verticalEnabled ? -Int32(y * float(next.childExtent.y - next.viewport.y)) : 0};
    const Point2I delta{x == next.x ? 0 : p.x - next.childPosition.x,
                        y == next.y ? 0 : p.y - next.childPosition.y};
    p = next.childPosition + delta;
    if (!thumbs(p, next.childExtent, next) || !moveChildren(delta)) {
        return false;
    }
    geometry_ = next;
    return true;
}

Scroll::Region Scroll::hitRegion(Point2I p) const {
    const auto& g = geometry_;
    if (g.verticalEnabled && g.vertical) {
        if (inside(p, g.up)) {
            return Region::UpArrow;
        }
        if (inside(p, g.down)) {
            return Region::DownArrow;
        }
        if (inside(p, g.verticalTrack)) {
            return p.y < g.verticalThumb                    ? Region::UpPage
                   : p.y < g.verticalThumb + g.verticalSize ? Region::VerticalThumb
                                                            : Region::DownPage;
        }
    }
    if (g.horizontalEnabled && g.horizontal) {
        if (inside(p, g.left)) {
            return Region::LeftArrow;
        }
        if (inside(p, g.right)) {
            return Region::RightArrow;
        }
        if (inside(p, g.horizontalTrack)) {
            return p.x < g.horizontalThumb                      ? Region::LeftPage
                   : p.x < g.horizontalThumb + g.horizontalSize ? Region::HorizontalThumb
                                                                : Region::RightPage;
        }
    }
    return Region::None;
}

bool Scroll::scrollBy(Region r) {
    if (!computeSizes()) {
        return false;
    }
    auto* c = content();
    const auto& g = geometry_;
    if (g.verticalEnabled && !c->objects().empty()) {
        if (auto* array = dynamic_cast<Array*>(c->objects().front());
            array && array->cellSize.y > 0 && array->cells.y > 0) {
            const auto page =
                (c->fields().extent.y / array->cellSize.y) * (array->cellSize.y - 1) - 1;
            const auto amount = r == Region::UpPage      ? page
                                : r == Region::DownPage  ? -page
                                : r == Region::UpArrow   ? array->cellSize.y
                                : r == Region::DownArrow ? -array->cellSize.y
                                                         : 0;
            if (amount) {
                auto p = array->fields().position;
                p.y = std::min(
                    0, std::max(c->fields().extent.y - array->fields().extent.y, p.y + amount));
                return array->resize(p, array->fields().extent) && computeSizes();
            }
        }
    }
    float lx = .1f, ly = .1f, px = .2f, py = .2f;
    if (!c->objects().empty()) {
        const auto e = static_cast<Control*>(c->objects().front())->fields().extent;
        if (e.x) {
            lx = float(Int32(.1f * float(e.x))) / float(e.x);
            px = float(Int32(.2f * float(e.x))) / float(e.x);
        }
        ly = py = .1f;
    }
    auto x = g.x, y = g.y;
    switch (r) {
        case Region::UpArrow:
            y -= ly;
            break;
        case Region::DownArrow:
            y += ly;
            break;
        case Region::LeftArrow:
            x -= lx;
            break;
        case Region::RightArrow:
            x += lx;
            break;
        case Region::UpPage:
            y -= py;
            break;
        case Region::DownPage:
            y += py;
            break;
        case Region::LeftPage:
            x -= px;
            break;
        case Region::RightPage:
            x += px;
            break;
        default:
            return true;
    }
    return scrollTo(x, y);
}

bool Scroll::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        depressed_ = false;
        region_ = Region::None;
        return true;
    }
    if (disabled) {
        return Control::event(e);
    }
    if (!computeSizes()) {
        return false;
    }
    if (e.type == EventType::Wheel) {
        return scrollBy(e.wheel > 0 ? Region::UpArrow : Region::DownArrow);
    }
    if (e.type == EventType::KeyDown && willFirstRespond) {
        switch (e.key) {
            case Key::Left:
                return scrollBy(Region::LeftArrow);
            case Key::Right:
                return scrollBy(Region::RightArrow);
            case Key::Up:
                return scrollBy(Region::UpArrow);
            case Key::Down:
                return scrollBy(Region::DownArrow);
            case Key::PageUp:
                return scrollBy(Region::UpPage);
            case Key::PageDown:
                return scrollBy(Region::DownPage);
            default:
                break;
        }
    }
    if (e.type == EventType::Repeat && e.button == 1) {
        if (!depressed_) {
            return true;
        }
        if (!scrollBy(region_)) {
            return false;
        }
        if (region_ != Region::VerticalThumb && region_ != Region::HorizontalThumb) {
            depressed_ = hitRegion(mouse_) == region_;
        }
        return true;
    }
    if (e.type == EventType::Down && e.button == 1) {
        if (!canvas() || !canvas()->lockMouse(this)) {
            return false;
        }
        if (willFirstRespond) {
            canvas()->makeFirstResponder(this);
        }
        mouse_ = globalToLocal(e.point);
        region_ = hitRegion(mouse_);
        depressed_ = true;
        if (!scrollBy(region_)) {
            return false;
        }
        if (region_ == Region::VerticalThumb) {
            anchor_ = geometry_.y;
            thumbDelta_ = mouse_.y - geometry_.verticalThumb;
        } else if (region_ == Region::HorizontalThumb) {
            anchor_ = geometry_.x;
            thumbDelta_ = mouse_.x - geometry_.horizontalThumb;
        }
        return true;
    }
    if (e.type == EventType::Up && e.button == 1) {
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        region_ = Region::None;
        depressed_ = false;
        return true;
    }
    if (e.type == EventType::Drag && e.button == 1 && canvas() && canvas()->capture() == this) {
        mouse_ = globalToLocal(e.point);
        const auto& g = geometry_;
        if (region_ == Region::VerticalThumb) {
            auto r = g.verticalTrack;
            r.left -= barThickness;
            r.right += barThickness;
            r.top -= barThickness;
            r.bottom += barThickness;
            const auto length = g.verticalTrack.bottom - g.verticalTrack.top - g.verticalSize;
            return scrollTo(g.x,
                            inside(mouse_, r) && length > 0
                                ? float(mouse_.y - thumbDelta_ - g.verticalTrack.top) /
                                      float(length)
                                : anchor_);
        }
        if (region_ == Region::HorizontalThumb) {
            auto r = g.horizontalTrack;
            r.left -= barThickness;
            r.right += barThickness;
            r.top -= barThickness;
            r.bottom += barThickness;
            const auto length = g.horizontalTrack.right - g.horizontalTrack.left - g.horizontalSize;
            return scrollTo(inside(mouse_, r) && length > 0
                                ? float(mouse_.x - thumbDelta_ - g.horizontalTrack.left) /
                                      float(length)
                                : anchor_,
                            g.y);
        }
        depressed_ = hitRegion(mouse_) == region_;
        return true;
    }
    return Control::event(e);
}

void Scroll::drawBorder(gfx::SoftwareSurface& s, RectI border) const {
    const auto& st = fields_.style;
    if (st.opaque) {
        s.fill(border, disabled ? st.ghostFill : st.fill);
    }
    if (st.border) {
        s.rectangle(border, disabled ? st.ghostBorder : st.borderColor);
    }
}

bool Scroll::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    const auto& g = geometry_;
    const auto header = headerSize();
    const RectI border{
        p.x + header.x, p.y + header.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1};
    drawBorder(s, border);
    const auto part = [&](int index, RectI r) {
        if (!r.valid()) {
            return true;
        }
        r = translated(r, p);
        if (!frames) {
            s.fill(r, UInt8(index % 3 == 1 ? 249 : 250));
            s.rectangle(r, 254);
            return true;
        }
        if (std::size_t(index) >= frames->bitmaps.size()) {
            return false;
        }
        return s.stretch(frames->bitmaps[index], r);
    };
    // Native scroll geometry uses half-open RectI edges. Raster destination
    // rectangles use inclusive edges; arrows/caps retain their bitmap size.
    const auto arrow = [&](int base, Region r, RectI rect, bool enabled) {
        --rect.right;
        --rect.bottom;
        return part(base + (!enabled ? 0 : depressed_ && region_ == r ? 1 : 2), rect);
    };
    if (g.vertical) {
        if (!arrow(0, Region::UpArrow, g.up, g.verticalEnabled) ||
            !arrow(3, Region::DownArrow, g.down, g.verticalEnabled)) {
            return false;
        }
        if (!g.verticalEnabled) {
            auto track = g.verticalTrack;
            --track.right;
            --track.bottom;
            if (!part(21, track)) {
                return false;
            }
        } else {
            const auto t = g.verticalTrack;
            const bool selected = depressed_ && region_ == Region::VerticalThumb;
            const int delta = selected && frames && frames->bitmaps.size() >= 31 ? 13 : 0;
            if (!part(depressed_ && region_ == Region::UpPage ? 22 : 23,
                      {t.left, t.top, t.right - 1, g.verticalThumb - 1}) ||
                !part(depressed_ && region_ == Region::DownPage ? 22 : 23,
                      {t.left, g.verticalThumb + g.verticalSize, t.right - 1, t.bottom - 1})) {
                return false;
            }
            const auto top = frames ? frames->bitmaps[17 + delta].height : 2,
                       bottom = frames ? frames->bitmaps[16 + delta].height : 2;
            if (!part(17 + delta,
                      {t.left, g.verticalThumb, t.right - 1, g.verticalThumb + top - 1}) ||
                !part(15 + delta,
                      {t.left,
                       g.verticalThumb + top,
                       t.right - 1,
                       g.verticalThumb + g.verticalSize - bottom - 1}) ||
                !part(16 + delta,
                      {t.left,
                       g.verticalThumb + g.verticalSize - bottom,
                       t.right - 1,
                       g.verticalThumb + g.verticalSize - 1})) {
                return false;
            }
        }
    }
    if (g.horizontal) {
        if (!arrow(6, Region::LeftArrow, g.left, g.horizontalEnabled) ||
            !arrow(9, Region::RightArrow, g.right, g.horizontalEnabled)) {
            return false;
        }
        if (!g.horizontalEnabled) {
            auto track = g.horizontalTrack;
            --track.right;
            --track.bottom;
            if (!part(18, track)) {
                return false;
            }
        } else {
            const auto t = g.horizontalTrack;
            const bool selected = depressed_ && region_ == Region::HorizontalThumb;
            const int delta = selected && frames && frames->bitmaps.size() >= 31 ? 13 : 0;
            if (!part(depressed_ && region_ == Region::LeftPage ? 19 : 20,
                      {t.left, t.top, g.horizontalThumb - 1, t.bottom - 1}) ||
                !part(depressed_ && region_ == Region::RightPage ? 19 : 20,
                      {g.horizontalThumb + g.horizontalSize, t.top, t.right - 1, t.bottom - 1})) {
                return false;
            }
            const auto left = frames ? frames->bitmaps[13 + delta].width : 2,
                       right = frames ? frames->bitmaps[14 + delta].width : 2;
            if (!part(13 + delta,
                      {g.horizontalThumb, t.top, g.horizontalThumb + left - 1, t.bottom - 1}) ||
                !part(12 + delta,
                      {g.horizontalThumb + left,
                       t.top,
                       g.horizontalThumb + g.horizontalSize - right - 1,
                       t.bottom - 1}) ||
                !part(14 + delta,
                      {g.horizontalThumb + g.horizontalSize - right,
                       t.top,
                       g.horizontalThumb + g.horizontalSize - 1,
                       t.bottom - 1})) {
                return false;
            }
        }
    }
    if (g.horizontal && g.vertical) {
        const auto width = frames ? frames->bitmaps[24].width : barThickness,
                   height = frames ? frames->bitmaps[24].height : barThickness;
        if (!part(24,
                  {g.right.right,
                   g.right.top,
                   g.right.right + width - 1,
                   g.right.top + height - 1})) {
            return false;
        }
    }
    return drawChildren(s, p, clip);
}

std::function<void()> Scroll::checkpoint() {
    auto base = Control::checkpoint();
    const auto g = geometry_;
    const auto r = region_;
    const auto d = depressed_;
    const auto m = mouse_;
    const auto a = anchor_;
    const auto t = thumbDelta_, bar = barThickness, arrow = arrowLength, thumb = baseThumbSize;
    return [this, base = std::move(base), g, r, d, m, a, t, bar, arrow, thumb] {
        base();
        geometry_ = g;
        region_ = r;
        depressed_ = d;
        mouse_ = m;
        anchor_ = a;
        thumbDelta_ = t;
        barThickness = bar;
        arrowLength = arrow;
        baseThumbSize = thumb;
    };
}

Error Scroll::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (horizontalPolicy < AlwaysOn || horizontalPolicy > Dynamic || verticalPolicy < AlwaysOn ||
        verticalPolicy > Dynamic || !s.write(arrayTag) || !s.write(willFirstRespond) ||
        !s.write(horizontalPolicy) || !s.write(verticalPolicy) || !s.write(constantThumb)) {
        return Error::WriteError;
    }
    return Control::write(s, v, u, c);
}

Error Scroll::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 tag{}, h{}, vert{};
    bool respond{}, constant{};
    if (!s.read(tag) || !s.read(respond) || !s.read(h) || !s.read(vert) || !s.read(constant) ||
        h < AlwaysOn || h > Dynamic || vert < AlwaysOn || vert > Dynamic) {
        return Error::ReadError;
    }
    const auto r = Control::read(s, v, u, c);
    if (r == Error::Ok) {
        arrayTag = tag;
        willFirstRespond = respond != 0;
        horizontalPolicy = h;
        verticalPolicy = vert;
        constantThumb = constant;
    }
    return r;
}

Error Matrix::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!dimensions(header) || !s.write(header.x) || !s.write(header.y)) {
        return Error::WriteError;
    }
    return Scroll::write(s, v, u, c);
}

Error Matrix::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Point2I h;
    if (!s.read(h.x) || !s.read(h.y) || !dimensions(h)) {
        return Error::ReadError;
    }
    const auto r = Scroll::read(s, v, u, c);
    if (r == Error::Ok) {
        header = h;
    }
    return r;
}

class ComboBox::Popup : public Control {
public:
    class Items : public TextList {
    public:
        explicit Items(Popup& popup) : popup_(popup) {}

        void action() override {
            popup_.defer(true);
        }

    private:
        Popup& popup_;
    };

    explicit Popup(ComboBox& owner) : items(*this), owner_(owner) {
        scroll.horizontalPolicy = Scroll::AlwaysOff;
        scroll.willFirstRespond = false;
        Control::addObject(scroll);
        scroll.addObject(items);
    }

    Scroll scroll;
    Items items;

    Control* hit(Point2I) override {
        return this;
    }

    bool event(const Event& e) override {
        if (e.type == EventType::KeyDown && e.key == Key::Escape) {
            defer(false);
            return true;
        }
        if (e.type == EventType::KeyDown && e.key == Key::Return) {
            defer(true);
            return true;
        }
        if (e.type == EventType::Cancel) {
            return true;
        }
        if (e.type == EventType::Down && e.button == 1) {
            if (scroll.hitRegion(scroll.globalToLocal(e.point)) != Scroll::Region::None) {
                return scroll.event(e);
            }
            const auto p = items.globalToLocal(e.point);
            const Point2I cell{0, items.cellSize.y > 0 && p.y >= 0 ? p.y / items.cellSize.y : -1};
            if (p.x >= 0 && p.x < items.fields().extent.x && cell.y >= 0 &&
                std::size_t(cell.y) < items.entries.size()) {
                if (!items.select(cell)) {
                    return false;
                }
                defer(true);
            } else {
                defer(false);
            }
            return true;
        }
        if (e.type == EventType::Move) {
            auto copy = e;
            copy.type = EventType::Move;
            return items.event(copy);
        }
        if (e.type == EventType::Wheel) {
            return scroll.scrollBy(e.wheel > 0 ? Scroll::Region::UpArrow
                                               : Scroll::Region::DownArrow);
        }
        if (e.type == EventType::Up || e.type == EventType::Drag || e.type == EventType::Repeat) {
            return true;
        }
        return Control::event(e);
    }

private:
    void defer(bool accept) {
        canvas()->afterDispatch([this, accept] {
            owner_.dismiss(accept);
        });
    }

    ComboBox& owner_;
};

ComboBox::ComboBox() {
    active = true;
}

ComboBox::~ComboBox() {
    close();
}

TextList& ComboBox::list() {
    if (!popup_) {
        popup_ = std::make_unique<Popup>(*this);
    }
    return popup_->items;
}

Scroll& ComboBox::popupScroll() {
    list();
    return popup_->scroll;
}

bool ComboBox::isOpen() const {
    return popup_ && popup_->canvas();
}

bool ComboBox::open() {
    if (isOpen() || !canvas()) {
        return false;
    }
    list();
    auto& pop = *popup_;
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& c) -> void {
        undo.push_back(c.checkpoint());
        for (auto* o : c.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    save(save, pop);
    const auto oldFont = pop.items.font, oldHighlight = pop.items.highlightFont;
    bool published = false;

    struct Restore {
        std::function<void()> restore;

        ~Restore() {
            restore();
        }
    } restore{[&] {
        if (!published) {
            for (auto& action : undo) {
                action();
            }
            pop.items.font = oldFont;
            pop.items.highlightFont = oldHighlight;
        }
    }};

    pop.items.font = font;
    pop.items.highlightFont = highlightFont ? highlightFont : font;
    auto* root = canvas();
    auto pos = localToGlobal({});
    const auto top = root->top();
    if (!top) {
        return false;
    }
    const auto available = top->fields().extent.y;
    const auto h = std::min(100, std::max(40, available));
    if (pos.y + fields_.extent.y + h > available) {
        pos.y = std::max(0, pos.y - h);
    } else {
        pos.y += fields_.extent.y;
    }
    if (!pop.resize({}, top->fields().extent) ||
        !pop.scroll.resize(pos, {std::max(40, fields_.extent.x), h})) {
        return false;
    }
    if (!pop.scroll.computeSizes()) {
        return false;
    }
    // Native setDim uses the content width minus one, after loading scroll art.
    // Keep that width through TextList::wake, which derives cellSize from extent.
    const auto width = std::max(1, pop.scroll.content()->fields().extent.x - 1);
    if (!pop.items.resize(pop.items.fields().position, {width, pop.items.fields().extent.y})) {
        return false;
    }
    auto* previous = root->focus();
    if (!root->pushDialog(pop)) {
        return false;
    }
    if (!root->lockMouse(&pop)) {
        root->popDialog();
        root->makeFirstResponder(previous);
        return false;
    }
    published = true;
    return true;
}

bool ComboBox::dismiss(bool accept) {
    if (!isOpen()) {
        return false;
    }
    auto* root = popup_->canvas();
    if (root->top() != popup_.get()) {
        return false;
    }
    const auto old = text;
    const auto life = lifetime();
    auto next = text;
    if (accept) {
        if (const auto* entry = popup_->items.selection()) {
            next = entry->text;
        }
    }
    // Publish the value only after the action accepts it. A rejected action
    // leaves both popup and selection available for retry.
    text = next;
    try {
        if (accept) {
            selectionCommitted();
            if (life.expired())
                return true;
            Text::action();
            if (life.expired())
                return true;
        }
    } catch (...) {
        if (life.expired())
            return true;
        text = old;
        return false;
    }
    try {
        root->unlockMouse();
    } catch (...) {
        if (life.expired())
            return true;
        text = old;
        return false;
    }
    if (life.expired())
        return true;
    if (!root->popDialog()) {
        if (life.expired())
            return true;
        text = old;
        return false;
    }
    if (life.expired())
        return true;
    root->makeFirstResponder(this);
    return true;
}

void ComboBox::action() {
    if (canvas()) {
        canvas()->afterDispatch([this] {
            open();
        });
    }
}

void ComboBox::close() {
    if (isOpen()) {
        auto* root = popup_->canvas();
        if (root->top() == popup_.get()) {
            root->popDialog();
        }
    }
}

void ComboBox::onRemove() {
    close();
    Text::onRemove();
}

bool ComboBox::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    const RectI r{p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1};
    s.fill(r, 253);
    s.rectangle(r, 254);
    return Text::draw(s, p, clip);
}

bool registerContainerClasses(Persistent::ClassRegistry& r) {
    return r.registerClass<Scroll>("SimGui::ScrollCtrl", makeFourCC('S', 'G', 's', 'c')) &&
           r.registerClass<ScrollContent>("SimGui::ScrollContentCtrl",
                                          makeFourCC('S', 'G', 's', 'C')) &&
           r.registerClass<Matrix>("SimGui::MatrixCtrl", makeFourCC('S', 'G', 'm', 'c')) &&
           r.registerClass<ComboBox>("SimGui::ComboBox", makeFourCC('S', 'G', 'c', 'b'));
}
} // namespace opent1::gui
