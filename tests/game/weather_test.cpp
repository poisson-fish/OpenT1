#include "test_registry.hpp"
#include <ranges>
#include "test_support.hpp"
#include <opent1/game/game_app.hpp>
#include <opent1/game/snowfall.hpp>

namespace {
using namespace opent1;
using namespace opent1::game;

bool near(const Point3F& a, const Point3F& b) {
    return (a - b).lenf() < .002f;
}

void weatherArguments() {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createClientWorld());
    CHECK(app.fearPlugin().focusClient());
    // RPG ChangeWeather passes a vector string as wind-x, then wind-y and rain.
    CHECK(app.console()
              .evaluate(R"(newObject("weather", Snowfall, "0.4", "-0.5 0.25 -280", 0, 1);)")
              .ok());
    const auto* rain =
        dynamic_cast<Snowfall*>(app.fearPlugin().clientGlobals().manager->findObject("weather"));
    CHECK(rain && rain->isRain() && !rain->isRenderingSuspended());
    CHECK(near(rain->getWind(), {-.5f, 0, -75}));
    CHECK(rain->numFlakes() == 819);
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < rain->numFlakes();
                  }))
        CHECK(near(rain->flakes()[i].vel, {-1, 0, -150}));

    Snowfall snow;
    const std::array<std::string, 4> nativeSnow{"0.5", "2", "-3", "0"};
    CHECK(snow.processArguments(nativeSnow));
    CHECK(!snow.isRain() && near(snow.getWind(), {2, -3, -35}));
    CHECK(snow.numFlakes() == 512);
    // Keep the existing extended XYZ/suspension/type form available to callers.
    const std::array<std::string, 6> extendedSnow{"0.25", "4", "-2", "-30", "1", "0"};
    CHECK(snow.processArguments(extendedSnow));
    CHECK(!snow.isRain() && snow.isRenderingSuspended());
    CHECK(near(snow.getWind(), {4, -2, -30}) && snow.numFlakes() == 256);
}

void rainMotion() {
    Snowfall rain;
    rain.setWind(Point2F{2, -3});
    CHECK(near(rain.getWind(), {2, -3, -75}));
    rain.setIntensity(.01f);
    rain.snowInit();
    const auto before = rain.flakes();
    rain.snowLogicFrame(.1f);
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < rain.numFlakes();
                  })) {
        CHECK(near(rain.flakes()[i].vel, {4, -6, -150}));
        CHECK(near(rain.flakes()[i].pos - before[i].pos, {.4f, -.6f, -15}));
    }
    // Wind changes affect existing drops as well as newly spawned ones.
    rain.setWind(Point2F{-1, 0});
    const auto changed = rain.flakes()[0].pos;
    rain.snowLogicFrame(.1f);
    CHECK(near(rain.flakes()[0].pos - changed, {-.2f, 0, -15}));
}

struct WeatherSample {
    std::vector<Snowfall::SnowFlake> flakes;
    Int32 count;
    UInt32 seed;
};

WeatherSample sampleClientWeather(Int32 stepMs) {
    FearGameApp app;
    CHECK(app.init());
    CHECK(app.fearPlugin().createClientWorld());
    CHECK(app.fearPlugin().focusClient());
    CHECK(app.stepFrame(0, false));
    CHECK(app.console()
              .evaluate(R"(newObject("weather", Snowfall, "0.02", "0", "0", "-15", "0", "0");)")
              .ok());
    const auto* snow =
        dynamic_cast<Snowfall*>(app.fearPlugin().clientGlobals().manager->findObject("weather"));
    CHECK(snow && snow->numFlakes() > 0);
    const auto initial = snow->flakes()[0].pos;
    // Rendering/stepping again at the same time must not advance precipitation.
    CHECK(app.stepFrame(0, false));
    CHECK(snow->flakes()[0].pos == initial);
    for (Int32 ms = stepMs; ms <= 1024; ms += stepMs)
        CHECK(app.stepFrame(double(ms) / 1000, false));
    return {{snow->flakes().begin(), snow->flakes().begin() + snow->numFlakes()},
            snow->numFlakes(),
            snow->seed()};
}

void weatherFrameRate() {
    const auto slow = sampleClientWeather(32);
    const auto fast = sampleClientWeather(8);
    CHECK(slow.count == fast.count && slow.seed == fast.seed);
    for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                      return i < slow.count;
                  })) {
        CHECK(near(slow.flakes[i].pos, fast.flakes[i].pos));
        CHECK(std::abs(slow.flakes[i].timeout - fast.flakes[i].timeout) < .001f);
    }
}
}

int opent1_test_weather_arguments() {
    return opent1::test::run(weatherArguments);
}

int opent1_test_rain_motion() {
    return opent1::test::run(rainMotion);
}

int opent1_test_weather_frame_rate() {
    return opent1::test::run(weatherFrameRate);
}

OPENT1_REGISTER_TEST("game", "weather_arguments", opent1_test_weather_arguments);
OPENT1_REGISTER_TEST("game", "rain_motion", opent1_test_rain_motion);
OPENT1_REGISTER_TEST("game", "weather_frame_rate", opent1_test_weather_frame_rate);
