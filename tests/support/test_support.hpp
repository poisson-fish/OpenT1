#pragma once
#include <opent1/core/base.hpp>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace opent1::test {
inline void check(bool ok,
                  std::string_view expression,
                  std::source_location at = std::source_location::current()) {
    if (!ok) {
        throw std::runtime_error(std::format("{}:{}: {}", at.file_name(), at.line(), expression));
    }
}

template <class Fn>
void rejects(Fn&& fn, std::source_location at = std::source_location::current()) {
    try {
        std::forward<Fn>(fn)();
    } catch (const AssertException&) {
        return;
    }
    check(false, "Expected AssertException", at);
}

template <class Fn> int run(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "%s\n", ex.what());
        return 1;
    }
}
}

#define CHECK(...) ::opent1::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__)
