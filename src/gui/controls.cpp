#include <opent1/gui/controls.hpp>
#include <ranges>
#include <opent1/gui/containers.hpp>
#include <opent1/gui/text_format.hpp>
#include <opent1/gui/edit_control.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace opent1::gui {
namespace {
using Error = Persistent::Error;

bool dimensions(Point2I p) {
    return p.x >= 0 && p.y >= 0 && p.x <= 32768 && p.y <= 32768;
}

std::string fold(std::string_view value) {
    std::string out(value);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return out;
}

bool transparentBlit(gfx::SoftwareSurface& s, const gfx::Bitmap& source, Point2I p) {
    auto copy = source;
    copy.attributes |= gfx::BitmapTransparent;
    return s.blit(copy, p);
}
}

bool Array::setSize(Point2I size) {
    if (!dimensions(size) || cellSize.x < 1 || cellSize.y < 1 || !dimensions(cellSize) ||
        !dimensions(header) || Int64(size.x) * size.y > 65536) {
        return false;
    }
    const auto x = Int64(size.x) * cellSize.x + header.x, y = Int64(size.y) * cellSize.y + header.y;
    if (x > 32768 || y > 32768 || !resize(fields_.position, {Int32(x), Int32(y)})) {
        return false;
    }
    cells = size;
    return true;
}

void Array::wake() {
    if (!setSize(cells)) {
        throw std::runtime_error("GUI array extent");
    }
}

bool Array::select(Point2I cell) {
    if (cell != Point2I{-1, -1} &&
        (cell.x < 0 || cell.y < 0 || cell.x >= cells.x || cell.y >= cells.y)) {
        return false;
    }
    auto* scroll = parent() ? dynamic_cast<Scroll*>(parent()->parent()) : nullptr;
    std::vector<std::function<void()>> undo;
    if (scroll) {
        const auto save = [&](auto&& self, Control& c) -> void {
            undo.push_back(c.checkpoint());
            for (auto* o : c.objects()) {
                self(self, *static_cast<Control*>(o));
            }
        };
        save(save, *scroll);
    }
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
        }
    }};

    if (parent() && cell.x != -1) {
        auto p = fields_.position;
        const auto e = parent()->fields().extent;
        const Point2I at{p.x + cell.x * cellSize.x, p.y + cell.y * cellSize.y};
        if (at.x <= 0) {
            p.x -= at.x;
        } else if (at.x + cellSize.x > e.x) {
            p.x += e.x - at.x - cellSize.x;
        }
        if (at.y <= 0) {
            p.y -= at.y;
        } else if (at.y + cellSize.y > e.y) {
            p.y += e.y - at.y - cellSize.y;
        }
        if (!resize(p, fields_.extent)) {
            return false;
        }
    }
    // Original ScrollContent::childResized recalculates the enclosing bars
    // when selection scrolls an Array. Publish the same geometry immediately.
    if (scroll && !scroll->computeSizes()) {
        return false;
    }
    selected = cell;
    published = true;
    return true;
}

