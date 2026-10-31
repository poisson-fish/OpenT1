#include <opent1/core/registry.hpp>
#include <opent1/core/file_stream.hpp>

namespace opent1 {
namespace {
constexpr UInt32 magic = makeFourCC('O', 'T', 'R', 'G');
constexpr UInt32 maxRecords = 65536, maxText = 1024 * 1024, maxBudget = 16 * maxText;

std::string fold(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    return result;
}

std::optional<std::string> normalize(std::string_view key) {
    if (key.empty() || key.size() > 4096 || key.find('\0') != key.npos) {
        return {};
    }
    auto path = fold(key);
    std::ranges::replace(path, '\\', '/');
    auto parts = path | std::views::split('/');
    if (std::ranges::any_of(parts, [](auto part) {
            const std::string_view text(part.begin(), part.end());
            return text.empty() || text == "." || text == "..";
        })) {
        return {};
    }
    return path;
}

bool validText(std::string_view text, std::size_t limit = maxText) {
    return text.size() <= limit && text.find('\0') == text.npos;
}

bool readText(StreamIO& stream, std::string& text, UInt32& budget) {
    UInt32 size{};
    if (!stream.read(size) || size == 0 || size > std::min(maxText + 1, budget)) {
        return false;
    }
    std::string value(size, '\0');
    if (!stream.readBytes(std::as_writable_bytes(std::span(value))) || value.back() != '\0' ||
        value.find('\0') != size - 1) {
        return false;
    }
    value.pop_back();
    text = std::move(value);
    budget -= size;
    return true;
}
}

bool Registry::open(std::string_view key, bool create) {
    close();
    const auto path = normalize(key);
    if (!path) {
        return false;
    }
    if (create) {
        keys_.try_emplace(*path);
        auto end = path->find('/');
        while (end != path->npos) { // hierarchical path parsing
            keys_.try_emplace(path->substr(0, end));
            end = path->find('/', end + 1);
        }
    }
    if (!keys_.contains(*path)) {
        return false;
    }
    current_ = *path;
    return true;
}

bool Registry::remove(std::string_view subkey) {
    const auto child = normalize(subkey);
    if (!current_ || !child) {
        return false;
    }
    const auto path = *current_ + '/' + *child;
    const auto count = std::erase_if(keys_, [&](const auto& entry) {
        return entry.first == path || entry.first.starts_with(path + '/');
    });
    return count != 0;
}

std::vector<std::string> Registry::enumerateKeys() const {
    std::vector<std::string> result;
    if (!current_) {
        return result;
    }
    const auto prefix = *current_ + '/';
    std::ranges::for_each(keys_, [&](const auto& key) {
        if (key.first.starts_with(prefix)) {
            const auto name = key.first.substr(prefix.size());
            if (name.find('/') == name.npos) {
                result.push_back(name);
            }
        }
    });
    return result;
}

std::vector<std::string> Registry::enumerateTags() const {
    std::vector<std::string> result;
    if (current_) {
        std::ranges::transform(
            keys_.at(*current_), std::back_inserter(result), [](const auto& tag) {
                return tag.first;
            });
    }
    return result;
}

bool Registry::set(std::string_view tag, Value value) {
    if (!current_ || !validText(tag, 4096)) {
        return false;
    }
    const bool valid = std::visit(
        [](const auto& data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::same_as<T, std::string>) {
                return validText(data);
            } else if constexpr (std::same_as<T, std::vector<std::string>>) {
                return data.size() <= maxRecords && std::ranges::all_of(data, [](const auto& s) {
                           return validText(s);
                       });
            } else if constexpr (std::same_as<T, std::vector<std::byte>>) {
                return data.size() <= maxText;
            } else {
                return true;
            }
        },
        value);
    if (!valid) {
        return false;
    }
    keys_.at(*current_).insert_or_assign(fold(tag), std::move(value));
    return true;
}

std::optional<Registry::Value> Registry::get(std::string_view tag) const {
    if (!current_ || !validText(tag, 4096)) {
        return {};
    }
    const auto& tags = keys_.at(*current_);
    const auto it = tags.find(fold(tag));
    return it == tags.end() ? std::nullopt : std::optional<Value>(it->second);
}

