#include <opent1/core/res_manager.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <opent1/core/vol_stream.hpp>
#include <map>
#include <set>

namespace opent1 {
namespace {
char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string fold(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), lower);
    std::ranges::replace(result, '\\', '/');
    return result;
}

std::optional<std::string> keyOf(std::string_view name) {
    if (name.empty() || name.size() > 4096 || name.find('\0') != name.npos ||
        name.find(':') != name.npos) {
        return {};
    }
    auto key = fold(name);
    auto parts = key | std::views::split('/');
    if (std::ranges::any_of(parts, [](auto part) {
            const std::string_view s(part.begin(), part.end());
            return s.empty() || s == "." || s == "..";
        })) {
        return {};
    }
    return key;
}

bool matches(std::string_view pattern, std::string_view name) {
    std::size_t p = 0, n = 0, star = pattern.npos, retry = 0;
    while (n < name.size()) { // wildcard backtracking
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = n;
        } else if (star != pattern.npos) {
            p = star + 1;
            n = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

class OwnedStream final : public StreamIO {
public:
    explicit OwnedStream(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)), memory_(std::span<const std::byte>(bytes_)) {}

    Int32 getPosition() const override {
        return memory_.getPosition();
    }

    Int32 getCapabilities(StreamCap cap) const override {
        return memory_.getCapabilities(cap);
    }

    Bool setPosition(Int32 position) override {
        if (strm_status == STRM_EOS) {
            clrStatus();
        }
        if (strm_status != STRM_OK) {
            return false;
        }
        const bool ok = memory_.setPosition(position);
        strm_status = memory_.getStatus();
        return ok;
    }

    Bool readBytes(std::span<std::byte> bytes) override {
        lastBytes = 0;
        if (strm_status != STRM_OK) {
            return false;
        }
        const bool ok = memory_.readBytes(bytes);
        strm_status = memory_.getStatus();
        lastBytes = memory_.getLastSize();
        return ok;
    }

    void close() override {
        memory_.close();
        StreamIO::close();
    }

private:
    std::vector<std::byte> bytes_;
    MemRWStream memory_;
};
}

UInt32 resourceType(std::string_view name) noexcept {
    const auto at = name.rfind('.');
    if (at == name.npos) {
        return kInvalidResourceId;
    }
    UInt32 result = kInvalidResourceId;
    const auto ext = name.substr(at + 1, 4);
    std::ranges::for_each(std::views::iota(std::size_t{0}, ext.size()), [&](auto i) {
        const auto shift = static_cast<unsigned>(i * 8);
        result =
            (result & ~(0xFFU << shift)) | (UInt32(static_cast<UInt8>(lower(ext[i]))) << shift);
    });
    return result;
}

UInt32 CRC::calculate(std::span<const std::byte> bytes, UInt32 initial) noexcept {
    static constexpr auto table = [] {
        std::array<UInt32, 256> values{};
        std::ranges::transform(std::views::iota(0U, 256U), values.begin(), [](UInt32 value) {
            std::ranges::for_each(std::views::iota(0, 8), [&](int) {
                value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xEDB88320U : 0U);
            });
            return value;
        });
        return values;
    }();
    UInt32 crc = initial;
    std::ranges::for_each(bytes, [&](std::byte byte) {
        crc = table[(crc ^ std::to_integer<UInt8>(byte)) & 255U] ^ (crc >> 8U);
    });
    return crc;
}

struct ResourceManager::Impl {
    struct File {
        std::filesystem::path path;
        UInt32 size;
    };

    struct Volume {
        std::string name;
        std::filesystem::path path;
        VolumeRStream stream;
        std::size_t refs{1};
        std::map<std::string, std::size_t, std::less<>> entries;
    };

    struct Factory {
        std::type_index type;
        Loader load;
        std::chrono::milliseconds timeout;
    };

    std::map<std::string, File, std::less<>> files;
    std::vector<std::unique_ptr<Volume>> volumes;
    std::map<UInt32, Factory> loaders;
    std::set<std::string, std::less<>> loading;
    std::map<std::string, std::shared_ptr<detail::ResourceState>, std::less<>> cache, added;

    auto volumeFor(std::string_view key) const {
        return std::ranges::find_if(volumes, [&](const auto& v) {
            return v->entries.contains(key);
        });
    }
};

ResourceManager::ResourceManager() : impl_(std::make_unique<Impl>()) {
    auto raw = [](StreamIO& stream, UInt32 size) {
        auto data = std::make_unique<RawData>();
        data->bytes.resize(size);
        return stream.readBytes(data->bytes) ? std::move(data) : nullptr;
    };
    registerLoader<RawData>(".dat", raw);
    registerLoader<RawData>(".sdt", raw);
}

ResourceManager::~ResourceManager() = default;

