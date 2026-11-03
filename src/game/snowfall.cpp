#include <opent1/game/snowfall.hpp>
#include <ranges>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace opent1::game {

namespace {

[[nodiscard]] Flt32 parseFloatArg(std::string_view arg) noexcept {
    Flt32 val = 0.0f;
    std::from_chars(arg.data(), arg.data() + arg.size(), val);
    return val;
}

[[nodiscard]] Int32 parseIntArg(std::string_view arg) noexcept {
    Int32 val = 0;
    std::from_chars(arg.data(), arg.data() + arg.size(), val);
    return val;
}

} // namespace

Snowfall::Snowfall() {
    setId(SnowfallId);
    setType(SimDefaultObjectType);
    setNetFlags(ScopeAlways | Ghostable);
}

Snowfall::~Snowfall() = default;

void Snowfall::initPersistFields() {}

UInt32 Snowfall::fastRand() noexcept {
    seed_ = seed_ * 5U;
    seed_ += (seed_ >> 13U) + 1U;
    return seed_;
}

void Snowfall::setRendering(bool r) {
    suspendRendering_ = !r;
    setMaskBits(updateRendering);
}

void Snowfall::setWind(const Point3F& w) {
    wind_ = w;
    setMaskBits(updateWind);
}

void Snowfall::setWind(const Point2F& w) {
    wind_.set(w.x, w.y, rain_ ? -75.0f : -35.0f);
    setMaskBits(updateWind);
}

void Snowfall::setIntensity(Flt32 i) {
    if (i > 1.0f) {
        i = 1.0f;
    } else if (i < 0.0f) {
        i = 0.0f;
    }
    intensity_ = i;
    setMaskBits(updateIntensity);
}

bool Snowfall::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return true;
    }
    setIntensity(parseFloatArg(args[0]));
    if (args.size() <= 4) {
        // Native newObject arguments are intensity, wind-x, wind-y, rain.
        // Reading the rain flag as wind-z makes RPG rain drift upward.
        setRain(args.size() >= 4 && parseIntArg(args[3]) != 0);
        setWind(Point2F{args.size() >= 2 ? parseFloatArg(args[1]) : 0.0f,
                        args.size() >= 3 ? parseFloatArg(args[2]) : 0.0f});
        setRendering(intensity_ > 0.0f);
    } else {
        // Preserve the extended XYZ, suspension, optional rain form.
        setWind(Point3F{parseFloatArg(args[1]), parseFloatArg(args[2]), parseFloatArg(args[3])});
        setRendering(parseIntArg(args[4]) == 0);
        if (args.size() >= 6) {
            setRain(parseIntArg(args[5]) != 0);
        }
    }
    // newObject calls onAdd before processArguments; replace its default flakes.
    snowInit();
    return true;
}

bool Snowfall::onAdd() {
    if (const auto inten = getFieldValue("intensity"); inten && !inten->empty()) {
        setIntensity(parseFloatArg(*inten));
    }
    if (const auto rainStr = getFieldValue("rain"); rainStr && !rainStr->empty()) {
        rain_ = ((*rainStr)[0] == 't' || (*rainStr)[0] == 'T' || (*rainStr)[0] == '1');
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        mgr->assignId(*this, SnowfallId);
        addToSet(SimRenderSetId);
        addToSet(SimFrameEndNotifySetId);
    }
    snowInit();
    return true;
}

bool Snowfall::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool Snowfall::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

void Snowfall::snowInit(Flt32 currentTime) {
    flakes_.fill(SnowFlake{});
    Flt32 maxFlakes = rain_ ? 2048.0f : 1024.0f;
    if (precipPref >= 0.0f && precipPref <= 1.0f) {
        maxFlakes *= precipPref;
    }
    numFlakes_ =
        std::clamp(static_cast<Int32>(maxFlakes * intensity_), 0, static_cast<Int32>(MAXFLAKES));
    for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                      return x < numFlakes_;
                  })) {
        snowCreateFlake(x, true, currentTime);
    }
}

