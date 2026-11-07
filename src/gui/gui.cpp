#include <opent1/gui/gui.hpp>
#include <ranges>
#include <opent1/gui/controls.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cctype>
#include <cstring>

namespace opent1::gui {
namespace {
bool valid(const Fields& f) {
    const auto coord = [](Int32 v) {
        return v >= -Control::MaxCoordinate && v <= Control::MaxCoordinate;
    };
    return coord(f.position.x) && coord(f.position.y) && f.extent.x >= 0 && f.extent.y >= 0 &&
           f.extent.x <= 32768 && f.extent.y <= 32768 && f.horizontal >= 0 && f.horizontal <= 4 &&
           f.vertical >= 0 && f.vertical <= 4 && f.command.size() <= 80 &&
           f.alternateCommand.size() <= 80 && f.variable.size() <= 80 &&
           f.command.find('\0') == std::string::npos &&
           f.alternateCommand.find('\0') == std::string::npos &&
           f.variable.find('\0') == std::string::npos;
}

Point2I add(Point2I a, Point2I b) {
    return {a.x + b.x, a.y + b.y};
}

RectI bounds(Point2I p, Point2I e) {
    return {p.x, p.y, p.x + e.x - 1, p.y + e.y - 1};
}

bool in(Point2I p, Point2I e) {
    return p.x >= 0 && p.y >= 0 && p.x < e.x && p.y < e.y;
}

bool string8(StreamIO& s, std::string& value) {
    UInt8 n{};
    if (!s.read(n) || n > 80) {
        return false;
    }
    std::string result(n, '\0');
    if (n && !s.readBytes(std::as_writable_bytes(std::span(result.data(), result.size())))) {
        return false;
    }
    if (result.find('\0') != std::string::npos) {
        return false;
    }
    value = std::move(result);
    return true;
}

bool string8(StreamIO& s, const std::string& value) {
    return s.write(static_cast<UInt8>(value.size())) &&
           (value.empty() || s.writeBytes(std::as_bytes(std::span(value.data(), value.size()))));
}

bool pointRead(StreamIO& s, Point2I& p) {
    return s.read(p.x) && s.read(p.y);
}

bool pointWrite(StreamIO& s, Point2I p) {
    return s.write(p.x) && s.write(p.y);
}

struct ClipRestore {
    gfx::SoftwareSurface& surface;
    RectI clip;

    ~ClipRestore() {
        surface.setClip(clip);
    }
};

bool utf8(std::string_view s, bool multiline = false) {
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<UInt8>(s[i++]);
        if (multiline && (c == '\n' || c == '\t')) {
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        if (c < 0x80) {
            continue;
        }
        int n{};
        UInt32 cp{}, minimum{};
        if (c >= 0xc2 && c <= 0xdf) {
            n = 1;
            cp = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2;
            cp = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3;
            cp = c & 7;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (s.size() - i < static_cast<std::size_t>(n)) {
            return false;
        }
        while (n--) {
            c = static_cast<UInt8>(s[i++]);
            if ((c & 0xc0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

bool boundary(std::string_view s, std::size_t i) {
    return i <= s.size() && (i == s.size() || (static_cast<UInt8>(s[i]) & 0xc0) != 0x80);
}

std::string expandedTabs(std::string_view value) {
    std::string result;
    for (const auto c : value) {
        if (c == '\t') {
            result.append(4, ' ');
        } else {
            result += c;
        }
    }
    return result;
}

std::size_t previous(std::string_view s, std::size_t i) {
    if (i) {
        do {
            --i;
        } while (i && (static_cast<UInt8>(s[i]) & 0xc0) == 0x80);
    }
    return i;
}

std::size_t next(std::string_view s, std::size_t i) {
    if (i < s.size()) {
        do {
            ++i;
        } while (i < s.size() && (static_cast<UInt8>(s[i]) & 0xc0) == 0x80);
    }
    return i;
}

void lifecycle(Control& c, bool waking) {
    for (auto* child : c.objects()) {
        lifecycle(*static_cast<Control*>(child), waking);
    }
    if (waking) {
        c.wake();
    } else {
        c.close();
    }
}
}

Control::Control() {
    setStable(true);
}

std::function<void()> Control::checkpoint() {
    auto fields = fields_;
    const auto sprite = mouseCursorTag;
    return [this, fields = std::move(fields), sprite] {
        tag(fields.tag);
        fields_ = fields;
        mouseCursorTag = sprite;
    };
}

Control::~Control() {
    setCanvas(nullptr);
    if (auto* owner = parent()) {
        owner->removeObject(*this);
    }
}

bool Control::onAdd() {
    if (!SimGroup::onAdd()) {
        return false;
    }
    if (!getName().empty()) {
        addToSet(sim::NamedGuiSetId);
    }
    if (fields_.tag) {
        addToSet(sim::TaggedGuiSetId);
    }
    return fields_.variable.empty() || !getManager() || !getManager()->console() ||
           variableChanged(getManager()->console()->getVariable(fields_.variable));
}

bool Control::processArguments(std::span<const std::string> arguments) {
    auto next = fields_;
    if (arguments.size() >= 4) {
        std::array<Int32, 4> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::string_view text = arguments[i];
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
                text.remove_prefix(1);
            }
            if (!text.empty() && text.front() == '+') {
                text.remove_prefix(1);
            }
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), values[i]);
            if (parsed.ec == std::errc::result_out_of_range) {
                return false;
            }
        }
        next.position = {values[0], values[1]};
        next.extent = {values[2], values[3]};
    }
    if (arguments.size() >= 5) {
        next.variable = arguments[4];
    }
    if (arguments.size() >= 6) {
        next.command = arguments[5];
    }
    return configure(std::move(next));
}

void Control::onNameChange(std::string_view) {
    if (getManager()) {
        addToSet(sim::NamedGuiSetId);
    }
}

void Control::tag(UInt32 value) {
    if (getManager()) {
        if (!fields_.tag && value) {
            addToSet(sim::TaggedGuiSetId);
        } else if (fields_.tag && !value) {
            removeFromSet(sim::TaggedGuiSetId);
        }
    }
    fields_.tag = value;
}

void Control::onRemove() {
    setCanvas(nullptr);
    SimGroup::onRemove();
}

void Control::setCanvas(Canvas* value) {
    if (canvas_ == value) {
        return;
    }
    if (canvas_) {
        canvas_->invalidate(*this);
    }
    canvas_ = value;
    if (canvas_) {
        canvas_->layoutDirty_ = true;
        canvas_->retainedDirty_ = true;
    }
    for (auto* object : objects()) {
        static_cast<Control*>(object)->setCanvas(value);
    }
}

bool Control::addObject(sim::SimObject& object) {
    auto* child = dynamic_cast<Control*>(&object);
    if (!child) {
        return false;
    }
    if (child->canvas() && !child->parent() && child->getGroup() != this) {
        return false;
    }
    const auto count = [](auto&& self, const Control& c) -> std::size_t {
        std::size_t total = 1;
        for (auto* o : c.objects()) {
            total += self(self, *static_cast<const Control*>(o));
            if (total > 4096) {
                break;
            }
        }
        return total;
    };
    const Control* root = this;
    while (root->parent()) {
        root = root->parent();
    }
    const Control* oldRoot = child;
    while (oldRoot->parent()) {
        oldRoot = oldRoot->parent();
    }
    if (root != oldRoot && count(count, *root) + count(count, *child) > 4096) {
        return false;
    }
    std::size_t depth = 1;
    for (auto* p = this; p; p = p->parent()) {
        if (p == child || ++depth > 64) {
            return false;
        }
    }
    const auto height = [](auto&& self, const Control& c) -> std::size_t {
        std::size_t h = 1;
        for (auto* o : c.objects()) {
            h = std::max(h, 1 + self(self, *static_cast<const Control*>(o)));
        }
        return h;
    };
    if (depth - 1 + height(height, *child) > 64 || objects().size() >= 4096 ||
        !SimGroup::addObject(object)) {
        return false;
    }
    child->setCanvas(canvas_);
    if (canvas_) {
        canvas_->layoutDirty_ = true;
        canvas_->retainedDirty_ = true;
        canvas_->rebuildTabs();
    }
    return true;
}

bool Control::removeObject(sim::SimObject& object) {
    auto* child = dynamic_cast<Control*>(&object);
    if (!child || child->getGroup() != this) {
        return false;
    }
    child->setCanvas(nullptr);
    const bool result = SimGroup::removeObject(object);
    if (canvas_) {
        canvas_->rebuildTabs();
    }
    return result;
}

bool Control::configure(Fields value) {
    if (!valid(value)) {
        return false;
    }
    if (value == fields_) {
        return true;
    }
    if (!resize(value.position, value.extent)) {
        return false;
    }
    const auto oldTag = fields_.tag;
    if (canvas_) {
        canvas_->layoutDirty_ = true;
        canvas_->retainedDirty_ = true;
    }
    fields_ = std::move(value);
    const auto newTag = fields_.tag;
    fields_.tag = oldTag;
    tag(newTag);
    if (canvas_) {
        canvas_->rebuildTabs();
    }
    return true;
}

bool Control::resize(Point2I position, Point2I extent) {
    struct Change {
        Control* control;
        Fields fields;
    };

    std::vector<Change> changes;
    const auto visit =
        [&](auto&& self, Control& control, Point2I p, Point2I e, std::size_t depth) -> bool {
        if (depth > 64 || changes.size() >= 4096) {
            return false;
        }
        auto fields = control.fields_;
        fields.position = p;
        fields.extent = e;
        if (!valid(fields)) {
            return false;
        }
        changes.push_back({&control, std::move(fields)});
        for (auto* object : control.objects()) {
            auto& child = *static_cast<Control*>(object);
            auto cp = child.fields_.position, ce = child.fields_.extent;
            child.layoutInParent(control.fields_.extent, e, cp, ce);
            if (!self(self, child, cp, ce, depth + 1)) {
                return false;
            }
        }
        return true;
    };
    if (!visit(visit, *this, position, extent, 0)) {
        return false;
    }
    for (auto& change : changes) {
        if (change.control->canvas_ && change.control->fields_ != change.fields) {
            change.control->canvas_->layoutDirty_ = true;
            change.control->canvas_->retainedDirty_ = true;
        }
        change.control->fields_ = std::move(change.fields);
    }
    return true;
}

bool Control::parentResized(Point2I oldExtent, Point2I newExtent) {
    auto p = fields_.position, e = fields_.extent;
    layoutInParent(oldExtent, newExtent, p, e);
    return resize(p, e);
}

void Control::layoutInParent(Point2I oldExtent, Point2I newExtent, Point2I& p, Point2I& e) const {
    // Relative (4) is intentionally a no-op in the active original.
    switch (fields_.horizontal) {
        case 1:
            e.x += newExtent.x - oldExtent.x;
            break;
        case 2:
            p.x += newExtent.x - oldExtent.x;
            break;
        case 3:
            p.x = (newExtent.x - e.x) >> 1;
            break;
        default:
            break;
    }
    switch (fields_.vertical) {
        case 1:
            e.y += newExtent.y - oldExtent.y;
            break;
        case 2:
            p.y += newExtent.y - oldExtent.y;
            break;
        case 3:
            p.y = (newExtent.y - e.y) >> 1;
            break;
        default:
            break;
    }
}

Point2I Control::localToGlobal(Point2I p) const {
    for (auto* c = this; c; c = c->parent()) {
        p = add(p, c->fields_.position);
    }
    return p;
}

Point2I Control::globalToLocal(Point2I p) const {
    const auto origin = localToGlobal({});
    return {p.x - origin.x, p.y - origin.y};
}

Control* Control::hit(Point2I p) {
    for (auto* object : objects() | std::views::reverse) {
        auto* child = static_cast<Control*>(object);
        auto q = Point2I{p.x - child->fields_.position.x, p.y - child->fields_.position.y};
        if (child->visible() && child->pointInControl(q)) {
            return child->hit(q);
        }
    }
    return this;
}

bool Control::pointInControl(Point2I p) const {
    return in(p, fields_.extent);
}

Control* Control::findTag(UInt32 tag) {
    for (auto* object : objects() | std::views::reverse) {
        auto* child = static_cast<Control*>(object);
        if (child->fields_.tag == tag) {
            return child;
        }
        if (auto* found = child->findTag(tag)) {
            return found;
        }
    }
    return nullptr;
}

Control* Control::findNamed(std::string_view name) {
    if (name.empty()) {
        return nullptr;
    }
    const auto equal = [](char a, char b) {
        const auto fold = [](char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : int(static_cast<UInt8>(c));
        };
        return fold(a) == fold(b);
    };
    for (auto* object : objects() | std::views::reverse) {
        auto* child = static_cast<Control*>(object);
        const auto childName = child->getName();
        if (std::equal(name.begin(), name.end(), childName.begin(), childName.end(), equal)) {
            return child;
        }
        if (auto* found = child->findNamed(name)) {
            return found;
        }
    }
    return nullptr;
}

Control* Control::findRootTag(UInt32 value) {
    auto* root = this;
    while (root->parent()) {
        root = root->parent();
    }
    return root->findTag(value);
}

void Control::visible(bool value) {
    if (visible() != value && canvas_) {
        canvas_->retainedDirty_ = true;
    }
    if (value) {
        fields_.flags |= 1;
    } else {
        fields_.flags &= ~1U;
    }
    if (canvas_) {
        canvas_->rebuildTabs();
    }
}

bool Control::event(const Event& e) {
    return parent()                          ? parent()->event(e)
           : canvas_ && canvas_->onUnhandled ? canvas_->onUnhandled(e)
                                             : false;
}

bool Control::message(Control& sender, UInt32 value) {
    return parent() ? parent()->message(sender, value)
                    : !canvas_ || !canvas_->onMessage || canvas_->onMessage(sender, value);
}

bool Control::messageSiblings(UInt32 value) {
    if (!parent()) {
        return false;
    }
    bool accepted = true;
    for (auto* object : parent()->objects()) {
        auto* sibling = static_cast<Control*>(object);
        if (sibling != this) {
            accepted = sibling->message(*this, value) && accepted;
        }
    }
    return accepted;
}

bool Control::drawChildren(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    ClipRestore restore{s, s.clip()};
    for (auto* object : objects()) {
        const auto& child = *static_cast<const Control*>(object);
        if (!child.visible()) {
            continue;
        }
        const auto origin = add(offset, child.fields_.position);
        auto childClip = intersect(clip, bounds(origin, child.fields_.extent));
        if (childClip) {
            s.setClip(*childClip);
            if (!child.draw(s, origin, *childClip)) {
                return false;
            }
        }
    }
    return true;
}

bool Control::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    const auto& style = fields_.style;
    if (style.opaque) {
        s.fill(bounds(offset, fields_.extent), style.fill);
    }
    if (style.border) {
        s.rectangle(bounds(offset, fields_.extent), style.borderColor);
    }
    return drawChildren(s, offset, clip);
}

bool Control::writeFields(StreamIO& s, const Fields& f) {
    if (!valid(f)) {
        return false;
    }
    const auto& st = f.style;
    std::array<char, 81> variable{};
    std::copy(f.variable.begin(), f.variable.end(), variable.begin());
    return s.write(UInt8{4}) && s.write(UInt32{}) && s.write(st.opaque) && s.write(st.fill) &&
           s.write(st.selectedFill) && s.write(st.ghostFill) && s.write(st.border) &&
           s.write(st.borderColor) && s.write(st.selectedBorder) && s.write(st.ghostBorder) &&
           string8(s, f.command) && string8(s, f.alternateCommand) && pointWrite(s, f.position) &&
           pointWrite(s, f.extent) && s.write(f.flags) && s.write(f.tag) && s.write(f.horizontal) &&
           s.write(f.vertical) && s.write(f.helpTag) &&
           s.writeBytes(std::as_bytes(std::span(variable)));
}

bool Control::readFields(StreamIO& s, Fields& destination) {
    Fields f;
    UInt8 version{};
    UInt32 unused{};
    auto& st = f.style;
    if (!s.read(version) || version > 4 || !s.read(unused)) {
        return false;
    }
    if (version == 0) {
        UInt32 ignored{};
        UInt8 a{}, b{};
        if (!s.read(ignored) || !s.read(a) || !s.read(b)) {
            return false;
        }
    } else {
        if (!s.read(st.opaque) || !s.read(st.fill)) {
            return false;
        }
        if (version >= 2 && (!s.read(st.selectedFill) || !s.read(st.ghostFill))) {
            return false;
        }
        if (!s.read(st.border) || !s.read(st.borderColor)) {
            return false;
        }
        if (version >= 2 && (!s.read(st.selectedBorder) || !s.read(st.ghostBorder))) {
            return false;
        }
    }
    if (!string8(s, f.command) || (version >= 3 && !string8(s, f.alternateCommand)) ||
        !pointRead(s, f.position) || !pointRead(s, f.extent) || !s.read(f.flags) ||
        !s.read(f.tag) || !s.read(f.horizontal) || !s.read(f.vertical) ||
        (version >= 4 && !s.read(f.helpTag))) {
        return false;
    }
    std::array<char, 81> variable{};
    if (!s.readBytes(std::as_writable_bytes(std::span(variable)))) {
        return false;
    }
    const auto end = std::ranges::find(variable, '\0');
    if (end == variable.end()) {
        return false;
    }
    f.variable.assign(variable.begin(), end);
    if (!valid(f)) {
        return false;
    }
    destination = std::move(f);
    return true;
}

Persistent::Error Control::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!writeFields(s, fields_)) {
        return Persistent::Error::WriteError;
    }
    return SimGroup::write(s, v, u, c);
}