bool Array::event(const Event& e) {
    if (cellSize.x < 1 || cellSize.y < 1 || !dimensions(cells)) {
        return false;
    }
    if (e.type == EventType::KeyDown && e.repeat && (!canvas() || canvas()->focus() != this)) {
        return true;
    }
    if (e.type == EventType::Cancel || e.type == EventType::Leave) {
        hoverCell = {-1, -1};
        if (e.type == EventType::Leave) {
            over = false;
            return true;
        }
    }
    if ((e.type == EventType::Down && e.button == 1 && active) || e.type == EventType::Enter ||
        e.type == EventType::Move) {
        auto p = globalToLocal(e.point);
        p.x -= header.x;
        p.y -= header.y;
        Point2I cell{p.x < 0 ? -1 : p.x / cellSize.x, p.y < 0 ? -1 : p.y / cellSize.y};
        if (cell.x < 0 || cell.y < 0 || cell.x >= cells.x || cell.y >= cells.y) {
            cell = {-1, -1};
        }
        if (e.type == EventType::Down) {
            if (cell.x >= 0 && !select(cell)) {
                return false;
            }
        } else {
            hoverCell = cell;
            over = cell.x >= 0;
            return true;
        }
    }
    if (active && e.type == EventType::KeyDown) {
        Point2I d;
        switch (e.key) {
            case Key::Left:
                d = {-1, 0};
                break;
            case Key::Right:
                d = {1, 0};
                break;
            case Key::Up:
                d = {0, -1};
                break;
            case Key::Down:
                d = {0, 1};
                break;
            default:
                return Active::event(e);
        }
        if (!cells.x || !cells.y) {
            return true;
        }
        if (selected.x == -1 || selected.y == -1) {
            return select({0, 0});
        }
        auto cell = selected;
        cell += d;
        while (cell.x >= 0 && cell.y >= 0 && cell.x < cells.x && cell.y < cells.y) {
            if (select(cell)) {
                action();
                break;
            }
            cell += d;
        }
        return true;
    }
    return Active::event(e);
}

bool Array::drawCell(
    gfx::SoftwareSurface& s, Point2I p, Point2I cell, bool chosen, bool hovered) const {
    s.fill({p.x, p.y, p.x + cellSize.x, p.y + cellSize.y},
           chosen    ? 255
           : hovered ? 0
                     : UInt8(cell.x + cell.y));
    return true;
}

bool Array::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (!parent()) {
        return true;
    }
    if (cellSize.x < 1 || cellSize.y < 1 || !dimensions(cells) ||
        Int64(cells.x) * cells.y > 65536) {
        return false;
    }
    const auto old = s.clip();

    struct Restore {
        gfx::SoftwareSurface& s;
        RectI r;

        ~Restore() {
            s.setClip(r);
        }
    } restore{s, old};

    const auto origin = parent()->localToGlobal({});
    if (header.y) {
        const auto headerClip =
            gfx::intersect(clip, {origin.x + header.x, origin.y, clip.right, origin.y + header.y});
        if (headerClip) {
            s.setClip(*headerClip);
            if (fields_.style.border) {
                s.fill({p.x + header.x, origin.y, p.x + fields_.extent.x, origin.y + header.y},
                       fields_.style.borderColor);
            }
            clip.top = headerClip->bottom;
        }
        p.y += header.y;
    }
    const auto originalClip = clip;
    if (header.x) {
        clip.left = std::max(clip.left, origin.x + header.x);
        p.x += header.x;
    }
    for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                      return y < cells.y;
                  })) {
        const auto top = p.y + y * cellSize.y;
        if (top > originalClip.bottom) {
            break;
        }
        if (top + cellSize.y < originalClip.top) {
            continue;
        }
        if (header.x) {
            if (const auto rowClip = gfx::intersect(
                    originalClip, {origin.x, top, origin.x + header.x, top + cellSize.y})) {
                s.setClip(*rowClip);
                s.fill({origin.x, top, origin.x + header.x, top + cellSize.y}, y % 2 ? 254 : 253);
            }
        }
        for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                          return x < cells.x;
                      })) {
            const Point2I at{p.x + x * cellSize.x, top};
            auto cellClip =
                gfx::intersect(clip, {at.x, at.y, at.x + cellSize.x - 1, at.y + cellSize.y - 1});
            if (!cellClip) {
                continue;
            }
            s.setClip(*cellClip);
            if (!drawCell(s, at, {x, y}, selected == Point2I{x, y}, hoverCell == Point2I{x, y})) {
                return false;
            }
        }
    }
    return true;
}

std::function<void()> Array::checkpoint() {
    auto base = Active::checkpoint();
    const auto chosen = selected, hover = hoverCell, size = cells, cell = cellSize, head = header;
    return [this, base = std::move(base), chosen, hover, size, cell, head] {
        base();
        selected = chosen;
        hoverCell = hover;
        cells = size;
        cellSize = cell;
        header = head;
    };
}

