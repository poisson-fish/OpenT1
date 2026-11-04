#include "test_support.hpp"
#include <ranges>
#include <opent1/console/console.hpp>
#include <opent1/core/res_manager.hpp>
#include <opent1/core/timer.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>

namespace {
using namespace opent1;
using namespace opent1::script;

std::string eval(Console& c, std::string_view source) {
    const auto result = c.evaluate(source, "unit.cs");
    if (!result.ok()) {
        throw std::runtime_error(result.diagnostics.front().format());
    }
    return result.value;
}

void error(const ExecutionResult& result, DiagnosticCode code) {
    CHECK(!result.ok());
    CHECK(result.diagnostics.front().code == code);
}

void basics() {
    Console c;
    CHECK(eval(c, "function owned(%a) { return %a+2; }").empty());
    CHECK(c.isFunction("OwNeD"));
    CHECK(c.execute("owned", std::array<std::string, 1>{"5"}).value == "7");
    CHECK(eval(c, "return owned(3);") == "5");
    CHECK(c.removeCommand("OWNED") && !c.removeCommand("owned"));
    CHECK(c.execute("owned").value == "False");
    CHECK(eval(c, "return 1;") == "1");
    CHECK(eval(c, "").empty());
    c.setVariable("MixedCase", "4");
    CHECK(c.getVariable("$MIXEDCASE") == "4" && c.hasVariable("mixedcase"));
    CHECK(eval(c, "$mixedCase+=1; return $MIXEDCASE;") == "5");
    CHECK(c.removeVariable("mixedCASE"));
    CHECK(!c.hasVariable("MixedCase"));
    c.setVariable("empty", "");
    CHECK(!c.hasVariable("empty"));
    const auto failed = c.evaluate("$partial=1; function incomplete(", "bad.cs");
    CHECK(!failed.ok() && failed.diagnostics.front().sourceName == "bad.cs");
    CHECK(!c.hasVariable("partial"));
    std::string terminated = "return 7;";
    terminated.push_back('\0');
    terminated += "$bad=1;";
    CHECK(eval(c, terminated) == "7" && !c.hasVariable("bad"));
    std::string bounded = "return 8; garbage";
    CHECK(eval(c, std::string_view(bounded).substr(0, 9)) == "8");
    CHECK(eval(c, "%root=9; function local() { return %root; } return local();").empty());
    CHECK(eval(c, "return %root;") == "9");
    CHECK(eval(c, "function show($g) { return $g; } $g=9;") == "9");
    CHECK(c.execute("show", std::array<std::string, 1>{"2"}).value == "9");
    CHECK(eval(c, "return show(3);") == "3");
    CHECK(c.getVariable("g") == "3");
    auto program = parse("function fromProgram(%x) { return %x*3; }", "owned.cs");
    CHECK(program.ok());
    CHECK(script::evaluate(c, std::move(*program.program)).ok());
    program = {};
    CHECK(c.execute("fromProgram", std::array<std::string, 1>{"7"}).value == "21");
    // The old empty no-return function could read residual string-buffer bytes.
    CHECK(eval(c, "function emptyFn() {} return emptyFn();").empty());
    CHECK(eval(c, "return floor(-2.9);") == "-2");
    CHECK(toBool("turtle") && !toBool("FALSE") && toNumber("turtle") == 0);
    CHECK(toNumber("tRuE") == 1 && toInteger("1e3") == 1 && toNumber("1e3") == 1000);
    CHECK(toInteger("2147483648") == 2147483647 && toInteger("-2147483649") == (-2147483647 - 1));
    CHECK(eval(c, "return 1 << 33;") == "2");
    CHECK(eval(c, "return 0x80000000 % -1;") == "0");
    CHECK(eval(c, "return 2147483648;") == "-2147483648");
    CHECK(eval(c, "return 4294967296;") == "0");
    // Wasteland HellsHammer.cs uses this literal in a function body.
    CHECK(eval(c, "function hugeExp() { return 10000000000000000000000000; } return hugeExp();") ==
          "1241513984");
}

void callbacks() {
    Console c;
    c.addCommand(
        71, "Engine::Move", [](Console& console, Int32 id, std::span<const std::string> args) {
            CHECK(id == 71);
            CHECK(args.size() == 3 && args[0] == "Engine::Move" && args[1] == "5" &&
                  args[2] == "7");
            console.setVariable("position", args[1] + " " + args[2]);
            return "moved";
        });
    CHECK(eval(c, "return ENGINE::MOVE(5,7);") == "moved");
    CHECK(c.getVariable("position") == "5 7");
    c.addCommand(0, "self", [](Console& console, auto, auto) {
        CHECK(console.removeCommand("self"));
        return console.evaluate("function self() { return 12; } return 3;").value;
    });
    CHECK(eval(c, "return self() @ \":\" @ self();") == "3:12");
    c.addCommand(1, "self", [](auto&, auto, auto) {
        return "native";
    });
    CHECK(eval(c, "return self();") == "native");
    c.addCommand(0, "null", [](auto&, auto, auto) {
        return std::nullopt;
    });
    CHECK(eval(c, "return null();") == "False" && c.execute("null").value == "True");
    const std::string first(200'000, 'a'), second(250'000, 'b');
    c.setVariable("first", first);
    c.addCommand(0, "nested", [&](Console& console, auto, auto) {
        CHECK(console.evaluate("function replacement() { return 17; }").ok());
        console.setVariable("first", "changed");
        return second;
    });
    c.addCommand(0, "join", [&](Console&, auto, auto args) {
        CHECK(args.size() == 4 && args[1] == first && args[2] == second && args[3] == "changed");
        return "stable";
    });
    CHECK(eval(c, "return join($first,nested(),$first);") == "stable");
    c.addCommand(0,
                 "throwing",
                 [](Console&, Int32, std::span<const std::string>) -> std::optional<std::string> {
                     throw std::runtime_error("engine failure");
                 });
    CHECK(eval(c, "function fail() { %x=5; throwing(); }").empty());
    const auto failed = c.evaluate("fail();", "caller.cs");
    error(failed, DiagnosticCode::CommandError);
    CHECK(failed.diagnostics.front().sourceName == "unit.cs");
    CHECK(failed.diagnostics.front().range.begin.column > 1);
    CHECK(eval(c, "return %x;").empty());
    CHECK(eval(c, "return replacement();") == "17");
    c.setOutput([&](auto) {
        c.setOutput({});
        CHECK(c.execute("floor", std::array<std::string, 1>{"3.5"}).value == "3");
    });
    CHECK(eval(c, "echo(\"outer\");") == "True");
    CHECK(c.log().back() == "outer");
}

void variables() {
    Console c;
    Int32 n = 1;
    float f = 2;
    double d = 3;
    bool b = false;
    c.setVariable("bound", "12");
    c.bindVariable("bound", n);
    CHECK(n == 12);
    c.bindVariable("float", f);
    c.bindVariable("double", d);
    c.bindVariable("bool", b);
    CHECK(eval(c, "$bound+=3; $float=1/3; $double=1/3; $bool=\"turtle\";") == "turtle");
    CHECK(n == 15 && f > 0.3333f && d > 0.3333 && b);
    n = 91;
    CHECK(c.getVariable("bound") == "91");
    CHECK(c.getVariable("double") == "0.333333");
    CHECK(eval(c, "$bound=\"True\"; $float=\"True\"; $double=\"True\";") == "True");
    CHECK(n == 0 && f == 0 && d == 0);
    CHECK(eval(c, "$bound=\"\";").empty());
    CHECK(c.hasVariable("bound") && n == 0);
    std::string stored;
    c.bindVariable(
        "custom",
        [&] {
            return stored;
        },
        [&](auto value) {
            stored = "engine:" + std::string(value);
        });
    CHECK(eval(c, "$custom=\"x\"; return $custom;") == "engine:x");
    c.bindVariable("oneShot",
                   [&] {
                       CHECK(c.removeVariable("oneShot"));
                       return "one";
                   },
                   {});
    CHECK(c.getVariable("oneShot") == "one" && !c.hasVariable("oneShot"));
    c.bindVariable("replace", {}, [&](auto) {
        CHECK(c.removeVariable("replace"));
        c.setVariable("replacement", "set");
    });
    c.setVariable("replace", "x");
    CHECK(c.getVariable("replacement") == "set");
    c.setVariable("Console::printLevel", "0");
    c.print("hidden");
    CHECK(c.log().empty());
    c.setVariable("Console::printLevel", "2");
    c.print("visible", 2);
    CHECK(c.log().size() == 1);
    c.setVariable("Console::logBufferEnabled", "False");
    c.print("unbuffered");
    CHECK(c.log().size() == 1);
    c.clearLog();
    CHECK(c.log().empty());
}

void limits() {
    Console native;
    CHECK(native.limits().maxSteps == ExecutionLimits::Unlimited &&
          native.limits().maxValueBytes == ExecutionLimits::Unlimited);
    CHECK(eval(native, "$plain=3; function plain() { return $plain; } return plain();") == "3");
    CHECK(eval(native, "for($i=0;$i<150000;$i++) {} return $i;") == "150000");
    CHECK(eval(native,
               "function recurse(%n) { if(%n==0) return 0; return recurse(%n-1)+1; } return "
               "recurse(80);") == "80");
    native.setVariable("large", std::string(1024 * 1024 + 1, 'x'));
    CHECK(eval(native, "return $large @ $large;").size() == 2 * (1024 * 1024 + 1));
    ExecutionLimits limits;
    limits.maxSteps = 1000;
    Console c(nullptr, limits);
    error(c.evaluate("while(1) {}", "loop.cs"), DiagnosticCode::ExecutionLimit);
    CHECK(eval(c, "return 2;") == "2");
    error(c.evaluate("function forever() { forever(); } forever();"),
          DiagnosticCode::ExecutionLimit);
    CHECK(eval(c, "return %x;").empty());
    std::string deep = "return 1";
    for ([[maybe_unused]] auto i : std::views::iota(0, 5000)) {
        deep += "+1";
    }
    deep += ';';
    error(c.evaluate(deep), DiagnosticCode::ExecutionLimit);
    limits.maxValueBytes = 64;
    limits.maxArguments = 2;
    Console bounded(nullptr, limits);
    bounded.setVariable("long", std::string(40, 'a'));
    try {
        bounded.setVariable("tooLong", std::string(65, 'x'));
        CHECK(false);
    } catch (const std::length_error&) {
    }
    error(bounded.evaluate("return $long @ $long;"), DiagnosticCode::ExecutionLimit);
    error(bounded.evaluate("echo(1,2,3);"), DiagnosticCode::ExecutionLimit);
    error(bounded.execute("echo", std::array<std::string, 3>{"a", "b", "c"}),
          DiagnosticCode::ExecutionLimit);
    bounded.addCommand(0, "huge", [](auto&, auto, auto) {
        return std::string(100, 'x');
    });
    error(bounded.execute("huge"), DiagnosticCode::ExecutionLimit);
    bounded.setScriptLoader([](auto) {
        return std::optional(std::string(17 * 1024 * 1024, 'x'));
    });
    error(bounded.executeFile("huge"), DiagnosticCode::SourceLimit);
    limits.maxDepth = 0;
    Console noDepth(nullptr, limits);
    error(noDepth.evaluate("return 1;"), DiagnosticCode::ExecutionLimit);
    limits.maxDepth = 5000;
    Console ceiling(nullptr, limits);
    CHECK(ceiling.limits().maxDepth == 5000);
    error(ceiling.evaluate(deep), DiagnosticCode::ExecutionLimit);
    Program invalid;
    invalid.sourceName = "invalid.cs";
    invalid.statements = {noNode};
    error(script::evaluate(c, std::move(invalid)), DiagnosticCode::InvalidProgram);
    Program cyclic;
    cyclic.nodes.push_back({{}, BinaryExpr{punctuation('+'), 0, 0}});
    cyclic.nodes.push_back({{}, ReturnStmt{0}});
    cyclic.statements = {1};
    error(script::evaluate(c, std::move(cyclic)), DiagnosticCode::InvalidProgram);
    // eval/callback recursion shares the same budget and can recover afterward.
    c.addCommand(0, "nestedLoop", [](Console& console, auto, auto) {
        return console.evaluate("while(1) {}").value;
    });
    error(c.evaluate("nestedLoop();"), DiagnosticCode::ExecutionLimit);
    CHECK(eval(c, "return 4;") == "4");
}

struct Data : ConsoleDataManager {
    int depth = 0, ended = 0;

