#include <opent1/core/persist.hpp>
#include <opent1/core/block_stream.hpp>
#include <opent1/core/file_stream.hpp>
#include <charconv>
#include <cmath>

namespace opent1 {
namespace {
template <class T> std::string number(T value) {
    std::array<char, 64> text{};
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? std::string(text.data(), result.ptr) : std::string{};
}

template <class T> bool parse(T& value, std::span<const std::string_view> args) {
    if (args.size() != 1 || args.front().empty()) {
        return false;
    }
    const auto text = args.front();
    T parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    if constexpr (std::floating_point<T>) {
        if (!std::isfinite(parsed)) {
            return false;
        }
    }
    value = parsed;
    return true;
}

bool sameName(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        };
        return lower(x) == lower(y);
    });
}

bool validName(std::string_view name) {
    return !name.empty() && name.size() < 128 && name.find('\0') == name.npos;
}
}

DataFieldManager::DataFieldManager() {
    auto getString = [](const std::string& s) {
        return s;
    };
    auto setString = [](std::string& s, std::span<const std::string_view> args) {
        if (args.size() != 1 || args.front().find('\0') != args.front().npos) {
            return false;
        }
        s = args.front();
        return true;
    };
    registerType<std::string>(TypeString, getString, setString);
    registerType<std::string>(TypeCaseString, getString, setString);
    registerType<Int32>(TypeInt, number<Int32>, parse<Int32>);
    registerType<Flt32>(TypeFloat, number<Flt32>, parse<Flt32>);
    registerType<bool>(
        TypeBool,
        [](bool b) {
            return b ? std::string("True") : std::string("False");
        },
        [](bool& b, std::span<const std::string_view> args) {
            if (args.size() != 1) {
                return false;
            }
            if (sameName(args.front(), "true") || args.front() == "1") {
                b = true;
                return true;
            }
            if (sameName(args.front(), "false") || args.front() == "0") {
                b = false;
                return true;
            }
            return false;
        });
}

namespace Persistent {
namespace {
bool rewind(StreamIO& stream, Int32 start) {
    stream.clrStatus();
    return stream.setPosition(start);
}

struct DepthGuard {
    std::size_t& depth;

    explicit DepthGuard(std::size_t& d) : depth(d) {
        ++depth;
    }

