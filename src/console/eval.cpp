#include "runtime_internal.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace opent1::script {
namespace {
// A script may recurse as far as the executing thread's real stack permits.
// Leave room to construct diagnostics and unwind instead of overflowing into
// the guard page. This is independent of configurable script depth budgets.
bool stackAvailable() {
    const char marker{};
    const auto current = reinterpret_cast<std::uintptr_t>(&marker);
    std::uintptr_t lower{}, upper{};
#if defined(_WIN32)
    ULONG_PTR stackLow{}, stackHigh{};
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    lower = stackLow;
    upper = stackHigh;
#elif defined(__linux__)
    struct Bounds {
        std::uintptr_t low{}, high{};
    };

    static thread_local const Bounds bounds = [] {
        Bounds result;
        pthread_attr_t attributes;
        if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
            void* address{};
            std::size_t size{};
            if (pthread_attr_getstack(&attributes, &address, &size) == 0) {
                result.low = reinterpret_cast<std::uintptr_t>(address);
                result.high = result.low + size;
            }
            pthread_attr_destroy(&attributes);
        }
        return result;
    }();
    lower = bounds.low;
    upper = bounds.high;
#elif defined(__APPLE__)
    upper = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    lower = upper - pthread_get_stacksize_np(pthread_self());
#endif
    return !lower || current < lower || current >= upper || current - lower > 128U * 1024U;
}

template <class Fn> struct OnExit {
    Fn fn;

    ~OnExit() {
        fn();
    }
};

std::int32_t integer(double n) {
    return std::isfinite(n) && n >= -2147483648.0 && n < 2147483648.0
               ? static_cast<std::int32_t>(n)
               : std::numeric_limits<std::int32_t>::min();
}

std::int32_t bits(std::uint32_t n) {
    return std::bit_cast<std::int32_t>(n);
}

std::int32_t intOp(TokenKind op, std::int32_t a, std::int32_t b) {
    const auto ua = std::bit_cast<std::uint32_t>(a), ub = std::bit_cast<std::uint32_t>(b);
    const auto count = ub & 31;
    switch (static_cast<int>(op)) {
        case '%':
            return b == 0 || (a == std::numeric_limits<std::int32_t>::min() && b == -1) ? 0 : a % b;
        case '&':
            return bits(ua & ub);
        case '|':
            return bits(ua | ub);
        case '^':
            return bits(ua ^ ub);
        case static_cast<int>(TokenKind::ShiftLeft):
            return bits(ua << count);
        case static_cast<int>(TokenKind::ShiftRight):
            return bits(count == 0
                            ? ua
                            : ((ua >> count) | (a < 0 ? (~std::uint32_t{0} << (32 - count)) : 0)));
        default:
            throw std::invalid_argument("Invalid integer operation");
    }
}

double floatOp(TokenKind op, double a, double b) {
    switch (static_cast<int>(op)) {
        case '+':
            return a + b;
        case '-':
            return a - b;
        case '*':
            return a * b;
        case '/':
            return a / b;
        default:
            throw std::invalid_argument("Invalid floating operation");
    }
}

bool floating(TokenKind op) {
    return op == punctuation('+') || op == punctuation('-') || op == punctuation('*') ||
           op == punctuation('/');
}

std::optional<std::string_view> view(const std::optional<std::string>& value) {
    return value ? std::optional<std::string_view>(*value) : std::nullopt;
}
} // namespace

std::string Evaluator::Value::text() const {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return v;
            } else if constexpr (std::is_same_v<T, double>) {
                return formatNumber(v);
            } else {
                return std::to_string(v);
            }
        },
        data);
}

double Evaluator::Value::number() const {
    return std::visit(
        [](const auto& v) -> double {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) {
                return toNumber(v);
            } else {
                return v;
            }
        },
        data);
}

std::int32_t Evaluator::Value::integer() const {
    return std::visit(
        [](const auto& v) -> std::int32_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return toInteger(v);
            } else if constexpr (std::is_same_v<T, double>) {
                return script::integer(v);
            } else {
                return v;
            }
        },
        data);
}

