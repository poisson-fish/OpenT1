#include <opent1/game/status_combat_huds.hpp>
#include <ranges>

#include <algorithm>
#include <cmath>
#include <format>

namespace opent1::game {

namespace {

constexpr float gCompassPoints[] = {
    //'N'
    4.0f,
    -2.0f,
    -18.0f,
    -2.0f,
    -24.0f,
    2.0f,
    -18.0f,
    2.0f,
    -24.0f,

    //'E'
    4.0f,
    18.0f,
    2.0f,
    18.0f,
    -2.0f,
    24.0f,
    -2.0f,
    24.0f,
    2.0f,
    2.0f,
    21.0f,
    -2.0f,
    21.0f,
    2.0f,

    //'W'
    5.0f,
    -24.0f,
    4.0f,
    -18.0f,
    2.0f,
    -24.0f,
    0.0f,
    -18.0f,
    -2.0f,
    -24.0f,
    -4.0f,

    //'S'
    12.0f,
    2.0f,
    20.0f,
    1.0f,
    19.0f,
    -1.0f,
    19.0f,
    -2.0f,
    20.0f,
    -2.0f,
    21.0f,
    -1.0f,
    22.0f,
    1.0f,
    22.0f,
    2.0f,
    23.0f,
    2.0f,
    24.0f,
    1.0f,
    25.0f,
    -1.0f,
    25.0f,
    -2.0f,
    24.0f,

    //4 marks
    2.0f,
    15.0f,
    -14.0f,
    19.0f,
    -18.0f,
    2.0f,
    15.0f,
    14.0f,
    19.0f,
    18.0f,
    2.0f,
    -14.0f,
    14.0f,
    -18.0f,
    18.0f,
    2.0f,
    -14.0f,
    -14.0f,
    -18.0f,
    -18.0f,

    //EOF
    -1.0f};

constexpr float lrCompassPoints[] = {
    //'N'
    4.0f,
    -1.5f,
    -10.5f,
    -1.5f,
    -13.5f,
    1.5f,
    -10.5f,
    1.5f,
    -13.5f,

    //'E'
    4.0f,
    9.5f,
    0.5f,
    9.5f,
    -1.5f,
    13.5f,
    -1.5f,
    13.5f,
    0.5f,
    2.0f,
    11.5f,
    -1.5f,
    11.5f,
    -0.5f,

    //'W'
    7.0f,
    -13.5f,
    2.5f,
    -11.5f,
    2.5f,
    -10.5f,
    1.5f,
    -11.5f,
    1.5f,
    -10.5f,
    -0.5f,
    -11.5f,
    -1.5f,
    -13.5f,
    -1.5f,

    //'S'
    6.0f,
    1.5f,
    9.5f,
    0.5f,
    9.5f,
    -0.5f,
    10.5f,
    1.5f,
    12.5f,
    0.5f,
    13.5f,
    -0.5f,
    13.5f,

    //4 marks
    2.0f,
    7.5f,
    -7.5f,
    9.5f,
    -9.5f,
    2.0f,
    7.5f,
    7.5f,
    9.5f,
    9.5f,
    2.0f,
    -7.5f,
    7.5f,
    -9.5f,
    9.5f,
    2.0f,
    -7.5f,
    -7.5f,
    -9.5f,
    -9.5f,

    //EOF
    -1.0f};

constexpr Flt32 kHalfPi = 1.5707963267948966f;

constexpr std::array<Point2I, 12> kHiResCompassPoints{{
    {0, -18},
    {0, 18},
    {-18, 0},
    {18, 0},
    {3, -15},
    {0, -24},
    {-3, -15},
    {3, -15},
    {1, -5},
    {1, 5},
    {-1, -5},
    {-1, 5},
}};

constexpr std::array<Point2I, 12> kLoResCompassPoints{{
    {0, -12},
    {0, 12},
    {-12, 0},
    {12, 0},
    {2, -10},
    {0, -16},
    {-2, -10},
    {2, -10},
    {1, -3},
    {1, 3},
    {-1, -3},
    {-1, 3},
}};

constexpr std::array<Point2F, 3> kWayPointTriangle{{
    {0.0f, -24.0f},
    {6.0f, -16.0f},
    {-6.0f, -16.0f},
}};

[[nodiscard]] Point2I
rotateVertex(Point2F pt, Flt32 cosAngle, Flt32 sinAngle, Point2I center) noexcept {
    return Point2I{center.x - static_cast<Int32>(pt.x * cosAngle - pt.y * sinAngle),
                   center.y + static_cast<Int32>(pt.x * sinAngle + pt.y * cosAngle)};
}

} // namespace

CompassHud::CompassHud() {
    fields_.extent = {69, 91};
}

CompassHud::State CompassHud::computeState(
    const Point3F& playerPos, Flt32 playerYaw, Point2F wayPoint, bool wayPointActive, bool lowRes) {
    State st{};
    st.playerPos = playerPos;
    st.playerYaw = playerYaw;
    st.wayPoint = wayPoint;
    st.wayPointActive = wayPointActive;
    st.lowRes = lowRes;

    const Point2I center = lowRes ? Point2I{21, 21} : Point2I{34, 34};
    const Flt32 cosYaw = std::cos(playerYaw);
    const Flt32 sinYaw = std::sin(playerYaw);
    const auto& srcPts = lowRes ? kLoResCompassPoints : kHiResCompassPoints;
    for (auto i : std::views::iota(std::size_t{0}, srcPts.size())) {
        st.compassVertices[i] =
            rotateVertex({static_cast<Flt32>(srcPts[i].x), static_cast<Flt32>(srcPts[i].y)},
                         cosYaw,
                         sinYaw,
                         center);
    }

    const Flt32 dx = wayPoint.x - playerPos.x;
    const Flt32 dy = wayPoint.y - playerPos.y;
    const Flt32 distF = std::sqrt(dx * dx + dy * dy);
    const Int32 dist = static_cast<Int32>(distF);
    if (!wayPointActive || dist <= 2) {
        st.distanceDisplay = 0;
        st.distanceText = "000";
    } else {
        st.distanceDisplay = std::min<Int32>(999, dist);
        st.distanceText = std::format("{}", st.distanceDisplay);
    }

    // Legacy m_atan(dx, dy) computes atan2(dy, dx).
    const Flt32 wayPointDir = std::atan2(dy, dx) - playerYaw;
    st.wayPointAngle = kHalfPi - wayPointDir;
    const Flt32 cosWp = std::cos(st.wayPointAngle);
    const Flt32 sinWp = std::sin(st.wayPointAngle);
    const Flt32 scale = lowRes ? 0.6f : 1.0f;
    for (auto i : std::views::iota(std::size_t{0}, kWayPointTriangle.size())) {
        st.arrowVertices[i] = rotateVertex(
            {kWayPointTriangle[i].x * scale, kWayPointTriangle[i].y * scale}, cosWp, sinWp, center);
    }
    return st;
}

void CompassHud::updateState(
    const Point3F& playerPos, Flt32 playerYaw, Point2F wayPoint, bool wayPointActive, bool lowRes) {
    state_ = computeState(playerPos, playerYaw, wayPoint, wayPointActive, lowRes);
    lowResMode_ = lowRes;
    resize(fields_.position, lowRes ? Point2I{43, 56} : Point2I{69, 91});
}

bool CompassHud::onAdd() {
    if (!Parent::onAdd()) {
        return false;
    }
    fields_.extent = {69, 91};
    return true;
}

bool CompassHud::draw(gfx::SoftwareSurface& s, Point2I offset, RectI clip) const {
    if (!Parent::draw(s, offset, clip) ||
        !drawSprite(s, state_.lowRes ? "lr_Compass.bmp" : "Compass.bmp", offset)) {
        return false;
    }
    const auto* points = state_.lowRes ? lrCompassPoints : gCompassPoints;
    const auto center = state_.lowRes ? 18.5f : 31.5f;
    const auto co = std::cos(state_.playerYaw), si = std::sin(state_.playerYaw);
    while (*points > 0) {
        const auto count = static_cast<int>(*points++);
        Point2I previous;
        for (auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                          return i < count;
                      })) {
            const auto x = *points++, y = *points++;
            const Point2I point{offset.x + Int32(center + x * co - y * si),
                                offset.y + Int32(center + x * si + y * co)};
            if (i) {
                s.line(previous, point, ShellPaletteColor::GREEN);
            }
            previous = point;
        }
    }
    const Point2I middle = offset + Point2I{Int32(center), Int32(center)};
    s.line(middle + Point2I{0, state_.lowRes ? -8 : -14},
           middle + Point2I{0, state_.lowRes ? -15 : -29},
           ShellPaletteColor::RED);
    if (state_.wayPointActive) {
        for (auto i : std::views::iota(std::size_t{0}, std::size_t{3})) {
            s.line(offset + state_.arrowVertices[i],
                   offset + state_.arrowVertices[(i + 1) % 3],
                   ShellPaletteColor::BLUE_HILITE);
        }
    }
    return !hudFont || s.text(*hudFont,
                              offset + (state_.lowRes ? Point2I{14, 38} : Point2I{24, 65}),
                              state_.distanceText);
}

} // namespace opent1::game
