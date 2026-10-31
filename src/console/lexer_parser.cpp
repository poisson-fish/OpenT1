#include <opent1/console/lexer_parser.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace opent1::script {
std::string Diagnostic::format() const {
    return sourceName + ":" + std::to_string(range.begin.line) + ":" +
           std::to_string(range.begin.column) + ": " + message;
}

namespace {
using K = TokenKind;

constexpr K p(char c) {
    return punctuation(c);
}

bool letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

bool tail(char c) {
    return letter(c) || digit(c) || c == ':';
}

int hex(char c) {
    if (digit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string canonical(std::string_view s) {
    std::string result(s);
    std::ranges::transform(result, result.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    return result;
}

constexpr auto keywords = std::to_array<std::pair<std::string_view, K>>({{"break", K::Break},
                                                                         {"nil", K::Nil},
                                                                         {"class", K::Class},
                                                                         {"return", K::Return},
                                                                         {"else", K::Else},
                                                                         {"global", K::Global},
                                                                         {"while", K::While},
                                                                         {"if", K::If},
                                                                         {"for", K::For},
                                                                         {"continue", K::Continue},
                                                                         {"function", K::Function},
                                                                         {"instant", K::Instant}});
constexpr auto operators = std::to_array<std::pair<std::string_view, K>>(
    {{"<<=", K::ShiftLeftAssign}, {">>=", K::ShiftRightAssign}, {"==", K::Equal},
     {"!=", K::NotEqual},         {">=", K::GreaterEqual},      {"<=", K::LessEqual},
     {"&&", K::LogicalAnd},       {"||", K::LogicalOr},         {"::", K::ColonColon},
     {"--", K::Decrement},        {"++", K::Increment},         {"$+", K::Concat},
     {"<<", K::ShiftLeft},        {">>", K::ShiftRight},        {"+=", K::PlusAssign},
     {"-=", K::MinusAssign},      {"*=", K::MultiplyAssign},    {"/=", K::DivideAssign},
     {"%=", K::ModuloAssign},     {"&=", K::AndAssign},         {"^=", K::XorAssign},
     {"|=", K::OrAssign}});

struct Scanner {
    std::string_view source, name;
    Limits limits;
    TokenizeResult result;
    SourceLocation at;
    std::unordered_map<std::string, std::string> names;

    char peek(std::size_t n = 0) const {
        return n < source.size() - at.offset ? source[at.offset + n] : '\0';
    }

    void advance() {
        if (peek() == '\n') {
            ++at.line;
            at.column = 1;
        } else {
            ++at.column;
        }
        ++at.offset;
    }

    void error(DiagnosticCode code, SourceRange range, std::string message) {
        result.diagnostics.push_back({code, std::string(name), range, std::move(message)});
    }

    void run() {
        // Only inspect the bounded prefix when checking for a legacy NUL end.
        const auto extent =
            limits.maxSourceBytes < source.size() ? limits.maxSourceBytes + 1 : source.size();
        const auto nul = source.substr(0, extent).find('\0');
        if (nul != std::string_view::npos) {
            source = source.substr(0, nul);
        }
        if (source.size() > limits.maxSourceBytes) {
            error(DiagnosticCode::SourceLimit, {at, at}, "source byte limit exceeded");
        } else {
            while (at.offset < source.size()) {
                const char c = peek();
                if (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n') {
                    advance();
                    continue;
                }
                if (c == '#' || (c == '/' && peek(1) == '/')) {
                    while (peek() != '\0' && peek() != '\r' && peek() != '\n') {
                        advance();
                    }
                    continue;
                }
                if (result.tokens.size() >= limits.maxTokens) {
                    error(DiagnosticCode::TokenLimit, {at, at}, "token count limit exceeded");
                    break;
                }
                const auto begin = at;
                Token token;
                token.kind = K::Illegal;
                bool decoded = false;
                if (letter(c) || ((c == '$' || c == '%') && letter(peek(1)))) {
                    advance();
                    while (tail(peek())) {
                        advance();
                    }
                    const auto raw = source.substr(begin.offset, at.offset - begin.offset);
                    token.kind =
                        (c == '$' || c == '%')
                            ? K::Variable
                            : (raw.starts_with("ID") && raw.size() > 2 ? K::Tag : K::Identifier);
                    if (token.kind == K::Identifier) {
                        const auto keyword = std::ranges::find(keywords, raw, [](const auto& item) {
                            return item.first;
                        });
                        if (keyword != keywords.end()) {
                            token.kind = keyword->second;
                        }
                    }
                } else if ((c == '$' || c == '%') && digit(peek(1))) {
                    // ILID wins longest match only if digits are followed by a letter.
                    std::size_t n = 1;
                    while (digit(peek(n))) {
                        ++n;
                    }
                    if (letter(peek(n))) {
                        do {
                            advance();
                        } while (tail(peek()));
                        error(DiagnosticCode::InvalidVariable,
                              {begin, at},
                              "variable names must start with a letter or underscore");
                    } else {
                        advance();
                        if (c == '%') {
                            token.kind = p('%');
                        } else {
                            error(DiagnosticCode::IllegalCharacter,
                                  {begin, at},
                                  "expected a variable name after '$'");
                        }
                    }
                } else if (digit(c)) {
                    bool isHex = false, isFloat = false;
                    if (c == '0' && (peek(1) == 'x' || peek(1) == 'X') && hex(peek(2)) >= 0) {
                        advance();
                        advance();
                        while (hex(peek()) >= 0) {
                            advance();
                        }
                        isHex = true;
                    } else {
                        while (digit(peek())) {
                            advance();
                        }
                        if (peek() == '.' && digit(peek(1))) {
                            isFloat = true;
                            advance();
                            while (digit(peek())) {
                                advance();
                            }
                        }
                        if (peek() == 'e' || peek() == 'E') {
                            std::size_t n = 1;
                            if (peek(n) == '+' || peek(n) == '-') {
                                ++n;
                            }
                            if (digit(peek(n))) {
                                isFloat = true;
                                do {
                                    advance();
                                } while (--n != 0);
                                while (digit(peek())) {
                                    advance();
                                }
                            }
                        }
                    }
                    token.kind = isFloat ? K::Float : K::Integer;
                    const auto raw = source.substr(begin.offset + (isHex ? 2 : 0),
                                                   at.offset - begin.offset - (isHex ? 2 : 0));
                    std::errc ec;
                    if (isFloat) {
                        ec = std::from_chars(raw.data(),
                                             raw.data() + raw.size(),
                                             token.number,
                                             std::chars_format::general)
                                 .ec;
                        if (!std::isfinite(token.number)) {
                            ec = std::errc::result_out_of_range;
                        }
                    } else if (isHex) {
                        std::uint32_t value = 0;
                        ec = std::from_chars(raw.data(), raw.data() + raw.size(), value, 16).ec;
                        token.integer = std::bit_cast<std::int32_t>(value);
                    } else {
                        ec = std::from_chars(raw.data(), raw.data() + raw.size(), token.integer).ec;
                    }
                    if (ec != std::errc{}) {
                        error(DiagnosticCode::NumberOutOfRange,
                              {begin, at},
                              "numeric literal is out of range");
                    }
                } else if (c == '"' || c == '\'') {
                    const char quote = c;
                    advance();
                    bool closed = false, badEscape = false;
                    while (peek() != '\0' && peek() != '\n' && (quote == '\'' || peek() != '\r')) {
                        if (peek() == quote) {
                            advance();
                            closed = true;
                            break;
                        }
                        if (peek() == '\\' && peek(1) != '\0' && peek(1) != '\n') {
                            advance();
                            const char escaped = peek();
                            advance();
                            if (quote == '\'') {
                                token.text += '\\';
                                token.text += escaped;
                            } else if (escaped == 'x') {
                                if (hex(peek()) < 0 || hex(peek(1)) < 0) {
                                    badEscape = true;
                                } else {
                                    const char value =
                                        static_cast<char>(hex(peek()) * 16 + hex(peek(1)));
                                    advance();
                                    advance();
                                    token.text += value;
                                }
                            } else {
                                token.text += escaped == 'n'   ? '\n'
                                              : escaped == 't' ? '\t'
                                                               : escaped;
                            }
                        } else {
                            token.text += peek();
                            advance();
                        }
                        if (at.offset - begin.offset > limits.maxTokenBytes) {
                            break;
                        }
                    }
                    token.kind = K::String;
                    decoded = true;
                    // Legacy strings are C strings even when \x00 was decoded.
                    if (const auto zero = token.text.find('\0'); zero != std::string::npos) {
                        token.text.resize(zero);
                    }
                    if (!closed && at.offset - begin.offset <= limits.maxTokenBytes) {
                        error(DiagnosticCode::UnterminatedString,
                              {begin, at},
                              "unterminated string literal");
                    }
                    if (badEscape) {
                        error(DiagnosticCode::InvalidEscape,
                              {begin, at},
                              "expected two hexadecimal digits after '\\x'");
                    }
                } else {
                    for (const auto& [word, kind] : operators) {
                        if (source.substr(at.offset).starts_with(word)) {
                            for (std::size_t n = 0; n < word.size(); ++n) {
                                advance();
                            }
                            token.kind = kind;
                            break;
                        }
                    }
                    if (token.kind == K::Illegal) {
                        advance();
                        if (std::string_view("@[]()+-*/<>|.!:;{},&%^~=").find(c) !=
                            std::string_view::npos) {
                            token.kind = p(c);
                        } else {
                            error(DiagnosticCode::IllegalCharacter,
                                  {begin, at},
                                  "illegal character in script");
                        }
                    }
                }
                token.range = {begin, at};
                if (at.offset - begin.offset > limits.maxTokenBytes) {
                    error(
                        DiagnosticCode::TokenLengthLimit, token.range, "token byte limit exceeded");
                    break;
                }
                if (!decoded) {
                    token.text = source.substr(begin.offset, at.offset - begin.offset);
                }
                if (token.kind == K::Variable || token.kind == K::Identifier ||
                    token.kind == K::Tag) {
                    token.text = names.try_emplace(canonical(token.text), token.text).first->second;
                }
                result.tokens.push_back(std::move(token));
            }
        }
        result.tokens.push_back({K::End, {at, at}, {}, 0, 0});
    }
};

struct ParseFailure {};

struct Expr {
    NodeId id;
    bool assignable = false, statement = false;
};

struct Parser {
    const std::vector<Token>& tokens;
    std::string_view name;
    Limits limits;
    Program program;
    std::vector<Diagnostic> diagnostics;
    std::size_t cursor = 0, nesting = 0;

    const Token& current() const {
        return tokens[cursor];
    }

    K kind(std::size_t ahead = 0) const {
        return tokens[std::min(cursor + ahead, tokens.size() - 1)].kind;
    }

    const Token& take() {
        return tokens[cursor++];
    }

    bool match(K k) {
        if (kind() != k) {
            return false;
        }
        ++cursor;
        return true;
    }

    [[noreturn]] void fail(std::string message,
                           DiagnosticCode code = DiagnosticCode::UnexpectedToken) {
        diagnostics.push_back({code, std::string(name), current().range, std::move(message)});
        throw ParseFailure{};
    }

    const Token& expect(K k, std::string_view expected) {
        if (kind() != k) {
            fail("expected " + std::string(expected) +
                 (kind() == K::End ? ", found end of input" : ", found '" + current().text + "'"));
        }
        return take();
    }

    struct Depth {
        Parser& parser;

        explicit Depth(Parser& in) : parser(in) {
            if (parser.nesting >= std::min<std::size_t>(parser.limits.maxNesting, 128)) {
                parser.fail("parser nesting limit exceeded", DiagnosticCode::NestingLimit);
            }
            ++parser.nesting;
        }

        ~Depth() {
            --parser.nesting;
        }
    };

    template <class Data> NodeId add(SourceLocation begin, Data data) {
        if (program.nodes.size() >= limits.maxNodes || program.nodes.size() >= noNode) {
            fail("AST node limit exceeded", DiagnosticCode::NodeLimit);
        }
        const auto id = static_cast<NodeId>(program.nodes.size());
        program.nodes.push_back(
            {{begin, cursor == 0 ? begin : tokens[cursor - 1].range.end}, std::move(data)});
        return id;
    }

    template <class Data> bool is(NodeId id) const {
        return std::holds_alternative<Data>(program[id].data);
    }

    SourceLocation begin(NodeId id) const {
        return program[id].range.begin;
    }

    static int precedence(K k) {
        if (k == p('=') || (k >= K::PlusAssign && k <= K::ShiftRightAssign)) {
            return 1;
        }
        if (k == K::LogicalOr) {
            return 2;
        }
        if (k == K::LogicalAnd) {
            return 3;
        }
        if (k == p('|')) {
            return 4;
        }
        if (k == p('^')) {
            return 5;
        }
        if (k == p('&')) {
            return 6;
        }
        if (k == K::Equal || k == K::NotEqual) {
            return 7;
        }
        if (k == p('<') || k == p('>') || k == K::LessEqual || k == K::GreaterEqual) {
            return 8;
        }
        if (k == p('@') || k == K::Concat) {
            return 9;
        }
        if (k == K::ShiftLeft || k == K::ShiftRight) {
            return 10;
        }
        if (k == p('+') || k == p('-')) {
            return 11;
        }
        if (k == p('*') || k == p('/') || k == p('%')) {
            return 12;
        }
        return 0;
    }

    std::vector<NodeId> expressionList(K close) {
        std::vector<NodeId> out;
        do {
            out.push_back(expression().id);
        } while (match(p(',')));
        expect(close, close == p(')') ? "')'" : "'}'");
        return out;
    }

    NodeId arrayIndex() {
        auto value = expression().id;
        while (match(p(','))) {
            const auto right = expression().id;
            value = add(begin(value), CommaExpr{value, right});
        }
        expect(p(']'), "']'");
        return value;
    }

    Expr primary() {
        const auto token = take();
        const auto start = token.range.begin;
        if (token.kind == K::Integer) {
            return {add(start, IntegerExpr{token.integer})};
        }
        if (token.kind == K::Float) {
            return {add(start, FloatExpr{token.number})};
        }
        if (token.kind == K::String) {
            return {add(start, StringExpr{token.text})};
        }
        if (token.kind == K::Tag) {
            return {add(start, TagExpr{token.text})};
        }
        if (token.kind == K::Break) {
            return {add(start, ConstantExpr{"break"})};
        }
        if (token.kind == K::Variable) {
            const auto index = match(p('[')) ? arrayIndex() : noNode;
            return {add(start, VariableExpr{token.text, index}), true};
        }
        if (token.kind == K::Identifier) {
            if (match(p('('))) {
                auto args = match(p(')')) ? std::vector<NodeId>{} : expressionList(p(')'));
                return {add(start, CallExpr{token.text, std::move(args)}), false, true};
            }
            return {add(start, ConstantExpr{token.text})};
        }
        if (token.kind == p('(')) {
            auto value = expression();
            expect(p(')'), "')'");
            program.nodes[value.id].range = {start, tokens[cursor - 1].range.end};
            value.assignable = false;
            value.statement = false;
            return value;
        }
        if (token.kind == p('-') || token.kind == p('!') || token.kind == p('~') ||
            token.kind == p('*')) {
            const auto operand = expression(13);
            return {add(start, UnaryExpr{token.kind, operand.id})};
        }
        // Rewind so the diagnostic identifies the unexpected token, even EOF.
        --cursor;
        fail("expected an expression");
    }

    Expr expression(int minimum = 1) {
        Depth depth(*this);
        auto lhs = primary();
        for (;;) {
            if (kind() == p('.')) {
                take();
                const auto slot = expect(K::Identifier, "a field or method name");
                if (match(p('('))) {
                    auto args = match(p(')')) ? std::vector<NodeId>{} : expressionList(p(')'));
                    lhs = {add(begin(lhs.id), CallExpr{slot.text, std::move(args), lhs.id}),
                           false,
                           true};
                } else {
                    auto index = match(p('[')) ? arrayIndex() : noNode;
                    NodeId second = noNode;
                    if (index != noNode && match(p('['))) {
                        second = index;
                        index = expression().id;
                        expect(p(']'), "']'");
                    }
                    lhs = {add(begin(lhs.id), SlotExpr{slot.text, lhs.id, index, second}), true};
                }
                continue;
            }
            const auto op = kind();
            const bool increment = op == K::Increment || op == K::Decrement;
            const int prec = increment ? 13 : precedence(op);
            // gram.y recognizes VAR/slot assignments as stmt_expr before it
            // reduces them to expr. They can therefore begin an operand even
            // under a higher-precedence operator (e.g. 1 + $a = 2).
            const bool assignmentOperand = lhs.assignable && prec == 1;
            if ((prec < minimum && !assignmentOperand) || prec == 0) {
                break;
            }
            if (prec == 1 || increment) {
                if (!lhs.assignable) {
                    fail("assignment requires a variable or field");
                }
                take();
                if (op == p('=')) {
                    std::vector<NodeId> values;
                    if (match(p('{'))) {
                        if (!is<SlotExpr>(lhs.id)) {
                            fail("brace values require a field assignment");
                        }
                        values = expressionList(p('}'));
                    } else {
                        values.push_back(expression(prec).id);
                    }
                    lhs = {add(begin(lhs.id), AssignExpr{lhs.id, std::move(values)}), false, true};
                } else {
                    K arithmetic;
                    bool integer = false;
                    switch (op) {
                        case K::PlusAssign:
                        case K::Increment:
                            arithmetic = p('+');
                            break;
                        case K::MinusAssign:
                        case K::Decrement:
                            arithmetic = p('-');
                            break;
                        case K::MultiplyAssign:
                            arithmetic = p('*');
                            break;
                        case K::DivideAssign:
                            arithmetic = p('/');
                            break;
                        case K::ModuloAssign:
                            arithmetic = p('%');
                            integer = true;
                            break;
                        case K::AndAssign:
                            arithmetic = p('&');
                            integer = true;
                            break;
                        case K::XorAssign:
                            arithmetic = p('^');
                            integer = true;
                            break;
                        case K::OrAssign:
                            arithmetic = p('|');
                            integer = true;
                            break;
                        case K::ShiftLeftAssign:
                            arithmetic = K::ShiftLeft;
                            integer = true;
                            break;
                        default:
                            arithmetic = K::ShiftRight;
                            integer = true;
                            break;
                    }
                    const auto rhs = increment ? add(tokens[cursor - 1].range.begin, FloatExpr{1.0})
                                               : expression(prec).id;
                    lhs = {add(begin(lhs.id), AssignOpExpr{lhs.id, arithmetic, rhs, integer}),
                           false,
                           true};
                }
            } else {
                take();
                const auto rhs = expression(prec + 1);
                lhs = {
                    add(begin(lhs.id), BinaryExpr{op == p('@') ? K::Concat : op, lhs.id, rhs.id})};
            }
        }
        return lhs;
    }

    std::vector<NodeId> statements() {
        std::vector<NodeId> out;
        while (kind() != p('}') && kind() != K::End) {
            out.push_back(statement());
        }
        expect(p('}'), "'}'");
        return out;
    }

    std::vector<NodeId> block() {
        if (match(p('{'))) {
            return statements();
        }
        return {statement()};
    }

    NodeId initializer() {
        const auto field = expect(K::Identifier, "a field name");
        const auto index = match(p('[')) ? arrayIndex() : noNode;
        NodeId second = noNode;
        if (index != noNode && match(p('['))) {
            second = expression().id;
            expect(p(']'), "']'");
        }
        expect(p('='), "'='");
        std::vector<NodeId> values;
        if (match(p('{'))) {
            if (second != noNode) {
                fail("brace initializer supports at most one field index");
            }
            values = expressionList(p('}'));
        } else {
            values.push_back(expression().id);
        }
        expect(p(';'), "';'");
        return add(field.range.begin,
                   SlotInitializer{field.text, index, second, std::move(values)});
    }

    NodeId statement() {
        Depth depth(*this);
        const auto start = current().range.begin;
        if (match(K::Function)) {
            const auto fn = expect(K::Identifier, "a function name");
            expect(p('('), "'('");
            std::vector<std::string> parameters;
            if (!match(p(')'))) {
                do {
                    parameters.push_back(expect(K::Variable, "a parameter variable").text);
                } while (match(p(',')));
                expect(p(')'), "')'");
            }
            expect(p('{'), "'{'");
            auto body = statements();
            return add(start, FunctionStmt{fn.text, std::move(parameters), std::move(body)});
        }
        if (match(K::If)) {
            expect(p('('), "'('");
            const auto condition = expression().id;
            expect(p(')'), "')'");
            auto body = block();
            auto otherwise = match(K::Else) ? block() : std::vector<NodeId>{};
            return add(start, IfStmt{condition, std::move(body), std::move(otherwise)});
        }
        if (kind() == K::While || kind() == K::For) {
            const bool isFor = take().kind == K::For;
            expect(p('('), "'('");
            NodeId initial = noNode, advance = noNode;
            if (isFor) {
                initial = expression().id;
                expect(p(';'), "';'");
            }
            const auto condition = expression().id;
            if (isFor) {
                expect(p(';'), "';'");
                advance = expression().id;
            }
            expect(p(')'), "')'");
            auto body = block();
            return add(start, LoopStmt{initial, condition, advance, std::move(body)});
        }
        if (kind() == K::Break && kind(1) == p(';')) {
            take();
            take();
            return add(start, BreakStmt{});
        }
        if (match(K::Continue)) {
            expect(p(';'), "';'");
            return add(start, ContinueStmt{});
        }
        if (match(K::Return)) {
            const auto value = kind() == p(';') ? noNode : expression().id;
            expect(p(';'), "';'");
            return add(start, ReturnStmt{value});
        }
        if (match(K::Instant)) {
            const auto cls = expect(K::Identifier, "an object class");
            const auto obj = kind() != p('{') && kind() != p(';') ? expression().id : noNode;
            std::vector<NodeId> slots, objects;
            if (match(p('{'))) {
                while (kind() == K::Identifier) {
                    slots.push_back(initializer());
                }
                while (kind() == K::Instant) {
                    objects.push_back(statement());
                }
                if (slots.empty() && objects.empty()) {
                    fail("object block requires a field or child object");
                }
                expect(p('}'), "'}'");
            }
            expect(p(';'), "';'");
            return add(start, ObjectStmt{cls.text, obj, std::move(slots), std::move(objects)});
        }
        if (kind() == K::Identifier && (kind(1) == K::Identifier || kind(1) == p('{'))) {
            const auto cls = take();
            std::string obj;
            if (kind() == K::Identifier) {
                obj = take().text;
            }
            std::vector<NodeId> slots;
            if (match(p('{'))) {
                if (kind() != K::Identifier) {
                    fail("struct block requires a field");
                }
                do {
                    slots.push_back(initializer());
                } while (kind() == K::Identifier);
                expect(p('}'), "'}'");
            }
            expect(p(';'), "';'");
            return add(start, StructStmt{cls.text, std::move(obj), std::move(slots)});
        }
        if (kind() == K::Tag && kind(1) == p('=')) {
            const auto tag = take();
            take();
            const auto value = expression().id;
            const auto text = match(p(',')) ? expression().id : noNode;
            expect(p(';'), "';'");
            return add(start, TagSetStmt{tag.text, value, text});
        }
        const auto value = expression();
        if (!value.statement) {
            fail("expected an assignment or function call statement");
        }
        expect(p(';'), "';'");
        return value.id;
    }

    void run() {
        program.sourceName = name;
        while (kind() != K::End) {
            program.statements.push_back(statement());
        }
    }
};
} // namespace

TokenizeResult tokenize(std::string_view source, std::string_view sourceName, Limits limits) {
    Scanner scanner{source, sourceName, limits, {}, {}, {}};
    scanner.run();
    return std::move(scanner.result);
}

ParseResult parse(std::string_view source, std::string_view sourceName, Limits limits) {
    auto lexed = tokenize(source, sourceName, limits);
    if (!lexed.ok()) {
        return {std::nullopt, std::move(lexed.diagnostics)};
    }
    Parser parser{lexed.tokens, sourceName, limits, {}, {}};
    try {
        parser.run();
    } catch (const ParseFailure&) {
        return {std::nullopt, std::move(parser.diagnostics)};
    }
    return {std::move(parser.program), {}};
}
} // namespace opent1::script