ExecutionResult Evaluator::run(Console& c, std::function<void()> action) {
    auto& s = *c.impl_;
    const bool outer = s.invocations == 0;
    if (outer) {
        s.steps = 0;
        s.result.clear();
        s.diagnostics.clear();
        s.sourceName = "<script>";
        s.range = {};
    }
    const auto firstDiagnostic = s.diagnostics.size();
    ++s.invocations;
    const OnExit leave{[&] {
        --s.invocations;
    }};
    try {
        if (s.depth >= s.limits.maxDepth || !stackAvailable()) {
            s.fail(DiagnosticCode::ExecutionLimit,
                   "script call depth limit or native stack capacity reached");
        }
        ++s.depth;
        const OnExit depth{[&] {
            --s.depth;
        }};
        action();
    } catch (const RuntimeFault& fault) {
        s.diagnostics.push_back(fault.diagnostic);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        s.diagnostics.push_back({DiagnosticCode::CommandError, s.sourceName, s.range, ex.what()});
    }
    return {s.result,
            {s.diagnostics.begin() + static_cast<std::ptrdiff_t>(firstDiagnostic),
             s.diagnostics.end()}};
}

ExecutionResult evaluate(Console& c, Program program) {
    return Evaluator::run(c, [&] {
        Evaluator::program(c, std::make_shared<const Program>(std::move(program)));
    });
}

void Evaluator::program(Console& c, std::shared_ptr<const Program> p) {
    auto& s = *c.impl_;
    const auto previous = s.sourceName;
    s.sourceName = p->sourceName;
    const OnExit source{[&] {
        s.sourceName = previous;
    }};
    // Original scanner interns all identifiers before execution. Programs stay
    // immutable while the console retains instance-wide first spellings.
    std::ranges::for_each(p->nodes, [&](const auto& node) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (requires { n.name; }) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(n.name)>, std::string>) {
                        s.intern(n.name);
                    }
                }
                if constexpr (requires { n.className; }) {
                    s.intern(n.className);
                }
                if constexpr (std::is_same_v<T, FunctionStmt>) {
                    std::ranges::for_each(n.parameters, [&](const auto& name) {
                        s.intern(name);
                    });
                }
            },
            node.data);
    });
    Evaluator evaluator{c, std::move(p)};
    // Legacy evaluate ignores top-level return/break/continue flow codes.
    std::ranges::for_each(evaluator.program_->statements, [&](NodeId id) {
        (void)evaluator.statement(id);
    });
}

void Evaluator::call(Console& c,
                     std::string_view name,
                     std::vector<std::string> args,
                     bool fromScript) {
    auto& s = *c.impl_;
    s.step();
    if (s.depth >= s.limits.maxDepth || !stackAvailable()) {
        s.fail(DiagnosticCode::ExecutionLimit,
               "script call depth limit or native stack capacity reached");
    }
    ++s.depth;
    const OnExit depth{[&] {
        --s.depth;
    }};
    if (args.size() > s.limits.maxArguments) {
        s.fail(DiagnosticCode::ExecutionLimit, "command argument limit reached");
    }
    const auto found = s.commands.find(canonical(name));
    if (found == s.commands.end()) {
        c.print(std::string(cText(name)) + ": Unknown command.");
        s.result = "False";
        return;
    }
    // Copies allow a command/function to replace or delete itself safely.
    const auto entry = found->second;
    if (entry.function) {
        const auto fn = *entry.function;
        const auto& function = std::get<FunctionStmt>((*fn.program)[fn.node].data);
        s.frames.push_back({entry.name, {}});
        const OnExit frame{[&] {
            s.frames.pop_back();
        }};
        const auto count = std::min(args.size(), function.parameters.size());
        std::ranges::for_each(std::views::iota(std::size_t{0}, count), [&](auto i) {
            if (fromScript) {
                s.write(function.parameters[i], args[i]);
            } else if (!args[i].empty()) {
                s.frames.back().variables[canonical(function.parameters[i])] = args[i];
            }
        });
        const auto previous = s.sourceName;
        s.sourceName = fn.program->sourceName;
        const OnExit source{[&] {
            s.sourceName = previous;
        }};
        Evaluator evaluator{c, fn.program};
        (void)evaluator.block(function.body);
    } else {
        args.insert(args.begin(), entry.name);
        const auto result = entry.command(c, entry.id, args);
        s.result = result ? std::string(cText(*result)) : (fromScript ? "False" : "True");
        s.check(s.result);
    }
}