bool TextList::insert(ListEntry e, bool front) {
    if (entries.size() >= 4096 || e.text.size() > 65536 || e.text.find('\0') != std::string::npos) {
        return false;
    }
    if (noDuplicates && std::ranges::any_of(entries, [&](const auto& old) {
            return fold(old.text) == fold(e.text);
        })) {
        return true;
    }
    const auto n = Int32(entries.size() + 1);
    if (!setSize({1, n})) {
        return false;
    }
    if (front) {
        entries.insert(entries.begin(), std::move(e));
    } else {
        entries.push_back(std::move(e));
    }
    return true;
}

bool TextList::insertTag(Int32 tag,
                         const std::function<std::optional<std::string>(Int32)>& resolve,
                         bool front) {
    if (!tag || !resolve) {
        return false;
    }
    try {
        auto text = resolve(tag);
        return text && insert({tag, std::move(*text)}, front);
    } catch (...) {
        return false;
    }
}

bool TextList::variableChanged(std::string_view value) {
    for (auto i : std::views::iota(std::size_t{0}, entries.size())) {
        if (entries[i].text == value) {
            const Point2I cell{0, Int32(i)};
            return selected == cell || select(cell);
        }
    }
    return true;
}

std::function<void()> TextList::checkpoint() {
    auto base = Array::checkpoint();
    auto oldEntries = entries;
    auto normal = font, highlight = highlightFont;
    const auto normalTag = fontTag, highlightTag = highlightFontTag, delta = verticalDelta;
    const auto numbered = enumerate, duplicates = noDuplicates;
    return [this,
            base = std::move(base),
            oldEntries = std::move(oldEntries),
            normal = std::move(normal),
            highlight = std::move(highlight),
            normalTag,
            highlightTag,
            delta,
            numbered,
            duplicates] {
        base();
        entries = oldEntries;
        font = normal;
        highlightFont = highlight;
        fontTag = normalTag;
        highlightFontTag = highlightTag;
        verticalDelta = delta;
        enumerate = numbered;
        noDuplicates = duplicates;
    };
}

bool TextList::erase(std::size_t i) {
    if (i >= entries.size() || !setSize({1, Int32(entries.size() - 1)})) {
        return false;
    }
    entries.erase(entries.begin() + i);
    selected = {-1, -1};
    return true;
}

void TextList::clear() {
    entries.clear();
    selected = {-1, -1};
    setSize({1, 0});
}

void TextList::sort() {
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return fold(a.text) < fold(b.text);
    });
}

const ListEntry* TextList::selection() const {
    return selected.y >= 0 && std::size_t(selected.y) < entries.size() ? &entries[selected.y]
                                                                       : nullptr;
}

void TextList::wake() {
    if (font) {
        cellSize = {fields_.extent.x, font->info.height + 4 + verticalDelta};
    }
    if (!setSize({1, Int32(entries.size())})) {
        throw std::runtime_error("GUI list layout");
    }
}

bool TextList::drawCell(
    gfx::SoftwareSurface& s, Point2I p, Point2I cell, bool chosen, bool hovered) const {
    (void)hovered;
    if (cell.y < 0 || std::size_t(cell.y) >= entries.size()) {
        return false;
    }
    if (chosen) {
        s.fill({p.x, p.y, p.x + cellSize.x - 1, p.y + cellSize.y - 1}, 249);
    }
    auto f = chosen ? highlightFont : font;
    if (!highlightFont || !f) {
        return true;
    }
    return s.text(*f, {p.x + 10, p.y + verticalDelta}, entries[cell.y].text);
}

Error TextList::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!s.write(fontTag) || !s.write(highlightFontTag) || !s.write(verticalDelta) ||
        !s.write(enumerate) || !s.write(noDuplicates)) {
        return Error::WriteError;
    }
    return Active::write(s, v, u, c);
}

