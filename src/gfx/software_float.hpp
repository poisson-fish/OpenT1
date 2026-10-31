#pragma once
#include <cmath>
#include <utility>

namespace opent1::gfx::detail {
// The original rasterizer runs x87 extended precision with rounding toward
// zero, then stores double planes and 16.16 integers. A nearest-rounded double
// can cross an exact texel/shade boundary. Keep the extra eleven significand
// bits explicitly, without modifying the host FPU or requiring long double
// (which has only double precision on MSVC).
class RasterFloat {
public:
    RasterFloat(double value = 0) : high_(value) {}

    [[nodiscard]] double stored() const {
        return (high_ > 0 && low_ < 0) || (high_ < 0 && low_ > 0) ? std::nextafter(high_, 0.)
                                                                  : high_;
    }

    [[nodiscard]] float storedFloat() const {
        const double value = stored();
        float result = float(value);
        if ((result > 0 && double(result) > value) || (result < 0 && double(result) < value)) {
            result = std::nextafter(result, 0.f);
        }
        return result;
    }

    friend RasterFloat operator+(RasterFloat a, RasterFloat b) {
        const auto [high, error] = sum(a.high_, b.high_);
        return rounded(high, (error + a.low_) + b.low_);
    }

    friend RasterFloat operator-(RasterFloat a, RasterFloat b) {
        return a + RasterFloat(-b.high_, -b.low_);
    }

    friend RasterFloat operator*(RasterFloat a, RasterFloat b) {
        const double high = a.high_ * b.high_;
        const double low =
            (std::fma(a.high_, b.high_, -high) + a.high_ * b.low_) + a.low_ * b.high_;
        return rounded(high, low + a.low_ * b.low_);
    }

    friend RasterFloat operator/(RasterFloat a, RasterFloat b) {
        const double high = a.high_ / b.high_;
        // Compute the residual at double-double precision before the x87 round.
        const double product = b.high_ * high;
        const double productError = std::fma(b.high_, high, -product) + b.low_ * high;
        const auto [remainder, error] = sum(a.high_, -product);
        return rounded(high, ((remainder + (error + a.low_ - productError)) / b.high_));
    }

private:
    double high_{}, low_{};

    RasterFloat(double high, double low) : high_(high), low_(low) {}

    static std::pair<double, double> sum(double a, double b) {
        const double high = a + b, part = high - a;
        return {high, (a - (high - part)) + (b - part)};
    }

    static RasterFloat rounded(double high, double low) {
        const auto normalized = sum(high, low);
        high = normalized.first;
        low = normalized.second;
        if (!std::isfinite(high) || high == 0) {
            return RasterFloat(high);
        }
        int exponent{};
        (void)std::frexp(high, &exponent);
        const double quantum = std::ldexp(1., exponent - 64);
        if (quantum == 0) {
            return RasterFloat(high);
        }
        const double fraction = low / quantum;
        low = (high > 0 ? std::floor(fraction) : std::ceil(fraction)) * quantum;
        const auto result = sum(high, low);
        return RasterFloat(result.first, result.second);
    }
};
}
