#pragma once
#include <chrono>
#include <cstdint>

namespace opent1::gfx {
// CPU work within an explicitly instrumented render. Timings overlap their
// enclosing frame stages; counters include attempted work on rejected frames.
struct RenderWorkStats {
    double terrainLodMs{}, lightSamplesMs{};
    std::uint64_t terrainLodCalls{}, terrainNodes{}, terrainValidatedHeights{};
    std::uint64_t terrainMergeHeights{}, terrainMaterialChecks{}, terrainDistanceRejects{};
    std::uint64_t lightSampleCalls{}, lightDeltaDecodes{}, lightSampleCopyBytes{};
    std::uint64_t terrainTileBuilds{}, terrainTileHits{}, terrainTileEvictions{};
    // Sum of resident payloads after each terrain LOD call; excludes caller snapshots.
    std::uint64_t terrainCacheEntries{}, terrainCacheBytes{};
    double skyMs{};
    std::uint64_t hardwareSkyPolygons{};
    std::uint64_t bitmapCopyBytes{}, layerScanPixels{}, surfaceInversions{};
    std::uint64_t interiorGeometryBuilds{}, interiorLightMapBuilds{}, interiorLightMapBytes{};
};

inline thread_local RenderWorkStats* currentRenderWorkStats{};

class ScopedRenderWorkStats {
public:
    explicit ScopedRenderWorkStats(RenderWorkStats& stats) noexcept
        : saved_(currentRenderWorkStats) {
        currentRenderWorkStats = &stats;
    }

    ~ScopedRenderWorkStats() {
        currentRenderWorkStats = saved_;
    }

    ScopedRenderWorkStats(const ScopedRenderWorkStats&) = delete;
    ScopedRenderWorkStats& operator=(const ScopedRenderWorkStats&) = delete;

private:
    RenderWorkStats* saved_;
};

class ScopedRenderWorkTimer {
public:
    explicit ScopedRenderWorkTimer(double RenderWorkStats::* field) noexcept
        : output_(currentRenderWorkStats ? &(currentRenderWorkStats->*field) : nullptr),
          start_(output_ ? Clock::now() : Clock::time_point{}) {}

    ~ScopedRenderWorkTimer() {
        if (output_) {
            *output_ += std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        }
    }

    ScopedRenderWorkTimer(const ScopedRenderWorkTimer&) = delete;
    ScopedRenderWorkTimer& operator=(const ScopedRenderWorkTimer&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    double* output_;
    Clock::time_point start_;
};
} // namespace opent1::gfx