Error TextList::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 f{}, h{}, d{};
    bool e{}, n{};
    if (!s.read(f) || !s.read(h) || !s.read(d) || std::abs(Int64(d)) > 32768 || !s.read(e) ||
        !s.read(n)) {
        return Error::ReadError;
    }
    const auto r = Active::read(s, v, u, c);
    if (r == Error::Ok) {
        fontTag = f;
        highlightFontTag = h;
        verticalDelta = d;
        enumerate = e;
        noDuplicates = n;
    }
    return r;
}

void Timer::wake() {
    if (!std::isfinite(initialAlarm) || initialAlarm < 0 || !std::isfinite(repeatAlarm) ||
        repeatAlarm < 0) {
        throw std::runtime_error("GUI timer interval");
    }
    alarm_ = initialAlarm ? std::optional<double>((canvas() ? canvas()->time() : 0) + initialAlarm)
                          : std::nullopt;
}

void Timer::tick(double t) {
    if (alarm_ && t >= *alarm_) {
        if (canvas() && canvas()->onAction &&
            !canvas()->onAction(*this, message, fields_.command)) {
            throw std::runtime_error("GUI timer action");
        }
        alarm_ = repeatAlarm ? std::optional<double>(t + repeatAlarm) : std::nullopt;
    }
}

std::function<void()> Timer::checkpoint() {
    auto base = Control::checkpoint();
    const auto alarm = alarm_;
    const auto m = message;
    const auto initial = initialAlarm, repeat = repeatAlarm;
    return [this, base = std::move(base), alarm, m, initial, repeat] {
        base();
        alarm_ = alarm;
        message = m;
        initialAlarm = initial;
        repeatAlarm = repeat;
    };
}

Error Timer::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!std::isfinite(initialAlarm) || initialAlarm < 0 || !std::isfinite(repeatAlarm) ||
        repeatAlarm < 0 || !s.write(message) || !s.write(initialAlarm) || !s.write(repeatAlarm)) {
        return Error::WriteError;
    }
    return Control::write(s, v, u, c);
}

Error Timer::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    UInt32 m{};
    float a{}, b{};
    if (!s.read(m) || !s.read(a) || !s.read(b) || !std::isfinite(a) || a < 0 || !std::isfinite(b) ||
        b < 0) {
        return Error::ReadError;
    }
    const auto r = Control::read(s, v, u, c);
    if (r == Error::Ok) {
        message = m;
        initialAlarm = a;
        repeatAlarm = b;
    }
    return r;
}

void Animate::start() {
    animating = true;
    frame = 0;
}

bool Animate::onAdd() {
    if (!Active::onAdd()) {
        return false;
    }
    last_ = getManager() ? getManager()->getCurrentTime() : 0;
    clockInitialized_ = true;
    if (!looping) {
        animating = false;
    }
    return true;
}

void Animate::wake() {
    if (!std::isfinite(timePerFrame) || timePerFrame <= 0 || sleep < 0 || sleep > 65536) {
        throw std::runtime_error("GUI animation interval");
    }
    if (!clockInitialized_) {
        last_ = canvas() ? canvas()->time() : 0;
        clockInitialized_ = true;
        if (!looping) {
            animating = false;
        }
    }
}

void Animate::tick(double t) {
    if (!visible() || !frames || frames->bitmaps.empty() || (!looping && !animating)) {
        return;
    }
    if (t - last_ > timePerFrame) {
        ++frame;
        if (frame >= frames->bitmaps.size() + std::size_t(sleep)) {
            frame = 0;
            if (!looping) {
                animating = false;
                visible(false);
            }
        }
        last_ = t;
    }
}

bool Animate::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    if (!frames || frames->bitmaps.empty()) {
        return true;
    }
    const auto* part = frames->get(Int32(std::min<std::size_t>(frame, frames->bitmaps.size() - 1)));
    return part && transparentBlit(s, *part, p);
}

