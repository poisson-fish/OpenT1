#include <opent1/game/shell_controls.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace opent1::game {

void FGSlider::wake() {
    Parent::wake();
    const auto saved = scriptValue();
    thumbWidth = 30;
    const int left = bitmaps_ && bitmaps_->get(2) ? bitmaps_->get(2)->width : 14;
    const int right = bitmaps_ && bitmaps_->get(5) ? bitmaps_->get(5)->width : 14;
    setThumbRange(left, std::max(left, fields_.extent.x - right - thumbWidth));
    setScriptValue(saved);
}

bool FGSlider::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    if (!bitmaps_ || bitmaps_->bitmaps.size() < 14) {
        return Parent::draw(s, p, {});
    }
    const auto& b = bitmaps_->bitmaps;
    const bool enabled = active && (!canvas() || canvas()->contains(this));
    const int sel = enabled && depressed ? 1 : 0;
    const auto part = [&](int i, int x, int w) {
        return w <= 0 || s.stretch(b[i], {p.x + x, p.y, p.x + x + w - 1, p.y + b[i].height - 1});
    };
    return part(enabled ? 2 : 0, 0, b[2].width) &&
           part(enabled ? 5 : 3, fields_.extent.x - b[5].width, b[5].width) &&
           part(enabled ? 6 : 7, minThumbX_, fields_.extent.x - minThumbX_ - b[5].width) &&
           part(8 + sel, curThumbX_, b[8 + sel].width) &&
           part(10 + sel,
                curThumbX_ + b[8 + sel].width,
                thumbWidth - b[8 + sel].width - b[12 + sel].width) &&
           part(12 + sel, curThumbX_ + thumbWidth - b[12 + sel].width, b[12 + sel].width);
}

bool FGSlider::event(const gui::Event& e) {
    if (!active) {
        return false;
    }
    const auto x = globalToLocal(e.point).x;
    const auto move = [&](int position) {
        curThumbX_ = std::clamp(position, minThumbX_, maxThumbX_);
        const auto v = scriptValue();
        setScriptValue(v);
        action();
    };
    if (e.type == gui::EventType::Cancel) {
        dragging_ = false;
        return gui::Active::event(e);
    }
    if (e.type == gui::EventType::Down && e.button == 1) {
        if (!canvas() || !canvas()->lockMouse(this)) {
            return false;
        }
        canvas()->makeFirstResponder(this);
        depressed = true;
        dragging_ = x >= curThumbX_ && x < curThumbX_ + thumbWidth;
        dragOffset_ = x - curThumbX_;
        if (!dragging_) {
            const int step = numDiscreteValues_ > 1
                                 ? std::max(1, (maxThumbX_ - minThumbX_) / (numDiscreteValues_ - 1))
                                 : std::max(1, (maxThumbX_ - minThumbX_) / 10);
            move(curThumbX_ + (x < curThumbX_ ? -step : step));
        }
        return true;
    }
    if (e.type == gui::EventType::Drag && canvas() && canvas()->capture() == this) {
        if (dragging_) {
            move(x - dragOffset_);
        }
        return true;
    }
    if (e.type == gui::EventType::Up && e.button == 1) {
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        dragging_ = false;
        depressed = false;
        return true;
    }
    if (e.type == gui::EventType::KeyDown &&
        (e.key == gui::Key::Left || e.key == gui::Key::Right)) {
        const int step = numDiscreteValues_ > 1
                             ? std::max(1, (maxThumbX_ - minThumbX_) / (numDiscreteValues_ - 1))
                             : std::max(1, (maxThumbX_ - minThumbX_) / 20);
        move(curThumbX_ + (e.key == gui::Key::Left ? -step : step));
        return true;
    }
    return gui::Active::event(e);
}

void FGSlider::setDiscretePositions(Int32 count) noexcept {
    numDiscreteValues_ = count < 2 ? 0 : count;
    if (numDiscreteValues_ <= 1) {
        low = 0.0f;
        high = 1.0f;
    }
    setScriptValue("0");
}