Persistent::Error Control::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    if (canvas_ || !objects().empty()) {
        return Persistent::Error::ReadError;
    }
    Fields fields;
    if (!readFields(s, fields)) {
        return Persistent::Error::ReadError;
    }
    const auto result = SimGroup::read(s, v, u, c);
    if (result != Persistent::Error::Ok) {
        return result;
    }
    fields_ = std::move(fields);
    return Persistent::Error::Ok;
}

void Active::action() {
    if (canvas() && canvas()->onAction && !canvas()->onAction(*this, message, fields_.command)) {
        throw std::runtime_error("GUI action rejected");
    }
}

std::function<void()> Active::checkpoint() {
    auto base = Control::checkpoint();
    const auto a = active, d = depressed, o = over;
    const auto sprite = selectedCursorTag;
    return [this, base = std::move(base), a, d, o, sprite] {
        base();
        active = a;
        depressed = d;
        over = o;
        selectedCursorTag = sprite;
    };
}

bool Active::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        depressed = over = false;
        return true;
    }
    if (!active) {
        if (e.type == EventType::Move) {
            mouseCursorTag = 0;
        }
        return Control::event(e);
    }
    auto* root = canvas();
    if (!root) {
        return false;
    }
    const auto local = globalToLocal(e.point);
    switch (e.type) {
        case EventType::Move:
            over = in(local, fields_.extent);
            mouseCursorTag = over ? selectedCursorTag : 0;
            return true;
        case EventType::Leave:
            over = false;
            return true;
        case EventType::Down:
            if (e.button != 1) {
                return Control::event(e);
            }
            if (over) {
                if (!root->lockMouse(this)) {
                    return false;
                }
                root->makeFirstResponder(this);
                depressed = true;
            }
            return true;
        case EventType::Drag:
            if (e.button != 1) {
                return Control::event(e);
            }
            if (root->capture() == this) {
                depressed = in(local, fields_.extent);
            }
            return true;
        case EventType::Up:
            if (e.button != 1) {
                return Control::event(e);
            }
            if (root->capture() == this) {
                root->unlockMouse();
            }
            if (depressed) {
                depressed = false;
                action();
            }
            return true;
        case EventType::Repeat:
            Control::event(e);
            return true;
        case EventType::KeyDown:
            if (e.key == Key::Return) {
                if (root->capture() != this) {
                    depressed = true;
                }
                return true;
            }
            break;
        case EventType::KeyUp:
            if (e.key == Key::Return) {
                if (root->capture() != this && depressed) {
                    depressed = false;
                    action();
                }
                return true;
            }
            break;
        default:
            break;
    }
    return Control::event(e);
}

Persistent::Error Active::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!s.write(active) || !s.write(message)) {
        return Persistent::Error::WriteError;
    }
    return Control::write(s, v, u, c);
}

Persistent::Error Active::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    bool a{};
    UInt32 m{};
    if (!s.read(a) || !s.read(m)) {
        return Persistent::Error::ReadError;
    }
    const auto result = Control::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        active = a;
        message = m;
    }
    return result;
}

bool Progress::setRange(Int32 l, Int32 h) {
    if (l <= -32768 || h >= 32767 || l >= h) {
        return false;
    }
    low = l;
    high = h;
    return true;
}

bool Progress::percent(float p) {
    if (!std::isfinite(p) || low <= -32768 || high >= 32767 || low >= high) {
        return false;
    }
    value = p <= 0 ? low : p >= 1 ? high : low + Int32(p * float(high - low));
    return true;
}

Int32 Progress::offset(Int32 n) {
    const auto old = value;
    const auto v = Int64(value) + n;
    if (v >= low && v <= high) {
        value = static_cast<Int32>(v);
    }
    return old;
}

Int32 Progress::step() {
    const auto old = value;
    const auto v = Int64(value) + increment;
    if (v > low && v < high) {
        value = static_cast<Int32>(v);
    }
    return old;
}

bool Progress::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    if (low <= -32768 || high >= 32767 || low >= high || value < -32768 || value > 32767) {
        return false;
    }
    const auto width = Int32(float(fields_.extent.x) / float(high - low) * float(value));
    s.fill({p.x, p.y, p.x + width, p.y + fields_.extent.y - 1}, fields_.style.fill);
    s.rectangle(bounds(p, fields_.extent), fields_.style.borderColor);
    return true;
}

Persistent::Error
Progress::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (low <= -32768 || high >= 32767 || low >= high || !s.write(low) || !s.write(high) ||
        !s.write(increment)) {
        return Persistent::Error::WriteError;
    }
    return Active::write(s, v, u, c);
}

std::function<void()> Progress::checkpoint() {
    auto base = Active::checkpoint();
    const auto l = low, h = high, i = increment, v = value;
    return [this, base = std::move(base), l, h, i, v] {
        base();
        low = l;
        high = h;
        increment = i;
        value = v;
    };
}

Persistent::Error Progress::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 l{}, h{}, i{};
    if (!s.read(l) || !s.read(h) || !s.read(i) || l >= h || l <= -32768 || h >= 32767) {
        return Persistent::Error::ReadError;
    }
    const auto result = Active::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        low = l;
        high = h;
        increment = i;
    }
    return result;
}

bool Slider::setValue(float v) {
    if (!std::isfinite(v) || !std::isfinite(low) || !std::isfinite(high) ||
        !std::isfinite(high - low) || low >= high) {
        return false;
    }
    value = std::clamp(v, low, high);
    return true;
}

std::function<void()> Slider::checkpoint() {
    auto base = Active::checkpoint();
    const auto v = value;
    const auto drag = dragging_, left = pageLeft_;
    return [this, base = std::move(base), v, drag, left] {
        base();
        value = v;
        dragging_ = drag;
        pageLeft_ = left;
    };
}

