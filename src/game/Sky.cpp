#include <opent1/game/sky.hpp>
#include <ranges>
#include <opent1/game/string_table.hpp>
#include <opent1/sim/sim_world_game.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
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
} // namespace

Sky::Sky() {
    setType(SimSkyType);
    setNetFlags(ScopeAlways | Ghostable);
}

Sky::Sky(std::string_view in_dmlName) : Sky() {
    setDmlName(in_dmlName);
}

Sky::~Sky() = default;

void Sky::setDmlName(std::string_view name) {
    // Only the legacy binary file field is limited to 15 characters; mission
    // scripts and ghost updates carry full material-list resource names.
    if (dmlName_ != name) {
        materials_.reset();
    }
    dmlName_.assign(name);
    setMaskBits(Modified);
}

const world::TsMaterialResources* Sky::materials() const {
    if (!materials_ && getManager() && !dmlName_.empty()) {
        auto* service = sim::SimResource::find(*getManager());
        const auto list = service ? service->get().load<world::TsMaterialList>(dmlName_)
                                  : Resource<world::TsMaterialList>{};
        if (list) {
            materials_ = world::TsMaterialResources::load(service->get(), *list);
        }
    }
    return materials_ ? &*materials_ : nullptr;
}

void Sky::initPersistFields() {}

bool Sky::processArguments(std::span<const std::string> args) {
    if (args.empty()) {
        return true;
    }

    std::size_t idx = 0;
    if (args.size() >= 3) {
        Flt32 r = 0.0f;
        Flt32 g = 0.0f;
        Flt32 b = 0.0f;
        std::from_chars(args[0].data(), args[0].data() + args[0].size(), r);
        std::from_chars(args[1].data(), args[1].data() + args[1].size(), g);
        std::from_chars(args[2].data(), args[2].data() + args[2].size(), b);
        skyColor_.set(r, g, b);
        hazeColor_.set(r, g, b);
        idx = 3;
    }

    if (args.size() == 20 && idx < args.size()) {
        setDmlName(args[idx++]);
        if (idx < args.size()) {
            Flt32 fp = 0.0f;
            std::from_chars(args[idx].data(), args[idx].data() + args[idx].size(), fp);
            featurePosition_ = fp;
            ++idx;
        }
        for (auto& texture : textures_ | std::views::take(args.size() - idx)) {
            Int32 tex = 0;
            std::from_chars(args[idx].data(), args[idx].data() + args[idx].size(), tex);
            texture = tex & 0x0F;
            ++idx;
        }
    }
    return true;
}

bool Sky::onAdd() {
    if (const auto dml = getFieldValue("dmlName"); dml && !dml->empty()) {
        setDmlName(*dml);
    }
    if (const auto sc = getFieldValue("skyColor"); sc && !sc->empty()) {
        ColorF col{};
        if (parseColorField(*sc, col)) {
            skyColor_ = col;
        }
    }
    if (const auto hc = getFieldValue("hazeColor"); hc && !hc->empty()) {
        ColorF col{};
        if (parseColorField(*hc, col)) {
            hazeColor_ = col;
        }
    }
    if (const auto fp = getFieldValue("featurePosition"); fp && !fp->empty()) {
        Flt32 val = featurePosition_;
        std::from_chars(fp->data(), fp->data() + fp->size(), val);
        featurePosition_ = val;
    }
    if (const auto sz = getFieldValue("size"); sz && !sz->empty()) {
        Flt32 val = size_;
        std::from_chars(sz->data(), sz->data() + sz->size(), val);
        size_ = val;
    }
    for (auto i : std::views::iota(std::size_t{0}, textures_.size())) {
        if (const auto value = getFieldValue("textures" + std::to_string(i)); value) {
            Int32 texture{};
            const auto parsed =
                std::from_chars(value->data(), value->data() + value->size(), texture);
            if (parsed.ec == std::errc{} && texture >= 0 && texture < TEXTURES) {
                textures_[i] = texture;
            }
        }
    }
    if (!Parent::onAdd()) {
        return false;
    }
    if (auto* mgr = getManager()) {
        mgr->assignId(*this, TribesSkyId);
        addToSet(SimRenderSetId);
        if (auto* root = dynamic_cast<sim::SimContainer*>(mgr->findObject(sim::RootContainerId))) {
            root->addObject(*this);
        }
    }
    load();
    return true;
}

