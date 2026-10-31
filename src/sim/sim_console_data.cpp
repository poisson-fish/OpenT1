#include <opent1/sim/sim_world_game.hpp>
#include <opent1/core/file_stream.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace opent1::sim {
namespace {
std::string folded(std::string_view text) {
    std::string value(text);
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return value;
}
}

// A declaration stays unregistered until all fields and nested objects succeed.
// The owning batch is committed together, so onAdd observes initialized fields
// and the complete named hierarchy. Game data-block classes are supplied by
// an instance-owned provider; simulation objects retain name lookup priority.
class SimConsoleDataManager final : public ConsoleDataManager {
public:
    explicit SimConsoleDataManager(SimGame& game) : game_(game) {}

    void declareDataBlock(std::string_view className, std::string_view name) override {
        const auto provider = game_.dataBlocks_;
        if (!provider) {
            throw std::invalid_argument("No game data-block manager installed");
        }
        if (className.empty() || className.size() > 255 || name.empty() || name.size() > 255 ||
            className.find('\0') != className.npos || name.find('\0') != name.npos) {
            throw std::invalid_argument("Invalid data-block class or name");
        }
        if (game_.declarationDepth_ >= 64) {
            throw std::length_error("simulation declaration limit");
        }
        declarations_.push_back(provider);
        ++game_.declarationDepth_;
        try {
            game_.withWorlds([&] {
                provider->declareDataBlock(className, name);
            });
        } catch (...) {
            declarations_.pop_back();
            --game_.declarationDepth_;
            throw;
        }
    }

    void endDataBlock() override {
        if (declarations_.empty()) {
            return;
        }
        const auto provider = declarations_.back();
        // Leave the entry active on a failed commit so evaluator abort can
        // roll it back through the exact provider that began the declaration.
        game_.withWorlds([&] {
            provider->endDataBlock();
        });
        declarations_.pop_back();
        --game_.declarationDepth_;
    }

    void abortDataBlock() override {
        if (declarations_.empty()) {
            return;
        }
        const auto provider = declarations_.back();
        declarations_.pop_back();
        --game_.declarationDepth_;
        game_.withWorlds([&] {
            provider->abortDataBlock();
        });
    }

    void beginObject(std::string_view className, std::optional<std::string_view> name) override {
        if (game_.declarationDepth_ >= 64 || owned_.size() >= 65536) {
            throw std::length_error("simulation declaration limit");
        }
        if (stack_.empty()) {
            destination_ = game_.manager();
            failed_ = false;
        }
        if (!destination_) {
            throw std::invalid_argument("No focused simulation world");
        }
        owned_.reserve(owned_.size() + 1);
        stack_.reserve(stack_.size() + 1);
        ++game_.declarationDepth_;
        try {
            auto object = game_.makeObject(className);
            if (!object) {
                throw std::invalid_argument("Unknown simulation class");
            }
            object->assignName(name.value_or(""));
            if (!stack_.empty()) {
                auto* group = dynamic_cast<SimGroup*>(stack_.back());
                if (!group || !group->addObject(*object)) {
                    throw std::invalid_argument("Nested simulation objects require a group");
                }
            }
            auto* pointer = object.get();
            owned_.push_back(std::move(object));
            stack_.push_back(pointer);
        } catch (...) {
            --game_.declarationDepth_;
            throw;
        }
    }

    void endObject() override {
        if (stack_.empty()) {
            return;
        }
        auto* root = stack_.back();
        stack_.pop_back();
        --game_.declarationDepth_;
        if (stack_.empty()) {
            if (failed_) {
                owned_.clear();
                return;
            }
            // Keep world replacement disabled throughout registration callbacks.
            ++game_.declarationDepth_;

            struct Guard {
                std::size_t& n;

                ~Guard() {
                    --n;
                }
            } guard{game_.declarationDepth_};

            if (!destination_->adoptObjects(std::move(owned_), *root)) {
                throw std::runtime_error("Simulation declaration registration failed");
            }
            owned_.clear();
        }
    }

    void abortObject() override {
        failed_ = true;
        endObject();
    }

    std::string getDataField(std::string_view object,
                             std::string_view field,
                             std::optional<std::string_view> index) override {
        std::string value;
        game_.withWorlds([&] {
            value = getField(object, field, index);
        });
        return value;
    }