bool ResourceManager::setSearchPaths(std::span<const std::filesystem::path> roots) {
    decltype(impl_->files) files;
    try {
        bool ok = true;
        std::ranges::for_each(roots, [&](const auto& root) {
            if (!ok) {
                return;
            }
            if (!std::filesystem::is_directory(root)) {
                ok = false;
                return;
            }
            decltype(files) rootFiles;
            auto it = std::filesystem::recursive_directory_iterator(root);
            const auto end = std::filesystem::recursive_directory_iterator{};
            while (it != end) { // filesystem iteration can throw; commit index only after success
                if (it->is_symlink()) {
                    it.disable_recursion_pending();
                } else if (it->is_regular_file()) {
                    const auto size = it->file_size();
                    const auto relative = it->path().lexically_relative(root).generic_u8string();
                    const auto key = keyOf(std::string(relative.begin(), relative.end()));
                    if (!key || size > UInt32(std::numeric_limits<Int32>::max())) {
                        ok = false;
                        return;
                    }
                    // Case-colliding files on case-sensitive hosts have deterministic spelling order.
                    const auto path = std::filesystem::absolute(it->path()).lexically_normal();
                    const auto existing = rootFiles.find(*key);
                    if (existing == rootFiles.end() || path < existing->second.path) {
                        rootFiles.insert_or_assign(*key,
                                                   Impl::File{path, static_cast<UInt32>(size)});
                    }
                }
                ++it;
            }
            files.merge(rootFiles); // earlier roots retain precedence
        });
        if (!ok) {
            return false;
        }
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
    impl_->files.swap(files);
    impl_->cache.clear();
    return true;
}

bool ResourceManager::addVolume(std::string_view name) {
    const auto key = keyOf(name);
    if (!key) {
        return false;
    }
    const auto old = std::ranges::find_if(impl_->volumes, [&](const auto& v) {
        return v->name == *key;
    });
    if (old != impl_->volumes.end()) {
        ++(*old)->refs;
        return true;
    }
    const auto file = impl_->files.find(*key);
    if (file == impl_->files.end()) {
        return false;
    }
    auto volume = std::make_unique<Impl::Volume>();
    volume->name = *key;
    volume->path = file->second.path;
    if (!volume->stream.openVolume(volume->path)) {
        return false;
    }
    const auto entries = volume->stream.getVolumeItems();
    std::ranges::for_each(std::views::iota(std::size_t{0}, entries.size()), [&](auto index) {
        if (entries[index].name) {
            const auto entryKey = keyOf(*entries[index].name);
            if (entryKey) {
                volume->entries.try_emplace(*entryKey, index);
            }
        }
    });
    impl_->volumes.insert(impl_->volumes.begin(), std::move(volume));
    impl_->cache.clear();
    return true;
}

bool ResourceManager::removeVolume(std::string_view name) {
    const auto key = keyOf(name);
    if (!key) {
        return false;
    }
    const auto it = std::ranges::find_if(impl_->volumes, [&](const auto& v) {
        return v->name == *key;
    });
    if (it == impl_->volumes.end()) {
        return false;
    }
    if (--(*it)->refs == 0) {
        impl_->volumes.erase(it);
        impl_->cache.clear();
    }
    return true;
}

bool ResourceManager::contains(std::string_view name) const {
    const auto key = keyOf(name);
    return key && (impl_->files.contains(*key) || impl_->volumeFor(*key) != impl_->volumes.end() ||
                   impl_->added.contains(*key));
}

std::optional<UInt32> ResourceManager::getSize(std::string_view name) const {
    const auto key = keyOf(name);
    if (!key) {
        return {};
    }
    if (const auto it = impl_->files.find(*key); it != impl_->files.end()) {
        return it->second.size;
    }
    const auto volume = impl_->volumeFor(*key);
    if (volume == impl_->volumes.end()) {
        return {};
    }
    return (*volume)->stream.getVolumeItems()[(*volume)->entries.at(*key)].size;
}

std::optional<std::filesystem::path> ResourceManager::getFullPath(std::string_view name) const {
    const auto key = keyOf(name);
    if (!key) {
        return {};
    }
    if (const auto it = impl_->files.find(*key); it != impl_->files.end()) {
        return it->second.path;
    }
    const auto volume = impl_->volumeFor(*key);
    return volume == impl_->volumes.end() ? std::nullopt : std::optional((*volume)->path);
}

std::vector<std::string> ResourceManager::findMatches(std::string_view expression) const {
    std::set<std::string> names;
    auto collect = [&](const auto& entries) {
        std::ranges::for_each(entries, [&](const auto& entry) {
            names.insert(entry.first);
        });
    };
    collect(impl_->files);
    collect(impl_->added);
    std::ranges::for_each(impl_->volumes, [&](const auto& v) {
        collect(v->entries);
    });
    const auto pattern = fold(expression);
    std::vector<std::string> result;
    std::ranges::copy_if(names, std::back_inserter(result), [&](const auto& name) {
        return matches(pattern, name);
    });
    return result;
}

bool ResourceManager::read(std::string_view name, std::vector<std::byte>& bytes, UInt32 maxBytes) {
    const auto key = keyOf(name);
    if (!key) {
        return false;
    }
    std::vector<std::byte> result;
    if (const auto file = impl_->files.find(*key); file != impl_->files.end()) {
        FileRStream stream(file->second.path);
        if (stream.getStatus() != STRM_OK) {
            return false;
        }
        const auto size = stream.getSize();
        if (size > maxBytes) {
            return false;
        }
        result.resize(size);
        if (!stream.readBytes(result)) {
            return false;
        }
    } else {
        const auto volume = impl_->volumeFor(*key);
        if (volume == impl_->volumes.end() ||
            !(*volume)->stream.openEntry((*volume)->entries.at(*key), maxBytes)) {
            return false;
        }
        const auto data = (*volume)->stream.data();
        result.assign(data.begin(), data.end());
        (*volume)->stream.close();
    }
    bytes.swap(result);
    return true;
}

std::unique_ptr<StreamIO> ResourceManager::openStream(std::string_view name, UInt32 maxBytes) {
    std::vector<std::byte> bytes;
    if (!read(name, bytes, maxBytes)) {
        return {};
    }
    return std::make_unique<OwnedStream>(std::move(bytes));
}

bool ResourceManager::getCrc(std::string_view name, UInt32& crc, UInt32 initial) {
    std::vector<std::byte> bytes;
    if (!read(name, bytes)) {
        return false;
    }
    crc = CRC::calculate(bytes, initial);
    return true;
}

bool ResourceManager::registerErased(UInt32 id,
                                     std::type_index type,
                                     Loader loader,
                                     std::chrono::milliseconds timeout) {
    if (id == kInvalidResourceId || timeout.count() < 0 || !loader) {
        return false;
    }
    return impl_->loaders.emplace(id, Impl::Factory{type, std::move(loader), timeout}).second;
}

bool ResourceManager::addErased(std::string_view name,
                                std::type_index type,
                                std::unique_ptr<detail::ResourcePayload> payload,
                                std::chrono::milliseconds timeout) {
    const auto key = keyOf(name);
    if (!key || timeout.count() < 0 || contains(*key)) {
        return false;
    }
    auto state = std::make_shared<detail::ResourceState>();
    state->name = *key;
    state->type = type;
    state->payload = std::move(payload);
    state->timeout = timeout;
    impl_->added.emplace(*key, std::move(state));
    return true;
}

std::shared_ptr<detail::ResourceState> ResourceManager::loadErased(std::string_view name,
                                                                   std::type_index type) {
    const auto key = keyOf(name);
    if (!key) {
        return {};
    }
    if (const auto found = impl_->cache.find(*key); found != impl_->cache.end()) {
        return found->second->type == type ? found->second : nullptr;
    }
    // Added resources remain a fallback if new disk paths subsequently shadow them.
    if (!impl_->files.contains(*key) && impl_->volumeFor(*key) == impl_->volumes.end()) {
        const auto added = impl_->added.find(*key);
        return added != impl_->added.end() && added->second->type == type ? added->second : nullptr;
    }
    const auto factory = impl_->loaders.find(resourceType(*key));
    if (factory == impl_->loaders.end() || factory->second.type != type) {
        return {};
    }
    if (!impl_->loading.insert(*key).second) {
        return {};
    }

    struct LoadingGuard {
        decltype(impl_->loading)& loading;
        const std::string& key;

        ~LoadingGuard() {
            loading.erase(key);
        }
    } guard{impl_->loading, *key};

    std::vector<std::byte> bytes;
    if (!read(*key, bytes)) {
        return {};
    }
    MemRWStream stream{std::span<const std::byte>(bytes)};
    auto payload = factory->second.load(stream, static_cast<UInt32>(bytes.size()));
    if (!payload || stream.getStatus() != STRM_OK) {
        return {};
    }
    auto state = std::make_shared<detail::ResourceState>();
    state->name = *key;
    state->type = type;
    state->payload = std::move(payload);
    state->timeout = factory->second.timeout;
    impl_->cache.emplace(*key, state);
    return state;
}

bool ResourceManager::evict(std::string_view name) {
    const auto key = keyOf(name);
    if (!key) {
        return false;
    }
    const auto found = impl_->cache.find(*key);
    if (found == impl_->cache.end() || found->second.use_count() != 1) {
        return false;
    }
    impl_->cache.erase(found);
    return true;
}

std::size_t ResourceManager::purge(bool force) {
    std::size_t total = 0, count;
    do { // destruction may release dependency handles visited earlier in the map
        const auto now = std::chrono::steady_clock::now();
        const auto remove = [&](const auto& item) {
            return item.second.use_count() == 1 &&
                   (force || now - item.second->released >= item.second->timeout);
        };
        count = std::erase_if(impl_->cache, remove) + std::erase_if(impl_->added, remove);
        total += count;
    } while (force && count != 0);
    return total;
}
} // namespace opent1