std::function<void()> Animate::checkpoint() {
    auto base = Active::checkpoint();
    const auto f = frame;
    const auto a = animating, loop = looping, initialized = clockInitialized_;
    const auto l = last_;
    const auto tag = arrayTag, wait = sleep;
    const auto interval = timePerFrame;
    const auto art = frames;
    return [this, base = std::move(base), f, a, loop, initialized, l, tag, wait, interval, art] {
        base();
        frame = f;
        animating = a;
        looping = loop;
        clockInitialized_ = initialized;
        last_ = l;
        arrayTag = tag;
        sleep = wait;
        timePerFrame = interval;
        frames = art;
    };
}

Error Animate::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    const Int32 dummy = !looping;
    if (!std::isfinite(timePerFrame) || timePerFrame <= 0 || sleep < 0 || sleep > 65536 ||
        !s.write(arrayTag) || !s.write(timePerFrame) || !s.write(dummy) || !s.write(sleep)) {
        return Error::WriteError;
    }
    for ([[maybe_unused]] auto i : std::views::iota(0, 8)) {
        if (!s.write(dummy)) {
            return Error::WriteError;
        }
    }
    return Active::write(s, v, u, c);
}

Error Animate::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 tag{}, dummy{}, wait{};
    float dt{};
    if (!s.read(tag) || !s.read(dt) || !s.read(dummy) || !s.read(wait) || !std::isfinite(dt) ||
        dt <= 0 || wait < 0 || wait > 65536) {
        return Error::ReadError;
    }
    const bool loop = !dummy;
    for ([[maybe_unused]] auto i : std::views::iota(0, 8)) {
        if (!s.read(dummy)) {
            return Error::ReadError;
        }
    }
    const auto r = Active::read(s, v, u, c);
    if (r == Error::Ok) {
        arrayTag = tag;
        timePerFrame = dt;
        looping = loop;
        sleep = wait;
    }
    return r;
}

void PaletteControl::wake() {
    // An activation may replace its own callback/resource before returning.
    const auto callback = activate;
    const auto resource = palette;
    if (resource && callback && !callback(resource)) {
        throw std::runtime_error("GUI palette activation");
    }
}

std::function<void()> PaletteControl::checkpoint() {
    auto base = Control::checkpoint();
    const auto tag = paletteTag;
    const auto resource = palette;
    const auto callback = activate;
    return [this, base = std::move(base), tag, resource, callback] {
        base();
        paletteTag = tag;
        palette = resource;
        activate = callback;
    };
}

Error PaletteControl::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    const std::array<std::byte, 81> reserved{};
    if (!s.write(paletteTag) || !s.writeBytes(reserved)) {
        return Error::WriteError;
    }
    return Control::write(s, v, u, c);
}

Error PaletteControl::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 tag{};
    std::array<std::byte, 81> reserved{};
    if (!s.read(tag) || !s.readBytes(reserved)) {
        return Error::ReadError;
    }
    const auto r = Control::read(s, v, u, c);
    if (r == Error::Ok) {
        paletteTag = tag;
    }
    return r;
}

void Help::setText(std::optional<std::string> text) {
    if (text && text->size() > 65536) {
        throw std::length_error("GUI help text");
    }
    helpText = std::move(text);
    if (helpText) {
        helpTag = 0;
    }
}

void Help::setTag(Int32 tag) {
    helpTag = tag;
    helpText.reset();
}

std::function<void()> BitmapBox::checkpoint() {
    auto base = Control::checkpoint();
    const auto art = frames;
    const auto tag = arrayTag;
    return [this, base = std::move(base), art, tag] {
        base();
        frames = art;
        arrayTag = tag;
    };
}