void Snowfall::snowCreateFlake(Int32 index, bool startRandom, Flt32 currentTime) {
    if (index < 0 || index >= MAXFLAKES) {
        return;
    }
    SnowFlake& flake = flakes_[static_cast<std::size_t>(index)];

    Point3F vel(camVel_.x, camVel_.y, 0.0f);
    const Flt32 len = vel.lenf();
    if (len > 40.0f) {
        vel *= 40.0f / len;
    }
    vel += camPos_;

    flake.pos.x = static_cast<Flt32>(static_cast<Int32>(
                      (fastRand() >> 12U) % static_cast<UInt32>(SnowFocus_.x * 20.0f))) /
                      10.0f -
                  SnowFocus_.x + vel.x;
    flake.pos.y = static_cast<Flt32>(static_cast<Int32>(
                      (fastRand() >> 12U) % static_cast<UInt32>(SnowFocus_.y * 20.0f))) /
                      10.0f -
                  SnowFocus_.y + vel.y;
    if (startRandom) {
        flake.pos.z = static_cast<Flt32>(static_cast<Int32>(
                          (fastRand() >> 12U) % static_cast<UInt32>(SnowFocus_.z * 20.0f))) /
                          10.0f -
                      SnowFocus_.z + vel.z;
    } else {
        flake.pos.z = SnowFocus_.z + vel.z -
                      static_cast<Flt32>(static_cast<Int32>((fastRand() >> 12U) % 50U)) / 10.0f;
    }

    if (flake.pos.x - camPos_.x > -nearDist_ && flake.pos.x - camPos_.x < nearDist_ &&
        flake.pos.y - camPos_.y > -nearDist_ && flake.pos.y - camPos_.y < nearDist_) {
        flake.pos.x += nearDist_ * 2.0f;
        flake.pos.y += nearDist_ * 2.0f;
    }

    if (rain_) {
        // Native rain follows one wind vector at twice the configured speed.
        flake.vel = wind_ * 2.0f;
    } else {
        flake.vel.x =
            static_cast<Flt32>(static_cast<Int32>((fastRand() >> 12U) % 40U) - 20) / 10.0f +
            wind_.x;
        flake.vel.y =
            static_cast<Flt32>(static_cast<Int32>((fastRand() >> 12U) % 40U) - 20) / 10.0f +
            wind_.y;
        flake.vel.z =
            (-static_cast<Flt32>(static_cast<Int32>((fastRand() >> 12U) % 20U)) / 10.0f) + wind_.z;
        flake.vel *= 0.25f;
    }
    // Preserve the lifetime at the former 32 ms step without counting render frames.
    flake.timeout = static_cast<Flt32>(50U + (fastRand() >> 12U) % 100U) * 0.032f;
    flake.flags = FlakeActive | FlakeOutside;
    flake.nextLookupTime = currentTime + (static_cast<Flt32>(fastRand() & 0xFFU) / 512.0f);
    flake.size = static_cast<Int32>((fastRand() >> 13U) & 0x01U);
}

