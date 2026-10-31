#pragma once
#include <opent1/math/point.hpp>
#include <bit>
#include <cmath>
#include <optional>
#include <vector>

namespace opent1::gfx {
// Native TS directional shadow bytes, repeated in world XY. Value ownership
// prevents a light provider from changing an already captured frame.
struct DirectionalShadowMap {
    UInt32 side{};
    Point2F offset{};
    Flt32 scale{1};
    std::vector<UInt8> samples;
    bool operator==(const DirectionalShadowMap&) const = default;

    [[nodiscard]] bool valid() const {
        return side && side <= 4096 && std::has_single_bit(side) &&
               samples.size() == std::size_t(side) * side && std::isfinite(offset.x) &&
               std::isfinite(offset.y) && std::isfinite(scale);
    }

    [[nodiscard]] std::optional<Flt32> attenuation(Point3F world) const {
        if (!valid()) {
            return {};
        }
        const auto x = (world.x - offset.x) * scale, y = (world.y - offset.y) * scale;
        // Native Int32 conversion truncates toward zero before wrapping. Reject
        // overflow rather than invoking the original undefined conversion.
        if (!std::isfinite(x) || !std::isfinite(y) || double(x) < -2147483648. ||
            double(x) >= 2147483648. || double(y) < -2147483648. || double(y) >= 2147483648.) {
            return {};
        }
        const auto mask = side - 1;
        return Flt32(samples[std::size_t(UInt32(Int32(y)) & mask) * side +
                             (UInt32(Int32(x)) & mask)]) /
               255.f;
    }
};
}