    void setDataField(std::optional<std::string_view> object,
                      std::string_view field,
                      std::optional<std::string_view> index,
                      std::span<const std::string> values) override {
        game_.withWorlds([&] {
            setField(object, field, index, values);
        });
    }

private:
    std::string getField(std::string_view object,
                         std::string_view field,
                         std::optional<std::string_view> index) {
        auto* target = find(object);
        if (!target) {
            const auto provider = game_.dataBlocks_;
            return provider && provider->contains(object)
                       ? provider->getDataField(object, field, index)
                       : "False";
        }
        const auto descriptor = findField(*target, field);
        if (descriptor) {
            const auto element = fieldIndex(*descriptor, index);
            if (!element) {
                return "False";
            }
            const auto value = descriptor->get(*target, *element, game_.dataFields_);
            return value.value_or("False");
        }
        // An unset dynamic slot is the empty string in the original console.
        // Scripts distinguish this from the literal boolean "False" (for
        // example, an unoccupied player's Station and a client's menu state).
        const auto value =
            target->getFieldValue(std::string(field) + std::string(index.value_or("")));
        return value ? std::string(*value) : std::string{};
    }

    void setField(std::optional<std::string_view> object,
                  std::string_view field,
                  std::optional<std::string_view> index,
                  std::span<const std::string> values) {
        if (values.empty()) {
            return;
        }
        auto* target = object ? find(*object) : stack_.empty() ? nullptr : stack_.back();
        if (!target) {
            const auto provider = game_.dataBlocks_;
            if (object && provider && provider->contains(*object)) {
                provider->setDataField(object, field, index, values);
            }
            return;
        }
        const auto descriptor = findField(*target, field);
        if (descriptor) {
            std::vector<std::string_view> args;
            std::ranges::transform(values, std::back_inserter(args), [](const auto& value) {
                return std::string_view(value);
            });
            if (!index && descriptor->elementCount > 1 &&
                values.size() == descriptor->elementCount) {
                if (!std::ranges::all_of(
                        std::views::iota(std::size_t{0}, values.size()), [&](auto i) {
                            return descriptor->set(
                                *target,
                                i,
                                game_.dataFields_,
                                std::span<const std::string_view>(args).subspan(i, 1));
                        })) {
                    throw std::invalid_argument("Invalid simulation array field value");
                }
                return;
            }
            const auto element = fieldIndex(*descriptor, index);
            if (!element || !descriptor->set(*target, *element, game_.dataFields_, args)) {
                throw std::invalid_argument("Invalid simulation field value or array index");
            }
        } else {
            const auto key = std::string(field) + std::string(index.value_or(""));
            if (key.size() > 1024) {
                throw std::length_error("simulation field name limit");
            }
            target->setFieldValue(key, values.front());
        }
    }

private:
    std::optional<Persistent::Field> findField(SimObject& object, std::string_view name) const {
        const auto fields = game_.classes_.fields(object);
        const auto key = folded(name);
        const auto found = std::ranges::find_if(fields, [&](const auto& f) {
            return folded(f.name) == key;
        });
        return found == fields.end() ? std::nullopt : std::optional(*found);
    }

    static std::optional<std::size_t> fieldIndex(const Persistent::Field& field,
                                                 std::optional<std::string_view> index) {
        if (!index) {
            return field.elementCount == 1 ? std::optional<std::size_t>(0) : std::nullopt;
        }
        std::size_t value{};
        const auto r = std::from_chars(index->data(), index->data() + index->size(), value);
        return r.ec == std::errc{} && r.ptr == index->data() + index->size() &&
                       value < field.elementCount
                   ? std::optional(value)
                   : std::nullopt;
    }

    SimObject* find(std::string_view name) {
        const auto key = folded(name);
        const auto found = std::ranges::find_if(owned_ | std::views::reverse, [&](const auto& p) {
            return folded(p->getName()) == key;
        });
        return found == owned_.rend() ? game_.findObject(name) : found->get();
    }

    SimGame& game_;
    SimManager* destination_{};
    bool failed_{};
    std::vector<std::unique_ptr<SimObject>> owned_;
    std::vector<SimObject*> stack_;
    std::vector<std::shared_ptr<SimDataBlockManager>> declarations_;
};

std::shared_ptr<ConsoleDataManager> makeSimConsoleDataManager(SimGame& game) {
    return std::make_shared<SimConsoleDataManager>(game);
}

std::unique_ptr<SimObject> SimGame::makeObject(std::string_view name) {
    const auto found = factories_.find(folded(name));
    if (found != factories_.end()) {
        const auto factory = found->second;
        return factory();
    }
    const auto* descriptor = classes_.find(name);
    if (!descriptor) {
        return {};
    }
    const auto factory = descriptor->create;
    auto object = factory();
    auto* sim = dynamic_cast<SimObject*>(object.get());
    if (!sim) {
        return {};
    }
    object.release();
    return std::unique_ptr<SimObject>(sim);
}

