#pragma once
#include <opent1/core/stream_io.hpp>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <typeindex>
#include <vector>
#include <limits>
#include <unordered_set>

namespace opent1 {
enum CoreTypes : Int32 {
    TypeString,
    TypeCaseString,
    TypeInt,
    TypeFloat,
    TypeBool,
    TypePoint3F,
    TypeColorF,
    TypeTMat3F,
    TypeBox3F,
    NumCoreTypes
};

// Converter dispatch retains exact C++ types; never truncates addresses or
// computes unchecked byte offsets. Later layers register their own math types.
class DataFieldManager {
public:
    DataFieldManager();

    template <class T, class Get, class Set> bool registerType(Int32 code, Get get, Set set) {
        if (code < 0 || code >= 256) {
            return false;
        }
        return types_
            .emplace(code,
                     Converter{typeid(T),
                               [get](const void* p) {
                                   return std::invoke(get, *static_cast<const T*>(p));
                               },
                               [set](void* p, std::span<const std::string_view> args) {
                                   return std::invoke(set, *static_cast<T*>(p), args);
                               }})
            .second;
    }

    template <class T> std::optional<std::string> getData(Int32 code, const T& value) const {
        const auto it = types_.find(code);
        if (it == types_.end() || it->second.type != typeid(T)) {
            return {};
        }
        return it->second.get(&value);
    }

    template <class T>
    bool setData(Int32 code, T& value, std::span<const std::string_view> args) const {
        const auto it = types_.find(code);
        return it != types_.end() && it->second.type == typeid(T) && it->second.set(&value, args);
    }

private:
    struct Converter {
        std::type_index type;
        std::function<std::string(const void*)> get;
        std::function<bool(void*, std::span<const std::string_view>)> set;
    };

    std::map<Int32, Converter> types_;
};

namespace Persistent {
inline constexpr UInt32 kPersTag = makeFourCC('P', 'E', 'R', 'S');
inline constexpr UInt32 kDictTag = makeFourCC('D', 'I', 'C', 'T');
enum class Error {
    Ok,
    OpenBlockError,
    UnregisteredClassError,
    ReadError,
    WriteError,
    InvalidName,
    IncorrectTypeFound,
    LimitExceeded
};
class LoadContext;
class StoreContext;

class Base {
public:
    virtual ~Base() = default;
    virtual Error read(StreamIO&, Int32 version, Int32 user, LoadContext&);
    virtual Error write(StreamIO&, Int32 version, Int32 user, StoreContext&) const;

    [[nodiscard]] virtual Int32 version() const {
        return 0;
    }

    [[nodiscard]] virtual bool versioned() const {
        return false;
    }
};

class VersionedBase : public Base {
public:
    [[nodiscard]] bool versioned() const final {
        return true;
    }
};

struct Field {
    std::string name;
    Int32 type{}, ranking{};
    std::size_t elementCount{};
    std::function<std::optional<std::string>(const Base&, std::size_t, const DataFieldManager&)>
        get;
    std::function<bool(
        Base&, std::size_t, const DataFieldManager&, std::span<const std::string_view>)>
        set;
};

template <class Owner, class T, std::size_t N>
Field arrayField(std::string_view name,
                 Int32 type,
                 std::array<T, N> Owner::* member,
                 Int32 ranking = 0) {
    return {
        std::string(name),
        type,
        ranking,
        N,
        [member, type](const Base& base,
                       std::size_t index,
                       const DataFieldManager& manager) -> std::optional<std::string> {
            const auto* owner = dynamic_cast<const Owner*>(&base);
            return owner && index < N ? manager.getData(type, (owner->*member)[index])
                                      : std::nullopt;
        },
        [member, type](Base& base, std::size_t index, const DataFieldManager& manager, auto args) {
            auto* owner = dynamic_cast<Owner*>(&base);
            return owner && index < N && manager.setData(type, (owner->*member)[index], args);
        }};
}

template <class Owner, class T>
Field field(std::string_view name, Int32 type, T Owner::* member, Int32 ranking = 0) {
    return {
        std::string(name),
        type,
        ranking,
        1,
        [member, type](const Base& base,
                       std::size_t index,
                       const DataFieldManager& manager) -> std::optional<std::string> {
            const auto* owner = dynamic_cast<const Owner*>(&base);
            return owner && index == 0 ? manager.getData(type, owner->*member) : std::nullopt;
        },
        [member, type](Base& base, std::size_t index, const DataFieldManager& manager, auto args) {
            auto* owner = dynamic_cast<Owner*>(&base);
            return owner && index == 0 && manager.setData(type, owner->*member, args);
        }};
}

// Explicit names are the on-disk identity, independent of RTTI mangling/namespace.
// Complete registration before using contexts; registry must outlive contexts.
// Immutable registration may be shared by independent contexts across threads.
// Stateful probe callbacks must also support concurrent use in that case.
// Untagged probes run newest registration first, matching the legacy list.
class ClassRegistry {
public:
    struct Descriptor {
        std::string name;
        UInt32 tag{}, alias{};
        std::type_index type{typeid(void)};
        std::function<std::unique_ptr<Base>()> create;
        std::function<bool(StreamIO&)> identify;
        Int32 user{};
        std::vector<Field> fields;
    };