Int32 Slider::thumbPosition() const {
    if (!std::isfinite(value) || !std::isfinite(low) || !std::isfinite(high) ||
        !std::isfinite(high - low) || low >= high || value < low || value > high ||
        thumbWidth < 1 || thumbWidth >= fields_.extent.x) {
        return 0;
    }
    return Int32((value - low) / (high - low) * float(fields_.extent.x - thumbWidth));
}

bool Slider::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        dragging_ = false;
        return Active::event(e);
    }
    if (!active) {
        return Active::event(e);
    }
    if (low >= high || !std::isfinite(low) || !std::isfinite(high) || !std::isfinite(high - low) ||
        !std::isfinite(value) || fields_.extent.x <= thumbWidth || thumbWidth < 1) {
        return false;
    }
    if (e.type == EventType::KeyDown && (e.key == Key::Left || e.key == Key::Right)) {
        return setValue(value + (e.key == Key::Right ? line : -line));
    }
    const auto x = globalToLocal(e.point).x;
    const auto pageAtMouse = [&] {
        if (!dragging_ && ((pageLeft_ && x < thumbPosition()) ||
                           (!pageLeft_ && x > thumbPosition() + thumbWidth))) {
            return setValue(value + (pageLeft_ ? -page : page) * (high - low));
        }
        return true;
    };
    if (e.type == EventType::Down && e.button == 1) {
        dragging_ = x >= thumbPosition() && x <= thumbPosition() + thumbWidth &&
                    globalToLocal(e.point).y >= 0 && globalToLocal(e.point).y <= fields_.extent.y;
        pageLeft_ = x < thumbPosition();
        if (!pageAtMouse()) {
            return false;
        }
    }
    if (e.type == EventType::Repeat && e.button == 1 && !pageAtMouse()) {
        return false;
    }
    if (e.type == EventType::Up && e.button == 1) {
        dragging_ = false;
    }
    if (e.type == EventType::Drag && e.button == 1) {
        if (!dragging_) {
            return true;
        }
        if (canvas() && canvas()->capture() == this) {
            setValue(float(x - (thumbWidth > 1)) / float(fields_.extent.x - thumbWidth) *
                         (high - low) +
                     low);
        }
    }
    return Active::event(e);
}

bool Slider::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(high - low) ||
        !std::isfinite(value) || low >= high || value < low || value > high || thumbWidth < 1 ||
        thumbWidth >= fields_.extent.x) {
        return false;
    }
    s.line({p.x + (thumbWidth >> 1), p.y + (fields_.extent.y >> 1)},
           {p.x + fields_.extent.x - (thumbWidth >> 1), p.y + (fields_.extent.y >> 1)},
           252);
    s.fill({p.x + thumbPosition(), p.y, p.x + thumbPosition() + thumbWidth, p.y + fields_.extent.y},
           depressed ? 249 : 250);
    return true;
}

Persistent::Error Slider::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(high - low) ||
        !std::isfinite(value) || low >= high || value < low || value > high || !s.write(low) ||
        !s.write(high) || !s.write(value)) {
        return Persistent::Error::WriteError;
    }
    return Active::write(s, v, u, c);
}

Persistent::Error Slider::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    float l{}, h{}, val{};
    if (!s.read(l) || !s.read(h) || !s.read(val) || !std::isfinite(l) || !std::isfinite(h) ||
        !std::isfinite(h - l) || !std::isfinite(val) || l >= h || val < l || val > h) {
        return Persistent::Error::ReadError;
    }
    const auto result = Active::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        low = l;
        high = h;
        value = val;
    }
    return result;
}

bool Text::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (text.size() > 65536 || alignment < 0 || alignment > 2 || verticalDelta < -MaxCoordinate ||
        verticalDelta > MaxCoordinate) {
        return false;
    }
    auto f = font;
    if (f) {
        const auto measured = f->textWidth(text);
        if (measured < -MaxCoordinate || measured > MaxCoordinate || f->info.height < 0 ||
            f->info.height > 32768) {
            return false;
        }
        const auto width = static_cast<Int32>(measured);
        const auto x = alignment == 1   ? fields_.extent.x - width
                       : alignment == 2 ? (fields_.extent.x - width) / 2
                                        : 0;
        if (!s.text(*f, {p.x + x, p.y + verticalDelta}, text)) {
            return false;
        }
    }
    return drawChildren(s, p, clip);
}

std::function<void()> Text::checkpoint() {
    auto base = Active::checkpoint();
    auto value = text;
    auto normal = font, highlight = highlightFont, disabled = disabledFont;
    const auto tag = textTag, normalTag = fontTag, highlightTag = highlightFontTag,
               disabledTag = disabledFontTag;
    const auto align = alignment, delta = verticalDelta;
    return [this,
            base = std::move(base),
            value = std::move(value),
            normal = std::move(normal),
            highlight = std::move(highlight),
            disabled = std::move(disabled),
            tag,
            normalTag,
            highlightTag,
            disabledTag,
            align,
            delta] {
        base();
        text = value;
        font = normal;
        highlightFont = highlight;
        disabledFont = disabled;
        textTag = tag;
        fontTag = normalTag;
        highlightFontTag = highlightTag;
        disabledFontTag = disabledTag;
        alignment = align;
        verticalDelta = delta;
    };
}

bool Text::variableChanged(std::string_view value) {
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos) {
        return false;
    }
    auto* edit = dynamic_cast<TextEdit*>(this);
    if (edit && value.size() > edit->maxBytes) {
        return false;
    }
    std::string next(value);
    text.swap(next);
    textTag = 0;
    if (edit) {
        edit->cursor = edit->anchor = text.size();
    }
    return true;
}

Persistent::Error Text::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (text.size() > 255 || alignment < 0 || alignment > 2) {
        return Persistent::Error::WriteError;
    }
    std::array<char, 81> bytes{};
    std::copy_n(text.begin(), std::min<std::size_t>(text.size(), 80), bytes.begin());
    if (!s.write(UInt32{}) || !s.write(UInt32{}) || !s.write(disabledFontTag) ||
        !s.write(fontTag) || !s.write(highlightFontTag) || !s.write(textTag) ||
        !s.writeBytes(std::as_bytes(std::span(bytes))) || !s.write(alignment) ||
        !s.write(verticalDelta)) {
        return Persistent::Error::WriteError;
    }
    return Active::write(s, v, u, c);
}

Persistent::Error Text::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    UInt32 unused{}, disabled{}, normal{}, highlight{}, tag{};
    Int32 align{}, delta{};
    std::array<char, 81> bytes{};
    if (!s.read(unused) || !s.read(unused) || !s.read(disabled) || !s.read(normal) ||
        !s.read(highlight) || !s.read(tag) ||
        !s.readBytes(std::as_writable_bytes(std::span(bytes))) || !s.read(align) ||
        !s.read(delta) || align < 0 || align > 2) {
        return Persistent::Error::ReadError;
    }
    const auto end = std::ranges::find(bytes, '\0');
    if (end == bytes.end()) {
        return Persistent::Error::ReadError;
    }
    std::string value(bytes.begin(), end);
    const auto result = Active::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        text = std::move(value);
        fontTag = normal;
        disabledFontTag = disabled;
        highlightFontTag = highlight;
        textTag = tag;
        alignment = align;
        verticalDelta = delta;
    }
    return result;
}

bool Button::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    auto f = over ? highlightFont : font;
    if (f) {
        const auto measured = f->textWidth(text);
        if (text.size() > 65536 || measured < 0 || measured > MaxCoordinate || alignment < 0 ||
            alignment > 2) {
            return false;
        }
        const auto border = [&](RectI r, UInt8 color) {
            s.line({r.left + 2, r.top}, {r.right - 2, r.top}, color);
            s.line({r.left + 2, r.bottom}, {r.right - 2, r.bottom}, color);
            s.line({r.left, r.top + 2}, {r.left, r.bottom - 2}, color);
            s.line({r.right, r.top + 2}, {r.right, r.bottom - 2}, color);
            for (auto q : std::array<Point2I, 4>{{{r.left + 1, r.top + 1},
                                                  {r.right - 1, r.top + 1},
                                                  {r.left + 1, r.bottom - 1},
                                                  {r.right - 1, r.bottom - 1}}}) {
                s.point(q, color);
            }
        };
        s.fill({p.x + 2, p.y + 2, p.x + fields_.extent.x - 3, p.y + fields_.extent.y - 3}, 249);
        border({p.x + 1, p.y + 1, p.x + fields_.extent.x - 2, p.y + fields_.extent.y - 2},
               canvas() && canvas()->focus() == this ? 245 : 255);
        border(bounds(p, fields_.extent), 249);
        const auto x = alignment == 1   ? fields_.extent.x - Int32(measured)
                       : alignment == 2 ? (fields_.extent.x - Int32(measured)) / 2
                                        : 0;
        if (!s.text(
                *f,
                {p.x + x + Int32(depressed),
                 p.y + (fields_.extent.y - f->info.height) / 2 + verticalDelta + Int32(depressed)},
                text)) {
            return false;
        }
    }
    return drawChildren(s, p, clip);
}

bool Check::variableChanged(std::string_view value) {
    checked = script::toBool(value);
    return true;
}

void Check::action() {
    checked = radio ? true : !checked;
    if (radio && parent()) {
        for (auto* object : parent()->objects()) {
            if (auto* c = dynamic_cast<Check*>(object); c && c->radio && c != this) {
                c->checked = false;
            }
        }
    }
    Active::action();
}

std::function<void()> Check::checkpoint() {
    auto base = Button::checkpoint();
    auto value = checked;
    return [this, base = std::move(base), value] {
        base();
        checked = value;
    };
}

bool Check::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    auto f = over ? highlightFont : font;
    if (f) {
        const auto width = f->textWidth(text) + 15;
        if (width < 0 || width > MaxCoordinate || text.size() > 65536 || alignment < 0 ||
            alignment > 2) {
            return false;
        }
        const auto x = alignment == 1   ? fields_.extent.x - Int32(width)
                       : alignment == 2 ? (fields_.extent.x - Int32(width)) / 2
                                        : 0;
        const Point2I k{p.x + x, p.y + (fields_.extent.y - 10) / 2};
        const auto color = fields_.style.selectedFill;
        s.fill({p.x, p.y, p.x + fields_.extent.x, p.y + fields_.extent.y}, fields_.style.fill);
        if (canvas() && canvas()->focus() == this) {
            s.rectangle({p.x + 1, p.y + 1, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1},
                        color);
        }
        if (!s.text(*f,
                    {p.x + x + 15, p.y + (fields_.extent.y - f->info.height) / 2 + verticalDelta},
                    text)) {
            return false;
        }
        s.rectangle({k.x, k.y, k.x + 10, k.y + 10}, color);
        if (depressed) {
            s.rectangle({k.x + 1, k.y + 1, k.x + 9, k.y + 9}, color);
        }
        if (checked) {
            if (radio) {
                s.rectangle({k.x + 4, k.y + 4, k.x + 6, k.y + 6}, color);
            } else {
                s.line(k, {k.x + 10, k.y + 10}, color);
                s.line({k.x, k.y + 10}, {k.x + 10, k.y}, color);
            }
        }
    }
    return drawChildren(s, p, clip);
}

Persistent::Error Check::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!s.write(checked)) {
        return Persistent::Error::WriteError;
    }
    return Text::write(s, v, u, c);
}

Persistent::Error Check::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    bool value{};
    if (!s.read(value)) {
        return Persistent::Error::ReadError;
    }
    const auto result = Text::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        checked = value;
    }
    return result;
}

bool TextEdit::setText(std::string value) {
    if (value.size() > maxBytes || !utf8(value, multiline)) {
        return false;
    }
    text = std::move(value);
    cursor = anchor = 0;
    scroll_ = lineScroll_ = 0;
    mouseScroll_ = 0;
    mouseBlock_.reset();
    selecting_ = false;
    undo_.clear();
    undoCursor_ = undoAnchor_ = 0;
    return true;
}