SimObject* SimGame::loadObject(std::string_view name, std::string_view filename) {
    auto* destination = manager();
    if (!destination || name.size() > 255 || name.find('\0') != name.npos || filename.empty() ||
        filename.find('\0') != filename.npos) {
        return nullptr;
    }
    std::unique_ptr<StreamIO> stream =
        std::make_unique<FileRStream>(std::filesystem::path(filename));
    if (stream->getStatus() != STRM_OK) {
        auto* resources = SimResource::find(*destination);
        stream = resources ? resources->get().openStream(filename) : nullptr;
    }
    if (!stream) {
        return nullptr;
    }
    return loadObjectStream(name, *stream, nullptr, -1, {});
}

SimObject* SimGame::loadObject(std::string_view name,
                               std::span<const std::byte> bytes,
                               SimSet* target,
                               const std::function<bool(SimObject&)>& accept) {
    if (bytes.empty() || bytes.size() > 16U * 1024U * 1024U) {
        return nullptr;
    }
    MemRWStream stream{bytes};
    return loadObjectStream(name, stream, target, Int32(bytes.size()), accept);
}

SimObject* SimGame::loadObjectStream(std::string_view name,
                                     StreamIO& stream,
                                     SimSet* target,
                                     Int32 exactBytes,
                                     const std::function<bool(SimObject&)>& accept) {
    auto* destination = manager();
    if (!destination || name.size() > 255 || name.find('\0') != name.npos ||
        (target && (target->getManager() != destination || target->isRemoved()))) {
        return nullptr;
    }
    const auto targetAddress = target ? target->address() : SimAddress{};
    // Reading/application factories may call the console; prevent world teardown.
    const auto previous = driving_;
    driving_ = true;

    struct Guard {
        bool& b;
        bool previous;

        ~Guard() {
            b = previous;
        }
    } guard{driving_, previous};

    Persistent::LoadContext context(classes_, true);
    const auto loaded = context.load(stream);
    if (!loaded || (exactBytes >= 0 && stream.getPosition() != exactBytes)) {
        return nullptr;
    }
    auto* root = dynamic_cast<SimObject*>(loaded.object);
    if (!root) {
        return nullptr;
    }
    std::vector<std::unique_ptr<SimObject>> objects;
    auto graph = context.takeObjects();
    if (!std::ranges::all_of(graph, [](const auto& p) {
            return dynamic_cast<SimObject*>(p.get()) != nullptr;
        })) {
        return nullptr;
    }
    objects.reserve(graph.size());
    std::ranges::for_each(graph, [&](auto& p) {
        auto* object = static_cast<SimObject*>(p.release());
        objects.emplace_back(object);
    });
    // Snapshot callbacks: a preparer can retire its own installation safely.
    // No object in this graph is registered until all resource work succeeds.
    const auto preparations = loadPreparations_;
    if (!preparations.empty()) {
        auto* resources = SimResource::find(*destination);
        if (!resources) {
            return nullptr;
        }
        try {
            for (const auto& object : objects) {
                for (const auto& [key, prepare] : preparations) {
                    (void)key;
                    if (!prepare(*object, resources->get())) {
                        return nullptr;
                    }
                }
            }
        } catch (...) {
            return nullptr;
        }
    }
    if (target) {
        target = dynamic_cast<SimSet*>(targetAddress.resolve(*destination));
        if (!target) {
            return nullptr;
        }
    }
    root->assignName(name);
    return destination->adoptObjects(std::move(objects), *root, target, accept);
}

std::optional<std::vector<std::byte>> SimGame::encodeObject(const SimObject& object, Int32 user) {
    // Serialize before opening the output so an unsupported class cannot truncate
    // an existing file. Block backpatches use the existing bounded memory stream.
    const auto previous = driving_;
    driving_ = true;

    struct Guard {
        bool& b;
        bool previous;

        ~Guard() {
            b = previous;
        }
    } guard{driving_, previous};

    std::vector<std::byte> bytes(16 * 1024 * 1024);
    MemRWStream output{std::span(bytes)};
    Persistent::StoreContext context(classes_, true);
    bool success{};
    const auto serialize = [&] {
        success = context.store(output, object, user) == Persistent::Error::Ok;
    };
    if (object.getManager()) {
        object.getManager()->dispatch(serialize);
    } else {
        serialize();
    }
    if (!success) {
        return {};
    }
    bytes.resize(std::size_t(output.getPosition()));
    return bytes;
}

bool SimGame::storeObject(const SimObject& object, const std::filesystem::path& path, Int32 user) {
    auto bytes = encodeObject(object, user);
    if (!bytes) {
        return false;
    }
    FileWStream file(path);
    return file.getStatus() == STRM_OK && file.writeBytes(*bytes) && file.flush();
}
}