std::string Evaluator::joined(std::string_view a, std::string_view b, std::string_view separator) {
    auto& s = *console_.impl_;
    if (a.size() > s.limits.maxValueBytes || separator.size() > s.limits.maxValueBytes - a.size() ||
        b.size() > s.limits.maxValueBytes - a.size() - separator.size()) {
        s.fail(DiagnosticCode::ExecutionLimit, "concatenation exceeds byte limit");
    }
    return std::string(a) + std::string(separator) + std::string(b);
}

std::string Evaluator::variable(const VariableExpr& n) {
    const auto name = console_.impl_->intern(n.name);
    if (n.index == noNode) {
        return name;
    }
    const auto suffix = expression(n.index).text();
    return console_.impl_->intern(joined(name, suffix));
}

std::optional<std::string> Evaluator::index(NodeId id) {
    return id == noNode ? std::nullopt : std::optional(expression(id).text());
}

int Evaluator::flags(NodeId id) const {
    if (id >= program_->nodes.size()) {
        console_.impl_->fail(DiagnosticCode::InvalidProgram, "invalid expression node ID");
    }
    const auto& n = (*program_)[id].data;
    if (std::holds_alternative<IntegerExpr>(n)) {
        return 2;
    }
    if (std::holds_alternative<FloatExpr>(n)) {
        return 1;
    }
    if (const auto* binary = std::get_if<BinaryExpr>(&n)) {
        return binary->op == punctuation('@') || binary->op == TokenKind::Concat
                   ? 0
                   : (floating(binary->op) ? 1 : 2);
    }
    if (const auto* unary = std::get_if<UnaryExpr>(&n)) {
        return unary->op == punctuation('*') ? 0 : (unary->op == punctuation('-') ? 1 : 2);
    }
    return 0;
}

double Evaluator::compare(NodeId left, NodeId right) {
    const auto lFlags = flags(left), rFlags = flags(right);
    if ((lFlags & rFlags & 2) != 0) {
        const auto a = expression(left, Mode::Integer).integer();
        const auto b = expression(right, Mode::Integer).integer();
        return bits(std::bit_cast<std::uint32_t>(a) - std::bit_cast<std::uint32_t>(b));
    }
    if (((lFlags | rFlags) & 3) != 0) {
        const auto a = expression(left, Mode::Number).number();
        const auto b = expression(right, Mode::Number).number();
        return a - b;
    }
    const auto a = expression(left).text();
    const auto b = expression(right).text();
    bool boolCheck = false;
    const auto number = [&](const std::string& text) -> std::optional<float> {
        const auto folded = canonical(text);
        if (folded == "true" || folded == "false") {
            boolCheck = true;
            return folded == "true" ? 1.0f : 0.0f;
        }
        if (text.empty() || text.find_first_not_of("0123456789.eE-+") != text.npos) {
            return {};
        }
        return static_cast<float>(toNumber(text));
    };
    const auto na = number(a), nb = number(b);
    if (!na || !nb) {
        return a.compare(b);
    }
    if (boolCheck) {
        return (*na == 0) != (*nb == 0);
    }
    return static_cast<float>(*na - *nb);
}

Evaluator::Value Evaluator::binary(TokenKind op, NodeId left, NodeId right) {
    if (op == punctuation('@') || op == TokenKind::Concat) {
        const auto a = expression(left).text();
        const auto b = expression(right).text();
        return {joined(a, b)};
    }
    if (floating(op)) {
        const auto a = expression(left, Mode::Number).number();
        const auto b = expression(right, Mode::Number).number();
        return {floatOp(op, a, b)};
    }
    if (op == TokenKind::LogicalAnd || op == TokenKind::LogicalOr) {
        const bool a = expression(left, Mode::Number).number() != 0;
        const bool result = op == TokenKind::LogicalAnd
                                ? (a && expression(right, Mode::Number).number() != 0)
                                : (a || expression(right, Mode::Number).number() != 0);
        return {static_cast<std::int32_t>(result)};
    }
    if (op == punctuation('<') || op == punctuation('>') || op == TokenKind::Equal ||
        op == TokenKind::NotEqual || op == TokenKind::GreaterEqual || op == TokenKind::LessEqual) {
        const auto difference = compare(left, right);
        bool result = false;
        switch (static_cast<int>(op)) {
            case '<':
                result = difference < 0;
                break;
            case '>':
                result = difference > 0;
                break;
            case static_cast<int>(TokenKind::Equal):
                result = difference == 0;
                break;
            case static_cast<int>(TokenKind::NotEqual):
                result = difference != 0;
                break;
            case static_cast<int>(TokenKind::GreaterEqual):
                result = difference >= 0;
                break;
            case static_cast<int>(TokenKind::LessEqual):
                result = difference <= 0;
                break;
            default:
                break;
        }
        return {static_cast<std::int32_t>(result)};
    }
    if (op == punctuation('%')) {
        const auto b = expression(right, Mode::Integer).integer();
        return {b ? intOp(op, expression(left, Mode::Integer).integer(), b) : 0};
    }
    const auto a = expression(left, Mode::Integer).integer();
    const auto b = expression(right, Mode::Integer).integer();
    return {intOp(op, a, b)};
}