    ~DepthGuard() {
        --depth;
    }
};
}

Error Base::read(StreamIO&, Int32, Int32, LoadContext&) {
    return Error::Ok;
}

Error Base::write(StreamIO&, Int32, Int32, StoreContext&) const {
    return Error::Ok;
}

bool ClassRegistry::insert(Descriptor descriptor) {
    if (!validName(descriptor.name) || !descriptor.create ||
        (descriptor.tag == 0 && !descriptor.identify) || descriptor.tag == kDictTag ||
        descriptor.alias == kDictTag || descriptor.alias == kPersTag) {
        return false;
    }
    if (std::ranges::any_of(classes_, [&](const auto& other) {
            if ((descriptor.tag != 0) == (other.tag != 0) &&
                (sameName(other.name, descriptor.name) || other.type == descriptor.type) &&
                (descriptor.tag != 0 || other.user == descriptor.user)) {
                return true;
            }
            const auto collides = [&](UInt32 tag) {
                return tag != 0 && tag != kPersTag && (tag == other.tag || tag == other.alias);
            };
            return collides(descriptor.tag) || collides(descriptor.alias);
        })) {
        return false;
    }
    std::vector<std::string> names;
    if (!std::ranges::all_of(descriptor.fields, [&](const auto& f) {
            if (!validName(f.name) || f.elementCount == 0 || !f.get || !f.set ||
                std::ranges::any_of(names, [&](const auto& n) {
                    return sameName(n, f.name);
                })) {
                return false;
            }
            names.push_back(f.name);
            return true;
        })) {
        return false;
    }
    classes_.push_back(std::move(descriptor));
    return true;
}

const ClassRegistry::Descriptor* ClassRegistry::find(UInt32 tag) const {
    if (tag == 0 || tag == kPersTag) {
        return nullptr;
    }
    const auto it = std::ranges::find_if(classes_, [&](const auto& c) {
        return c.tag == tag || c.alias == tag;
    });
    return it == classes_.end() ? nullptr : &*it;
}

const ClassRegistry::Descriptor* ClassRegistry::find(std::string_view name) const {
    const auto it = std::ranges::find_if(classes_, [&](const auto& c) {
        return c.tag != 0 && sameName(c.name, name);
    });
    return it == classes_.end() ? nullptr : &*it;
}

const ClassRegistry::Descriptor* ClassRegistry::find(std::type_index type, Int32 user) const {
    const auto tagged = std::ranges::find_if(classes_, [&](const auto& c) {
        return user == 0 && c.tag != 0 && c.type == type;
    });
    if (tagged != classes_.end()) {
        return &*tagged;
    }
    const auto raw = std::ranges::find_if(classes_, [&](const auto& c) {
        return c.tag == 0 && c.type == type && c.user == user;
    });
    return raw == classes_.end() ? nullptr : &*raw;
}

std::span<const Field> ClassRegistry::fields(const Base& object) const {
    const auto* descriptor = find(typeid(object));
    return descriptor ? std::span<const Field>(descriptor->fields) : std::span<const Field>{};
}

LoadContext::Result LoadContext::load(StreamIO& stream) {
    if (depth_ >= limits_.maxDepth) {
        return {nullptr, Error::LimitExceeded};
    }
    const auto savedObjects = objects_.size(), savedDictionary = dictionary_.size();
    DepthGuard guard(depth_);
    try {
        auto result = loadObject(stream);
        if (!result) {
            dictionary_.resize(savedDictionary);
            objects_.resize(savedObjects);
        }
        return result;
    } catch (...) {
        dictionary_.resize(savedDictionary);
        objects_.resize(savedObjects);
        throw;
    }
}

LoadContext::Result LoadContext::loadObject(StreamIO& stream) {
    if (stream.getStatus() != STRM_OK) {
        return {nullptr, Error::ReadError};
    }
    const auto start = stream.getPosition();
    BlockRStream block;
    const bool opened = block.open(stream);
    const ClassRegistry::Descriptor* descriptor = nullptr;
    bool raw = false;
    if (opened) {
        const auto tag = static_cast<UInt32>(block.getID());
        if (tag == kDictTag) {
            Int32 index{};
            if (!dictionaryEnabled_ || block.getDataSize() != 4 || !block.read(index) ||
                index < 0 || static_cast<std::size_t>(index) >= dictionary_.size()) {
                return {nullptr, Error::IncorrectTypeFound};
            }
            block.close();
            return block.getStatus() == STRM_CLOSED
                       ? Result{dictionary_[static_cast<std::size_t>(index)], Error::Ok}
                       : Result{nullptr, Error::ReadError};
        }
        if (tag == kPersTag) {
            Int16 length{};
            if (!block.read(length)) {
                return {nullptr, Error::ReadError};
            }
            if (length <= 0 || length >= 128) {
                return {nullptr, Error::InvalidName};
            }
            std::array<std::byte, 128> bytes{};
            const auto padded = (static_cast<std::size_t>(length) + 1U) & ~std::size_t{1};
            if (!block.readBytes(std::span(bytes).first(padded))) {
                return {nullptr, Error::ReadError};
            }
            const std::string_view name(reinterpret_cast<const char*>(bytes.data()),
                                        static_cast<std::size_t>(length));
            if (!validName(name)) {
                return {nullptr, Error::InvalidName};
            }
            descriptor = registry_.find(name);
        } else {
            descriptor = registry_.find(tag);
        }
    }
    if (!descriptor) {
        if (opened) {
            block.undoOpen();
        }
        if (!rewind(stream, start)) {
            return {nullptr, Error::ReadError};
        }
        // Probes are isolated: reset cursor and sticky status after every attempt,
        // including a successful probe, so read sees the complete raw object.
        auto candidates = registry_.classes_ | std::views::reverse;
        const auto it = std::ranges::find_if(candidates, [&](const auto& c) {
            if (c.tag != 0) {
                return false;
            }
            bool identified = false;
            try {
                identified = c.identify(stream);
            } catch (...) {
                rewind(stream, start);
                throw;
            }
            const bool restored = rewind(stream, start);
            return identified && restored;
        });
        if (it == candidates.end()) {
            return {nullptr, Error::UnregisteredClassError};
        }
        descriptor = &*it;
        raw = true;
    }
    if (!raw && static_cast<UInt32>(block.getDataSize()) > limits_.maxBlockBytes) {
        return {nullptr, Error::LimitExceeded};
    }
    if (objects_.size() >= limits_.maxObjects) {
        return {nullptr, Error::LimitExceeded};
    }
    auto object = descriptor->create();
    if (!object || (raw && object->versioned())) {
        return {nullptr, Error::IncorrectTypeFound};
    }
    Base* pointer = object.get();
    objects_.push_back(std::move(object));
    if (dictionaryEnabled_) {
        dictionary_.push_back(pointer);
    }
    StreamIO& input = raw ? stream : static_cast<StreamIO&>(block);
    Int32 version = 0;
    if (pointer->versioned() && !input.read(version)) {
        return {nullptr, Error::ReadError};
    }
    const auto error = pointer->read(input, version, raw ? descriptor->user : 0, *this);
    if (error != Error::Ok) {
        return {nullptr, error};
    }
    if (input.getStatus() != STRM_OK) {
        return {nullptr, Error::ReadError};
    }
    if (!raw) {
        // Validate skipped tails and physical alignment rather than seeking past
        // a truncated file. Unknown fields in newer versions remain skippable.
        std::array<std::byte, 256> discard{};
        while (block.getPosition() < block.getDataSize()) {
            const auto size =
                std::min(discard.size(),
                         static_cast<std::size_t>(block.getDataSize() - block.getPosition()));
            if (!block.readBytes(std::span(discard).first(size))) {
                return {nullptr, Error::ReadError};
            }
        }
        const auto padding = block.getBlockSize() - 8 - block.getDataSize();
        if (!stream.readBytes(std::span(discard).first(static_cast<std::size_t>(padding)))) {
            return {nullptr, Error::ReadError};
        }
        block.close();
        if (block.getStatus() != STRM_CLOSED) {
            return {nullptr, Error::ReadError};
        }
    }
    return {pointer, Error::Ok};
}

LoadContext::Result LoadContext::fileLoad(const std::filesystem::path& path) {
    FileRStream stream(path);
    return stream.getStatus() == STRM_OK ? load(stream) : Result{nullptr, Error::ReadError};
}

std::vector<std::unique_ptr<Base>> LoadContext::takeObjects() {
    if (depth_) {
        throw std::logic_error("cannot transfer objects during a load");
    }
    dictionary_.clear();
    auto result = std::move(objects_);
    objects_.clear();
    return result;
}

Error StoreContext::store(StreamIO& stream, const Base& object, Int32 user) {
    if (depth_ >= limits_.maxDepth) {
        return Error::LimitExceeded;
    }
    if (!dictionaryEnabled_ && !active_.insert(&object).second) {
        return Error::IncorrectTypeFound;
    }

    struct ActiveGuard {
        std::unordered_set<const Base*>& active;
        const Base* object;

        ~ActiveGuard() {
            active.erase(object);
        }
    } activeGuard{active_, &object};

    const auto saved = dictionary_.size(), savedCount = objectCount_;
    DepthGuard guard(depth_);
    try {
        const auto error = storeObject(stream, object, user);
        if (error != Error::Ok) {
            dictionary_.resize(saved);
            objectCount_ = savedCount;
        }
        return error;
    } catch (...) {
        dictionary_.resize(saved);
        objectCount_ = savedCount;
        throw;
    }
}

Error StoreContext::storeObject(StreamIO& stream, const Base& object, Int32 user) {
    if (stream.getStatus() != STRM_OK) {
        return Error::WriteError;
    }
    const auto* descriptor = registry_.find(typeid(object), user);
    if (!descriptor) {
        return Error::UnregisteredClassError;
    }
    if (dictionaryEnabled_) {
        const auto it = std::ranges::find(dictionary_, &object);
        if (it != dictionary_.end()) {
            if (limits_.maxBlockBytes < sizeof(Int32)) {
                return Error::LimitExceeded;
            }
            BlockRWStream block;
            if (!block.open(stream, std::bit_cast<Int32>(kDictTag), 0, false)) {
                return Error::OpenBlockError;
            }
            if (!block.write(static_cast<Int32>(it - dictionary_.begin()))) {
                return Error::WriteError;
            }
            block.close();
            return block.getStatus() == STRM_CLOSED ? Error::Ok : Error::WriteError;
        }
        if (dictionary_.size() >= limits_.maxObjects) {
            return Error::LimitExceeded;
        }
        dictionary_.push_back(&object);
    }
    if (objectCount_ >= limits_.maxObjects) {
        return Error::LimitExceeded;
    }
    ++objectCount_;
    if (descriptor->tag == 0) {
        if (object.versioned()) {
            return Error::IncorrectTypeFound;
        }
        const auto error = object.write(stream, object.version(), user, *this);
        return error == Error::Ok && stream.getStatus() != STRM_OK ? Error::WriteError : error;
    }
    BlockRWStream block;
    if (!block.open(stream, std::bit_cast<Int32>(descriptor->tag), 0, false)) {
        return Error::OpenBlockError;
    }
    if (descriptor->tag == kPersTag) {
        const auto size = descriptor->name.size();
        std::array<std::byte, 128> name{};
        std::ranges::transform(descriptor->name, name.begin(), [](char c) {
            return static_cast<std::byte>(c);
        });
        if (!block.write(static_cast<Int16>(size)) ||
            !block.writeBytes(std::span(name).first((size + 1U) & ~std::size_t{1}))) {
            return Error::WriteError;
        }
    }
    if (object.versioned() && !block.write(object.version())) {
        return Error::WriteError;
    }
    auto error = object.write(block, object.version(), 0, *this);
    if (error == Error::Ok && block.getStatus() != STRM_OK) {
        error = Error::WriteError;
    }
    block.close(); // surface backpatch/padding errors before reporting success
    if (error == Error::Ok && static_cast<UInt32>(block.getDataSize()) > limits_.maxBlockBytes) {
        error = Error::LimitExceeded;
    }
    return error == Error::Ok && block.getStatus() != STRM_CLOSED ? Error::WriteError : error;
}

Error StoreContext::fileStore(const std::filesystem::path& path, const Base& object, Int32 user) {
    FileWStream stream(path);
    if (stream.getStatus() != STRM_OK) {
        return Error::WriteError;
    }
    const auto error = store(stream, object, user);
    return error == Error::Ok && !stream.flush() ? Error::WriteError : error;
}
} // namespace Persistent
} // namespace opent1
