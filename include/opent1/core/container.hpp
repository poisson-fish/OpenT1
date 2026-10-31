#pragma once
#include <algorithm>
#include <functional>
#include <memory_resource>
#include <span>
#include <vector>

namespace opent1 {
// A caller-owned pool replaces the global raw allocation cache. It must outlive
// its vectors and needs external synchronization between threads.
using RVectorAlloc = std::pmr::unsynchronized_pool_resource;
template <class T> using RVector = std::pmr::vector<T>;

// Typed, bounded replacement for m_qsort; no process-wide sorting lock.
template <class T, class Compare = std::ranges::less, class Projection = std::identity>
void sort(std::span<T> values, Compare compare = {}, Projection projection = {}) {
    std::ranges::sort(values, std::move(compare), std::move(projection));
}
} // namespace opent1
