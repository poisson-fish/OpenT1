#include <opent1/game/star_field.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <cmath>

namespace opent1::game {

StarField::StarField() {
    setType(SimStarsType);
    setNetFlags(ScopeAlways | Ghostable);
}

StarField::~StarField() = default;

void StarField::initPersistFields() {}

bool StarField::processArguments(std::span<const std::string> args) {
    return args.empty();
}

void StarField::setVisibility(bool vis) {
    if (visible_ != vis) {
        visible_ = vis;
        setMaskBits(Modified);
    }
}

void StarField::toggleVisibility() {
    setVisibility(!visible_);
}

void StarField::setBottomVisible(bool vis) {
    if (bottomVisible_ != vis) {
        bottomVisible_ = vis;
        setMaskBits(Modified);
    }
}

bool StarField::onAdd() {
    if (const auto vis = getFieldValue("visible"); vis && !vis->empty()) {
        visible_ = ((*vis)[0] == 't' || (*vis)[0] == 'T' || (*vis)[0] == '1');
    }
    if (const auto bvis = getFieldValue("bottomVisible"); bvis && !bvis->empty()) {
        bottomVisible_ = ((*bvis)[0] == 't' || (*bvis)[0] == 'T' || (*bvis)[0] == '1');
    }
    if (const auto front = getFieldValue("inFrontOfSky"); front && !front->empty()) {
        inFrontOfSky_ = ((*front)[0] == 't' || (*front)[0] == 'T' || (*front)[0] == '1');
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        mgr->assignId(*this, StarFieldId);
        addToSet(SimRenderSetId);
    }
    if (!isGhost()) {
        initialize();
    }
    return true;
}

void StarField::onRemove() {
    removeFromSet(SimRenderSetId);
    Parent::onRemove();
}

bool StarField::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

void StarField::setPalette() {
    paletteColors_ = {0, 0, 0};
}

void StarField::initialize() {
    setPalette();

    const Flt32 incStep = static_cast<Flt32>(28.0 * (kRealDPi / 180.0));
    const Flt32 azStep = static_cast<Flt32>(30.0 * (kRealDPi / 180.0));

    Flt32 incidence = static_cast<Flt32>(-5.0 * (kRealDPi / 180.0));
    std::size_t region = 0;
    for (Int32 i = 0; i < 3; ++i) {
        Flt32 azimuth = 0.0f;
        const Flt32 sinL = std::sin(incidence) * GLOBE_RADIUS;
        const Flt32 cosL = std::cos(incidence) * GLOBE_RADIUS;
        const Flt32 sinH = std::sin(incidence + incStep) * GLOBE_RADIUS;
        const Flt32 cosH = std::cos(incidence + incStep) * GLOBE_RADIUS;
        for (Int32 j = 0; j < 12; ++j) {
            regions_[region].stars.clear();
            regions_[region].corners[0].set(
                std::cos(azimuth) * cosL, std::sin(azimuth) * cosL, sinL);
            regions_[region].corners[1].set(
                std::cos(azimuth + azStep) * cosL, std::sin(azimuth + azStep) * cosL, sinL);
            regions_[region].corners[2].set(
                std::cos(azimuth + azStep) * cosH, std::sin(azimuth + azStep) * cosH, sinH);
            regions_[region].corners[3].set(
                std::cos(azimuth) * cosH, std::sin(azimuth) * cosH, sinH);
            ++region;
            azimuth += azStep;
        }
        incidence += incStep;
    }

    regions_[REGION_COUNT].stars.clear();
    Random randGen;
    for (Int32 i = 0; i < 2 * MAX_STARS; ++i) {
        Point3F p{};
        do {
            // Preserve x86 __thiscall Point3F(x, y, z) argument evaluation order (z, y, x)
            const Flt32 z =
                (i < MAX_STARS) ? randGen.getFloat(0.0f, 1.0f) : randGen.getFloat(-1.0f, 0.0f);
            const Flt32 y = randGen.getFloat(-1.0f, 1.0f);
            const Flt32 x = randGen.getFloat(-1.0f, 1.0f);
            p.set(x, y, z);
        } while (p.lenSq() > 1.0f);

        p.normalize(GLOBE_RADIUS);
        const Flt32 inc = static_cast<Flt32>(std::asin(static_cast<RealD>(p.z / GLOBE_RADIUS)) *
                                             (180.0 / kRealDPi));
        if (inc >= -5.0f) {
            Flt32 az = static_cast<Flt32>(
                std::atan2(static_cast<RealD>(p.y), static_cast<RealD>(p.x)) * (180.0 / kRealDPi));
            if (az < 0.0f) {
                az += 360.0f;
            }
            const Int32 incIndex = static_cast<Int32>((inc + 5.0f) / 28.0f);
            const Int32 azIndex = static_cast<Int32>(az / 30.0f);
            const Int32 regIdx = incIndex * 12 + azIndex;
            if (regIdx >= 0 && regIdx < REGION_COUNT) {
                const Flt32 b = static_cast<Flt32>(i % MAX_STARS) / static_cast<Flt32>(MAX_STARS);
                regions_[static_cast<std::size_t>(regIdx)].stars.push_back(Star{p, b});
            }
        } else if (inc <= -45.0f) {
            const Flt32 b = static_cast<Flt32>(i % MAX_STARS) / static_cast<Flt32>(MAX_STARS);
            regions_[REGION_COUNT].stars.push_back(Star{p, b});
        }
    }
    initialized_ = true;
}

bool StarField::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_STARS_VISIBLE, visible_);
    inspector.write(IDITG_STARS_BOTTOM_VISIBLE, bottomVisible_);
    inspector.write(IDITG_STARS_FRONT_OF_SKY, inFrontOfSky_);
    inspector.write(IDITG_STARS_COLOR1, Point3F{colors_[0].red, colors_[0].green, colors_[0].blue});
    inspector.write(IDITG_STARS_COLOR2, Point3F{colors_[1].red, colors_[1].green, colors_[1].blue});
    inspector.write(IDITG_STARS_COLOR3, Point3F{colors_[2].red, colors_[2].green, colors_[2].blue});
    return true;
}