std::string FGSlider::scriptValue() const {
    if (numDiscreteValues_ > 1) {
        const Flt32 pageValue = static_cast<Flt32>(maxThumbX_ - minThumbX_) /
                                static_cast<Flt32>(numDiscreteValues_ - 1);
        Int32 curPage =
            pageValue > 0.0f
                ? static_cast<Int32>(static_cast<Flt32>(curThumbX_ - minThumbX_) / pageValue)
                : 0;
        if ((curThumbX_ - minThumbX_) -
                static_cast<Int32>(static_cast<Flt32>(curPage) * pageValue) >=
            static_cast<Int32>(pageValue / 2.0f)) {
            ++curPage;
        }
        curPage = std::clamp<Int32>(curPage, 0, numDiscreteValues_ - 1);
        return std::format("{}", curPage);
    }
    const Flt32 span = static_cast<Flt32>(std::max<Int32>(1, maxThumbX_ - minThumbX_));
    const Flt32 val = low + ((static_cast<Flt32>(curThumbX_ - minThumbX_) / span) * (high - low));
    return std::format("{:.6f}", val);
}

bool FGSlider::setScriptValue(std::string_view val) {
    Flt32 parsed = 0.0f;
    const auto res = std::from_chars(val.data(), val.data() + val.size(), parsed);
    if (res.ec != std::errc{} || res.ptr != val.data() + val.size() || !std::isfinite(parsed)) {
        return false;
    }
    if (numDiscreteValues_ > 1) {
        const Flt32 pageValue = static_cast<Flt32>(maxThumbX_ - minThumbX_) /
                                static_cast<Flt32>(numDiscreteValues_ - 1);
        const Int32 curPage =
            std::clamp<Int32>(static_cast<Int32>(parsed), 0, numDiscreteValues_ - 1);
        curThumbX_ = std::clamp<Int32>(
            minThumbX_ + static_cast<Int32>(static_cast<Flt32>(curPage) * pageValue),
            minThumbX_,
            maxThumbX_);
        value = static_cast<float>(curPage) / (numDiscreteValues_ - 1);
        return true;
    }
    const Flt32 newValue = std::clamp(parsed, low, high);
    const Flt32 range = std::max(0.0001f, high - low);
    const Int32 newThumbX =
        minThumbX_ + static_cast<Int32>(((newValue - low) / range) *
                                        static_cast<Flt32>(maxThumbX_ - minThumbX_));
    curThumbX_ = std::clamp<Int32>(newThumbX, minThumbX_, maxThumbX_);
    return setValue(newValue);
}

std::function<void()> FGSlider::checkpoint() {
    auto base = Parent::checkpoint();
    const auto nd = numDiscreteValues_;
    const auto minT = minThumbX_;
    const auto maxT = maxThumbX_;
    const auto curT = curThumbX_;
    const auto bitmaps = bitmaps_;
    const auto dragging = dragging_;
    const auto dragOffset = dragOffset_;
    return [this, base = std::move(base), nd, minT, maxT, curT, bitmaps, dragging, dragOffset] {
        base();
        numDiscreteValues_ = nd;
        minThumbX_ = minT;
        maxThumbX_ = maxT;
        curThumbX_ = curT;
        bitmaps_ = bitmaps;
        dragging_ = dragging;
        dragOffset_ = dragOffset;
    };
}

Persistent::Error
FGSlider::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    Int32 curVer{};
    Int32 numDisc{};
    if (!sio.read(curVer) || curVer != ControlVersion || !sio.read(numDisc) || numDisc < 0 ||
        numDisc > 65536) {
        return Persistent::Error::ReadError;
    }
    const auto err = Parent::read(sio, version, user, ctx);
    if (err != Persistent::Error::Ok) {
        return err;
    }
    numDiscreteValues_ = numDisc;
    if (numDiscreteValues_ <= 1) {
        low = 0.0f;
        high = 1.0f;
        value = std::clamp(value, low, high);
    }
    return Persistent::Error::Ok;
}

Persistent::Error
FGSlider::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (numDiscreteValues_ < 0 || numDiscreteValues_ > 65536 || !sio.write(ControlVersion) ||
        !sio.write(numDiscreteValues_)) {
        return Persistent::Error::WriteError;
    }
    return Parent::write(sio, version, user, ctx);
}

} // namespace opent1::game