bool BitmapBox::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (!frames) {
        return true;
    }
    for (auto i : std::views::iota(Int32{0}, Int32{9})) {
        if (!frames->get(i)) {
            return false;
        }
    }
    const auto& b = frames->bitmaps;
    if (std::ranges::any_of(b, [](const auto& x) {
            return !x.valid() || x.width <= 0 || x.height <= 0;
        })) {
        return false;
    }
    const auto tile = [&](int index, int x, int y) {
        return index == 4 ? s.blit(b[index], {p.x + x, p.y + y})
                          : transparentBlit(s, b[index], {p.x + x, p.y + y});
    };
    const auto old = s.clip();

    struct Restore {
        gfx::SoftwareSurface& s;
        RectI r;

        ~Restore() {
            s.setClip(r);
        }
    } restore{s, old};

    const auto left = b[3].width, right = fields_.extent.x - b[5].width, top = b[1].height,
               bottom = fields_.extent.y - b[1].height;
    if (auto region = gfx::intersect(clip, {p.x + left, p.y + top, p.x + right, p.y + bottom})) {
        s.setClip(*region);
        for (int y = top; y < bottom; y += b[4].height) {
            for (int x = left; x < right; x += b[4].width) {
                if (!tile(4, x, y)) {
                    return false;
                }
            }
        }
    }
    s.setClip(old);
    for (int y = b[0].height; y < fields_.extent.y - b[6].height; y += b[3].height) {
        if (!tile(3, 0, y)) {
            return false;
        }
    }
    for (int y = b[2].height; y < fields_.extent.y - b[8].height; y += b[5].height) {
        if (!tile(5, fields_.extent.x - b[5].width, y)) {
            return false;
        }
    }
    for (int x = b[0].width; x < fields_.extent.x - b[2].width; x += b[1].width) {
        if (!tile(1, x, 0)) {
            return false;
        }
    }
    for (int x = b[6].width; x < fields_.extent.x - b[8].width; x += b[7].width) {
        if (!tile(7, x, fields_.extent.y - b[7].height)) {
            return false;
        }
    }
    if (!tile(0, 0, 0) || !tile(2, fields_.extent.x - b[2].width, 0) ||
        !tile(8, fields_.extent.x - b[8].width, fields_.extent.y - b[8].height) ||
        !tile(6, 0, fields_.extent.y - b[6].height)) {
        return false;
    }
    return Control::draw(s, p, clip);
}

Error BitmapBox::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (!s.write(arrayTag)) {
        return Error::WriteError;
    }
    return Control::write(s, v, u, c);
}

Error BitmapBox::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 tag{};
    if (!s.read(tag)) {
        return Error::ReadError;
    }
    const auto r = Control::read(s, v, u, c);
    if (r == Error::Ok) {
        arrayTag = tag;
    }
    return r;
}

bool ChunkedBitmap::set(const gfx::Bitmap& bitmap, bool transparent) {
    if (!bitmap.valid() || bitmap.bitDepth != 8) {
        return false;
    }
    Point2I count{(bitmap.width + 255) / 256, (bitmap.height + 255) / 256};
    std::vector<gfx::Bitmap> chunks;
    const auto width = bitmap.width / count.x, height = bitmap.height / count.y;
    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < count.y;
                  })) {
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < count.x;
                      })) {
            auto chunk =
                bitmap.crop({x * width, y * height, (x + 1) * width - 1, (y + 1) * height - 1});
            if (!chunk) {
                return false;
            }
            if (transparent) {
                chunk->attributes |= gfx::BitmapTransparent;
            } else {
                chunk->attributes &= ~UInt32(gfx::BitmapTransparent);
            }
            chunks.push_back(std::move(*chunk));
        }
    }
    count_ = count;
    chunks_ = std::move(chunks);
    return true;
}

void ChunkedBitmap::setTransparent(bool transparent) noexcept {
    for (auto& chunk : chunks_) {
        if (transparent) {
            chunk.attributes |= gfx::BitmapTransparent;
        } else {
            chunk.attributes &= ~UInt32(gfx::BitmapTransparent);
        }
    }
}

