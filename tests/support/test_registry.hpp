#pragma once
#include <string_view>
#include <vector>

namespace opent1::test {
struct Case {
    std::string_view suite;
    std::string_view name;
    int (*run)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> registered;
    return registered;
}

struct Register {
    Register(std::string_view suite, std::string_view name, int (*run)()) {
        cases().push_back({suite, name, run});
    }
};
}

#define OPENT1_REGISTER_TEST(suite, name, entry)                                                   \
    namespace {                                                                                    \
    const ::opent1::test::Register register_##entry{suite, name, entry};                           \
    }