Evaluator::Value Evaluator::expression(NodeId id, Mode mode) {
    auto& s = *console_.impl_;
    if (id >= program_->nodes.size()) {
        s.fail(DiagnosticCode::InvalidProgram, "invalid expression node ID");
    }
    if (!activeExpressions_.insert(id).second) {
        s.fail(DiagnosticCode::InvalidProgram, "cyclic expression node");
    }
    const OnExit node{[&] {
        activeExpressions_.erase(id);
    }};
    s.step();
    if (s.depth >= s.limits.maxDepth || !stackAvailable()) {
        s.fail(DiagnosticCode::ExecutionLimit,
               "script expression depth limit or native stack capacity reached");
    }
    ++s.depth;
    const auto previous = s.range;
    s.range = (*program_)[id].range;
    const OnExit depth{[&] {
        --s.depth;
        s.range = previous;
    }};
    try {
        auto result = value(id);
        if (mode == Mode::Text || std::holds_alternative<std::string>(result.data)) {
            const auto text = result.text();
            s.result = cText(text);
            s.check(s.result);
        }
        if (mode == Mode::Text) {
            return {s.result};
        }
        if (mode == Mode::Number) {
            return {result.number()};
        }
        return {result.integer()};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        s.fail(DiagnosticCode::CommandError, ex.what());
    }
}

