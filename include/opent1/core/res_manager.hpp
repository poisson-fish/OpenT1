#pragma once
#include <opent1/core/stream_io.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <vector>

namespace opent1 {
inline constexpr UInt32 kInvalidResourceId = 0xFFFFFFFFU;
inline constexpr auto kResDefaultTimeout = std::chrono::minutes(5);
[[nodiscard]] UInt32 resourceType(std::string_view name) noexcept;

namespace CRC {
// Legacy reflected IEEE accumulator; deliberately no final XOR.
[[nodiscard]] UInt32 calculate(std::span<const std::byte> bytes,
                               UInt32 initial = 0xFFFFFFFFU) noexcept;
}

namespace detail {
struct ResourcePayload {
    virtual ~ResourcePayload() = default;
    virtual void* pointer() noexcept = 0;
};

template <class T> struct TypedPayload final : ResourcePayload {
    explicit TypedPayload(std::unique_ptr<T> object) : value(std::move(object)) {}

    void* pointer() noexcept override {
        return value.get();
    }

    std::unique_ptr<T> value;
};

struct ResourceState {
    std::unique_ptr<ResourcePayload> payload;
    std::type_index type{typeid(void)};
    std::string name;
    std::chrono::steady_clock::time_point released{std::chrono::steady_clock::now()};
    std::chrono::milliseconds timeout{0};
};
}
class ResourceManager;

// Strong typed handles retain objects across purge, path changes, unmount, and
// manager destruction. ResourceManager and related handles need external locking.
template <class T> class Resource {
public:
    Resource() = default;
    Resource(const Resource&) = default;
    Resource(Resource&&) noexcept = default;

    Resource& operator=(const Resource& other) {
        if (this != &other) {
            reset();
            state_ = other.state_;
        }
        return *this;
    }

    Resource& operator=(Resource&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~Resource() {
        reset();
    }

    void reset() noexcept {
        if (state_) {
            state_->released = std::chrono::steady_clock::now();
            state_.reset();
        }
    }

    [[nodiscard]] T* get() const noexcept {
        return state_ ? static_cast<T*>(state_->payload->pointer()) : nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return get() != nullptr;
    }

    T* operator->() const noexcept {
        return get();
    }

    T& operator*() const noexcept {
        return *get();
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return state_ ? state_->name : std::string_view{};
    }

private:
    friend class ResourceManager;

    explicit Resource(std::shared_ptr<detail::ResourceState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::ResourceState> state_;
};

struct RawData {
    std::vector<std::byte> bytes;
};

// Indexed recursive disk roots (first root wins), then mounted archives (last
// mounted wins), then added objects. ASCII case folding and both slashes match
// legacy asset names. Virtual names reject traversal/absolute paths. All streams
// returned here own their bytes. File/decoded-entry size defaults to 64 MiB.
// Path/mount changes invalidate the cache; existing handles stay valid.
// Loaders may load dependencies; cycles fail. Loaders must not mutate paths/mounts.
class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;
    bool setSearchPaths(std::span<const std::filesystem::path> roots);
    bool addVolume(std::string_view name);    // volume must be in indexed disk roots
    bool removeVolume(std::string_view name); // repeated mounts are reference counted
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> findMatches(std::string_view expression = "*") const;
    [[nodiscard]] std::optional<UInt32> getSize(std::string_view name) const;
    [[nodiscard]] std::optional<std::filesystem::path> getFullPath(std::string_view name) const;
    bool read(std::string_view name,
              std::vector<std::byte>& bytes,
              UInt32 maxBytes = 64U * 1024U * 1024U);
    [[nodiscard]] std::unique_ptr<StreamIO> openStream(std::string_view name,
                                                       UInt32 maxBytes = 64U * 1024U * 1024U);
    bool getCrc(std::string_view name, UInt32& crc, UInt32 initial = 0xFFFFFFFFU);
    std::size_t purge(bool force = false); // force still retains live handles
    // Drop one unused decoded disk/volume cache entry. Live handles and added
    // objects remain intact; a later load can decode the source again.
    bool evict(std::string_view name);

    template <class T, class Fn>
    bool registerLoader(std::string_view extension,
                        Fn loader,
                        std::chrono::milliseconds timeout = kResDefaultTimeout) {
        return registerErased(
            resourceType(extension),
            typeid(T),
            [fn = std::move(loader)](StreamIO& stream,
                                     UInt32 size) -> std::unique_ptr<detail::ResourcePayload> {
                auto value = std::invoke(fn, stream, size);
                if (!value) {
                    return {};
                }
                return std::make_unique<detail::TypedPayload<T>>(std::move(value));
            },
            timeout);
    }

    template <class T> [[nodiscard]] Resource<T> load(std::string_view name) {
        return Resource<T>(loadErased(name, typeid(T)));
    }

    template <class T>
    bool add(std::string_view name,
             std::unique_ptr<T> object,
             std::chrono::milliseconds timeout = kResDefaultTimeout) {
        if (!object) {
            return false;
        }
        return addErased(
            name, typeid(T), std::make_unique<detail::TypedPayload<T>>(std::move(object)), timeout);
    }

private:
    using Loader = std::function<std::unique_ptr<detail::ResourcePayload>(StreamIO&, UInt32)>;
    bool registerErased(UInt32 id,
                        std::type_index type,
                        Loader loader,
                        std::chrono::milliseconds timeout);
    bool addErased(std::string_view name,
                   std::type_index type,
                   std::unique_ptr<detail::ResourcePayload> payload,
                   std::chrono::milliseconds timeout);
    std::shared_ptr<detail::ResourceState> loadErased(std::string_view name, std::type_index type);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace opent1
