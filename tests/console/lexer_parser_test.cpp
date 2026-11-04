#include "test_support.hpp"
#include <ranges>
#include <opent1/console/lexer_parser.hpp>
#include <array>
#include <future>

namespace {
using namespace opent1::script;
using K = TokenKind;

template <class T> const T& node(const Program& program, NodeId id) {
    return std::get<T>(program[id].data);
}

Program good(std::string_view source) {
    auto result = parse(source, "unit.cs");
    if (!result.ok()) {
        throw std::runtime_error(result.diagnostics.front().format());
    }
    return std::move(*result.program);
}

void bad(std::string_view source,
         DiagnosticCode code = DiagnosticCode::UnexpectedToken,
         Limits limits = {}) {
    const auto result = parse(source, "bad.cs", limits);
    CHECK(!result.ok());
    CHECK(!result.program);
    CHECK(!result.diagnostics.empty());
    CHECK(result.diagnostics.front().code == code);
    CHECK(result.diagnostics.front().format().starts_with("bad.cs:"));
}

void lexer() {
    const auto out = tokenize("# ignored\r\n$Mixed::x = 0Xffffffff;\n%a=1.25e+2; echo($mixed::X, "
                              "'raw\\n', \"a\\n\\t\\x41\\q\");",
                              "lex.cs");
    CHECK(out.ok());
    CHECK(out.tokens[0].kind == K::Variable);
    CHECK(out.tokens[0].text == "$Mixed::x");
    CHECK(out.tokens[0].range.begin == (SourceLocation{11, 2, 1}));
    CHECK(out.tokens[2].integer == -1);
    CHECK(out.tokens[6].number == 125);
    CHECK(out.tokens[10].text == "$Mixed::x");
    CHECK(out.tokens[12].text == "raw\\n");
    CHECK(out.tokens[14].text == "a\n\tAq");
    CHECK(out.tokens.back().kind == K::End);
    const std::array bounded{'$', 'a', '=', '1', ';', '!'};
    CHECK(parse(std::string_view(bounded.data(), 5)).ok());
    const std::array nul{'$', 'a', '=', '1', ';', '\0', '?'};
    CHECK(parse(std::string_view(nul.data(), nul.size())).ok());
    CHECK(tokenize("\"before\\x00after\"").tokens[0].text == "before");
    CHECK(tokenize("'a\rb'").tokens[0].text == "a\rb");
    CHECK(tokenize("FUNCTION if If ID IDa idA").tokens[0].kind == K::Identifier);
    CHECK(tokenize("FUNCTION if If ID IDa idA").tokens[1].kind == K::If);
    CHECK(tokenize("FUNCTION if If ID IDa idA").tokens[4].kind == K::Tag);
    CHECK(tokenize("1. .5 1e 0x").tokens.size() == 9);
    bad("$a=\"unclosed", DiagnosticCode::UnterminatedString);
    bad("$a='unclosed", DiagnosticCode::UnterminatedString);
    for (const auto text : {"\"\\x\"", "\"\\x1\"", "\"\\xgg\""}) {
        bad(text, DiagnosticCode::InvalidEscape);
    }
    bad("$1bad=1;", DiagnosticCode::InvalidVariable);
    bad("echo(?);", DiagnosticCode::IllegalCharacter);
    for (const auto text : {"$a=2147483648;", "$a=0x100000000;", "$a=1e9999;", "$a=1e-9999;"}) {
        bad(text, DiagnosticCode::NumberOutOfRange);
    }
    Limits legacy;
    legacy.legacyDecimalIntegers = true;
    const auto wrapped = tokenize("2147483648 10000000000000000000000000", "legacy.cs", legacy);
    CHECK(wrapped.ok() && wrapped.tokens[0].integer == (-2147483647 - 1));
    CHECK(wrapped.tokens[1].integer == 1241513984);
}

void structure() {
    auto program =
        good("$a=$b=1+2*3; $a[1,2]=3; %o.field[1,2][3]={4,5}; %o.call(1).x++; ID_F=1,\"text\";");
    const auto& assign = node<AssignExpr>(program, program.statements[0]);
    CHECK(node<VariableExpr>(program, assign.target).name == "$a");
    const auto& inner = node<AssignExpr>(program, assign.values[0]);
    const auto& sum = node<BinaryExpr>(program, inner.values[0]);
    CHECK(sum.op == punctuation('+'));
    CHECK(node<IntegerExpr>(program, sum.left).value == 1);
    CHECK(node<BinaryExpr>(program, sum.right).op == punctuation('*'));
    const auto& array =
        node<VariableExpr>(program, node<AssignExpr>(program, program.statements[1]).target);
    const auto& comma = node<CommaExpr>(program, array.index);
    CHECK(node<IntegerExpr>(program, comma.left).value == 1);
    CHECK(node<IntegerExpr>(program, comma.right).value == 2);
    const auto& fields = node<AssignExpr>(program, program.statements[2]);
    const auto& slot = node<SlotExpr>(program, fields.target);
    CHECK(fields.values.size() == 2);
    CHECK(node<IntegerExpr>(program, slot.index).value == 3);
    CHECK(std::holds_alternative<CommaExpr>(program[slot.secondIndex].data));
    const auto& compound = node<AssignOpExpr>(program, program.statements[3]);
    CHECK(compound.op == punctuation('+'));
    CHECK(!compound.integer);
    CHECK(node<FloatExpr>(program, compound.value).value == 1);
    const auto& call = node<CallExpr>(program, node<SlotExpr>(program, compound.target).object);
    CHECK(call.name == "call");
    CHECK(call.arguments.size() == 1);
    CHECK(call.object != noNode);
    CHECK(node<TagSetStmt>(program, program.statements[4]).text != noNode);
    const auto nestedAssignment = good("return 1+$a=2*3;");
    const auto& legacySum = node<BinaryExpr>(
        nestedAssignment, node<ReturnStmt>(nestedAssignment, nestedAssignment.statements[0]).value);
    CHECK(std::holds_alternative<AssignExpr>(nestedAssignment[legacySum.right].data));
    CHECK(good("break.method(); break.field=1;").statements.size() == 2);

    auto prec = good("return 1 || 2 && 3 | 4 ^ 5 & 6 == 7 < 8 @ 9 << 10 + 11 * -12; return 9-3-1;");
    auto id = node<ReturnStmt>(prec, prec.statements[0]).value;
    for (const auto op : {K::LogicalOr,
                          K::LogicalAnd,
                          punctuation('|'),
                          punctuation('^'),
                          punctuation('&'),
                          K::Equal,
                          punctuation('<'),
                          K::Concat,
                          K::ShiftLeft,
                          punctuation('+'),
                          punctuation('*')}) {
        const auto& binary = node<BinaryExpr>(prec, id);
        CHECK(binary.op == op);
        id = binary.right;
    }
    CHECK(node<UnaryExpr>(prec, id).op == punctuation('-'));
    const auto& sub = node<BinaryExpr>(prec, node<ReturnStmt>(prec, prec.statements[1]).value);
    CHECK(node<BinaryExpr>(prec, sub.left).op == punctuation('-'));

    auto flow = good("function F(%x,$Y) { if(%x) if($Y) return 1; else return 2; while(%x) %x--; "
                     "for(%i=0;%i<2;%i++) continue; } instant Group \"g\" { field[1][2]=3; instant "
                     "Child; }; Thing Name {field={1,2};};");
    const auto& fn = node<FunctionStmt>(flow, flow.statements[0]);
    CHECK(fn.name == "F");
    CHECK(fn.parameters == (std::vector<std::string>{"%x", "$Y"}));
    CHECK(fn.body.size() == 3);
    const auto& outer = node<IfStmt>(flow, fn.body[0]);
    CHECK(outer.elseBody.empty());
    CHECK(node<IfStmt>(flow, outer.body[0]).elseBody.size() == 1);
    CHECK(node<LoopStmt>(flow, fn.body[1]).initial == noNode);
    const auto& loop = node<LoopStmt>(flow, fn.body[2]);
    CHECK(loop.initial != noNode && loop.advance != noNode);
    const auto& object = node<ObjectStmt>(flow, flow.statements[1]);
    CHECK(object.className == "Group");
    CHECK(object.objects.size() == 1);
    CHECK(object.slots.size() == 1);
    const auto& init = node<SlotInitializer>(flow, object.slots[0]);
    CHECK(node<IntegerExpr>(flow, init.index).value == 1);
    CHECK(node<IntegerExpr>(flow, init.secondIndex).value == 2);
    CHECK(node<StructStmt>(flow, flow.statements[2]).name == "Name");

    for (const auto source : {";",
                              "{}",
                              "1;",
                              "$a;",
                              "echo() + 1;",
                              "(echo());",
                              "($a)=1;",
                              "++$a;",
                              "for(;;) break;",
                              "instant Thing {};",
                              "Thing {};",
                              "echo(nil);",
                              "global $a;",
                              "echo(1,);",
                              "$a={1,2};",
                              "$a[1][2]=3;",
                              "Thing X {field[1][2]={1,2};};",
                              "instant Thing { instant Child; field=1; };"}) {
        bad(source);
    }
    const auto diagnostic = parse("$a=1;\n  echo(1,);", "syntax.cs").diagnostics.front();
    CHECK(diagnostic.range.begin.line == 2 && diagnostic.range.begin.column == 10);
    CHECK(diagnostic.message == "expected an expression");
    auto owned = [] {
        std::string source = "function Owned(%x){ return \"value\"; }";
        return good(source);
    }();
    auto moved = std::move(owned);
    CHECK(node<FunctionStmt>(moved, moved.statements[0]).name == "Owned");
    CHECK(node<StringExpr>(
              moved,
              node<ReturnStmt>(moved, node<FunctionStmt>(moved, moved.statements[0]).body[0]).value)
              .value == "value");
}

void bounds() {
    Limits limits;
    limits.maxSourceBytes = 4;
    bad("$a=1;", DiagnosticCode::SourceLimit, limits);
    CHECK(parse(std::string_view("\0ignored", 8), "nul", limits).ok());
    limits = {};
    limits.maxTokenBytes = 3;
    bad("echo();", DiagnosticCode::TokenLengthLimit, limits);
    bad("$a=\"long\";", DiagnosticCode::TokenLengthLimit, limits);
    CHECK(parse("// a comment may exceed the token limit\n$a=1;", "comment", limits).ok());
    limits = {};
    limits.maxTokens = 4;
    bad("$a=1; $b=2;", DiagnosticCode::TokenLimit, limits);
    CHECK(parse("$a=1;", "exact", limits).ok());
    limits = {};
    limits.maxNodes = 2;
    bad("$a=1;", DiagnosticCode::NodeLimit, limits);
    limits.maxNodes = 3;
    CHECK(parse("$a=1;", "exact", limits).ok());
    limits = {};
    limits.maxNesting = 5;
    bad("return (((((1)))));", DiagnosticCode::NestingLimit, limits);
    limits.maxNesting = 1'000'000;
    bad("return " + std::string(2000, '(') + "1" + std::string(2000, ')') + ";",
        DiagnosticCode::NestingLimit,
        limits);
    CHECK(tokenize(std::string(4096, 'a')).ok());
    CHECK(!tokenize(std::string(4097, 'a')).ok());
    CHECK(tokenize("\"" + std::string(4094, 'a') + "\"").ok());
    CHECK(!tokenize("\"" + std::string(4095, 'a') + "\"").ok());
    CHECK(parse("//" + std::string(65536, 'a')).ok());
    for (const auto& source :
         {"return " + std::string(2000, '-') + "1;",
          "return " + std::string(2000, '(') + "1" + std::string(2000, ')') + ";"}) {
        bad(source,
            source.find('(') != std::string::npos ? DiagnosticCode::NestingLimit
                                                  : DiagnosticCode::UnexpectedToken);
    }
    std::string assignments;
    for ([[maybe_unused]] auto i : std::views::iota(0, 2000)) {
        assignments += "$a=";
    }
    bad(assignments + "1;", DiagnosticCode::NestingLimit);
    std::string nested;
    for ([[maybe_unused]] auto i : std::views::iota(0, 2000)) {
        nested += "if(1)";
    }
    bad(nested + "break;", DiagnosticCode::NestingLimit);
    std::string wide;
    for ([[maybe_unused]] auto i : std::views::iota(0, 10000)) {
        wide += "$a=1;";
    }
    CHECK(good(wide).statements.size() == 10000);
    const std::string flat = "return 1" + [] {
        std::string s;
        for ([[maybe_unused]] auto i : std::views::iota(0, 10000)) {
            s += "+1";
        }
        return s;
    }() + ";";
    CHECK(good(flat).nodes.size() == 20002); // destruction uses no recursive chain
    const std::string example = "function f(%a){ if(%a) %a.x[1,2]=3; else echo(\"x\"); }";
    for (auto n : std::views::iota(std::size_t{0}) | std::views::take_while([&](std::size_t n) {
                      return n <= example.size();
                  })) {
        static_cast<void>(parse(std::string_view(example).substr(0, n)));
    }
    std::uint32_t rng = 1234;
    for (auto trial : std::views::iota(0, 3000)) {
        std::string input;
        for ([[maybe_unused]] auto i : std::views::iota(0) | std::views::take_while([&](int i) {
                                           return i < trial % 80;
                                       })) {
            rng = rng * 1664525 + 1013904223;
            input += static_cast<char>(rng >> 24);
        }
        const auto result = parse(input);
        CHECK(result.ok() || !result.diagnostics.empty());
    }
    std::vector<std::future<void>> jobs;
    for ([[maybe_unused]] auto i : std::views::iota(0, 4)) {
        jobs.push_back(std::async(std::launch::async, [] {
            for ([[maybe_unused]] auto j : std::views::iota(0, 50)) {
                CHECK(good("$a=1; echo($a);").statements.size() == 2);
            }
        }));
    }
    for (auto& job : jobs) {
        job.get();
    }
}

void tests() {
    lexer();
    structure();
    bounds();
}
}

int opent1_test_console_lexer_parser() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("console", "console_lexer_parser", opent1_test_console_lexer_parser);
