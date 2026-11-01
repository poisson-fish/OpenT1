#pragma once
#include <opent1/console/eval.hpp>
#include <opent1/console/tag_dictionary.hpp>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace opent1 {
class ResourceManager;

namespace script {
class Evaluator;
}

// Engine seam. Optional names/indices preserve legacy null versus empty.
// Returned strings are owned. The runtime balances completed beginObject calls
// when evaluation fails, using the same manager that began each object.
class ConsoleDataManager {
public:
    virtual ~ConsoleDataManager() = default;
    virtual void declareDataBlock(std::string_view className, std::string_view name);
    virtual void endDataBlock();
    virtual void abortDataBlock(); // called on field or commit failure after declaration succeeds
    virtual void beginObject(std::string_view className, std::optional<std::string_view> name);
    virtual void endObject();
    virtual void abortObject(); // default balances endObject; transactional adapters discard
    virtual std::string getDataField(std::string_view object,
                                     std::string_view field,
                                     std::optional<std::string_view> index);
    virtual void setDataField(std::optional<std::string_view> object,
                              std::string_view field,
                              std::optional<std::string_view> index,
                              std::span<const std::string> values);
};

// Instance-owned runtime. Independent consoles may run concurrently; each
// console and its resources require external synchronization. Reentrant calls
// on the same thread are supported. All inputs stop at their first NUL.
class Console {
public:
    // argv[0] is the registered name; remaining arguments are evaluated left
    // to right. nullopt maps to False in scripts, True for direct execute.
    using Command =
        std::function<std::optional<std::string>(Console&, Int32, std::span<const std::string>)>;
    using VariableGetter = std::function<std::string()>;
    using VariableSetter = std::function<void(std::string_view)>;
    using Output = std::function<void(std::string_view)>;
    using ScriptLoader = std::function<std::optional<std::string>(std::string_view)>;

    // A supplied resource manager must outlive this console.
    explicit Console(ResourceManager* resources = nullptr, script::ExecutionLimits limits = {});
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    [[nodiscard]] script::ExecutionResult evaluate(std::string_view source,
                                                   std::string_view sourceName = "<script>");
    [[nodiscard]] script::ExecutionResult execute(std::string_view name,
                                                  std::span<const std::string> arguments = {});
    [[nodiscard]] script::ExecutionResult executeFile(std::string_view name);
    void addCommand(Int32 id, std::string_view name, Command command);
    // An exclusive registration lease. Releasing it removes only this exact
    // registration, and is safe after replacement or Console destruction.
    [[nodiscard]] std::shared_ptr<void> scopedCommand(Int32, std::string_view, Command);
    bool removeCommand(std::string_view name);
    [[nodiscard]] bool isFunction(std::string_view name) const;
    // Native StringTableEntry fields share script identifiers' first spelling.
    [[nodiscard]] std::string internString(std::string_view text);
    // Host variable APIs always address globals and supply an omitted '$'.
    void setVariable(std::string_view name, std::string_view value);
    [[nodiscard]] std::string getVariable(std::string_view name) const;
    [[nodiscard]] bool hasVariable(std::string_view name) const;
    bool removeVariable(std::string_view name);
    // Getters/setters replace legacy void* bindings. Captured references must
    // outlive the binding. Existing script values are applied on registration.
    void bindVariable(std::string_view name, VariableGetter get, VariableSetter set);
    void bindVariable(std::string_view name, bool& value);
    void bindVariable(std::string_view name, Int32& value);
    void bindVariable(std::string_view name, float& value);
    void bindVariable(std::string_view name, double& value);
    void setDataManager(std::shared_ptr<ConsoleDataManager> manager);
    void setScriptLoader(ScriptLoader loader);
    void setFileRoot(std::filesystem::path root);
    [[nodiscard]] std::filesystem::path filePath(std::string_view name) const;
    void setOutput(Output output);
    // Keep the returned token alive to subscribe. Releasing it detaches safely,
    // even after Console destruction; setOutput remains an independent sink.
    [[nodiscard]] std::shared_ptr<Output> subscribeOutput(Output);
    void print(std::string_view line, Int32 level = 1);
    [[nodiscard]] const std::vector<std::string>& log() const noexcept;
    void clearLog();
    [[nodiscard]] TagDictionary& tags() noexcept;
    [[nodiscard]] const script::ExecutionLimits& limits() const noexcept;

private:
    friend class script::Evaluator;
    friend script::ExecutionResult script::evaluate(Console&, script::Program);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace opent1
