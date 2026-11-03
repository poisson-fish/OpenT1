#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace opent1::detail {
// Owned allocation capacity, excluding object storage and allocator metadata.
inline std::size_t allocationBytes(const std::string& value) {
    const auto p = reinterpret_cast<std::uintptr_t>(value.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(&value);
    return p >= begin && p < begin + sizeof(value) ? 0 : value.capacity() + 1;
}

template <class T> std::size_t allocationBytes(const std::vector<T>& values) {
    std::size_t result = values.capacity() * sizeof(T);
    if constexpr (std::is_same_v<T, std::string>) {
        for (const auto& v : values)
            result += allocationBytes(v);
    } else if constexpr (requires(const T& value) { resourceResidentBytes(value); }) {
        for (const auto& v : values)
            result += resourceResidentBytes(v) - sizeof(T);
    }
    return result;
}
} // namespace opent1::detail