bool ChunkedBitmap::draw(gfx::SoftwareSurface& s, RectI r) const {
    if (chunks_.empty()) {
        return true;
    }
    if (!r.valid() || r.width() < count_.x || r.height() < count_.y || r.width() > 16384 ||
        r.height() > 16384) {
        return false;
    }
    const int w = Int32(r.width()) / count_.x, h = Int32(r.height()) / count_.y;
    for (auto y : std::views::iota(0) | std::views::take_while([&](int y) {
                      return y < count_.y;
                  })) {
        for (auto x : std::views::iota(0) | std::views::take_while([&](int x) {
                          return x < count_.x;
                      })) {
            RectI target{r.left + x * w,
                         r.top + y * h,
                         x == count_.x - 1 ? r.right : r.left + (x + 1) * w - 1,
                         y == count_.y - 1 ? r.bottom : r.top + (y + 1) * h - 1};
            if (!s.stretch(chunks_[y * count_.x + x], target)) {
                return false;
            }
        }
    }
    return true;
}

bool TestControl::event(const Event& e) {
    if (e.type == EventType::Enter) {
        highlight_ = true;
        return true;
    }
    if (e.type == EventType::Leave || e.type == EventType::Cancel) {
        highlight_ = false;
        return true;
    }
    return Control::event(e);
}

bool TestControl::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    s.fill({p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1},
           UInt8(highlight_ ? 2 : 1));
    return drawChildren(s, p, clip);
}

std::function<void()> TestControl::checkpoint() {
    auto base = Control::checkpoint();
    const auto h = highlight_;
    return [this, base = std::move(base), h] {
        base();
        highlight_ = h;
    };
}

std::optional<std::vector<TextLine>>
wrapText(const gfx::BitmapFont& font, std::string_view text, Int32 width) {
    std::vector<TextLine> lines;
    if (text.size() > 65536 || text.find('\0') != std::string_view::npos || width > 32768) {
        return {};
    }
    if (width < 20 || width < font.glyph('W').info.width || text.empty()) {
        return lines;
    }
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    std::size_t i = 0;
    while (i < text.size()) {
        const auto start = i;
        std::size_t length = 0;
        const auto end = std::ranges::find_if(text.substr(start), [&](const char c) {
            ++length;
            return length >= 255 || c == '\n' || font.textWidth(text.substr(start, length)) > width;
        });
        if (length >= 255) {
            return {};
        }
        i = std::size_t(end - text.begin());
        const bool newline = end != text.end();
        if (!newline) {
            lines.push_back({UInt32(start), UInt32(length)});
            return lines;
        }
        if (space(text[start + length - 1])) {
            lines.push_back({UInt32(start), UInt32(length - 1)});
            ++i;
        } else {
            Int32 j = Int32(length) - 1;
            bool slash = false;
            for (; j >= 0; --j) {
                const auto c = text[start + std::size_t(j)];
                if (space(c)) {
                    break;
                }
                if (slash) {
                    if (c != '\\') {
                        ++j;
                        break;
                    }
                } else if (c == '\\') {
                    slash = true;
                }
            }
            if (j < 0) {
                j = Int32(length) - 1;
            }
            if (j == 0) {
                return {};
            }
            lines.push_back({UInt32(start), UInt32(j)});
            i = start + std::size_t(j);
        }
        while (i < text.size() && space(text[i]) && text[i] != '\n') {
            ++i;
        }
    }
    return lines;
}

bool TextWrap::setText(std::string_view value) {
    if (value.size() > 65536 || value.find('\0') != std::string_view::npos) {
        return false;
    }
    std::string next(value);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i + 2 < next.size();
                  })) {
        if (next[i] == '\\' && next[i + 1] == 'n') {
            next[i] = '\n';
            next[i + 1] = ' ';
        }
    }
    text = std::move(next);
    return true;
}

std::function<void()> TextWrap::checkpoint() {
    auto base = Active::checkpoint();
    auto value = text;
    return [this, base = std::move(base), value = std::move(value)] {
        base();
        text = value;
    };
}

bool TextWrap::pointInControl(Point2I p) const {
    return (!ignoreClick || (canvas() && canvas()->editMode)) && Control::pointInControl(p);
}