void Sky::onRemove() {
    if (auto* cont = getContainer()) {
        cont->removeObject(*this);
    }
    removeFromSet(SimRenderSetId);
    Parent::onRemove();
}

void Sky::initialize() {
    Flt32 a = 90.0f + featurePosition_ - 22.5f;
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        const RealD rad = static_cast<RealD>(a) * kRealDPi / static_cast<RealD>(180.0f);
        points_[i] = Point3F(static_cast<Flt32>(std::cos(rad) * static_cast<RealD>(distance_)),
                             static_cast<Flt32>(std::sin(rad) * static_cast<RealD>(distance_)),
                             0.0f);
        a -= 22.5f;
    }
    initialized_ = true;
}

void Sky::load() {
    setMaskBits(Modified);
    initialized_ = false;
    setPalette();
    initialize();
}

void Sky::setPalette() {
    // Palette lookup updates skyIndex_ / hazeIndex_ when a palette is active
    skyIndex_ = 0;
    hazeIndex_ = 0;
}

bool Sky::processQuery(sim::SimQuery& query) {
    return Parent::processQuery(query);
}

bool Sky::inspectWrite(sim::Inspect& inspector) const {
    if (!Parent::inspectWrite(inspector)) {
        return false;
    }
    inspector.write(IDITG_SKY_DML_NAME, dmlName_);
    inspector.write(IDITG_SKY_MATLIST_TAG, sim::Inspect::Tag{true, "IDDML_*", matListTag_});
    inspector.write(IDITG_SKY_SOLID_COLOR, Point3F{skyColor_.red, skyColor_.green, skyColor_.blue});
    inspector.write(IDITG_SKY_HAZE_COLOR,
                    Point3F{hazeColor_.red, hazeColor_.green, hazeColor_.blue});
    inspector.write(IDITG_SKY_SIZE, size_);
    inspector.write(IDITG_SKY_FEATURE_POS, featurePosition_);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        inspector.write(static_cast<Int32>(IDITG_SKY_ROT_0 + i),
                        sim::Inspect::Integer{textures_[i], sim::Inspect::Display::Decimal});
    }
    return true;
}

bool Sky::inspectRead(sim::Inspect& inspector) {
    if (!Parent::inspectRead(inspector)) {
        return false;
    }
    sim::Inspect::Value v{};
    if (!inspector.read(sim::Inspect::Type::IString, IDITG_SKY_DML_NAME, v)) {
        return false;
    }
    setDmlName(std::get<std::string>(v));

    if (!inspector.read(sim::Inspect::Type::Tag, IDITG_SKY_MATLIST_TAG, v)) {
        return false;
    }
    matListTag_ = std::get<sim::Inspect::Tag>(v).value;

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_SKY_SOLID_COLOR, v)) {
        return false;
    }
    const auto sc = std::get<Point3F>(v);
    skyColor_.set(sc.x, sc.y, sc.z);

    if (!inspector.read(sim::Inspect::Type::Point3F, IDITG_SKY_HAZE_COLOR, v)) {
        return false;
    }
    const auto hc = std::get<Point3F>(v);
    hazeColor_.set(hc.x, hc.y, hc.z);

    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_SKY_SIZE, v)) {
        return false;
    }
    size_ = std::get<Flt32>(v);

    if (!inspector.read(sim::Inspect::Type::RealF, IDITG_SKY_FEATURE_POS, v)) {
        return false;
    }
    featurePosition_ = std::get<Flt32>(v);

    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        if (!inspector.read(
                sim::Inspect::Type::Int32, static_cast<Int32>(IDITG_SKY_ROT_0 + i), v)) {
            return false;
        }
        textures_[i] = std::get<sim::Inspect::Integer>(v).value & 0x0F;
    }
    load();
    return true;
}

UInt32
Sky::packUpdate(sim::network::GhostManager& /*gm*/, UInt32 /*mask*/, BitStream& stream) const {
    stream.writeString(dmlName_);
    stream.writeBytes(std::as_bytes(std::span(&skyColor_, 1)));
    stream.writeBytes(std::as_bytes(std::span(&hazeColor_, 1)));
    stream.write(featurePosition_);
    stream.write(size_);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        stream.writeInt(textures_[i], 4);
    }
    return 0;
}

