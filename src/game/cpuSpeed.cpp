#include <opent1/game/cpu_speed.hpp>
#include <cstring>
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
#include <cpuid.h>
#endif

namespace opent1::game {
namespace {

constexpr std::array<CpuSpeedEntry, kCpuSpeedTableCount> kCpuSpeeds{{
    {60, 66, 133},   {90, 94, 133},   {100, 110, 133}, {120, 125, 133}, {133, 140, 133},
    {166, 172, 166}, {200, 210, 200}, {233, 240, 233}, {266, 280, 266}, {300, 310, 300},
    {333, 340, 333}, {350, 355, 333}, {400, 410, 333}, {450, 460, 333}, {500, 510, 333},
    {533, 550, 333}, {566, 570, 333}, {600, 610, 333}, {0, 0, 0},
}};

constexpr std::array<std::string_view, 4> kVendorNames{{
    "Intel",
    "AMD",
    "Cyrix",
    "Unknown",
}};

} // namespace

std::span<const CpuSpeedEntry, kCpuSpeedTableCount> cpuSpeedTable() noexcept {
    return kCpuSpeeds;
}

void classifyCpuSpeed(Flt32 speedMhz, Int32& outHz, Int32& outPref) noexcept {
    if (speedMhz < 0.0f) {
        outHz = -1;
        outPref = -1;
        return;
    }
    std::size_t cpu = 0;
    while (kCpuSpeeds[cpu].hz != 0 && speedMhz > static_cast<Flt32>(kCpuSpeeds[cpu].maxHz)) {
        ++cpu;
    }
    outHz = kCpuSpeeds[cpu].hz;
    outPref = kCpuSpeeds[cpu].pref;
}

std::string_view cpuVendorName(CpuVendor vendor) noexcept {
    const auto idx = static_cast<Int32>(vendor);
    if (idx < 0 || idx >= static_cast<Int32>(kVendorNames.size())) {
        return kVendorNames[3];
    }
    return kVendorNames[static_cast<std::size_t>(idx)];
}

CpuVendor parseCpuVendorId(std::string_view vendorId) noexcept {
    if (vendorId == "GenuineIntel" || vendorId == "Intel") {
        return CpuVendor::Intel;
    }
    if (vendorId == "AuthenticAMD" || vendorId == "AMD") {
        return CpuVendor::AMD;
    }
    if (vendorId == "CyrixInstead" || vendorId == "Cyrix") {
        return CpuVendor::Cyrix;
    }
    return CpuVendor::Unknown;
}

CpuVendor execCpuid(std::string* outRawVendorId) noexcept {
    char vendorBuf[13]{};
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    int regs[4]{};
    __cpuid(regs, 0);
    std::memcpy(vendorBuf + 0, &regs[1], 4);
    std::memcpy(vendorBuf + 4, &regs[3], 4);
    std::memcpy(vendorBuf + 8, &regs[2], 4);
    vendorBuf[12] = '\0';
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) != 0) {
        std::memcpy(vendorBuf + 0, &ebx, 4);
        std::memcpy(vendorBuf + 4, &edx, 4);
        std::memcpy(vendorBuf + 8, &ecx, 4);
        vendorBuf[12] = '\0';
    }
#endif
    if (outRawVendorId) {
        *outRawVendorId = vendorBuf;
    }
    return parseCpuVendorId(vendorBuf);
}

void getCPUMetrics(
    Flt32* speed, Int32* hz, Int32* pref, Int32* cpuType, Flt32 overrideSpeed) noexcept {
    if (speed) {
        *speed = overrideSpeed;
    }
    const auto vendor = execCpuid();
    if (cpuType) {
        *cpuType = static_cast<Int32>(vendor);
    }
    Int32 computedHz = 0;
    Int32 computedPref = 0;
    classifyCpuSpeed(overrideSpeed, computedHz, computedPref);
    if (hz) {
        *hz = computedHz;
    }
    if (pref) {
        *pref = computedPref;
    }
}

CpuMetrics detectCpuMetrics(Flt32 overrideSpeed) {
    CpuMetrics metrics{};
    metrics.rawSpeed = overrideSpeed;
    metrics.vendor = execCpuid(&metrics.rawVendorId);
    metrics.vendorName = std::string(cpuVendorName(metrics.vendor));
    classifyCpuSpeed(overrideSpeed, metrics.hz, metrics.pref);
    return metrics;
}

void setCpuConsoleVars(Console& console, const CpuMetrics& metrics) {
    console.setVariable("CPU::estimatedSpeed", std::to_string(metrics.hz));
    console.setVariable("CPU::recommendPrefSpeed", std::to_string(metrics.pref));
    console.setVariable("CPU::Vendor", metrics.vendorName);
}

void setCPUConsoleVars(Console& console, Flt32 overrideSpeed) {
    setCpuConsoleVars(console, detectCpuMetrics(overrideSpeed));
}

} // namespace opent1::game
