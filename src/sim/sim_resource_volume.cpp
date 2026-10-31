#include <opent1/sim/sim_resource_volume.hpp>
#include <opent1/gfx/palette.hpp>

namespace opent1::sim {
namespace {
std::string canonical(std::string_view name) {
    std::string result(name.substr(0, name.find('\0')));
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        if (c == '\\') {
            return '/';
        }
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return result;
}

bool filenameValid(std::string_view name) {
    return !name.empty() && name.size() <= 255 && name.find('\0') == std::string_view::npos;
}
}

SimResource::SimResource(std::shared_ptr<ResourceManager> resources)
    : resources_(resources ? std::move(resources) : std::make_shared<ResourceManager>()) {
    setId(ResourceObjectId);
    gfx::registerPaletteLoaders(*resources_);
}

SimResource* SimResource::find(SimManager& manager) {
    return dynamic_cast<SimResource*>(manager.findObject(ResourceObjectId));
}

SimVolume::~SimVolume() {
    close();
}

bool SimVolume::onAdd() {
    if (filename_.empty()) {
        if (const auto name = getFieldValue("fileName"); name && !name->empty()) {
            if (!filenameValid(*name)) {
                return false;
            }
            filename_ = *name;
        }
    }
    if (!SimNetObject::onAdd()) {
        return false;
    }
    if (auto* service = SimResource::find(*getManager())) {
        resources_ = service->shared();
    }
    return filename_.empty() || open(filename_);
}

void SimVolume::onRemove() {
    close();
    resources_.reset();
    SimNetObject::onRemove();
}

bool SimVolume::prepareArguments(std::span<const std::string> args) {
    if (args.empty() || args.size() > 2) {
        return false;
    }
    const bool ghost = args.size() == 2 && !args[1].empty() &&
                       (args[1].front() == 't' || args[1].front() == 'T' || args[1].front() == '1');
    return !getManager() && setFileName(args[0]) &&
           setNetFlags(ghost ? netFlags() | Ghostable | ScopeAlways
                             : netFlags() & ~(Ghostable | ScopeAlways));
}

bool SimVolume::processArguments(std::span<const std::string> args) {
    return !args.empty() && args.size() <= 2 && open(args[0]);
}

bool SimVolume::setFileName(std::string_view name) {
    if (name.empty()) {
        close();
        filename_.clear();
        setMaskBits(1);
        return true;
    }
    if (!filenameValid(name)) {
        return false;
    }
    if (getManager()) {
        return open(name);
    }
    filename_ = name;
    return true;
}

bool SimVolume::open(std::string_view name) {
    if (!resources_ || !filenameValid(name)) {
        return false;
    }
    std::string replacement(name);
    if (opened_ && canonical(replacement) == canonical(filename_)) {
        return true;
    }
    if (!resources_->addVolume(replacement)) {
        return false;
    }
    close();
    filename_ = std::move(replacement);
    opened_ = true;
    setMaskBits(1);
    return true;
}

void SimVolume::close() {
    if (opened_) {
        resources_->removeVolume(filename_);
        opened_ = false;
    }
}

bool SimVolume::packUpdate(BitStream& stream) const {
    return stream.writeString(filename_);
}

bool SimVolume::unpackUpdate(BitStream& stream) {
    std::string name;
    return stream.readString(name) && setFileName(name);
}

Persistent::Error
SimVolume::write(StreamIO& stream, Int32, Int32, Persistent::StoreContext&) const {
    const auto flags = (netFlags() & ~(1U << 9)) | (isLocked() ? 1U << 9 : 0);
    // Original SimNetObject prefix: id, flags, then a default 32-bit SimAddress.
    // The pointer slot is a reserved zero word, independent of native pointers.
    return stream.write(isDynamicId() ? 0U : getId()) && stream.write(flags) &&
                   stream.write(UInt32{0}) && stream.write(UInt32{0xffffffff}) &&
                   stream.write(UInt32{0}) && stream.writeString(filename_)
               ? Persistent::Error::Ok
               : Persistent::Error::WriteError;
}

Persistent::Error SimVolume::read(StreamIO& stream, Int32, Int32, Persistent::LoadContext&) {
    std::string name;
    UInt32 id{}, flags{}, pointer{}, objectId{}, managerId{};
    if (getManager() || !stream.read(id) || !stream.read(flags) || !stream.read(pointer) ||
        !stream.read(objectId) || !stream.read(managerId) || pointer != 0 ||
        !stream.readString(name) || (!name.empty() && !filenameValid(name))) {
        return Persistent::Error::ReadError;
    }
    setId(id);
    if (!setNetFlags(netFlags() | flags)) {
        return Persistent::Error::ReadError;
    }
    filename_ = std::move(name);
    return Persistent::Error::Ok;
}

bool SimObject::inspectWrite(Inspect& inspect) const {
    return inspect.write(110066, std::string(getName()));
}

bool SimObject::inspectRead(Inspect& inspect) {
    Inspect::Value value;
    if (!inspect.read(Inspect::Type::IString, 110066, value)) {
        return false;
    }
    assignName(std::get<std::string>(value));
    return true;
}

bool SimVolume::inspectWrite(Inspect& inspect) const {
    return SimObject::inspectWrite(inspect) && inspect.write(1100002, filename_);
}

bool SimVolume::inspectRead(Inspect& inspect) {
    if (!SimObject::inspectRead(inspect)) {
        return false;
    }
    Inspect::Value value;
    return inspect.read(Inspect::Type::IString, 1100002, value) &&
           setFileName(std::get<std::string>(value));
}

bool SimPreLoadManager::registerLoader(std::string_view extension, Loader loader) {
    const auto key = canonical(extension);
    return loader && key.size() >= 2 && key.front() == '.' &&
           loaders_.emplace(key, std::move(loader)).second;
}

bool SimPreLoadManager::preLoad(std::string_view name, bool delay) {
    if (!filenameValid(name) || entries_.size() >= 65536) {
        return false;
    }
    const auto key = canonical(name);
    entries_.try_emplace(key, Entry{std::string(name), {}, false});
    return delay || load(key, 0);
}

bool SimPreLoadManager::load(std::string_view name, unsigned depth) {
    auto it = entries_.find(std::string(name));
    if (it == entries_.end() || depth >= 64) {
        return false;
    }
    auto& entry = it->second;
    if (entry.retained) {
        return true;
    }
    if (entry.loading || !getManager()) {
        return false;
    }
    auto* service = SimResource::find(*getManager());
    if (!service) {
        return false;
    }
    // Retain the service independently while user loaders run.
    auto resources = service->shared();
    const auto dot = name.find_last_of('.');
    const auto handler = dot == std::string_view::npos
                             ? loaders_.end()
                             : loaders_.find(std::string(name.substr(dot)));
    const auto loader = handler == loaders_.end() ? Loader{} : handler->second;
    entry.loading = true;
    try {
        Loaded result;
        if (loader) {
            result = loader(*resources, entry.name);
        } else {
            auto handle = resources->load<RawData>(entry.name);
            if (handle) {
                result.retained = std::make_shared<Resource<RawData>>(std::move(handle));
            }
        }
        bool success = bool(result.retained);
        for (const auto& dependency : result.dependencies) {
            if (!filenameValid(dependency) || entries_.size() >= 65536) {
                success = false;
                continue;
            }
            const auto dependencyKey = canonical(dependency);
            entries_.try_emplace(dependencyKey, Entry{dependency, {}, false});
            success = load(dependencyKey, depth + 1) && success;
        }
        // Failed dependencies can be retried on a later delayed-load pass.
        if (success) {
            entry.retained = std::move(result.retained);
        }
        entry.loading = false;
        return success;
    } catch (...) {
        entry.loading = false;
        throw;
    }
}

std::size_t SimPreLoadManager::preLoadDelayed() {
    std::vector<std::string> pending;
    std::ranges::for_each(entries_, [&](const auto& item) {
        if (!item.second.retained) {
            pending.push_back(item.first);
        }
    });
    std::ranges::sort(pending);
    return static_cast<std::size_t>(std::ranges::count_if(pending, [&](const auto& name) {
        return load(name, 0);
    }));
}

std::vector<std::string> SimPreLoadManager::getPreLoadedList() const {
    std::vector<std::string> result;
    std::ranges::for_each(entries_, [&](const auto& item) {
        result.push_back(item.second.name);
    });
    std::ranges::sort(result);
    return result;
}

bool SimPreLoadManager::isLoaded(std::string_view name) const {
    const auto it = entries_.find(canonical(name));
    return it != entries_.end() && bool(it->second.retained);
}

bool Inspect::append(Record record) {
    if (cursor_ >= 65536) {
        return false;
    }
    // Legacy top() permits rewriting the inspection stream and truncates its tail.
    records_.resize(cursor_);
    records_.push_back(std::move(record));
    ++cursor_;
    return true;
}

bool Inspect::write(Int32 description, Value value) {
    static constexpr std::array types{Type::Description,
                                      Type::Int32,
                                      Type::RealF,
                                      Type::Bool,
                                      Type::IString,
                                      Type::Point2I,
                                      Type::Point3I,
                                      Type::Point2F,
                                      Type::Point3F,
                                      Type::Tag,
                                      Type::TagRange,
                                      Type::List,
                                      Type::List,
                                      Type::ActionBtn,
                                      Type::StringIndexDescription};
    const bool valid = std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::same_as<T, std::string>) {
                return item.size() <= 80;
            } else if constexpr (std::same_as<T, Tag>) {
                return item.filter.size() <= 80;
            } else if constexpr (std::same_as<T, TagRange>) {
                return item.min < item.max;
            } else if constexpr (std::same_as<T, TagList>) {
                return !item.choices.empty() && item.choices.size() <= 4096;
            } else if constexpr (std::same_as<T, StringList>) {
                return !item.choices.empty() && item.choices.size() <= 4096 &&
                       std::ranges::all_of(item.choices, [](const auto& choice) {
                           return choice.text.size() <= 80;
                       });
            } else if constexpr (std::same_as<T, StringIndex>) {
                return item.text.size() <= 4096;
            } else {
                return true;
            }
        },
        value);
    return valid && append({types[value.index()], description, std::move(value)});
}

bool Inspect::writeStringDescription(std::string_view value, std::optional<Int32> index) {
    if (value.size() > 4096) {
        return false;
    }
    if (index) {
        return append({Type::StringIndexDescription, 0, StringIndex{std::string(value), *index}});
    }
    return append({Type::StringDescription, 0, std::string(value)});
}

bool Inspect::writeDivider() {
    return append({Type::Divider, 0, {}});
}

const Inspect::Record* Inspect::peek() const {
    return cursor_ < records_.size() ? &records_[cursor_] : nullptr;
}

bool Inspect::read(Type type, Int32 description, Value& value) {
    const auto* record = peek();
    if (!record || record->type != type || record->description != description) {
        return false;
    }
    value = record->value;
    ++cursor_;
    return true;
}

bool registerSimResourceClasses(Persistent::ClassRegistry& registry) {
    return registry.registerClass<SimVolume>("SimVolume", makeFourCC('S', 'V', 'o', 'l'), 1);
}
} // namespace opent1::sim