    template <std::derived_from<Base> T>
    bool registerClass(std::string_view name,
                       UInt32 tag = kPersTag,
                       UInt32 alias = 0,
                       std::vector<Field> fields = {}) {
        return insert({std::string(name),
                       tag,
                       alias,
                       typeid(T),
                       [] {
                           return std::make_unique<T>();
                       },
                       {},
                       0,
                       std::move(fields)});
    }

    template <std::derived_from<Base> T, class Probe>
    bool registerUntagged(std::string_view name, Probe probe, Int32 user = 0) {
        if constexpr (std::derived_from<T, VersionedBase>) {
            return false;
        } else {
            return insert({std::string(name),
                           0,
                           0,
                           typeid(T),
                           [] {
                               return std::make_unique<T>();
                           },
                           std::move(probe),
                           user,
                           {}});
        }
    }

    [[nodiscard]] const Descriptor* find(UInt32 tag) const;
    [[nodiscard]] const Descriptor* find(std::string_view name) const;
    [[nodiscard]] const Descriptor* find(std::type_index type, Int32 user = 0) const;
    [[nodiscard]] std::span<const Field> fields(const Base& object) const;

private:
    friend class LoadContext;
    bool insert(Descriptor descriptor);
    std::vector<Descriptor> classes_;
};

// Context owns every loaded object exactly once. DICT references and nested/cyclic
// links are borrowed pointers valid until context destruction. New top-level loads
// retain prior objects; failure rolls back the entire new subgraph/dictionary tail.
// Contexts are single-threaded. Never share a dictionary between unrelated streams.
// A failed store may leave partial bytes; discard that output before retrying.
struct ContextLimits {
    std::size_t maxObjects{static_cast<std::size_t>(std::numeric_limits<Int32>::max())};
    std::size_t maxDepth{std::numeric_limits<std::size_t>::max()};
    UInt32 maxBlockBytes{static_cast<UInt32>(std::numeric_limits<Int32>::max())};
};

class LoadContext {
public:
    struct Result {
        Base* object{};
        Error error{Error::Ok};

        explicit operator bool() const noexcept {
            return object && error == Error::Ok;
        }
    };

    explicit LoadContext(const ClassRegistry& registry,
                         bool dictionary = false,
                         ContextLimits limits = {})
        : registry_(registry), dictionaryEnabled_(dictionary), limits_(limits) {}

    Result load(StreamIO& stream);
    Result fileLoad(const std::filesystem::path& path);

    [[nodiscard]] std::size_t size() const noexcept {
        return objects_.size();
    }

    // Transfer the whole graph, never an individual object with borrowed DICT
    // links. Only between loads; clears the dictionary before any future load.
    [[nodiscard]] std::vector<std::unique_ptr<Base>> takeObjects();

private:
    Result loadObject(StreamIO& stream);
    const ClassRegistry& registry_;
    bool dictionaryEnabled_;
    std::vector<std::unique_ptr<Base>> objects_;
    std::vector<Base*> dictionary_;
    ContextLimits limits_;
    std::size_t depth_{};
};

class StoreContext {
public:
    // With a dictionary, source objects must outlive this context.
    explicit StoreContext(const ClassRegistry& registry,
                          bool dictionary = false,
                          ContextLimits limits = {})
        : registry_(registry), dictionaryEnabled_(dictionary), limits_(limits) {}

    Error store(StreamIO& stream, const Base& object, Int32 user = 0);
    Error fileStore(const std::filesystem::path& path, const Base& object, Int32 user = 0);

private:
    Error storeObject(StreamIO& stream, const Base& object, Int32 user);
    const ClassRegistry& registry_;
    bool dictionaryEnabled_;
    std::vector<const Base*> dictionary_;
    ContextLimits limits_;
    std::size_t depth_{}, objectCount_{};
    std::unordered_set<const Base*> active_;
};
} // namespace Persistent
} // namespace opent1
