#pragma once
#include <opent1/console/console.hpp>
#include <opent1/core/types.hpp>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace opent1::game {

enum class CpuVendor : Int32 { Intel = 0, AMD = 1, Cyrix = 2, Unknown = 3 };

struct CpuSpeedEntry {
    Int32 hz{};
    Int32 maxHz{};
    Int32 pref{};
};

inline constexpr std::size_t kCpuSpeedTableCount = 19;
[[nodiscard]] std::span<const CpuSpeedEntry, kCpuSpeedTableCount> cpuSpeedTable() noexcept;

// Legacy cpuSpeed.cpp lookup over the 19-entry calibration table (including {0, 0, 0} sentinel).
void classifyCpuSpeed(Flt32 speedMhz, Int32& outHz, Int32& outPref) noexcept;
[[nodiscard]] std::string_view cpuVendorName(CpuVendor vendor) noexcept;
[[nodiscard]] CpuVendor parseCpuVendorId(std::string_view vendorId) noexcept;

struct CpuMetrics {
    Flt32 rawSpeed{333.0f};
    Int32 hz{333};
    Int32 pref{333};
    CpuVendor vendor{CpuVendor::Unknown};
    std::string vendorName{"Unknown"};
    std::string rawVendorId{};
};

// Safe cross-platform replacement for legacy cpuid.asm + cpuSpeed.cpp calibration.
[[nodiscard]] CpuVendor execCpuid(std::string* outRawVendorId = nullptr) noexcept;
void getCPUMetrics(
    Flt32* speed, Int32* hz, Int32* pref, Int32* cpuType, Flt32 overrideSpeed = 333.0f) noexcept;
[[nodiscard]] CpuMetrics detectCpuMetrics(Flt32 overrideSpeed = 333.0f);
void setCpuConsoleVars(Console& console, const CpuMetrics& metrics);
void setCPUConsoleVars(Console& console, Flt32 overrideSpeed = 333.0f);

} // namespace opent1::game
