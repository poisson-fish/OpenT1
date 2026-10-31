#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace opent1::script {
// Punctuation uses its ASCII value; named values match the legacy gram.h.
enum class TokenKind : std::uint16_t {
    End = 0,
    Function = 257,
    Instant = 259,
    Break = 260,
    Class,
    Else,
    Continue,
    Global,
    If,
    Nil,
    Return,
    While,
    For = 272,
    Illegal,
    Integer = 275,
    Tag,
    Variable,
    Identifier,
    String,
    Float,
    Decrement,
    Increment,
    ShiftLeft = 284,
    ShiftRight,
    PlusAssign,
    MinusAssign,
    MultiplyAssign,
    DivideAssign,
    ModuloAssign,
    AndAssign,
    XorAssign,
    OrAssign,
    ShiftLeftAssign,
    ShiftRightAssign,
    Concat,
    Equal,
    NotEqual,
    GreaterEqual,
    LessEqual,
    LogicalAnd,
    LogicalOr,
    ColonColon
};

constexpr TokenKind punctuation(char c) noexcept {
    return static_cast<TokenKind>(c);
}

struct SourceLocation {
    std::size_t offset = 0, line = 1, column = 1; // byte offsets/columns; LF advances line
    bool operator==(const SourceLocation&) const = default;
};

struct SourceRange {
    SourceLocation begin, end;
}; // half open
enum class DiagnosticCode {
    IllegalCharacter,
    InvalidVariable,
    UnterminatedString,
    InvalidEscape,
    NumberOutOfRange,
    SourceLimit,
    TokenLengthLimit,
    TokenLimit,
    NodeLimit,
    NestingLimit,
    UnexpectedToken,
    ExecutionLimit,
    InvalidProgram,
    CommandError
};

struct Diagnostic {
    DiagnosticCode code{};
    std::string sourceName;
    SourceRange range;
    std::string message;
    [[nodiscard]] std::string format() const;
};

struct Limits {
    std::size_t maxSourceBytes = 16 * 1024 * 1024;
    std::size_t maxTokenBytes = 4096;
    std::size_t maxTokens = 1'000'000; // excludes End
    std::size_t maxNodes = 1'000'000;
    std::size_t maxNesting = 128; // hard ceiling 128, even if raised by caller
};

struct Token {
    TokenKind kind = TokenKind::End;
    SourceRange range;
    // Original spelling for punctuation, decoded bytes for strings, first
    // spelling within this invocation for ASCII-insensitive identifiers.
    std::string text;
    std::int32_t integer = 0;
    double number = 0;
};

struct TokenizeResult {
    std::vector<Token> tokens; // includes End, also on failure
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return diagnostics.empty();
    }
};

// Flat ownership avoids recursive destruction even for very wide/deep trees.
// IDs remain valid across arena growth and moves. No node references source,
// scanner, interner, console, or another parse result.
using NodeId = std::uint32_t;
inline constexpr NodeId noNode = std::numeric_limits<NodeId>::max();

struct IntegerExpr {
    std::int32_t value;
};

struct FloatExpr {
    double value;
};

struct StringExpr {
    std::string value;
};

struct ConstantExpr {
    std::string name;
};

struct VariableExpr {
    std::string name;
    NodeId index = noNode;
};

struct TagExpr {
    std::string name;
};

// '*' is tag dereference; '-' uses float arithmetic, '!'/'~' use integer.
struct UnaryExpr {
    TokenKind op;
    NodeId operand;
};

// +,-,*,/ use float arithmetic; @/$+ concatenate; remaining ops use integer.
struct BinaryExpr {
    TokenKind op;
    NodeId left, right;
};

struct CommaExpr {
    NodeId left, right;
}; // array index concatenation

struct CallExpr {
    std::string name;
    std::vector<NodeId> arguments;
    NodeId object = noNode;
};

struct SlotExpr {
    std::string name;
    NodeId object, index = noNode, secondIndex = noNode;
    // Preserve the legacy slot_acc action: for obj.slot[a][b], index is b
    // and secondIndex is a. SlotInitializer below keeps declaration order.
};

struct AssignExpr {
    NodeId target;
    std::vector<NodeId> values;
};

// op is the arithmetic operation, not the assignment token. ++/-- have a
// synthesized FloatExpr(1); integer selects the original compound-op path.
struct AssignOpExpr {
    NodeId target;
    TokenKind op;
    NodeId value;
    bool integer;
};

struct BreakStmt {};

struct ContinueStmt {};

struct ReturnStmt {
    NodeId value = noNode;
};

struct IfStmt {
    NodeId condition;
    std::vector<NodeId> body, elseBody;
};

struct LoopStmt {
    NodeId initial, condition, advance;
    std::vector<NodeId> body;
};

struct FunctionStmt {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<NodeId> body;
};

struct ObjectStmt {
    std::string className;
    NodeId name;
    std::vector<NodeId> slots, objects;
};

struct StructStmt {
    std::string className, name;
    std::vector<NodeId> slots;
};

struct TagSetStmt {
    std::string name;
    NodeId value, text = noNode;
};

struct SlotInitializer {
    std::string name;
    NodeId index, secondIndex;
    std::vector<NodeId> values;
};

using NodeData = std::variant<IntegerExpr,
                              FloatExpr,
                              StringExpr,
                              ConstantExpr,
                              VariableExpr,
                              TagExpr,
                              UnaryExpr,
                              BinaryExpr,
                              CommaExpr,
                              CallExpr,
                              SlotExpr,
                              AssignExpr,
                              AssignOpExpr,
                              BreakStmt,
                              ContinueStmt,
                              ReturnStmt,
                              IfStmt,
                              LoopStmt,
                              FunctionStmt,
                              ObjectStmt,
                              StructStmt,
                              TagSetStmt,
                              SlotInitializer>;

struct Node {
    SourceRange range;
    NodeData data;
};

struct Program {
    std::string sourceName;
    std::vector<Node> nodes;
    std::vector<NodeId> statements;

    [[nodiscard]] const Node& operator[](NodeId id) const {
        return nodes.at(id);
    }
};

struct ParseResult {
    // Failed parses publish no partial program, including on lexical errors.
    std::optional<Program> program;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return program.has_value();
    }
};

// Bounded string_view input stops at its first NUL, matching SetScanBuffer.
// Closed single quotes keep raw escapes. Keywords are case sensitive. Unlike
// legacy truncation/undefined behavior, malformed strings and out-of-range
// numbers produce diagnostics. Calls are independent and safe concurrently.
[[nodiscard]] TokenizeResult
tokenize(std::string_view source, std::string_view sourceName = "<script>", Limits limits = {});
[[nodiscard]] ParseResult
parse(std::string_view source, std::string_view sourceName = "<script>", Limits limits = {});
} // namespace opent1::script
