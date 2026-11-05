#include "test_support.hpp"
#include "test_registry.hpp"
#include <opent1/console/console.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
void examples() {
    const std::filesystem::path root(OPENT1_SCRIPT_EXAMPLES);
    for (const auto& [name, expected] : {
             std::pair{"score.cs", "8"}, {"config.cs", "25"}, {"callback.cs", "Alice:3"}}) {
        std::ifstream input(root / name);
        CHECK(input.is_open());
        const std::string source((std::istreambuf_iterator<char>(input)), {});
        opent1::Console console;
        const auto result = console.evaluate(source, name);
        if (!result.ok()) {
            throw std::runtime_error(result.diagnostics.front().format());
        }
        CHECK(result.value == expected);
        if (std::string_view(name) == "score.cs") {
            CHECK(console.evaluate("return Example::award(\"Alice\", -100);").value == "8");
        }
        if (std::string_view(name) == "config.cs") {
            CHECK(console.evaluate("return Example::clamp(-1);").value == "0");
            CHECK(console.evaluate("$Example::enabled=false; return Example::clamp(12);").value == "0");
        }
    }
}
int runExamples() { return opent1::test::run(examples); }
}
OPENT1_REGISTER_TEST("console", "documented_examples", runExamples);
