#include <opent1/game/planet.hpp>
#include <opent1/game/string_table.hpp>
#include <opent1/math/trig.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace opent1::game {

namespace {
bool parseColorField(std::string_view text, ColorF& out) noexcept {
    std::istringstream iss{std::string(text)};
    Flt32 r = 0.0f;
    Flt32 g = 0.0f;
    Flt32 b = 0.0f;
    if (!(iss >> r >> g >> b)) {
        return false;
    }
    out.set(r, g, b);
    return true;
}

[[nodiscard]] bool parseBoolArg(std::string_view arg) noexcept {
    if (arg.empty()) {
        return false;
    }
    if (arg[0] == 't' || arg[0] == 'T' || arg[0] == '1') {
        return true;
    }
    if (arg[0] == 'f' || arg[0] == 'F' || arg[0] == '0') {
        return false;
    }
    Int32 val = 0;
    std::from_chars(arg.data(), arg.data() + arg.size(), val);
    return val != 0;
}

[[nodiscard]] Flt32 parseFloatArg(std::string_view arg) noexcept {
    Flt32 val = 0.0f;
    std::from_chars(arg.data(), arg.data() + arg.size(), val);
    return val;
}

} // namespace

void PlanetRenderImage::initialize(Flt32 az,
                                   Flt32 inc,
                                   Flt32 sz,
                                   Flt32 dist,
                                   bool lensFlare,
                                   bool shadows,
                                   const ColorF& intens,
                                   const ColorF& amb) {
    size = sz;
    distance = dist;
    useLensFlare = lensFlare;
    castShadows = shadows;
    intensity = intens;
    ambient = amb;
    setPosition(az, inc);
}

void PlanetRenderImage::setPosition(Flt32 _azimuth, Flt32 _incidence) {
    azimuth = _azimuth;
    incidence = _incidence;
    if (incidence > 89.0f) {
        incidence = 89.0f;
    }
    if (incidence < -89.0f) {
        incidence = -89.0f;
    }

    constexpr RealD kDegRad = kRealDPi / static_cast<RealD>(180.0);
    const Flt32 az = azimuth + 90.0f;
    const Flt32 incRad = static_cast<Flt32>(kDegRad * static_cast<RealD>(incidence));
    const Flt32 azRad = static_cast<Flt32>(kDegRad * static_cast<RealD>(az));
    const Flt32 c = distance * m_cos(incRad);
    position = Point3F(c * m_cos(azRad), c * m_sin(azRad), distance * m_sin(incRad));

    Point3F direction = position;
    direction.normalize();
    direction *= -1.0f;
    lightDirection = direction;
}

void PlanetRenderImage::setTexture(std::string_view fname) {
    if (fname.empty() || fname == "0" || fname == "none") {
        fileName.clear();
    } else {
        const std::size_t len = std::min<std::size_t>(fname.size(), 255);
        fileName.assign(fname.data(), len);
    }
}

Planet::Planet() {
    setType(SimPlanetType);
    setNetFlags(ScopeAlways | Ghostable);
}

Planet::~Planet() {
    unload();
}

void Planet::initPersistFields() {}

bool Planet::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return true;
    }
    if (args.size() >= 1) {
        planet_.setTexture(args[0]);
    }
    if (args.size() >= 2) {
        planet_.azimuth = parseFloatArg(args[1]);
    }
    if (args.size() >= 3) {
        planet_.incidence = parseFloatArg(args[2]);
    }
    if (args.size() >= 4) {
        planet_.castShadows = parseBoolArg(args[3]);
    }
    if (args.size() >= 5) {
        planet_.useLensFlare = parseBoolArg(args[4]);
    }
    if (args.size() >= 8) {
        planet_.intensity.set(
            parseFloatArg(args[5]), parseFloatArg(args[6]), parseFloatArg(args[7]));
    }
    if (args.size() >= 11) {
        planet_.ambient.set(
            parseFloatArg(args[8]), parseFloatArg(args[9]), parseFloatArg(args[10]));
    }
    if (args.size() >= 12) {
        planet_.size = parseFloatArg(args[11]);
    }
    if (args.size() >= 13) {
        const Flt32 dist = parseFloatArg(args[12]);
        if (dist != 0.0f) {
            planet_.distance = dist;
        }
    }
    load();
    return true;
}