bool TextEdit::variableChanged(std::string_view value) {
    if (!Text::variableChanged(value)) {
        return false;
    }
    mouseBlock_.reset();
    mouseScroll_ = 0;
    selecting_ = false;
    return true;
}

void TextEdit::resetCursorBlink() noexcept {
    cursorOn = true;
    cursorFrames_ = 0;
    cursorTime_ = canvas() ? canvas()->time() : 0;
}

bool TextEdit::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        selecting_ = false;
        mouseScroll_ = 0;
        return Text::event(e);
    }
    // Original single-line TextEdit commits on every key release.
    if (!multiline && e.type == EventType::KeyUp) {
        action();
        return true;
    }
    if (!active) {
        return Text::event(e);
    }
    if (text.size() > maxBytes || maxBytes > 65536 || !utf8(text, multiline) ||
        !boundary(text, cursor) || !boundary(text, anchor)) {
        return false;
    }
    if (e.type == EventType::Down && e.button == 1) {
        if (!canvas() || !canvas()->makeFirstResponder(this) || !canvas()->lockMouse(this)) {
            return false;
        }
        selecting_ = true;
    }
    if (e.button == 1 && ((e.type == EventType::Down && selecting_) ||
                          (e.type == EventType::Drag && (selecting_ || !multiline)))) {
        const auto local = globalToLocal(e.point);
        const auto x = local.x + scroll_ - 3;
        std::size_t at = 0;
        if (!multiline) {
            const bool outside = local.x < 0 || local.x > fields_.extent.x - 1;
            if (e.type == EventType::Drag && outside) {
                mouseScroll_ = local.x < 0 ? -1 : 1;
            } else {
                mouseScroll_ = 0;
                if (local.x < 0) {
                    at = 0;
                } else if (local.x > fields_.extent.x - 1) {
                    at = text.size();
                } else {
                    Int64 width = 0;
                    while (at < text.size()) {
                        const auto end = next(text, at);
                        width += font ? font->glyph(UInt8(text[at])).info.width + 1 : 8;
                        if (width > x) {
                            break;
                        }
                        at = end;
                    }
                }
                cursor = at;
            }
            if (e.type == EventType::Down) {
                mouseDragAnchor_ = anchor = cursor;
                mouseBlock_.reset();
            } else {
                anchor = mouseDragAnchor_;
                mouseBlock_ = std::pair{std::min(cursor, anchor), std::max(cursor, anchor)};
            }
            return Text::event(e);
        }
        if (multiline) {
            const auto height = std::max(1, font ? font->info.height : 12);
            const auto row = std::max(0, (local.y + lineScroll_) / height);
            for ([[maybe_unused]] auto i :
                 std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                     return i < row && at < text.size();
                 })) {
                const auto end = text.find('\n', at);
                at = end == text.npos ? text.size() : end + 1;
            }
        }
        const auto start = at;
        while (at < text.size() && (!multiline || text[at] != '\n')) {
            const auto end = next(text, at);
            const auto part = std::string_view(text).substr(start, end - start);
            const auto width =
                font ? (multiline ? font->textWidth(expandedTabs(part)) : font->textWidth(part))
                     : Int32((end - start) * 8);
            if (width > x) {
                break;
            }
            at = end;
        }
        cursor = at;
        if (e.type == EventType::Down && !e.shift) {
            anchor = at;
        }
        return true;
    }
    if (e.type == EventType::Up && (selecting_ || !multiline) && e.button == 1 && canvas()) {
        if (canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        selecting_ = false;
        mouseScroll_ = 0;
        return Text::event(e);
    }
    if (e.type != EventType::Text && e.type != EventType::KeyDown) {
        return Text::event(e);
    }
    if (!multiline && e.type == EventType::KeyDown && e.shift &&
        (e.key == Key::Home || e.key == Key::End || e.key == Key::Left || e.key == Key::Right)) {
        auto [start, end] = selection();
        if (!mouseBlock_ && cursor == anchor) {
            start = end = 0;
        }
        auto pos = cursor;
        switch (e.key) {
            case Key::Home:
                start = 0;
                end = pos;
                pos = 0;
                break;
            case Key::End:
                start = pos;
                end = text.size();
                pos = text.size();
                break;
            case Key::Left:
                if (pos && !text.empty()) {
                    if (pos == end) {
                        pos = previous(text, pos);
                        end = pos;
                        if (end == start) {
                            start = end = 0;
                        }
                    } else {
                        pos = previous(text, pos);
                        start = pos;
                        if (!end) {
                            end = next(text, pos);
                        }
                    }
                }
                break;
            case Key::Right:
                if (pos < text.size()) {
                    if (pos == start && end) {
                        pos = next(text, pos);
                        start = pos;
                        if (start == end) {
                            start = end = 0;
                        }
                    } else {
                        if (!end) {
                            start = end = pos;
                        }
                        pos = next(text, pos);
                        end = pos;
                    }
                }
                break;
            default:
                break;
        }
        // Native DrawText clears a reversed/out-of-range block before drawing.
        if (start > end || end > text.size()) {
            start = end = 0;
        }
        cursor = pos;
        anchor = start == end ? pos : pos == start ? end : start;
        mouseBlock_ = std::pair{start, end};
        return true;
    }
    auto value = text;
    auto pos = cursor, mark = anchor;
    const auto block = selection();
    // A native empty block may retain zero or nonzero raw endpoints. Insertion
    // still belongs at the cursor, independently of that representation.
    const auto first = block.first == block.second ? cursor : block.first,
               last = block.first == block.second ? cursor : block.second;
    const auto replace = [&](std::string_view insert) {
        if (!utf8(insert, multiline) || value.size() - (last - first) + insert.size() > maxBytes) {
            return false;
        }
        auto candidate = value;
        candidate.replace(first, last - first, insert);
        if (numbersOnly) {
            bool dot = false;
            for (auto i : std::views::iota(std::size_t{0}, candidate.size())) {
                const auto c = candidate[i];
                if (multiline && c == '.' && !dot) {
                    dot = true;
                } else if (c == '-' && i == 0) {
                } else if (c < '0' || c > '9') {
                    return false;
                }
            }
        }
        value = std::move(candidate);
        pos = mark = first + insert.size();
        return true;
    };
    if (e.type == EventType::Text) {
        if (!multiline && !insertOn && first == last && pos < value.size()) {
            if (!utf8(e.text, false)) {
                return false;
            }
            auto end = pos;
            for (std::size_t i = 0; i < e.text.size(); i = next(e.text, i)) {
                end = next(value, end);
            }
            value.erase(pos, end - pos);
        }
        if (!replace(e.text)) {
            return false;
        }
    } else if (e.control) {
        switch (e.key) {
            case Key::A:
                mark = 0;
                pos = value.size();
                break;
            case Key::C:
            case Key::X:
                if (first == last) {
                    return true;
                }
                if (!canvas() || !canvas()->writeClipboard ||
                    !canvas()->writeClipboard(
                        std::string_view(value).substr(first, last - first))) {
                    return false;
                }
                if (e.key == Key::X && !replace("")) {
                    return false;
                }
                mark = pos;
                break;
            case Key::V: {
                if (!canvas() || !canvas()->readClipboard) {
                    return false;
                }
                auto paste = canvas()->readClipboard();
                if (!paste || !replace(*paste)) {
                    return false;
                }
                break;
            }
            case Key::Z:
            case Key::U:
                if (!utf8(undo_, multiline) || undo_.size() > maxBytes) {
                    return false;
                }
                if (selecting_ && !multiline) {
                    return true;
                }
                value = undo_;
                pos = undoCursor_;
                mark = undoAnchor_;
                break;
            case Key::Home:
                if (!multiline) {
                    return Text::event(e);
                }
                pos = 0;
                if (!e.shift) {
                    mark = pos;
                }
                break;
            case Key::End:
                if (!multiline) {
                    return Text::event(e);
                }
                pos = value.size();
                if (!e.shift) {
                    mark = pos;
                }
                break;
            case Key::Delete:
            case Key::Backspace:
                value.clear();
                pos = mark = 0;
                break;
            default:
                return Text::event(e);
        }
    } else {
        switch (e.key) {
            case Key::Left:
                pos = previous(value, pos);
                if (!e.shift) {
                    mark = pos;
                }
                break;
            case Key::Right:
                pos = next(value, pos);
                if (!e.shift) {
                    mark = pos;
                }
                break;
            case Key::Home:
                if (e.shift && !multiline) {
                    mark = pos;
                }
                pos = multiline && pos ? value.rfind('\n', pos - 1) : value.npos;
                pos = pos == value.npos ? 0 : pos + 1;
                if (!e.shift) {
                    mark = pos;
                }
                break;
            case Key::End:
                if (e.shift && !multiline) {
                    mark = pos;
                }
                pos = multiline ? value.find('\n', pos) : value.npos;
                pos = pos == value.npos ? value.size() : pos;
                if (!e.shift) {
                    mark = pos;
                }
                break;
            case Key::Return:
                if (!multiline) {
                    return true;
                }
                if (!replace("\n")) {
                    return false;
                }
                break;
            case Key::Insert:
                if (!multiline) {
                    insertOn = !insertOn;
                    return true;
                }
                return Text::event(e);
            case Key::Tab:
                if (!multiline || e.shift) {
                    return Text::event(e);
                }
                if (!replace("\t")) {
                    return false;
                }
                break;
            case Key::Up:
            case Key::Down: {
                if (!multiline) {
                    return Text::event(e);
                }
                auto begin = pos ? value.rfind('\n', pos - 1) : value.npos;
                begin = begin == value.npos ? 0 : begin + 1;
                const auto column = pos - begin;
                if (e.key == Key::Up) {
                    const auto finish = begin ? begin - 1 : 0;
                    auto previous = finish ? value.rfind('\n', finish - 1) : value.npos;
                    previous = previous == value.npos ? 0 : previous + 1;
                    pos = std::min(previous + column, finish);
                } else {
                    const auto finish = value.find('\n', pos);
                    if (finish != value.npos) {
                        const auto nextEnd = value.find('\n', finish + 1);
                        pos = std::min(finish + 1 + column,
                                       nextEnd == value.npos ? value.size() : nextEnd);
                    }
                }
                while (pos && !boundary(value, pos)) {
                    --pos;
                }
                if (!e.shift) {
                    mark = pos;
                }
                break;
            }
            case Key::Backspace:
                if (first != last) {
                    replace("");
                } else if (pos) {
                    const auto start = previous(value, pos);
                    value.erase(start, pos - start);
                    pos = mark = start;
                }
                break;
            case Key::Delete:
                if (first != last) {
                    replace("");
                } else if (pos < value.size()) {
                    value.erase(pos, next(value, pos) - pos);
                }
                break;
            default:
                return Text::event(e);
        }
    }
    const bool undoSwap =
        e.type == EventType::KeyDown && e.control && (e.key == Key::U || e.key == Key::Z);
    // Native Backspace does not save undo. Multiline keeps its richer history.
    const bool savesUnchanged =
        e.type == EventType::Text ||
        (e.type == EventType::KeyDown &&
         (e.key == Key::Delete ||
          (e.control && (e.key == Key::C || e.key == Key::X || e.key == Key::Backspace))));
    if ((value != text || undoSwap || savesUnchanged) &&
        (multiline || e.type != EventType::KeyDown || e.control || e.key != Key::Backspace)) {
        undo_ = text;
        undoCursor_ = cursor;
        undoAnchor_ = anchor;
    }
    text = std::move(value);
    cursor = pos;
    anchor = mark;
    mouseBlock_.reset();
    return true;
}

void TextEdit::drawBackground(gfx::SoftwareSurface& s, Point2I p) const {
    s.fill(bounds(p, fields_.extent), 255);
}