bool StarField::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_STARS_VISIBLE, v)) {
        return false;
    }
    visible_ = std::get<bool>(v);

    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_STARS_BOTTOM_VISIBLE, v)) {
        return false;
    }
    bottomVisible_ = std::get<bool>(v);

    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_STARS_FRONT_OF_SKY, v)) {
        return false;
    }
    inFrontOfSky_ = std::get<bool>(v);

    for (std::size_t i = 0; i < 3; ++i) {
        if (!inspector.read(
                sim::Inspect::Type::Point3F, static_cast<Int32>(IDITG_STARS_COLOR1 + i), v)) {
            return false;
        }
        const auto c = std::get<Point3F>(v);
        colors_[i].set(c.x, c.y, c.z);
    }
    setMaskBits(Modified);
    initialized_ = false;
    initialize();
    return true;
}

UInt32 StarField::packUpdate(sim::network::GhostManager& /*gm*/,
                             UInt32 /*mask*/,
                             BitStream& stream) const {
    stream.writeFlag(visible_);
    stream.writeFlag(bottomVisible_);
    stream.writeBytes(std::as_bytes(std::span(colors_)));
    stream.writeFlag(inFrontOfSky_);
    return 0;
}

bool StarField::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    const bool newVisible = stream.readFlag();
    const bool newBottomVisible = stream.readFlag();
    std::array<ColorF, 3> newColors{};
    if (!stream.readBytes(std::as_writable_bytes(std::span(newColors)))) {
        stream.setCurPos(savedPos);
        return false;
    }
    const bool newInFront = stream.readFlag();
    if (!stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }
    visible_ = newVisible;
    bottomVisible_ = newBottomVisible;
    colors_ = newColors;
    inFrontOfSky_ = newInFront;
    initialized_ = false;
    initialize();
    return true;
}

Persistent::Error
StarField::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    const auto savedPos = sio.getPosition();
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    bool newInFront = true;
    if (version >= 2) {
        if (!sio.read(newInFront)) {
            sio.setPosition(savedPos);
            return Persistent::Error::ReadError;
        }
    }
    std::array<ColorF, 3> newColors{};
    if (!sio.readBytes(std::as_writable_bytes(std::span(newColors)))) {
        sio.setPosition(savedPos);
        return Persistent::Error::ReadError;
    }
    inFrontOfSky_ = newInFront;
    colors_ = newColors;
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
StarField::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    sio.write(inFrontOfSky_);
    sio.writeBytes(std::as_bytes(std::span(colors_)));
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