    void beginObject(std::string_view, std::optional<std::string_view>) override {
        ++depth;
    }

    void endObject() override {
        --depth;
        ++ended;
    }

    void setDataField(std::optional<std::string_view>,
                      std::string_view name,
                      std::optional<std::string_view>,
                      std::span<const std::string>) override {
        if (name == "fail") {
            throw std::runtime_error("bad field");
        }
    }
};

struct BlockData : ConsoleDataManager {
    int begins{}, ends{}, aborts{}, fields{};
    bool failBegin{}, failEnd{};
    std::function<void()> begin;

    void declareDataBlock(std::string_view, std::string_view) override {
        if (failBegin) {
            throw std::runtime_error("begin failed");
        }
        ++begins;
        if (begin) {
            begin();
        }
    }

    void endDataBlock() override {
        if (failEnd) {
            throw std::runtime_error("commit failed");
        }
        ++ends;
    }

    void abortDataBlock() override {
        ++aborts;
    }

    void setDataField(std::optional<std::string_view>,
                      std::string_view name,
                      std::optional<std::string_view>,
                      std::span<const std::string>) override {
        ++fields;
        if (name == "fail") {
            throw std::runtime_error("field failed");
        }
    }
};

void objects() {
    Console c;
    auto manager = std::make_shared<Data>();
    c.setDataManager(manager);
    const auto result = c.evaluate("instant Group { instant Child { fail=1; }; };", "object.cs");
    error(result, DiagnosticCode::CommandError);
    CHECK(manager->depth == 0 && manager->ended == 2);
    CHECK(result.diagnostics.front().sourceName == "object.cs");
    CHECK(eval(c, "instant Thing { good=1; };") == "1");
    CHECK(manager->depth == 0);
    c.setDataManager({});
    CHECK(eval(c, "return object.field;").empty());
    auto blocks = std::make_shared<BlockData>();
    auto replacement = std::make_shared<BlockData>();
    c.setDataManager(blocks);
    blocks->begin = [&] {
        c.setDataManager(replacement);
    };
    CHECK(eval(c, "Thing Block { good=1; good=2; };") == "2");
    CHECK(blocks->begins == 1 && blocks->fields == 2 && blocks->ends == 1 &&
          replacement->fields == 0);
    c.setDataManager(blocks);
    error(c.evaluate("Thing Failed { good=1; fail=2; };"), DiagnosticCode::CommandError);
    CHECK(blocks->aborts == 1 && replacement->aborts == 0);
    blocks->begin = {};
    blocks->failEnd = true;
    c.setDataManager(blocks);
    error(c.evaluate("Thing Failed { good=1; };"), DiagnosticCode::CommandError);
    CHECK(blocks->aborts == 2);
    blocks->failEnd = false;
    blocks->failBegin = true;
    error(c.evaluate("Thing Failed { good=1; };"), DiagnosticCode::CommandError);
    CHECK(blocks->aborts == 2);
    blocks->failBegin = false;
    CHECK(eval(c, "Thing Recovered { good=3; };") == "3");
}

void files() {
    const auto root =
        std::filesystem::temp_directory_path() /
        std::format("opent1-runtime-{}", Timer::Clock::now().time_since_epoch().count());
    CHECK(std::filesystem::create_directories(root));
    {
        std::ofstream file(root / "child.cs");
        file << "function loaded(%x) { return %x+4; } $loaded+=1;";
    }
    {
        std::ofstream file(root / "parent.mis");
        file << "exec(\"child\"); exec(\"child\");";
    }
    {
        std::ofstream file(root / "broken.cs");
        file << "$partial=1; $bad=;";
    }
    {
        std::ofstream file(root / "empty.cs");
    }
    ResourceManager resources;
    CHECK(resources.setSearchPaths(std::array{root}));
    Console c(&resources);
    CHECK(c.executeFile("parent.mis").value == "True");
    CHECK(c.getVariable("loaded") == "2");
    CHECK(c.execute("loaded", std::array<std::string, 1>{"3"}).value == "7");
    CHECK(c.executeFile("missing").value == "False" && c.executeFile("empty").value == "False");
    const auto failed = c.executeFile("broken");
    CHECK(!failed.ok() && failed.value == "False");
    CHECK(failed.diagnostics.front().sourceName == "broken.cs" && !c.hasVariable("partial"));
    CHECK(c.executeFile("../child").value == "False");
    const std::string saved = "line\n\t\"\\";
    c.setVariable("Saved::B", saved);
    c.setVariable("Saved::A", "1");
    const std::array<std::string, 2> exportArgs{"Saved::*", (root / "saved.cs").string()};
    CHECK(c.execute("export", exportArgs).value == "True");
    std::ifstream exported(root / "saved.cs", std::ios::binary);
    const std::string text(std::istreambuf_iterator<char>(exported), {});
    CHECK(text == "$Saved::A = \"1\";\r\n$Saved::B = \"line\\n\\t\\\"\\\\\";\r\n");
    CHECK(c.removeVariable("Saved::A") && c.removeVariable("Saved::B"));
    CHECK(eval(c, text) == saved && c.getVariable("saved::b") == saved);
    c.bindVariable("ExportOnce",
                   [&] {
                       CHECK(c.removeVariable("ExportOnce"));
                       c.setVariable("new", "value");
                       return "copied";
                   },
                   {});
    CHECK(eval(c, "export(\"ExportOnce\");") == "True");
    c.setScriptLoader([](auto name) -> std::optional<std::string> {
        if (name == "recursive.cs") {
            return "exec(\"recursive\");";
        }
        return std::nullopt;
    });
    error(c.executeFile("recursive"), DiagnosticCode::ExecutionLimit);
    CHECK(eval(c, "return loaded(1);") == "5");
    // Remove only the known files and then their now-empty unique directory.
    exported.close();
    for (const auto file : {"child.cs", "parent.mis", "broken.cs", "empty.cs", "saved.cs"}) {
        CHECK(std::filesystem::remove(root / file));
    }
    CHECK(std::filesystem::remove(root));
}

void independence() {
    const auto run = [] {
        Console c;
        CHECK(c.evaluate("$global=1; ID_ISOLATED=1,\"text\"; function f(%n) { return %n+$global; }")
                  .ok());
        return eval(c, "return f(4);");
    };
    auto a = std::async(std::launch::async, run);
    auto b = std::async(std::launch::async, run);
    CHECK(a.get() == "5" && b.get() == "5");
    Console isolated;
    CHECK(!isolated.hasVariable("global") && !isolated.isFunction("f"));
    CHECK(isolated.tags().defineToId("ID_ISOLATED") == 0);
}

void tests() {
    basics();
    callbacks();
    variables();
    limits();
    objects();
    files();
    independence();
    // Exercise the script-facing native modes against actual file contents,
    // including append, open/close per line and retained-handle teardown.
    const auto oldDirectory = std::filesystem::current_path();
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("opent1-console-log-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);

    struct RestoreDirectory {
        std::filesystem::path path;

        ~RestoreDirectory() {
            std::filesystem::current_path(path);
        }
    } restore{oldDirectory};

    std::filesystem::current_path(directory);
    {
        std::ofstream seed("console.log", std::ios::binary);
        seed << "previous\r\n";
    }
    {
        Console c;
        CHECK(c.evaluate("$Console::logMode=1; echo(\"append\"); $Console::logMode=2; "
                         "echo(\"retained\"); $Console::logMode=0; echo(\"disabled\");")
                  .ok());
        CHECK(c.evaluate("$Console::logMode=2; echo(\"teardown\");").ok());
    }
    std::ifstream input("console.log", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    CHECK(text.starts_with("previous\r\n"));
    CHECK(text.find("append\r\n") != text.npos);
    CHECK(text.find("retained\r\n") != text.npos);
    CHECK(text.find("teardown\r\n") != text.npos);
    CHECK(text.find("disabled") == text.npos);
    input.close();
    std::filesystem::remove("console.log");
    std::filesystem::current_path(oldDirectory);
    std::filesystem::remove(directory);
}
} // namespace

int opent1_test_console_runtime() {
    return opent1::test::run(tests);
}

#include "test_registry.hpp"
OPENT1_REGISTER_TEST("console", "console_runtime", opent1_test_console_runtime);
