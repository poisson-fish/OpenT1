#include <opent1/game/status_combat_huds.hpp>

#include <cmath>
#include <format>

namespace opent1::game {

ClockHud::ClockHud() {
    fields_.extent = {68, 25};
}

std::string ClockHud::formatTime(Flt32 clockSeconds) {
    const Flt32 curTime = std::fabs(clockSeconds);
    const Int32 totalSecs = static_cast<Int32>(curTime);
    const Int32 hours = totalSecs / 3600;
    const Int32 mins = (totalSecs % 3600) / 60;
    const Int32 secs = totalSecs % 60;
    const Int32 tenths = static_cast<Int32>((curTime - static_cast<Flt32>(totalSecs)) * 10.0f);
    return std::format("{:02d}:{:02d}:{:02d}.{:1d}", hours, mins, secs, tenths);
}

void ClockHud::setClockTime(Flt32 seconds) {
    clockSeconds_ = seconds;
    formattedText_ = formatTime(seconds);
}

bool ClockHud::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    fields_.extent = {68, 25};
    return true;
}

bool ClockHud::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip)) {
        return false;
    }
    return !hudFont || s.text(*hudFont, offset + Point2I{4, 0}, formattedText_);
}

} // namespace opent1::game
