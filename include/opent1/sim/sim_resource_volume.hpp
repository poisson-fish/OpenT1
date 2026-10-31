#pragma once
#include <opent1/sim/sim_net.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/math/point.hpp>
#include <variant>

namespace opent1::sim {
class SimResource : public SimSet {
public:
    explicit SimResource(std::shared_ptr<ResourceManager> resources = {});

    ResourceManager& get() {
        return *resources_;
    }

    [[nodiscard]] std::shared_ptr<ResourceManager> shared() const {
        return resources_;
    }

    [[nodiscard]] static SimResource* find(SimManager&);

    template <class T> static Resource<T> loadByName(SimManager& manager, std::string_view name) {
        auto* service = find(manager);
        return service ? service->get().load<T>(name) : Resource<T>{};
    }

    template <class T> static Resource<T> loadByTag(SimManager& manager, Int32 tag) {
        const auto name = manager.tags().idToString(tag);
        return name ? loadByName<T>(manager, *name) : Resource<T>{};
    }

private:
    std::shared_ptr<ResourceManager> resources_;
};

// Retains its resource manager so unmount remains valid even when the service
// object is removed first. Local mounting and serialized filename are provided;
// ghost delivery uses the original alias-1 class and filename update payload.
class SimVolume : public SimNetObject {
public:
    SimVolume() {
        setNetFlags(ScopeAlways | Ghostable);
    }

    ~SimVolume() override;
    bool onAdd() override;
    void onRemove() override;
    bool prepareArguments(std::span<const std::string>) override;
    bool processArguments(std::span<const std::string>) override;
    bool setFileName(std::string_view); // can configure before registration
    bool open(std::string_view);
    void close();

    [[nodiscard]] std::string_view getFileName() const {
        return filename_;
    }

    [[nodiscard]] bool isOpen() const {
        return opened_;
    }

    bool packUpdate(BitStream&) const;
    bool unpackUpdate(BitStream&);

    UInt16 getGhostTag() const override {
        return 1;
    }

    UInt32 packUpdate(network::GhostManager&, UInt32, BitStream& b) const override {
        packUpdate(b);
        return 0;
    }

    bool unpackUpdate(network::GhostManager&, BitStream& b) override {
        return unpackUpdate(b);
    }

    bool inspectWrite(Inspect&) const override;
    bool inspectRead(Inspect&) override;
    Persistent::Error read(StreamIO&, Int32, Int32, Persistent::LoadContext&) override;
    Persistent::Error write(StreamIO&, Int32, Int32, Persistent::StoreContext&) const override;

private:
    std::string filename_;
    std::shared_ptr<ResourceManager> resources_;
    bool opened_{};
};

class SimPreLoadManager : public SimObject {
public:
    struct Loaded {
        std::shared_ptr<void> retained;
        std::vector<std::string> dependencies;
    };

    // Loaders may return dependency names; they must not mutate the simulation
    // graph or this preloader. Layer 6's registerWorldPreloaders installs typed
    // TS shape/material loaders and their bitmap dependency traversal.
    using Loader = std::function<Loaded(ResourceManager&, std::string_view)>;

    SimPreLoadManager() {
        setId(PreLoadManagerId);
    }

    bool registerLoader(std::string_view extension, Loader);

    template <class T> bool registerType(std::string_view extension) {
        return registerLoader(
            extension, [](ResourceManager& manager, std::string_view name) -> Loaded {
                auto handle = manager.load<T>(name);
                return handle ? Loaded{std::make_shared<Resource<T>>(std::move(handle)), {}}
                              : Loaded{};
            });
    }

    bool preLoad(std::string_view filename, bool delay = false);
    std::size_t preLoadDelayed(); // retries failed loads too
    [[nodiscard]] std::vector<std::string> getPreLoadedList() const;
    [[nodiscard]] bool isLoaded(std::string_view) const;

    void onRemove() override {
        entries_.clear();
    }

private:
    struct Entry {
        std::string name;
        std::shared_ptr<void> retained;
        bool loading{};
    };

    bool load(std::string_view, unsigned depth);
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, Loader> loaders_;
};

// Owned typed inspector records replace the legacy pointer/struct-dump scratch
// stream. This is an in-memory editor API, never an asset or network format.
class Inspect {
public:
    enum class Type : Int32 {
        Invalid = -1,
        Description,
        StringDescription,
        StringIndexDescription,
        Int32,
        RealF,
        Bool,
        IString,
        Tag,
        TagRange,
        Point2I,
        Point3I,
        Point2F,
        Point3F,
        SimTagValPair,
        IStringValPair,
        List,
        ActionBtn,
        Divider
    };
    enum class Display { Decimal, Hex };

    struct Integer {
        opent1::Int32 value{};
        Display display{};
    };

    struct Tag {
        bool listDefines{};
        std::string filter;
        opent1::Int32 value{};
    };

    struct TagRange {
        bool listDefines{};
        opent1::Int32 min{}, max{}, value{};
    };

    struct TagVal {
        opent1::Int32 tag{}, value{};
    };

    struct StringVal {
        std::string text;
        opent1::Int32 value{};
    };

    struct TagList {
        opent1::Int32 value{};
        std::vector<TagVal> choices;
    };

    struct StringList {
        opent1::Int32 value{};
        std::vector<StringVal> choices;
    };

    struct Action {
        SimAddress receiver;
        opent1::Int32 message{};
    };

    struct StringIndex {
        std::string text;
        opent1::Int32 index{};
    };

    using Value = std::variant<std::monostate,
                               Integer,
                               Flt32,
                               bool,
                               std::string,
                               opent1::Point2I,
                               opent1::Point3I,
                               opent1::Point2F,
                               opent1::Point3F,
                               Tag,
                               TagRange,
                               TagList,
                               StringList,
                               Action,
                               StringIndex>;

    struct Record {
        Type type;
        opent1::Int32 description;
        Value value;
    };

    bool write(opent1::Int32 description, Value value = {});
    bool writeStringDescription(std::string_view, std::optional<opent1::Int32> index = {});
    bool writeDivider();
    // Mismatched reads leave cursor and caller output unchanged.
    [[nodiscard]] const Record* peek() const;
    bool read(Type, opent1::Int32 description, Value&);

    void top() {
        cursor_ = 0;
    }

    void clear() {
        records_.clear();
        cursor_ = 0;
    }

    [[nodiscard]] std::size_t size() const {
        return records_.size();
    }

    [[nodiscard]] std::span<const Record> records() const {
        return records_;
    }

    // Editor changes preserve type, labels and choice metadata. Invalid edits
    // leave the record and stream cursor unchanged.
    bool setValue(std::size_t index, Value);
    [[nodiscard]] bool sameSchema(const Inspect&) const;

private:
    bool append(Record);
    std::vector<Record> records_;
    std::size_t cursor_{};
};

bool registerSimResourceClasses(Persistent::ClassRegistry&);
} // namespace opent1::sim