bool TextWrap::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    auto f = enabled ? font : disabledFont;
    if (!f) {
        return true;
    }
    if (alignment < 0 || alignment > 2 || inset.x < 0 || inset.y < 0 || inset.x > 32768 ||
        inset.y > 32768 || std::abs(Int64(lineSpacing)) > 32768 ||
        std::abs(Int64(verticalDelta)) > MaxCoordinate) {
        return false;
    }
    auto lines = wrapText(*f, text, fields_.extent.x - inset.x * 2);
    if (!lines) {
        return false;
    }
    Int64 y = Int64(p.y) + inset.y - f->info.height + f->info.baseline - verticalDelta;
    for (const auto& line : *lines) {
        const auto value = std::string_view(text).substr(line.start, line.length);
        const auto width = f->textWidth(value);
        Int64 x = Int64(p.x) + inset.x;
        if (alignment == 1) {
            x += fields_.extent.x - inset.x - width;
        }
        if (alignment == 2) {
            x += (fields_.extent.x - width) >> 1;
        }
        if (std::abs(x) > MaxCoordinate || std::abs(y) > MaxCoordinate ||
            !s.text(*f, {Int32(x), Int32(y)}, value)) {
            return false;
        }
        y += f->info.height + lineSpacing;
    }
    return true;
}

Error TextWrap::write(StreamIO& s, Int32 v, Int32 u, Persistent::StoreContext& c) const {
    if (alignment < 0 || alignment > 2 || !s.write(Int32(ignoreClick)) ||
        !s.write(disabledFontTag) || !s.write(UInt32(alignment)) || !s.write(fontTag) ||
        !s.write(textTag) || !s.write(lineSpacing)) {
        return Error::WriteError;
    }
    return Active::write(s, v, u, c);
}

Error TextWrap::read(StreamIO& s, Int32 v, Int32 u, Persistent::LoadContext& c) {
    Int32 ignore{}, disabled{}, align{}, normal{}, tag{}, spacing{};
    if (!s.read(ignore) || !s.read(disabled) || !s.read(align) || !s.read(normal) || !s.read(tag) ||
        !s.read(spacing) || align < 0 || align > 2 || std::abs(Int64(spacing)) > 32768) {
        return Error::ReadError;
    }
    const auto result = Active::read(s, v, u, c);
    if (result == Error::Ok) {
        ignoreClick = ignore != 0;
        disabledFontTag = disabled;
        alignment = align;
        fontTag = normal;
        textTag = tag;
        lineSpacing = spacing;
    }
    return result;
}

bool registerExtendedClasses(Persistent::ClassRegistry& r) {
    return r.registerClass<Array>("SimGui::ArrayCtrl", makeFourCC('S', 'G', 'A', 'c')) &&
           r.registerClass<TextList>("SimGui::TextList", makeFourCC('S', 'G', 't', 'l')) &&
           r.registerClass<Timer>("SimGui::TimerCtrl", makeFourCC('S', 'G', 't', 'm')) &&
           r.registerClass<Animate>("SimGui::AnimateBMA", makeFourCC('S', 'G', 'a', 'b')) &&
           r.registerClass<PaletteControl>("SimGui::PaletteCtrl", makeFourCC('S', 'G', 'p', 'l')) &&
           r.registerClass<Help>("SimGui::HelpCtrl", makeFourCC('S', 'G', 'h', 'c')) &&
           r.registerClass<BitmapBox>("SimGui::BitmapBox", makeFourCC('S', 'G', 'b', 'b')) &&
           r.registerClass<TestControl>("SimGui::TestControl", makeFourCC('S', 'G', 't', 'c')) &&
           r.registerClass<TextWrap>("SimGui::TextWrap", makeFourCC('S', 'G', 't', 'w')) &&
           r.registerClass<TextFormat>("SimGui::TextFormat", makeFourCC('S', 'G', 't', 'f')) &&
           r.registerClass<EditControl>("SimGui::EditControl") &&
           r.registerClass<Canvas>("SimGui::Canvas") && registerContainerClasses(r);
}
} // namespace opent1::gui