Evaluator::Value Evaluator::value(NodeId id) {
    auto& s = *console_.impl_;
    return std::visit(
        [&](const auto& n) -> Value {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, IntegerExpr> || std::is_same_v<T, FloatExpr> ||
                          std::is_same_v<T, StringExpr>) {
                return {n.value};
            } else if constexpr (std::is_same_v<T, ConstantExpr>) {
                return {s.intern(n.name)};
            } else if constexpr (std::is_same_v<T, VariableExpr>) {
                return {s.read(variable(n))};
            } else if constexpr (std::is_same_v<T, TagExpr>) {
                return {s.tags.defineToId(n.name)};
            } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                if (n.op == punctuation('-')) {
                    return {-expression(n.operand, Mode::Number).number()};
                }
                if (n.op == punctuation('!')) {
                    return {
                        static_cast<std::int32_t>(!expression(n.operand, Mode::Number).number())};
                }
                if (n.op == punctuation('~')) {
                    return {bits(~std::bit_cast<std::uint32_t>(
                        expression(n.operand, Mode::Integer).integer()))};
                }
                const auto tag = expression(n.operand, Mode::Integer).integer();
                return {std::string(s.tags.idToString(tag).value_or("<INVALID TAG>"))};
            } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                return binary(n.op, n.left, n.right);
            } else if constexpr (std::is_same_v<T, CommaExpr>) {
                const auto a = expression(n.left).text();
                const auto b = expression(n.right).text();
                return {joined(a, b, "_")};
            } else if constexpr (std::is_same_v<T, CallExpr>) {
                if (n.arguments.size() + (n.object != noNode) > s.limits.maxArguments) {
                    s.fail(DiagnosticCode::ExecutionLimit, "command argument limit reached");
                }
                std::vector<std::string> args;
                const auto previous = s.result;
                if (n.object != noNode) {
                    args.push_back(expression(n.object).text());
                }
                std::ranges::for_each(n.arguments, [&](NodeId arg) {
                    args.push_back(expression(arg).text());
                });
                s.result = args.empty() ? previous : args.front();
                const auto name = n.object == noNode ? n.name : "dot_op_" + n.name;
                call(console_, name, std::move(args), true);
                return {s.result};
            } else if constexpr (std::is_same_v<T, SlotExpr>) {
                const auto object = s.intern(expression(n.object).text());
                const auto array = index(n.index); // legacy ignores secondIndex
                const auto manager = s.data;
                return {manager->getDataField(object, s.intern(n.name), view(array))};
            } else if constexpr (std::is_same_v<T, AssignExpr>) {
                const auto& target = (*program_)[n.target].data;
                if (const auto* var = std::get_if<VariableExpr>(&target)) {
                    if (n.values.size() != 1) {
                        s.fail(DiagnosticCode::InvalidProgram,
                               "variable assignment requires one value");
                    }
                    const auto name = variable(*var);
                    const auto result = expression(n.values.front()).text();
                    s.write(name, result);
                    return {result};
                }
                const auto& slot = std::get<SlotExpr>(target);
                const auto object = s.intern(expression(slot.object).text());
                const auto array = index(slot.index);
                if (n.values.empty() || n.values.size() > 127) {
                    s.fail(DiagnosticCode::ExecutionLimit, "field argument limit reached");
                }
                std::vector<std::string> values;
                std::ranges::for_each(n.values, [&](NodeId v) {
                    values.push_back(expression(v).text());
                });
                const auto result = array ? *array : values.front();
                const auto manager = s.data;
                manager->setDataField(object, s.intern(slot.name), view(array), values);
                return {result};
            } else if constexpr (std::is_same_v<T, AssignOpExpr>) {
                const auto& target = (*program_)[n.target].data;
                std::string name, object, field, previous;
                std::optional<std::string> array;
                const auto* var = std::get_if<VariableExpr>(&target);
                const auto manager = s.data;
                if (var) {
                    name = variable(*var);
                    if (n.op != punctuation('%')) {
                        previous = s.read(name);
                    }
                } else {
                    const auto& slot = std::get<SlotExpr>(target);
                    object = s.intern(expression(slot.object).text());
                    array = index(slot.index);
                    field = s.intern(slot.name);
                    previous = manager->getDataField(object, field, view(array));
                }
                std::string result;
                if (n.integer) {
                    const auto right = expression(n.value, Mode::Integer).integer();
                    if (var && n.op == punctuation('%') && right) {
                        previous = s.read(name);
                    }
                    result = std::to_string(
                        intOp(n.op, var ? toInteger(previous) : parseInteger(previous), right));
                } else {
                    const auto right = expression(n.value, Mode::Number).number();
                    result = formatNumber(
                        floatOp(n.op, var ? toNumber(previous) : parseNumber(previous), right));
                }
                if (var) {
                    s.write(name, result);
                } else {
                    const std::array values{result};
                    manager->setDataField(object, field, view(array), values);
                }
                return {array && !var ? *array : result};
            } else {
                s.fail(DiagnosticCode::InvalidProgram, "statement used as an expression");
            }
        },
        (*program_)[id].data);
}

void Evaluator::fields(std::optional<std::string_view> object,
                       std::span<const NodeId> slots,
                       const std::shared_ptr<ConsoleDataManager>& manager) {
    auto& s = *console_.impl_;
    std::ranges::for_each(slots, [&](NodeId id) {
        s.step();
        const auto& slot = std::get<SlotInitializer>((*program_)[id].data);
        const auto array = index(slot.index);
        if (slot.values.empty() || slot.values.size() > 127) {
            s.fail(DiagnosticCode::ExecutionLimit, "field argument limit reached");
        }
        std::vector<std::string> values;
        std::ranges::for_each(slot.values, [&](NodeId value) {
            values.push_back(expression(value).text());
        });
        manager->setDataField(object, s.intern(slot.name), view(array), values);
        s.result = array ? *array : values.front();
    });
}

Evaluator::Flow Evaluator::block(std::span<const NodeId> nodes) {
    for (const auto id : nodes) {
        const auto flow = statement(id);
        if (flow != Flow::Normal) {
            return flow;
        }
    }
    return Flow::Normal;
}