bool TextEdit::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (!utf8(text, multiline) || !boundary(text, cursor) || !boundary(text, anchor) ||
        text.size() > maxBytes) {
        return false;
    }
    drawBackground(s, p);
    const auto f = !active && disabledFont ? disabledFont : font;
    if (multiline) {
        if (!f) {
            return drawChildren(s, p, clip);
        }
        const auto height = std::max(1, f->info.height);
        if (height > 4096) {
            return false;
        }
        const bool focused = canvas() && canvas()->focus() == this;
        const auto line =
            Int32(std::count(text.begin(), text.begin() + std::ptrdiff_t(cursor), '\n'));
        if (focused) {
            if (line * height < lineScroll_) {
                lineScroll_ = line * height;
            }
            if ((line + 1) * height > lineScroll_ + fields_.extent.y) {
                lineScroll_ = std::max(0, (line + 1) * height - fields_.extent.y);
            }
        }
        const auto expanded = expandedTabs;
        if (focused) {
            auto begin = cursor ? text.rfind('\n', cursor - 1) : text.npos;
            begin = begin == text.npos ? 0 : begin + 1;
            const auto width =
                f->textWidth(expanded(std::string_view(text).substr(begin, cursor - begin)));
            if (width < 0 || width > MaxCoordinate) {
                return false;
            }
            if (width < scroll_) {
                scroll_ = std::max(0, Int32(width) - 3);
            }
            if (width - scroll_ > fields_.extent.x - 6) {
                scroll_ = std::max(0, Int32(width) - fields_.extent.x + 6);
            }
        }
        std::size_t start = 0;
        Int32 row = 0;
        const auto first = std::min(cursor, anchor), last = std::max(cursor, anchor);
        do {
            const auto found = text.find('\n', start),
                       end = found == text.npos ? text.size() : found;
            const auto y = p.y + row * height - lineScroll_;
            if (y + height > p.y && y < p.y + fields_.extent.y) {
                const auto value = std::string_view(text).substr(start, end - start);
                if (!s.text(*f, {p.x + 3 - scroll_, y}, expanded(value))) {
                    return false;
                }
                if (focused && last > start && first < end) {
                    const auto from = std::max(first, start) - start,
                               to = std::min(last, end) - start;
                    const auto prefix = expanded(value.substr(0, from)),
                               selection = expanded(value.substr(from, to - from));
                    const auto x = p.x + 3 - scroll_ + Int32(f->textWidth(prefix));
                    if (highlightColor) {
                        s.fill({x, y, x + Int32(f->textWidth(selection)), y + height - 1},
                               highlightColor);
                    }
                    if (highlightFont && !s.text(*highlightFont, {x, y}, selection)) {
                        return false;
                    }
                }
                if (focused && cursorOn && cursor >= start && cursor <= end) {
                    const auto x = p.x + 3 - scroll_ +
                                   Int32(f->textWidth(expanded(value.substr(0, cursor - start))));
                    s.line({x, y}, {x, y + height - 1}, cursorColor);
                }
            }
            if (found == text.npos) {
                break;
            }
            start = end + 1;
            ++row;
        } while (start <= text.size());
        return drawChildren(s, p, clip);
    }
    if (f) {
        const auto width = [&](std::size_t n) {
            return Int32(f->textWidth(std::string_view(text).substr(0, n)));
        };
        if (selecting_ && mouseScroll_) {
            cursor = mouseScroll_ < 0 ? previous(text, cursor) : next(text, cursor);
        }
        const auto pen = width(cursor);
        if (std::abs(pen) > MaxCoordinate || std::abs(width(text.size())) > MaxCoordinate) {
            return false;
        }
        const bool focused = canvas() && canvas()->focus() == this;
        if (!focused) {
            anchor = cursor;
            mouseBlock_.reset();
        }
        if (width(text.size()) + 3 < fields_.extent.x - 1) {
            scroll_ = 0;
        }
        auto origin = 3 - scroll_;
        if (focused) {
            if (origin + pen < 0) {
                origin = 3 - pen;
            } else if (origin + pen > fields_.extent.x - 1) {
                origin = fields_.extent.x - 4 - pen;
            }
            scroll_ = 3 - origin;
        }
        Point2I offset{p.x + origin,
                       p.y + ((fields_.extent.y - 1 - f->info.height) >> 1) + verticalDelta};
        const auto block = selection();
        const std::size_t first = focused && highlightFont ? block.first : 0,
                          last = focused && highlightFont ? block.second : 0;
        if (first) {
            if (!s.text(*f, offset, std::string_view(text).substr(0, first))) {
                return false;
            }
            offset.x += width(first) + 1;
        }
        if (last) {
            const auto selected = std::string_view(text).substr(first, last - first);
            const auto selectedWidth = Int32(highlightFont->textWidth(selected));
            if (highlightColor) {
                s.fill({offset.x, p.y, offset.x + selectedWidth, p.y + fields_.extent.y - 1},
                       highlightColor);
            }
            if (!s.text(*highlightFont, offset, selected)) {
                return false;
            }
            offset.x += selectedWidth + 1;
        }
        if (!s.text(*f, offset, std::string_view(text).substr(last))) {
            return false;
        }
        if (focused && cursorOn) {
            s.line({p.x + origin + pen, p.y},
                   {p.x + origin + pen, p.y + fields_.extent.y - 1},
                   cursorColor);
        }
    }
    return drawChildren(s, p, clip);
}

void TextEdit::tick(double seconds) {
    if (canvas() && canvas()->focus() == this) {
        ++cursorFrames_;
        if (seconds - cursorTime_ > .5 && cursorFrames_ > 3) {
            cursorOn = !cursorOn;
            cursorTime_ = seconds;
            cursorFrames_ = 0;
        }
    }
}

std::function<void()> TextEdit::checkpoint() {
    auto base = Text::checkpoint();
    auto oldText = text, oldUndo = undo_;
    const auto pos = cursor, mark = anchor, up = undoCursor_, ua = undoAnchor_,
               drag = mouseDragAnchor_;
    const auto block = mouseBlock_;
    const auto selection = selecting_;
    const auto scroll = scroll_, lines = lineScroll_, direction = mouseScroll_;
    const auto multi = multiline;
    const auto insert = insertOn;
    const auto blink = cursorOn;
    const auto cursorTime = cursorTime_;
    const auto frames = cursorFrames_;
    const auto maximum = maxBytes;
    const auto numeric = numbersOnly;
    const auto highlight = highlightColor, color = cursorColor;
    return [this,
            base = std::move(base),
            oldText = std::move(oldText),
            oldUndo = std::move(oldUndo),
            pos,
            mark,
            up,
            ua,
            drag,
            block,
            selection,
            scroll,
            lines,
            direction,
            multi,
            insert,
            blink,
            cursorTime,
            frames,
            maximum,
            numeric,
            highlight,
            color] {
        base();
        text = oldText;
        undo_ = oldUndo;
        cursor = pos;
        anchor = mark;
        undoCursor_ = up;
        undoAnchor_ = ua;
        mouseDragAnchor_ = drag;
        mouseBlock_ = block;
        mouseScroll_ = direction;
        selecting_ = selection;
        scroll_ = scroll;
        lineScroll_ = lines;
        multiline = multi;
        insertOn = insert;
        cursorOn = blink;
        cursorTime_ = cursorTime;
        cursorFrames_ = frames;
        maxBytes = maximum;
        numbersOnly = numeric;
        highlightColor = highlight;
        cursorColor = color;
    };
}

Persistent::Error
TextEdit::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (multiline || maxBytes > 255 || !s.write(UInt32(numbersOnly)) || !s.write(UInt32{}) ||
        !s.write(UInt32{}) || !s.write(static_cast<Int32>(maxBytes))) {
        return Persistent::Error::WriteError;
    }
    return Text::write(s, v, u, c);
}

Persistent::Error TextEdit::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    UInt32 numbers{}, unused{};
    Int32 maximum{};
    if (!s.read(numbers) || !s.read(unused) || !s.read(unused) || !s.read(maximum) || maximum < 0 ||
        maximum > 255) {
        return Persistent::Error::ReadError;
    }
    const auto result = Text::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        numbersOnly = numbers != 0;
        maxBytes = maximum;
        multiline = false;
        scroll_ = lineScroll_ = 0;
        cursor = anchor = text.size();
    }
    return result;
}

std::function<void()> Bitmap::checkpoint() {
    auto base = Active::checkpoint();
    const auto image = bitmap;
    const auto tag = bitmapTag;
    const auto scale = stretch, alpha = transparent;
    return [this, base = std::move(base), image, tag, scale, alpha] {
        base();
        bitmap = image;
        bitmapTag = tag;
        stretch = scale;
        transparent = alpha;
    };
}

bool Bitmap::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (bitmap) {
        const gfx::Bitmap* source = bitmap.get();
        std::optional<gfx::Bitmap> adjusted;
        // Native BitmapCtrl chunks oversized art before stretching. Source
        // division drops remainders independently of destination remainders.
        if (stretch && (bitmap->width > 256 || bitmap->height > 256)) {
            ChunkedBitmap chunks;
            if (!chunks.set(*bitmap, transparent) || !chunks.draw(s, bounds(p, fields_.extent))) {
                return false;
            }
            return drawChildren(s, p, clip);
        }
        if (bool(bitmap->attributes & gfx::BitmapTransparent) != transparent) {
            adjusted = *bitmap;
            if (transparent) {
                adjusted->attributes |= gfx::BitmapTransparent;
            } else {
                adjusted->attributes &= ~UInt32(gfx::BitmapTransparent);
            }
            source = &*adjusted;
        }
        if (!(stretch ? s.stretch(*source, bounds(p, fields_.extent)) : s.blit(*source, p))) {
            return false;
        }
    }
    return drawChildren(s, p, clip);
}

Persistent::Error Bitmap::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    const std::array<std::byte, 81> unused{};
    if (!s.write(bitmapTag) || !s.writeBytes(unused) || !s.write(transparent)) {
        return Persistent::Error::WriteError;
    }
    return Active::write(s, v, u, c);
}

Persistent::Error Bitmap::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 tag{};
    bool t{};
    std::array<std::byte, 81> unused{};
    if (!s.read(tag) || !s.readBytes(unused) || !s.read(t)) {
        return Persistent::Error::ReadError;
    }
    const auto result = Active::read(s, v, u, c);
    if (result == Persistent::Error::Ok) {
        bitmapTag = tag;
        transparent = t;
    }
    return result;
}

Canvas::~Canvas() {
    cancel();
    while (!roots_.empty()) {
        roots_.back()->setCanvas(nullptr);
    }
}

Control* Canvas::top() const {
    return roots_.empty() ? nullptr : roots_.back();
}

bool Canvas::contains(const Control* c, bool includeCovered) const {
    if (!c || c->canvas() != this) {
        return false;
    }
    const auto* root = c;
    for (; root->parent(); root = root->parent()) {
        if (!root->visible()) {
            return false;
        }
    }
    return root->visible() &&
           (root == top() || (includeCovered && std::ranges::find(roots_, root) != roots_.end()));
}

void Canvas::invalidate(Control& c) {
    retainedDirty_ = true;
    layoutDirty_ = true;
    if (capture_ == &c) {
        if (captureMouse) {
            try {
                captureMouse(false);
            } catch (...) {
            }
        }
        capture_ = nullptr;
    }
    if (focus_ == &c) {
        focus_ = nullptr;
    }
    if (hover_ == &c) {
        hover_ = nullptr;
    }
    std::erase(tabs_, &c);
    std::erase(roots_, &c);
}