bool Registry::store(StreamIO& stream) const {
    UInt64 budget = 0, records = 0;
    std::ranges::for_each(keys_, [&](const auto& entry) {
        budget += entry.first.size() + 1;
        records += entry.second.size();
        std::ranges::for_each(entry.second, [&](const auto& tag) {
            budget += tag.first.size() + 1;
            std::visit(
                [&](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::same_as<T, std::string>) {
                        budget += data.size() + 1;
                    } else if constexpr (std::same_as<T, std::vector<std::byte>>) {
                        budget += data.size();
                    } else if constexpr (std::same_as<T, std::vector<std::string>>) {
                        records += data.size();
                        std::ranges::for_each(data, [&](const auto& s) {
                            budget += s.size() + 1;
                        });
                    }
                },
                tag.second);
        });
    });
    if (budget > maxBudget || records > maxRecords) {
        return false;
    }
    if (keys_.size() > maxRecords || !stream.write(magic) || !stream.write(UInt32{1}) ||
        !stream.write(static_cast<UInt32>(keys_.size()))) {
        return false;
    }
    bool ok = true;
    std::ranges::for_each(keys_, [&](const auto& entry) {
        if (!ok) {
            return;
        }
        ok = entry.second.size() <= maxRecords && stream.write(entry.first) &&
             stream.write(static_cast<UInt32>(entry.second.size()));
        std::ranges::for_each(entry.second, [&](const auto& tag) {
            if (!ok) {
                return;
            }
            ok = stream.write(tag.first) && stream.write(static_cast<UInt8>(tag.second.index()));
            if (!ok) {
                return;
            }
            ok = std::visit(
                [&](const auto& data) -> bool {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::same_as<T, std::vector<std::byte>>) {
                        return stream.write(static_cast<UInt32>(data.size())) &&
                               stream.writeBytes(data);
                    } else if constexpr (std::same_as<T, std::vector<std::string>>) {
                        return stream.write(static_cast<UInt32>(data.size())) &&
                               std::ranges::all_of(data, [&](const auto& s) {
                                   return stream.write(s);
                               });
                    } else {
                        return stream.write(data);
                    }
                },
                tag.second);
        });
    });
    return ok && stream.getStatus() == STRM_OK;
}

bool Registry::load(StreamIO& stream) {
    decltype(keys_) keys;
    UInt32 id{}, version{}, count{}, budget = maxBudget, records = 0;
    if (!stream.read(id) || id != magic || !stream.read(version) || version != 1 ||
        !stream.read(count) || count > maxRecords) {
        return false;
    }
    bool ok = true;
    std::ranges::for_each(std::views::iota(0U, count), [&](auto) {
        if (!ok) {
            return;
        }
        std::string key;
        UInt32 tags{};
        ok = readText(stream, key, budget) && normalize(key) == std::optional(key) &&
             !keys.contains(key) && stream.read(tags) && tags <= maxRecords - records;
        if (!ok) {
            return;
        }
        records += tags;
        auto& values = keys[key];
        std::ranges::for_each(std::views::iota(0U, tags), [&](auto) {
            if (!ok) {
                return;
            }
            std::string tag;
            UInt8 type{};
            ok = readText(stream, tag, budget) && tag.size() <= 4096 && tag == fold(tag) &&
                 !values.contains(tag) && stream.read(type);
            if (!ok) {
                return;
            }
            Value value;
            switch (type) {
                case 0: {
                    std::string s;
                    ok = readText(stream, s, budget);
                    value = std::move(s);
                    break;
                }
                case 1: {
                    UInt32 size{};
                    ok = stream.read(size) && size <= maxRecords - records;
                    if (!ok) {
                        break;
                    }
                    records += size;
                    std::vector<std::string> strings;
                    std::ranges::for_each(std::views::iota(0U, size), [&](auto) {
                        if (!ok) {
                            return;
                        }
                        std::string s;
                        ok = readText(stream, s, budget);
                        strings.push_back(std::move(s));
                    });
                    value = std::move(strings);
                    break;
                }
                case 2: {
                    Int32 n{};
                    ok = stream.read(n);
                    value = n;
                    break;
                }
                case 3: {
                    Flt32 f{};
                    ok = stream.read(f);
                    value = f;
                    break;
                }
                case 4: {
                    UInt32 size{};
                    ok = stream.read(size) && size <= std::min(maxText, budget);
                    if (!ok) {
                        break;
                    }
                    std::vector<std::byte> bytes(size);
                    budget -= size;
                    ok = stream.readBytes(bytes);
                    value = std::move(bytes);
                    break;
                }
                default:
                    ok = false;
                    break;
            }
            if (ok) {
                values.emplace(std::move(tag), std::move(value));
            }
        });
    });
    if (!ok) {
        return false;
    }
    if (!std::ranges::all_of(keys, [&](const auto& entry) {
            const auto at = entry.first.rfind('/');
            return at == std::string::npos || keys.contains(entry.first.substr(0, at));
        })) {
        return false;
    }
    keys_.swap(keys);
    close();
    return true;
}

bool Registry::fileStore(const std::filesystem::path& path) const {
    FileWStream stream(path);
    return stream.getStatus() == STRM_OK && store(stream) && stream.flush();
}

bool Registry::fileLoad(const std::filesystem::path& path) {
    FileRStream stream(path);
    return stream.getStatus() == STRM_OK && load(stream);
}
} // namespace opent1