Evaluator::Flow Evaluator::statement(NodeId id) {
    auto& s = *console_.impl_;
    if (id >= program_->nodes.size()) {
        s.fail(DiagnosticCode::InvalidProgram, "invalid statement node ID");
    }
    if (!activeStatements_.insert(id).second) {
        s.fail(DiagnosticCode::InvalidProgram, "cyclic statement node");
    }
    const OnExit node{[&] {
        activeStatements_.erase(id);
    }};
    s.step();
    if (s.depth >= s.limits.maxDepth || !stackAvailable()) {
        s.fail(DiagnosticCode::ExecutionLimit,
               "script statement depth limit or native stack capacity reached");
    }
    ++s.depth;
    const auto previous = s.range;
    s.range = (*program_)[id].range;
    const OnExit depth{[&] {
        --s.depth;
        s.range = previous;
    }};
    try {
        return std::visit(
            [&](const auto& n) -> Flow {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, BreakStmt>) {
                    return Flow::Break;
                } else if constexpr (std::is_same_v<T, ContinueStmt>) {
                    return Flow::Continue;
                } else if constexpr (std::is_same_v<T, ReturnStmt>) {
                    if (n.value == noNode) {
                        s.result.clear();
                    } else {
                        (void)expression(n.value);
                    }
                    return Flow::Return;
                } else if constexpr (std::is_same_v<T, IfStmt>) {
                    return block(expression(n.condition, Mode::Number).number() != 0 ? n.body
                                                                                     : n.elseBody);
                } else if constexpr (std::is_same_v<T, LoopStmt>) {
                    if (n.initial != noNode) {
                        (void)expression(n.initial);
                    }
                    while (expression(n.condition, Mode::Number).number() != 0) {
                        const auto flow = block(n.body);
                        if (flow == Flow::Break) {
                            return Flow::Normal;
                        }
                        if (flow == Flow::Return) {
                            return flow;
                        }
                        if (n.advance != noNode) {
                            (void)expression(n.advance);
                        }
                    }
                    return Flow::Normal;
                } else {
                    if constexpr (std::is_same_v<T, FunctionStmt>) {
                        const auto name = s.intern(n.name);
                        s.commands[canonical(name)] = {
                            name, 0, {}, Console::Impl::Function{program_, id}};
                    } else if constexpr (std::is_same_v<T, TagSetStmt>) {
                        const auto tagValue = expression(n.value).text();
                        const auto tagText =
                            n.text == noNode ? std::string{} : expression(n.text).text();
                        const auto tag = parseInteger(tagValue);
                        const auto name = s.intern(n.name);
                        if (tag > 0 && s.tags.idToDefine(tag)) {
                            console_.print("Error: id " + std::to_string(tag) +
                                           " already defined to a tag.");
                        } else if (tag > 0 && s.tags.defineToId(name)) {
                            console_.print("Error: define " + name + " already defined to tag " +
                                           std::to_string(s.tags.defineToId(name)) + ".");
                        } else {
                            (void)s.tags.addEntry(tag, name, tagText);
                        }
                        s.result = tagText;
                    } else if constexpr (std::is_same_v<T, StructStmt>) {
                        const auto manager = s.data;
                        const auto name = s.intern(n.name);
                        manager->declareDataBlock(s.intern(n.className), name);
                        try {
                            fields(name, n.slots, manager);
                            manager->endDataBlock();
                        } catch (...) {
                            try {
                                manager->abortDataBlock();
                            } catch (...) {
                            } // retain original failure
                            throw;
                        }
                    } else if constexpr (std::is_same_v<T, ObjectStmt>) {
                        const auto name = index(n.name);
                        const auto manager = s.data;
                        manager->beginObject(s.intern(n.className), view(name));
                        try {
                            fields(std::nullopt, n.slots, manager);
                            std::ranges::for_each(n.objects, [&](NodeId child) {
                                (void)statement(child);
                            });
                        } catch (...) {
                            try {
                                manager->abortObject();
                            } catch (...) {
                            } // retain original failure
                            throw;
                        }
                        manager->endObject();
                        if (name) {
                            s.result = *name;
                        }
                    } else {
                        (void)expression(id);
                    }
                    return Flow::Normal;
                }
            },
            (*program_)[id].data);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        s.fail(DiagnosticCode::CommandError, ex.what());
    }
}
} // namespace opent1::script