std::function<void()> Canvas::interactionCheckpoint() {
    return [this,
            tabs = tabs_,
            focus = focus_,
            capture = capture_,
            hover = hover_,
            mouse = lastMouse_,
            key = lastKey_,
            nextMouse = nextMouse_,
            nextKey = nextKey_,
            point = cursorPoint_,
            hoverTime = hoverTime_,
            clicked = mouseClicked_,
            left = leftDown_,
            right = rightDown_,
            sprite = cursor,
            enabled = cursorEnabled,
            shown = cursorVisible,
            help = help_,
            extent = updateExtent_,
            current = currentUpdate_,
            displayed = displayedUpdate_,
            old = oldUpdates_,
            last = lastCursorRegion_,
            rendered = renderedCursor_,
            frame = cursorFrame_,
            frameTime = cursorFrameTime_] {
        bool restored = true;
        if (capture_ != capture && captureMouse) {
            try {
                restored = captureMouse(capture != nullptr);
            } catch (...) {
                restored = false;
            }
        }
        tabs_ = tabs;
        lastMouse_ = mouse;
        lastKey_ = key;
        nextMouse_ = nextMouse;
        nextKey_ = nextKey;
        cursorPoint_ = point;
        hoverTime_ = hoverTime;
        mouseClicked_ = clicked;
        leftDown_ = left;
        rightDown_ = right;
        cursor = sprite;
        cursorEnabled = enabled;
        cursorVisible = shown;
        help_ = help;
        updateExtent_ = extent;
        currentUpdate_ = current;
        displayedUpdate_ = displayed;
        oldUpdates_ = old;
        lastCursorRegion_ = last;
        renderedCursor_ = rendered;
        cursorFrame_ = frame;
        cursorFrameTime_ = frameTime;
        if (restored) {
            focus_ = focus;
            capture_ = capture;
            hover_ = hover;
        } else {
            cancel();
        }
    };
}