void Planet::set(std::string_view fileName,
                 Flt32 azimuth,
                 Flt32 incidence,
                 bool castShadows,
                 bool useLensFlare,
                 ColorF intensity,
                 ColorF ambient,
                 Flt32 size,
                 Flt32 distance) {
    planet_.setTexture(fileName);
    planet_.azimuth = azimuth;
    planet_.incidence = incidence;
    planet_.castShadows = castShadows;
    planet_.useLensFlare = useLensFlare;
    planet_.intensity = intensity;
    planet_.ambient = ambient;
    planet_.size = size;
    if (distance != 0.0f) {
        planet_.distance = distance;
    }
}

void Planet::load() {
    unload();
    planet_.initialize(planet_.azimuth,
                       planet_.incidence,
                       planet_.size,
                       planet_.distance,
                       planet_.useLensFlare,
                       planet_.castShadows,
                       planet_.intensity,
                       planet_.ambient);
    if (planet_.useLensFlare && !lensFlare_.isInitialized()) {
        lensFlare_.initialize(getManager());
    }
    if (planet_.castShadows) {
        shadows = true;
        shadowDirection = planet_.lightDirection;
    }
    const bool emitsLight = (planet_.intensity.red != 0.0f || planet_.intensity.green != 0.0f ||
                             planet_.intensity.blue != 0.0f) ||
                            (planet_.ambient.red != 0.0f || planet_.ambient.green != 0.0f ||
                             planet_.ambient.blue != 0.0f);
    if (emitsLight) {
        light_.type = sim::SimLight::Type::DirectionalWrap;
        light_.intensity = planet_.intensity;
        light_.direction = planet_.lightDirection;
        if (getManager() != nullptr) {
            addToSet(SimLightSetId);
        }
        inLightSet_ = true;
    }
    setMaskBits(Modified);
}

void Planet::unload() {
    if (planet_.castShadows) {
        shadows = false;
    }
    if (inLightSet_) {
        if (getManager() != nullptr) {
            removeFromSet(SimLightSetId);
        }
        inLightSet_ = false;
    }
}

bool Planet::onAdd() {
    if (const auto fn = getFieldValue("fileName"); fn && !fn->empty()) {
        planet_.setTexture(*fn);
    }
    if (const auto az = getFieldValue("azimuth"); az && !az->empty()) {
        planet_.azimuth = parseFloatArg(*az);
    }
    if (const auto inc = getFieldValue("incidence"); inc && !inc->empty()) {
        planet_.incidence = parseFloatArg(*inc);
    }
    if (const auto sz = getFieldValue("size"); sz && !sz->empty()) {
        planet_.size = parseFloatArg(*sz);
    }
    if (const auto dist = getFieldValue("distance"); dist && !dist->empty()) {
        const Flt32 d = parseFloatArg(*dist);
        if (d > 0.0f) {
            planet_.distance = d;
        }
    }
    if (const auto cs = getFieldValue("castShadows"); cs && !cs->empty()) {
        planet_.castShadows = parseBoolArg(*cs);
    }
    if (const auto lf = getFieldValue("useLensFlare"); lf && !lf->empty()) {
        planet_.useLensFlare = parseBoolArg(*lf);
    }
    if (const auto inten = getFieldValue("intensity"); inten && !inten->empty()) {
        ColorF col{};
        if (parseColorField(*inten, col)) {
            planet_.intensity = col;
        }
    }
    if (const auto amb = getFieldValue("ambient"); amb && !amb->empty()) {
        ColorF col{};
        if (parseColorField(*amb, col)) {
            planet_.ambient = col;
        }
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (getManager() != nullptr) {
        addToSet(SimRenderSetId);
    }
    load();
    return true;
}

void Planet::onRemove() {
    unload();
    if (getManager() != nullptr) {
        removeFromSet(SimRenderSetId);
    }
    Parent::onRemove();
}

bool Planet::processEvent(const sim::SimEvent& event) {
    return Parent::processEvent(event);
}

bool Planet::processQuery(sim::SimQuery& query) {
    if (auto* lq = dynamic_cast<sim::SimLightQuery*>(&query)) {
        if (inLightSet_) {
            lq->ambient = planet_.ambient;
            lq->lights.push_back(light_);
            return true;
        }
        return false;
    }
    return Parent::processQuery(query);
}

bool Planet::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_PLANET_FILE_NAME, planet_.fileName);
    inspector.write(IDITG_PLANET_AZIMUTH, planet_.azimuth);
    inspector.write(IDITG_PLANET_INCIDENCE, planet_.incidence);
    inspector.write(IDITG_PLANET_SIZE, planet_.size);
    inspector.write(IDITG_PLANET_DISTANCE, planet_.distance);
    inspector.write(IDITG_PLANET_LENS_FLARE, planet_.useLensFlare);
    inspector.write(IDITG_PLANET_CAST_SHADOWS, planet_.castShadows);
    inspector.write(
        IDITG_PLANET_INTENSITY,
        Point3F{planet_.intensity.red, planet_.intensity.green, planet_.intensity.blue});
    inspector.write(IDITG_PLANET_AMBIENT,
                    Point3F{planet_.ambient.red, planet_.ambient.green, planet_.ambient.blue});
    return true;
}

