#pragma once
#include <opent1/console/console.hpp>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <fstream>

namespace opent1::script {
std::string_view cText(std::string_view text) noexcept;
std::string canonical(std::string_view text);
double parseNumber(std::string_view text) noexcept;
std::int32_t parseInteger(std::string_view text) noexcept;

struct RuntimeFault {
    Diagnostic diagnostic;
};

class Evaluator {
public:
    static ExecutionResult run(Console&, std::function<void()> action);
    static void program(Console&, std::shared_ptr<const Program>);
    static void call(Console&, std::string_view, std::vector<std::string>, bool fromScript);

private:
    enum class Mode { Text, Number, Integer };
    enum class Flow { Normal, Break, Continue, Return };

    struct Value {
        std::variant<std::int32_t, double, std::string> data;
        std::string text() const;
        double number() const;
        std::int32_t integer() const;
    };

    Console& console_;
    std::shared_ptr<const Program> program_;
    std::unordered_set<NodeId> activeExpressions_, activeStatements_;

    Evaluator(Console& console, std::shared_ptr<const Program> program)
        : console_(console), program_(std::move(program)) {}

    Value expression(NodeId, Mode = Mode::Text);
    Value value(NodeId);
    int flags(NodeId) const;
    double compare(NodeId, NodeId);
    Value binary(TokenKind, NodeId, NodeId);
    std::string variable(const VariableExpr&);
    std::string joined(std::string_view, std::string_view, std::string_view separator = "");
    std::optional<std::string> index(NodeId);
    void fields(std::optional<std::string_view>,
                std::span<const NodeId>,
                const std::shared_ptr<ConsoleDataManager>&);
    Flow statement(NodeId);
    Flow block(std::span<const NodeId>);
};
} // namespace opent1::script

namespace opent1 {
struct Console::Impl {
    struct Variable {
        std::string value;
        VariableGetter get;
        VariableSetter set;
    };

    struct Function {
        std::shared_ptr<const script::Program> program;
        script::NodeId node;
    };

    struct Entry {
        std::string name;
        Int32 id = 0;
        Command command;
        std::optional<Function> function;
        UInt64 registration{};
    };

    struct Frame {
        std::string name;
        std::unordered_map<std::string, std::string> variables;
    };

    script::ExecutionLimits limits;
    ResourceManager* resources;
    ScriptLoader loader;
    Output output;
    std::vector<std::weak_ptr<Output>> outputs;
    std::shared_ptr<ConsoleDataManager> data = std::make_shared<ConsoleDataManager>();
    TagDictionary tags;
    std::unordered_map<std::string, Variable> globals;
    std::unordered_map<std::string, Entry> commands;
    std::shared_ptr<const int> commandLifetime{std::make_shared<const int>(0)};
    UInt64 nextRegistration{1};
    std::unordered_map<std::string, std::string> names;
    std::vector<Frame> frames;
    std::vector<std::string> log;
    std::vector<script::Diagnostic> diagnostics;
    std::string result;
    std::string sourceName = "<script>";
    script::SourceRange range;
    std::size_t steps = 0, depth = 0, invocations = 0;
    bool logEnabled = true, metrics = false;
    Int32 printLevel = 1;
    Int32 logMode = 0;
    bool newLogFile = false;
    std::ofstream logFile;
    void setLogMode(Int32);
    void logToFile(std::string_view);

    explicit Impl(ResourceManager* r, script::ExecutionLimits l) : limits(l), resources(r) {}

    [[noreturn]] void fail(script::DiagnosticCode code, std::string message) const;
    void check(std::string_view text) const;
    void step();
    std::string intern(std::string_view name);
    std::string read(std::string_view name) const;
    void write(std::string_view name, std::string_view value);
    void builtins(Console&);
};
} // namespace opent1