bool Canvas::content(Control* c) {
    if (dispatching_ || changingContent_ ||
        (c && (c->parent() || (c->canvas() && c->canvas() != this)))) {
        return false;
    }
    if (c && c->canvas() == this) {
        return roots_.size() == 1 && top() == c;
    }
    changingContent_ = true;

    struct Reset {
        bool& value;

        ~Reset() {
            value = false;
        }
    } reset{changingContent_};

    auto nextRoots = std::vector<Control*>{};
    if (c) {
        nextRoots.push_back(c);
    }
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& control) -> void {
        undo.push_back(control.checkpoint());
        for (auto* o : control.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    if (c) {
        save(save, *c);
    }
    for (auto* root : roots_) {
        save(save, *root);
    }
    auto restoreInteraction = interactionCheckpoint();
    try {
        if (c) {
            c->setCanvas(this);
            lifecycle(*c, true);
        }
        for (auto* root : roots_) {
            lifecycle(*root, false);
        }
        if (capture_ && captureMouse && !captureMouse(false)) {
            throw std::runtime_error("GUI capture release failed");
        }
    } catch (...) {
        if (c) {
            try {
                lifecycle(*c, false);
            } catch (...) {
            }
            c->setCanvas(nullptr);
        }
        for (auto& restore : undo) {
            restore();
        }
        restoreInteraction();
        return false;
    }
    capture_ = nullptr;
    cancel();
    while (!roots_.empty()) {
        roots_.back()->setCanvas(nullptr);
    }
    roots_ = std::move(nextRoots);
    rebuildTabs();
    return true;
}

bool Canvas::pushDialog(Control& c) {
    if (dispatching_ || changingContent_ || c.parent() || c.canvas() || roots_.size() >= 64) {
        return false;
    }
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& control) -> void {
        undo.push_back(control.checkpoint());
        for (auto* o : control.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    save(save, c);
    for (auto* root : roots_) {
        save(save, *root);
    }
    changingContent_ = true;

    struct Reset {
        bool& value;

        ~Reset() {
            value = false;
        }
    } reset{changingContent_};

    roots_.reserve(roots_.size() + 1);
    auto restoreInteraction = interactionCheckpoint();
    try {
        c.setCanvas(this);
        lifecycle(c, true);
        if (capture_ && captureMouse && !captureMouse(false)) {
            throw std::runtime_error("GUI capture release failed");
        }
    } catch (...) {
        try {
            lifecycle(c, false);
        } catch (...) {
        }
        c.setCanvas(nullptr);
        for (auto& restore : undo) {
            restore();
        }
        restoreInteraction();
        return false;
    }
    capture_ = nullptr;
    cancel();
    roots_.push_back(&c);
    rebuildTabs();
    tab();
    return true;
}

bool Canvas::popDialog() {
    if (dispatching_ || changingContent_ || roots_.size() < 2) {
        return false;
    }
    changingContent_ = true;

    struct Reset {
        bool& value;

        ~Reset() {
            value = false;
        }
    } reset{changingContent_};

    auto* c = top();
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& control) -> void {
        undo.push_back(control.checkpoint());
        for (auto* o : control.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    for (auto* root : roots_) {
        save(save, *root);
    }
    auto restoreInteraction = interactionCheckpoint();
    try {
        lifecycle(*c, false);
        if (capture_ && captureMouse && !captureMouse(false)) {
            throw std::runtime_error("GUI capture release failed");
        }
    } catch (...) {
        for (auto& restore : undo) {
            restore();
        }
        restoreInteraction();
        return false;
    }
    capture_ = nullptr;
    cancel();
    c->setCanvas(nullptr);
    rebuildTabs();
    tab();
    return true;
}

bool Canvas::commitDialog(const std::function<bool()>& commit) {
    if (dispatching_ || changingContent_ || roots_.size() < 2) {
        return false;
    }
    changingContent_ = true;

    struct Reset {
        bool& value;

        ~Reset() {
            value = false;
        }
    } reset{changingContent_};

    auto* c = top();
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& control) -> void {
        undo.push_back(control.checkpoint());
        for (auto* o : control.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    for (auto* root : roots_) {
        save(save, *root);
    }
    auto restoreInteraction = interactionCheckpoint();
    std::vector<Control*> nextTabs;
    Control* nextFocus{};
    try {
        lifecycle(*c, false);
        if (capture_ && captureMouse && !captureMouse(false)) {
            throw std::runtime_error("GUI capture release failed");
        }
        capture_ = nullptr;
        cancel();
        const auto visit = [&](auto&& self, Control& control) -> void {
            if (!control.visible()) {
                return;
            }
            if (control.wantsTab()) {
                nextTabs.push_back(&control);
            }
            for (auto* child : control.objects()) {
                self(self, *static_cast<Control*>(child));
            }
        };
        visit(visit, *roots_[roots_.size() - 2]);
        for (auto* target : nextTabs) {
            if (target->becomeFirstResponder()) {
                nextFocus = target;
                break;
            }
        }
        if (auto* edit = dynamic_cast<TextEdit*>(nextFocus)) {
            edit->resetCursorBlink();
        }
        if (commit && !commit()) {
            throw std::runtime_error("GUI dialog publication failed");
        }
    } catch (...) {
        for (auto& restore : undo) {
            restore();
        }
        restoreInteraction();
        return false;
    }
    // Only nonthrowing pointer/vector publication follows the external commit.
    c->setCanvas(nullptr);
    tabs_ = std::move(nextTabs);
    focus_ = nextFocus;
    return true;
}

bool Canvas::makeFirstResponder(Control* c, bool allowCovered) {
    if (c && (!contains(c, allowCovered) || !c->becomeFirstResponder())) {
        if (focus_ && !focus_->loseFirstResponder()) {
            return false;
        }
        focus_ = nullptr;
        return false;
    }
    if (c && focus_ && !focus_->loseFirstResponder()) {
        return false;
    }
    focus_ = c;
    // Commit focus before resetting blink; a refused focus transition must not
    // mutate the candidate editor or the existing responder.
    if (auto* edit = dynamic_cast<TextEdit*>(c)) {
        edit->resetCursorBlink();
    }
    return true;
}

bool Canvas::lockMouse(Control* c) {
    if (!contains(c)) {
        return false;
    }
    if (capture_ == c) {
        return true;
    }
    if (captureMouse && !captureMouse(true)) {
        return false;
    }
    capture_ = c;
    return true;
}

void Canvas::unlockMouse() {
    // Input dispatch already owns these checkpoints. Direct host releases need
    // the same guarantee if a leave/enter callback rejects the reconciliation.
    const bool direct = !dispatching_;
    std::vector<std::function<void()>> undo;
    std::function<void()> restoreInteraction;
    if (direct) {
        const auto save = [&](auto&& self, Control& c) -> void {
            undo.push_back(c.checkpoint());
            for (auto* o : c.objects()) {
                self(self, *static_cast<Control*>(o));
            }
        };
        if (top()) {
            save(save, *top());
        }
        restoreInteraction = interactionCheckpoint();
        dispatching_ = true;
    }
    try {
        if (captureMouse && !captureMouse(false)) {
            throw std::runtime_error("GUI capture release failed");
        }
        auto* previous = capture_;
        capture_ = nullptr;
        auto* target = top() ? top()->hit(top()->globalToLocal(cursorPoint_)) : nullptr;
        if (target != previous) {
            Event transition;
            transition.point = cursorPoint_;
            transition.type = EventType::Leave;
            if (previous) {
                previous->event(transition);
            }
            hover_ = target;
            mouseClicked_ = false;
            transition.type = EventType::Enter;
            if (target) {
                target->event(transition);
            }
        }
    } catch (...) {
        for (auto& restore : undo) {
            restore();
        }
        if (restoreInteraction) {
            restoreInteraction();
        }
        if (direct) {
            dispatching_ = false;
            afterDispatch_.clear();
        }
        throw;
    }
    if (direct) {
        dispatching_ = false;
        auto deferred = std::move(afterDispatch_);
        afterDispatch_.clear();
        for (auto& action : deferred) {
            action();
        }
    }
}

void Canvas::rebuildTabs() {
    tabs_.clear();
    if (!top()) {
        return;
    }
    const auto visit = [&](auto&& self, Control& c) -> void {
        if (!c.visible()) {
            return;
        }
        if (c.wantsTab()) {
            tabs_.push_back(&c);
        }
        for (auto* o : c.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    visit(visit, *top());
    if (focus_ && (!contains(focus_, true) ||
                   (contains(focus_) && std::ranges::find(tabs_, focus_) == tabs_.end()))) {
        focus_ = nullptr;
    }
    if (capture_ && !contains(capture_)) {
        Event e;
        e.type = EventType::Cancel;
        try {
            capture_->event(e);
        } catch (...) {
        }
        if (captureMouse) {
            try {
                captureMouse(false);
            } catch (...) {
            }
        }
        capture_ = nullptr;
    }
    if (hover_ && !contains(hover_)) {
        hover_ = nullptr;
    }
}

void Canvas::tab(bool prev) {
    if (tabs_.empty()) {
        return;
    }
    const auto it = std::ranges::find(tabs_, focus_);
    auto index = it == tabs_.end() ? (prev ? 0 : tabs_.size() - 1)
                                   : static_cast<std::size_t>(it - tabs_.begin());
    for ([[maybe_unused]] auto i : std::views::iota(std::size_t{0}, tabs_.size())) {
        index = prev ? (index + tabs_.size() - 1) % tabs_.size() : (index + 1) % tabs_.size();
        if (makeFirstResponder(tabs_[index])) {
            break;
        }
    }
}

void Canvas::cancel() noexcept {
    // A viewport can capture the mouse without participating in keyboard tab
    // traversal. It still needs cancellation to roll back its active gesture.
    if (capture_ && std::ranges::find(tabs_, capture_) == tabs_.end()) {
        Event event;
        event.type = EventType::Cancel;
        try { capture_->event(event); } catch (...) {}
    }
    nextMouse_.reset();
    nextKey_.reset();
    leftDown_ = rightDown_ = mouseClicked_ = false;
    hoverTime_ = time_;
    help_ = {};
    if (captureMouse && capture_) {
        try {
            captureMouse(false);
        } catch (...) {
        }
    }
    capture_ = nullptr;
    for (auto* c : tabs_) {
        Event e;
        e.type = EventType::Cancel;
        try {
            c->event(e);
        } catch (...) {
        }
    }
    focus_ = hover_ = nullptr;
}

bool Canvas::dispatch(const Event& e) {
    retainedDirty_ = true;
    if (dispatching_ || !top() || !top()->visible() || e.text.size() > 65536 ||
        e.point.x < -Control::MaxCoordinate || e.point.x > Control::MaxCoordinate ||
        e.point.y < -Control::MaxCoordinate || e.point.y > Control::MaxCoordinate) {
        return false;
    }
    if (!cursorEnabled &&
        (e.type == EventType::Move || e.type == EventType::Down || e.type == EventType::Up ||
         e.type == EventType::Drag || e.type == EventType::Wheel)) {
        return false;
    }

    struct State {
        Active* control;
        bool depressed, over;
        Check* check;
        bool checked;
        Slider* slider;
        float value;
        bool dragging, pageLeft;
    };

    std::vector<State> states;
    const auto save = [&](auto&& self, Control& c) -> void {
        if (auto* a = dynamic_cast<Active*>(&c)) {
            auto* check = dynamic_cast<Check*>(a);
            auto* slider = dynamic_cast<Slider*>(a);
            states.push_back({a,
                              a->depressed,
                              a->over,
                              check,
                              check && check->checked,
                              slider,
                              slider ? slider->value : 0,
                              slider && slider->dragging_,
                              slider && slider->pageLeft_});
        }
        for (auto* child : c.objects()) {
            self(self, *static_cast<Control*>(child));
        }
    };
    std::vector<std::function<void()>> undo;
    const auto checkpoint = [&](auto&& self, Control& c) -> void {
        undo.push_back(c.checkpoint());
        for (auto* child : c.objects()) {
            self(self, *static_cast<Control*>(child));
        }
    };
    checkpoint(checkpoint, *top());
    save(save, *top());
    // Covered chat is an explicit keyboard responder. Include its graph in
    // rollback just as we do the active dialog's controls.
    if (focus_ && !contains(focus_)) {
        auto* root = focus_;
        while (root->parent()) {
            root = root->parent();
        }
        checkpoint(checkpoint, *root);
        save(save, *root);
    }
    auto restoreInteraction = interactionCheckpoint();
    dispatching_ = true;
    bool result{};
    try {
        if (e.type == EventType::Cancel) {
            cancel();
            result = true;
        } else if (e.type == EventType::KeyDown || e.type == EventType::KeyUp ||
                   e.type == EventType::Text) {
            result = e.type == EventType::KeyDown && keyCommand && keyCommand(e);
            if (!result) {
                result = focus_ && focus_->event(e);
            }
            if (!result && e.type == EventType::KeyDown &&
                (e.key == Key::Tab ||
                 (!(top()->fields().flags & 4) && (e.key == Key::Left || e.key == Key::Right ||
                                                   e.key == Key::Up || e.key == Key::Down)))) {
                tab(e.key == Key::Tab ? e.shift : e.key == Key::Left || e.key == Key::Up);
                result = true;
            }
        } else {
            cursorPoint_ = e.point;
            if (e.type == EventType::Down || e.type == EventType::Up) {
                hoverTime_ = time_;
                if (e.button == 1) {
                    leftDown_ = e.type == EventType::Down;
                } else if (e.button == 3) {
                    rightDown_ = e.type == EventType::Down;
                }
            }
            Control* target = capture_ ? capture_ : top()->hit(top()->globalToLocal(e.point));
            if (!capture_ && target != hover_ &&
                (e.type == EventType::Move || e.type == EventType::Drag)) {
                Event transition = e;
                transition.type = EventType::Leave;
                if (hover_) {
                    hover_->event(transition);
                }
                hover_ = target;
                if (e.type == EventType::Move) {
                    hoverTime_ = time_;
                    mouseClicked_ = false;
                } else if (e.button == 3) {
                    mouseClicked_ = false;
                }
                transition.type = EventType::Enter;
                if (target) {
                    target->event(transition);
                }
            }
            if (e.type == EventType::Drag && e.button == 3) {
                hoverTime_ = time_;
            }
            result = target && target->event(e);
        }
    } catch (...) {
        result = false;
    }
    if (!result) {
        for (auto& restore : undo) {
            restore();
        }
        for (auto& state : states) {
            state.control->depressed = state.depressed;
            state.control->over = state.over;
            if (state.check) {
                state.check->checked = state.checked;
            }
            if (state.slider) {
                state.slider->value = state.value;
                state.slider->dragging_ = state.dragging;
                state.slider->pageLeft_ = state.pageLeft;
            }
        }
        restoreInteraction();
    }
    // Pointer position belongs to the platform, even over passive controls or
    // outside the root. A rejected control event must not freeze the cursor.
    if (e.type == EventType::Move || e.type == EventType::Drag || e.type == EventType::Down ||
        e.type == EventType::Up || e.type == EventType::Wheel) {
        cursorPoint_ = e.point;
    }
    if (result) {
        if (e.type == EventType::Down) {
            cursorPoint_ = e.point;
            hoverTime_ = time_;
            if (e.button == 1) {
                lastMouse_ = e;
                nextMouse_ = time_ + .3;
                leftDown_ = true;
                mouseClicked_ = hover_ != nullptr;
            } else if (e.button == 3) {
                rightDown_ = true;
            }
        }
        if (e.type == EventType::Move || e.type == EventType::Drag) {
            cursorPoint_ = e.point;
            lastMouse_.point = e.point;
        }
        if (e.type == EventType::Up) {
            hoverTime_ = time_;
            if (e.button == 1) {
                nextMouse_.reset();
                leftDown_ = false;
            } else if (e.button == 3) {
                rightDown_ = false;
            }
        }
        if (e.type == EventType::KeyDown && !e.repeat) {
            lastKey_ = e;
            if (e.key == Key::Tab) {
                nextKey_.reset();
            } else {
                nextKey_ = time_ + .3;
            }
        }
        if (e.type == EventType::KeyUp) {
            nextKey_.reset();
        }
    }
    dispatching_ = false;
    auto deferred = std::move(afterDispatch_);
    afterDispatch_.clear();
    if (result) {
        for (auto& action : deferred) {
            action();
        }
    }
    return result;
}

bool Canvas::dispatch(const Event& e, double seconds) {
    if (!std::isfinite(seconds) || seconds < time_ || dispatching_) {
        return false;
    }
    const auto previous = time_;
    time_ = seconds;
    if (dispatch(e)) {
        return true;
    }
    time_ = previous;
    return false;
}

void Canvas::afterDispatch(std::function<void()> action) {
    if (dispatching_) {
        afterDispatch_.push_back(std::move(action));
    } else {
        action();
    }
}

bool Canvas::performClick(Active& control) {
    if (dispatching_ || !contains(&control)) {
        return false;
    }
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& c) -> void {
        undo.push_back(c.checkpoint());
        for (auto* child : c.objects()) {
            self(self, *static_cast<Control*>(child));
        }
    };
    save(save, *top());
    auto* oldFocus = focus_;
    auto* oldCapture = capture_;
    auto* oldHover = hover_;
    dispatching_ = true;
    bool success = true;
    try {
        control.action();
    } catch (...) {
        success = false;
        for (auto& restore : undo) {
            restore();
        }
        bool restored = true;
        if (capture_ != oldCapture && captureMouse) {
            try {
                restored = captureMouse(oldCapture != nullptr);
            } catch (...) {
                restored = false;
            }
        }
        if (restored) {
            focus_ = oldFocus;
            capture_ = oldCapture;
            hover_ = oldHover;
        } else {
            cancel();
        }
    }
    dispatching_ = false;
    auto deferred = std::move(afterDispatch_);
    afterDispatch_.clear();
    if (success) {
        for (auto& action : deferred) {
            action();
        }
    }
    return success;
}

bool Canvas::advance(double seconds) {
    if (dispatching_ || !std::isfinite(seconds) || seconds < time_) {
        return false;
    }
    std::vector<std::function<void()>> undo;
    std::vector<Control*> controls;
    const auto visit = [&](auto&& self, Control& c) -> void {
        undo.push_back(c.checkpoint());
        controls.push_back(&c);
        for (auto* o : c.objects()) {
            self(self, *static_cast<Control*>(o));
        }
    };
    for (auto* root : roots_) {
        visit(visit, *root);
    }
    const auto previous = time_;
    time_ = seconds;
    dispatching_ = true;
    auto restoreInteraction = interactionCheckpoint();
    try {
        for (auto* c : controls) {
            c->tick(seconds);
        }
        if (synthesizeKeyRepeat && nextKey_ && seconds - *nextKey_ > 1e-9 &&
            (focus_ || keyCommand)) {
            if (lastKey_.key == Key::Tab) {
                nextKey_.reset();
            } else {
                auto event = lastKey_;
                event.repeat = true;
                const bool handled =
                    (keyCommand && keyCommand(event)) || (focus_ && focus_->event(event));
                if (!handled) {
                    throw std::runtime_error("GUI key repeat");
                }
                nextKey_ = seconds + .05;
            }
        }
        if (nextMouse_ && seconds - *nextMouse_ > 1e-9 && capture_) {
            auto event = lastMouse_;
            event.type = EventType::Repeat;
            event.point = cursorPoint_;
            if (!capture_->event(event)) {
                throw std::runtime_error("GUI mouse repeat");
            }
        }
    } catch (...) {
        for (auto& restore : undo) {
            restore();
        }
        time_ = previous;
        restoreInteraction();
        dispatching_ = false;
        afterDispatch_.clear();
        return false;
    }
    dispatching_ = false;
    rebuildTabs();
    auto deferred = std::move(afterDispatch_);
    afterDispatch_.clear();
    for (auto& action : deferred) {
        action();
    }
    return true;
}

bool Canvas::setCursor(Cursor next) {
    if ((next.bitmap && !next.bitmap->valid()) ||
        (next.frames && (!next.frames->valid() || next.frames->bitmaps.empty() ||
                         !std::isfinite(next.frameTime) || next.frameTime <= 0)) ||
        std::abs(Int64(next.hotspot.x)) > Control::MaxCoordinate ||
        std::abs(Int64(next.hotspot.y)) > Control::MaxCoordinate) {
        return false;
    }
    cursor = std::move(next);
    renderedCursor_ = cursor;
    cursorFrame_ = 0;
    cursorFrameTime_ = time_;
    resetUpdateRegions();
    return true;
}

bool Canvas::addUpdateRegion(Point2I p, Point2I e) const {
    if (e.x < 0 || e.y < 0 || std::abs(Int64(p.x)) > Control::MaxCoordinate ||
        std::abs(Int64(p.y)) > Control::MaxCoordinate || e.x > Control::MaxCoordinate ||
        e.y > Control::MaxCoordinate) {
        return false;
    }
    if (!e.x || !e.y) {
        return true;
    }
    const auto region = bounds(p, e);
    if (!currentUpdate_) {
        currentUpdate_ = region;
    } else {
        auto& r = *currentUpdate_;
        r = {std::min(r.left, region.left),
             std::min(r.top, region.top),
             std::max(r.right, region.right),
             std::max(r.bottom, region.bottom)};
    }
    return true;
}

void Canvas::resetUpdateRegions() const {
    retainedDirty_ = true;
    const auto extent =
        updateExtent_.x ? updateExtent_ : (top() ? top()->fields().extent : Point2I{});
    currentUpdate_ =
        extent.x > 0 && extent.y > 0 ? std::optional(bounds({}, extent)) : std::nullopt;
    oldUpdates_.fill(currentUpdate_);
}

bool Canvas::fitContent(Point2I extent, bool onlyIfChanged) const {
    if (onlyIfChanged && !layoutDirty_ && fittedExtent_ == extent && fittedRoots_ == roots_) {
        return true;
    }
    std::vector<std::pair<Control*, Fields>> saved;
    const auto save = [&](auto&& self, Control& c) -> void {
        saved.emplace_back(&c, c.fields_);
        for (auto* object : c.objects()) {
            self(self, *static_cast<Control*>(object));
        }
    };
    for (auto* root : roots_) {
        save(save, *root);
    }
    for (auto* root : roots_) {
        if (!root->resize({}, extent)) {
            for (auto& [control, fields] : saved) {
                control->fields_ = std::move(fields);
            }
            return false;
        }
    }
    fittedExtent_ = extent;
    fittedRoots_ = roots_;
    layoutDirty_ = false;
    resetUpdateRegions();
    return true;
}

std::shared_ptr<const gfx::Bitmap> Canvas::render(Int32 width,
                                                  Int32 height,
                                                  const gfx::Palette* palette,
                                                  const gfx::Bitmap* background,
                                                  gfx::SoftwareSurface::BlendRenderer blendRenderer,
                                                  bool fitRoots) const {
    auto surface = gfx::SoftwareSurface::create(width, height);
    if (!surface || (palette && !surface->setPalette(*palette)) ||
        !renderInto(*surface, background, std::move(blendRenderer), fitRoots, false)) {
        return {};
    }
    auto output = std::move(*surface).releaseBitmap();
    if (palette) {
        output.palette = *palette;
    }
    return std::make_shared<const gfx::Bitmap>(std::move(output));
}

const gfx::Bitmap* Canvas::renderRetained(Int32 width,
                                          Int32 height,
                                          std::shared_ptr<const gfx::Palette> palette,
                                          const gfx::Bitmap* background,
                                          gfx::SoftwareSurface::BlendRenderer blendRenderer,
                                          bool fitRoots) const {
    const auto matches = [=](const auto& surface) {
        return surface && surface->bitmap().width == width && surface->bitmap().height == height;
    };
    if (!matches(retainedBack_)) {
        retainedBack_ = gfx::SoftwareSurface::create(width, height);
    }
    if (!retainedBack_ || (palette && !retainedBack_->setSharedPalette(palette))) {
        return nullptr;
    }
    if (!palette) {
        retainedBack_->clearPalette();
    }
    const bool forceFull = !matches(retainedFront_) || retainedFront_->palette() != palette.get() ||
                           retainedDirty_ || retainedTime_ != time_ || background ||
                           bool(blendRenderer) || bool(onHelp);
    retainedBack_->resetClip();
    if (!forceFull && !retainedBack_->blit(retainedFront_->bitmap(), {})) {
        return nullptr;
    }
    const bool rendered =
        renderInto(*retainedBack_, background, std::move(blendRenderer), fitRoots, true, forceFull);
    retainedBack_->setBlendRenderer({});
    if (!rendered) {
        return nullptr;
    }
    retainedFront_.swap(retainedBack_);
    retainedDirty_ = false;
    retainedTime_ = time_;
    return &retainedFront_->bitmap();
}

bool Canvas::renderInto(gfx::SoftwareSurface& surface,
                        const gfx::Bitmap* background,
                        gfx::SoftwareSurface::BlendRenderer blendRenderer,
                        bool fitRoots,
                        bool incremental,
                        bool forceFull) const {
    auto* s = &surface;
    const auto width = s->bitmap().width, height = s->bitmap().height;
    s->setBlendRenderer(std::move(blendRenderer));
    // A viewport publishes its picking projection only with successful drawing.
    // Later roots/cursors may still reject the frame, so retain the same local
    // checkpoints used by dispatch until the complete bitmap is published.
    std::vector<std::function<void()>> undo;
    const auto save = [&](auto&& self, Control& control) -> void {
        undo.push_back(control.checkpoint());
        for (auto* object : control.objects()) {
            self(self, *static_cast<Control*>(object));
        }
    };
    for (auto* root : roots_) {
        save(save, *root);
    }
    undo.push_back([this,
                    layoutDirty = layoutDirty_,
                    fittedExtent = fittedExtent_,
                    fittedRoots = fittedRoots_,
                    help = help_,
                    extent = updateExtent_,
                    current = currentUpdate_,
                    displayed = displayedUpdate_,
                    old = oldUpdates_,
                    last = lastCursorRegion_,
                    cursor = renderedCursor_,
                    frame = cursorFrame_,
                    time = cursorFrameTime_,
                    tag = controlCursorTag_,
                    control = controlCursor_,
                    controlFrame = controlCursorFrame_] {
        layoutDirty_ = layoutDirty;
        fittedExtent_ = fittedExtent;
        fittedRoots_ = fittedRoots;
        help_ = help;
        updateExtent_ = extent;
        currentUpdate_ = current;
        displayedUpdate_ = displayed;
        oldUpdates_ = old;
        lastCursorRegion_ = last;
        renderedCursor_ = cursor;
        cursorFrame_ = frame;
        cursorFrameTime_ = time;
        controlCursorTag_ = tag;
        controlCursor_ = control;
        controlCursorFrame_ = controlFrame;
    });
    bool committed = false;

    struct Restore {
        std::vector<std::function<void()>>& undo;
        bool& committed;

        ~Restore() {
            if (!committed) {
                for (auto& restore : undo) {
                    restore();
                }
            }
        }
    } restore{undo, committed};

    // The main game canvas fits content/dialog roots before drawing; viewport
    // capture and editor callers retain their authored bounds for clipping.
    // Resizing also applies the children's center/edge/stretch constraints.
    if (forceFull) {
        resetUpdateRegions();
    }
    if (fitRoots && !fitContent({width, height}, incremental)) {
        return {};
    }
    if (updateExtent_ != Point2I{width, height}) {
        updateExtent_ = {width, height};
        resetUpdateRegions();
    }
    if (renderedCursor_.bitmap != cursor.bitmap || renderedCursor_.frames != cursor.frames ||
        renderedCursor_.hotspot != cursor.hotspot ||
        renderedCursor_.frameTime != cursor.frameTime) {
        renderedCursor_ = cursor;
        cursorFrame_ = 0;
        cursorFrameTime_ = time_;
        resetUpdateRegions();
    }
    const auto tag = capture_ ? capture_->cursorTag() : hover_ ? hover_->cursorTag() : 0;
    if (tag && tag != controlCursorTag_) {
        if (!controlCursor) {
            return {};
        }
        auto next = controlCursor(tag);
        if (!next || (!next->bitmap && !next->frames)) {
            return {};
        }
        controlCursor_ = std::move(*next);
        controlCursorTag_ = tag;
        controlCursorFrame_ = 0;
        cursorFrameTime_ = time_;
    }
    const auto& sprite = tag ? controlCursor_ : cursor;
    auto& spriteFrame = tag ? controlCursorFrame_ : cursorFrame_;
    const gfx::Bitmap* bitmap = sprite.bitmap.get();
    if (!bitmap && sprite.frames) {
        if (!sprite.frames->valid() || sprite.frames->bitmaps.empty() ||
            !std::isfinite(sprite.frameTime) || sprite.frameTime <= 0) {
            if (cursorEnabled && cursorVisible) {
                return {};
            }
        } else {
            bitmap = &sprite.frames->bitmaps[spriteFrame % sprite.frames->bitmaps.size()];
        }
    }
    if (bitmap && !bitmap->valid()) {
        if (cursorEnabled && cursorVisible) {
            return {};
        }
        bitmap = nullptr;
    }
    if (lastCursorRegion_) {
        const auto r = *lastCursorRegion_;
        addUpdateRegion({r.left, r.top}, {Int32(r.width()), Int32(r.height())});
    }
    lastCursorRegion_.reset();
    if (cursorEnabled && bitmap) {
        if (std::abs(Int64(sprite.hotspot.x)) > Control::MaxCoordinate ||
            std::abs(Int64(sprite.hotspot.y)) > Control::MaxCoordinate) {
            return {};
        }
        const auto p = cursorPoint_ - sprite.hotspot;
        lastCursorRegion_ = bounds(p - Point2I{2, 2}, {bitmap->width + 4, bitmap->height + 4});
        const auto r = *lastCursorRegion_;
        addUpdateRegion({r.left, r.top}, {Int32(r.width()), Int32(r.height())});
    }
    auto update = currentUpdate_;
    for (auto old : oldUpdates_) {
        if (old) {
            if (!update) {
                update = old;
            } else {
                auto& r = *update;
                r = {std::min(r.left, old->left),
                     std::min(r.top, old->top),
                     std::max(r.right, old->right),
                     std::max(r.bottom, old->bottom)};
            }
        }
    }
    oldUpdates_[0] = oldUpdates_[1];
    oldUpdates_[1] = currentUpdate_;
    currentUpdate_.reset();
    displayedUpdate_ = update ? intersect(*update, s->bitmap().bounds()) : std::nullopt;
    HelpState help;
    help.elapsed = std::round((time_ - hoverTime_) * 1000);
    help.clicked = mouseClicked_;
    if (hover_ && cursorEnabled && !capture_ && !leftDown_ && !rightDown_) {
        help.tag = hover_->helpTag(help.elapsed);
        help.text = hover_->helpText(help.elapsed);
        if (help.text && help.text->size() > 65536) {
            return {};
        }
    }
    if (onHelp) {
        onHelp(help);
    }
    help_ = std::move(help);
    const auto damage = incremental ? displayedUpdate_ : std::optional(s->bitmap().bounds());
    if (!damage) {
        committed = true;
        return true;
    }
    s->resetClip();
    s->fill(*damage, 0);
    s->setClip(*damage);
    if (background && !s->blit(*background, {0, 0})) {
        return {};
    }
    for (auto* root : roots_) {
        if (!root->visible()) {
            continue;
        }
        const auto origin = root->fields().position;
        const auto clip = intersect(*damage, bounds(origin, root->fields().extent));
        if (clip) {
            s->setClip(*clip);
            if (!root->draw(*s, origin, *clip)) {
                return {};
            }
        }
    }
    if (cursorEnabled && cursorVisible && bitmap) {
        s->setClip(*damage);
        auto transparent = *bitmap;
        transparent.attributes |= gfx::BitmapTransparent;
        if (!s->blit(transparent, cursorPoint_ - sprite.hotspot)) {
            return {};
        }
        if (!sprite.bitmap && sprite.frames && time_ - cursorFrameTime_ > sprite.frameTime) {
            spriteFrame = (spriteFrame + 1) % sprite.frames->bitmaps.size();
            cursorFrameTime_ = time_;
        }
    }
    committed = true;
    return true;
}

bool registerClasses(Persistent::ClassRegistry& r) {
    return r.registerClass<Control>("SimGui::Control", makeFourCC('S', 'G', 'c', 't')) &&
           r.registerClass<Active>("SimGui::ActiveCtrl", makeFourCC('S', 'G', 'a', 'c')) &&
           r.registerClass<Progress>("SimGui::ProgressCtrl", makeFourCC('S', 'G', 'p', 'c')) &&
           r.registerClass<Slider>("SimGui::Slider", makeFourCC('S', 'G', 's', 'l')) &&
           r.registerClass<Text>("SimGui::SimpleText", makeFourCC('S', 'G', 's', 't')) &&
           r.registerClass<Button>("SimGui::TestButton", makeFourCC('S', 'G', 't', 'b')) &&
           r.registerClass<Check>("SimGui::TestCheck", makeFourCC('S', 'G', 't', 'k')) &&
           r.registerClass<Radio>("SimGui::TestRadial", makeFourCC('S', 'G', 't', 'r')) &&
           r.registerClass<TextEdit>("SimGui::TextEdit", makeFourCC('S', 'G', 't', 'e')) &&
           r.registerClass<Bitmap>("SimGui::BitmapCtrl", makeFourCC('S', 'G', 'b', 'm'));
}
} // namespace opent1::gui
