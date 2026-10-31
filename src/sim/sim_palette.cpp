#include <opent1/sim/sim_palette.hpp>
#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>

namespace opent1::sim {
namespace {
bool filenameValid(std::string_view name) {
    return name.size() <= 255 && name.find('\0') == name.npos;
}

bool argumentsValid(std::span<const std::string> args) {
    if (args.empty() || args.size() > 2 || args[0].empty() || !filenameValid(args[0])) {
        return false;
    }
    if (args.size() == 1) {
        return true;
    }
    std::string flag = args[1];
    std::ranges::transform(flag, flag.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return flag == "true"; // original forceLoad argument was accepted but ignored
}
}

bool SimPalette::onAdd() {
    if (filename_.empty()) {
        if (const auto name = getFieldValue("fileName"); name && !name->empty()) {
            if (!filenameValid(*name)) {
                return false;
            }
            filename_ = *name;
        }
    }
    return SimNetObject::onAdd() && (filename_.empty() || open(filename_));
}

void SimPalette::onRemove() {
    palette_.reset();
    SimNetObject::onRemove();
}

bool SimPalette::prepareArguments(std::span<const std::string> args) {
    return !getManager() && argumentsValid(args) && setFileName(args[0]);
}

bool SimPalette::processArguments(std::span<const std::string> args) {
    return argumentsValid(args) && open(args[0]);
}

bool SimPalette::setFileName(std::string_view name) {
    if (!filenameValid(name)) {
        return false;
    }
    if (getManager()) {
        if (isRemoved()) {
            return false;
        }
        if (!name.empty()) {
            return open(name);
        }
        filename_.clear();
        palette_.reset();
        setMaskBits(1);
        return true;
    }
    filename_ = name;
    palette_.reset();
    return true;
}

bool SimPalette::open(std::string_view name) {
    if (!getManager() || isRemoved() || name.empty() || !filenameValid(name)) {
        return false;
    }
    std::string replacement(name);
    auto loaded = SimResource::loadByName<gfx::Palette>(*getManager(), replacement);
    if (!loaded) {
        return false;
    }
    filename_ = std::move(replacement);
    palette_ = std::move(loaded);
    setMaskBits(1);
    return true;
}

bool SimPalette::setInWorld(SimWorld& world, Int32 transition) const {
    return palette_ && world.setPalette(palette_, transition);
}

bool SimPalette::packUpdate(BitStream& b) const {
    return b.writeString(filename_);
}

bool SimPalette::unpackUpdate(BitStream& b) {
    std::string name;
    return b.readString(name) && setFileName(name);
}

bool SimPalette::inspectWrite(Inspect& inspect) const {
    return SimObject::inspectWrite(inspect) && inspect.write(1100050, filename_);
}

bool SimPalette::inspectRead(Inspect& inspect) {
    if (!SimObject::inspectRead(inspect)) {
        return false;
    }
    Inspect::Value value;
    return inspect.read(Inspect::Type::IString, 1100050, value) &&
           setFileName(std::get<std::string>(value));
}

Persistent::Error
SimPalette::write(StreamIO& s, Int32 version, Int32 user, Persistent::StoreContext& context) const {
    return SimNetObject::write(s, version, user, context) == Persistent::Error::Ok &&
                   s.writeString(filename_)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimPalette::read(StreamIO& s, Int32, Int32, Persistent::LoadContext&) {
    UInt32 id{}, flags{}, pointer{}, object{}, manager{};
    std::string name;
    if (getManager() || !s.read(id) || !s.read(flags) || !s.read(pointer) || !s.read(object) ||
        !s.read(manager) || pointer || !s.readString(name) || !filenameValid(name)) {
        return Persistent::Error::ReadError;
    }
    setId(id);
    setNetFlags(netFlags() | flags);
    filename_ = std::move(name);
    palette_.reset();
    return Persistent::Error::Ok;
}

bool registerSimPaletteClass(Persistent::ClassRegistry& registry) {
    Persistent::Field filename{
        "fileName",
        TypeString,
        0,
        1,
        [](const Persistent::Base& object,
           std::size_t i,
           const DataFieldManager&) -> std::optional<std::string> {
            const auto* p = dynamic_cast<const SimPalette*>(&object);
            return p && i == 0 ? std::optional(std::string(p->getFileName())) : std::nullopt;
        },
        [](Persistent::Base& object,
           std::size_t i,
           const DataFieldManager&,
           std::span<const std::string_view> args) {
            auto* p = dynamic_cast<SimPalette*>(&object);
            return p && i == 0 && args.size() == 1 && p->setFileName(args[0]);
        }};
    return registry.registerClass<SimPalette>(
        "SimPalette", makeFourCC('S', 'p', 'p', 'l'), 3, {std::move(filename)});
}
} // namespace opent1::sim
