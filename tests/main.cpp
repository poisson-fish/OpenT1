#include "test_registry.hpp"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <string_view>

int main(int argc, char** argv) {
    using namespace std::literals;
    const bool list = argc == 2 && argv[1] == "--list"sv;
    std::string_view suite;
    if (argc == 3 && argv[1] == "--suite"sv) {
        suite = argv[2];
    } else if (argc != 1 && !list) {
        std::fputs("Usage: opent1_tests [--list | --suite NAME]\n", stderr);
        return 2;
    }
    auto& cases = opent1::test::cases();
    std::ranges::sort(cases, {}, &opent1::test::Case::name);
    unsigned count = 0, failed = 0;
    for (const auto& test : cases) {
        if (!suite.empty() && test.suite != suite) {
            continue;
        }
        if (list) {
            std::printf("%.*s/%.*s\n",
                        int(test.suite.size()),
                        test.suite.data(),
                        int(test.name.size()),
                        test.name.data());
            continue;
        }
        ++count;
        std::printf("[ RUN  ] %.*s/%.*s\n",
                    int(test.suite.size()),
                    test.suite.data(),
                    int(test.name.size()),
                    test.name.data());
        std::fflush(stdout);
        int result = 1;
        try {
            result = test.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
        } catch (...) {
            std::fputs("Unknown exception\n", stderr);
        }
        failed += result != 0;
        std::printf("[ %s ] %.*s\n",
                    result == 0 ? "PASS" : "FAIL",
                    int(test.name.size()),
                    test.name.data());
    }
    if (list) {
        return 0;
    }
    if (count == 0) {
        std::fputs("No matching tests\n", stderr);
        return 2;
    }
    std::printf("%u cases, %u failures\n", count, failed);
    return failed == 0 ? 0 : 1;
}