void Snowfall::snowLogicFrame(SimTime dt, Flt32 currentTime) {
    if (!std::isfinite(dt) || dt <= 0.0f) {
        return;
    }
    Flt32 maxFlakes = rain_ ? 2048.0f : 1024.0f;
    if (precipPref >= 0.0f && precipPref <= 1.0f) {
        maxFlakes *= precipPref;
    }
    numFlakes_ =
        std::clamp(static_cast<Int32>(maxFlakes * intensity_), 0, static_cast<Int32>(MAXFLAKES));

    const Point3F maxBox = camPos_ + SnowBox_;
    const Point3F minBox = camPos_ - SnowBox_;

    for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                      return x < numFlakes_;
                  })) {
        SnowFlake& flake = flakes_[static_cast<std::size_t>(x)];
        if ((flake.flags & FlakeActive) != 0) {
            if (rain_) {
                flake.vel = wind_ * 2.0f;
            }
            flake.pos += flake.vel * dt;
            if (flake.pos.z < minBox.z) {
                flake.pos.z += SnowBox_.z * 2.0f;
            }
            if (flake.pos.y < minBox.y) {
                flake.pos.y += SnowBox_.y * 2.0f;
            }
            if (flake.pos.x < minBox.x) {
                flake.pos.x += SnowBox_.x * 2.0f;
            }
            if (flake.pos.z > maxBox.z) {
                flake.pos.z -= SnowBox_.z * 2.0f;
            }
            if (flake.pos.y > maxBox.y) {
                flake.pos.y -= SnowBox_.y * 2.0f;
            }
            if (flake.pos.x > maxBox.x) {
                flake.pos.x -= SnowBox_.x * 2.0f;
            }
        }
        flake.timeout -= dt;
        if (flake.timeout <= 0) {
            snowCreateFlake(x, false, currentTime);
        }
    }
}

bool Snowfall::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_SNOW_INTENSITY, intensity_);
    inspector.write(IDITG_SNOW_WIND, wind_);
    inspector.write(IDITG_SNOW_SUSPEND_RENDER, suspendRendering_);
    inspector.write(IDITG_SNOW_RAIN, rain_);
    return true;
}

bool Snowfall::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_SNOW_INTENSITY, v)) {
        return false;
    }
    const Flt32 newIntensity = std::get<Flt32>(v);

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_SNOW_WIND, v)) {
        return false;
    }
    wind_ = std::get<Point3F>(v);

    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_SNOW_SUSPEND_RENDER, v)) {
        return false;
    }
    suspendRendering_ = std::get<bool>(v);

    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_SNOW_RAIN, v)) {
        return false;
    }
    rain_ = std::get<bool>(v);

    setIntensity(newIntensity);
    setMaskBits(updateIntensity | updateWind | updateRendering);
    snowInit();
    return true;
}

UInt32
Snowfall::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 mask, BitStream& stream) const {
    if (stream.writeFlag((mask & updateIntensity) != 0)) {
        stream.write(intensity_);
    }
    if (stream.writeFlag((mask & updateWind) != 0)) {
        stream.writeBytes(std::as_bytes(std::span(&wind_, 1)));
    }
    stream.writeFlag(suspendRendering_);
    stream.writeFlag(rain_);
    return 0;
}

bool Snowfall::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    Flt32 newIntensity = intensity_;
    Point3F newWind = wind_;
    if (stream.readFlag()) {
        if (!stream.read(newIntensity)) {
            stream.setCurPos(savedPos);
            return false;
        }
    }
    if (stream.readFlag()) {
        if (!stream.readBytes(std::as_writable_bytes(std::span(&newWind, 1)))) {
            stream.setCurPos(savedPos);
            return false;
        }
    }
    const bool newSuspend = stream.readFlag();
    const bool newRain = stream.readFlag();
    if (!stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }
    const bool wasRain = rain_;
    intensity_ = newIntensity;
    wind_ = newWind;
    suspendRendering_ = newSuspend;
    rain_ = newRain;
    if (wasRain != rain_ || numFlakes_ == 0) {
        snowInit();
    }
    return true;
}

Persistent::Error
Snowfall::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const auto savedPos = sio.getPosition();
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    Flt32 newIntensity = 0.0f;
    Point3F newWind{};
    bool newSuspend = false;
    bool newRain = false;
    if (!sio.read(newIntensity) || !sio.readBytes(std::as_writable_bytes(std::span(&newWind, 1))) ||
        !sio.read(newSuspend) || !sio.read(newRain)) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    intensity_ = newIntensity;
    wind_ = newWind;
    suspendRendering_ = newSuspend;
    rain_ = newRain;
    snowInit();
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
Snowfall::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    sio.write(intensity_);
    sio.writeBytes(std::as_bytes(std::span(&wind_, 1)));
    sio.write(suspendRendering_);
    sio.write(rain_);
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