bool Planet::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    unload();
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, IDITG_PLANET_FILE_NAME, v)) {
        return false;
    }
    planet_.setTexture(std::get<std::string>(v));

    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_PLANET_AZIMUTH, v)) {
        return false;
    }
    planet_.azimuth = std::get<Flt32>(v);

    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_PLANET_INCIDENCE, v)) {
        return false;
    }
    planet_.incidence = std::get<Flt32>(v);

    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_PLANET_SIZE, v)) {
        return false;
    }
    planet_.size = std::get<Flt32>(v);

    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_PLANET_DISTANCE, v)) {
        return false;
    }
    planet_.distance = std::get<Flt32>(v);

    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_PLANET_LENS_FLARE, v)) {
        return false;
    }
    planet_.useLensFlare = std::get<bool>(v);

    if (!inspector.read(sim::Inspect::Type::Bool, IDITG_PLANET_CAST_SHADOWS, v)) {
        return false;
    }
    planet_.castShadows = std::get<bool>(v);

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_PLANET_INTENSITY, v)) {
        return false;
    }
    const auto intens = std::get<Point3F>(v);
    planet_.intensity.set(intens.x, intens.y, intens.z);

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_PLANET_AMBIENT, v)) {
        return false;
    }
    const auto amb = std::get<Point3F>(v);
    planet_.ambient.set(amb.x, amb.y, amb.z);

    load();
    return true;
}

void Planet::streamWrite(StreamIO& sio) const {
    sio.writeString(planet_.fileName);
    sio.write(planet_.azimuth);
    sio.write(planet_.incidence);
    sio.write(planet_.castShadows);
    sio.write(planet_.useLensFlare);
    sio.writeBytes(std::as_bytes(std::span(&planet_.intensity, 1)));
    sio.writeBytes(std::as_bytes(std::span(&planet_.ambient, 1)));
}

bool Planet::streamRead(StreamIO& sio) {
    std::string fname{};
    Flt32 az = 0.0f;
    Flt32 inc = 0.0f;
    bool cShadows = false;
    bool uLens = false;
    ColorF intens{};
    ColorF amb{};
    if (!sio.readString(fname) || !sio.read(az) || !sio.read(inc) || !sio.read(cShadows) ||
        !sio.read(uLens) || !sio.readBytes(std::as_writable_bytes(std::span(&intens, 1))) ||
        !sio.readBytes(std::as_writable_bytes(std::span(&amb, 1)))) {
        return false;
    }
    planet_.setTexture(fname);
    planet_.azimuth = az;
    planet_.incidence = inc;
    planet_.castShadows = cShadows;
    planet_.useLensFlare = uLens;
    planet_.intensity = intens;
    planet_.ambient = amb;
    return true;
}

UInt32
Planet::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 /*mask*/, BitStream& stream) const {
    streamWrite(stream);
    stream.write(planet_.size);
    stream.write(planet_.distance);
    return 0;
}

bool Planet::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    Planet temp{};
    if (!temp.streamRead(stream)) {
        stream.setCurPos(savedPos);
        return false;
    }
    Flt32 sz = 0.0f;
    Flt32 dist = 0.0f;
    if (!stream.read(sz) || !stream.read(dist) || !stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }
    unload();
    planet_ = std::move(temp.planet_);
    planet_.size = sz;
    planet_.distance = dist;
    load();
    return true;
}

Persistent::Error
Planet::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::ReadError;
    }
    Planet temp{};
    if (!temp.streamRead(sio)) {
        return Persistent::Error::ReadError;
    }
    Flt32 sz = 0.0f;
    if (!sio.read(sz)) {
        return Persistent::Error::ReadError;
    }
    planet_ = std::move(temp.planet_);
    planet_.size = sz;
    load();
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
Planet::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    streamWrite(sio);
    sio.write(planet_.size);
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