bool Sky::unpackUpdate(sim::network::GhostManager& /*gm*/, BitStream& stream) {
    const auto savedPos = stream.getCurPos();
    std::string nameStr{};
    ColorF newSkyColor{};
    ColorF newHazeColor{};
    Flt32 newFeaturePos = 0.0f;
    Flt32 newSize = 0.0f;
    std::array<Int32, TEXTURES> newTextures{};
    if (!stream.readString(nameStr) ||
        !stream.readBytes(std::as_writable_bytes(std::span(&newSkyColor, 1))) ||
        !stream.readBytes(std::as_writable_bytes(std::span(&newHazeColor, 1))) ||
        !stream.read(newFeaturePos) || !stream.read(newSize)) {
        stream.setCurPos(savedPos);
        return false;
    }
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        newTextures[i] = stream.readInt(4);
    }
    if (!stream.isValid()) {
        stream.setCurPos(savedPos);
        return false;
    }
    skyIndex_ = -1;
    hazeIndex_ = -1;
    setDmlName(nameStr);
    skyColor_ = newSkyColor;
    hazeColor_ = newHazeColor;
    featurePosition_ = newFeaturePos;
    size_ = newSize;
    textures_ = newTextures;
    if (getManager() != nullptr) {
        load();
    } else {
        initialize();
    }
    return true;
}

Persistent::Error
Sky::read(StreamIO& sio, Int32 version, Int32 user, Persistent::LoadContext& ctx) {
    if (Parent::read(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::ReadError;
    }
    Int32 tag = 0;
    Flt32 sz = 0.0f;
    std::array<Int32, TEXTURES> tex{};
    Flt32 featPos = 0.0f;
    if (!sio.read(tag) || !sio.read(sz)) {
        return Persistent::Error::ReadError;
    }
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        Int32 index = 0;
        if (!sio.read(index)) {
            return Persistent::Error::ReadError;
        }
        tex[i] = index;
    }
    if (!sio.read(featPos)) {
        return Persistent::Error::ReadError;
    }
    std::string newDml{};
    if (version == 2) {
        std::array<char, 16> rawDml{};
        if (!sio.readBytes(std::as_writable_bytes(std::span(rawDml)))) {
            return Persistent::Error::ReadError;
        }
        rawDml[15] = '\0';
        const auto nul = std::find(rawDml.begin(), rawDml.end(), '\0');
        newDml.assign(rawDml.begin(), nul);
    }
    ColorF sc{};
    ColorF hc{};
    if (!sio.readBytes(std::as_writable_bytes(std::span(&sc, 1))) ||
        !sio.readBytes(std::as_writable_bytes(std::span(&hc, 1)))) {
        return Persistent::Error::ReadError;
    }

    matListTag_ = tag;
    size_ = sz;
    textures_ = tex;
    featurePosition_ = featPos;
    dmlName_ = std::move(newDml);
    skyColor_ = sc;
    hazeColor_ = hc;
    initialize();
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::ReadError;
}

Persistent::Error
Sky::write(StreamIO& sio, Int32 version, Int32 user, Persistent::StoreContext& ctx) const {
    if (Parent::write(sio, version, user, ctx) != Persistent::Error::Ok) {
        return Persistent::Error::WriteError;
    }
    sio.write(matListTag_);
    sio.write(size_);
    for (auto i : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t i) {
                      return i < TEXTURES;
                  })) {
        sio.write(static_cast<Int32>(textures_[i]));
    }
    sio.write(featurePosition_);
    std::array<char, 16> rawDml{};
    const std::size_t copyLen = std::min<std::size_t>(dmlName_.size(), 15);
    std::memcpy(rawDml.data(), dmlName_.data(), copyLen);
    sio.writeBytes(std::as_bytes(std::span(rawDml)));
    sio.writeBytes(std::as_bytes(std::span(&skyColor_, 1)));
    sio.writeBytes(std::as_bytes(std::span(&hazeColor_, 1)));
    return (sio.getStatus() == STRM_OK) ? Persistent::Error::Ok : Persistent::Error::WriteError;
}

} // namespace opent1::game
