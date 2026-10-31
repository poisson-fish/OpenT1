#include <opent1/audio/audio.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace opent1::audio {
namespace {
UInt32 le(std::span<const std::byte> b, std::size_t p, std::size_t n) {
    UInt32 v{};
    for (std::size_t i = 0; i < n; ++i) {
        v |= UInt32(std::to_integer<UInt8>(b[p + i])) << (8 * i);
    }
    return v;
}

bool tag(std::span<const std::byte> b, std::size_t p, const char* s) {
    return std::memcmp(b.data() + p, s, 4) == 0;
}
}

UInt32 Wave::frames() const {
    return (channels == 1 || channels == 2) && (bits == 8 || bits == 16) &&
                   samples.size() / (channels * (bits / 8)) <= std::numeric_limits<UInt32>::max()
               ? UInt32(samples.size() / (channels * (bits / 8)))
               : 0;
}

bool Wave::readFrames(UInt64 frame, std::span<std::byte> output, bool loop) const {
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16) || samples.empty() ||
        samples.size() % (channels * (bits / 8)) || output.size() % (channels * (bits / 8))) {
        return false;
    }
    const auto stride = std::size_t(channels * (bits / 8));
    const auto count = samples.size() / stride;
    std::size_t at{};
    // All writes follow complete validation, with bounded copies across wraps.
    while (at < output.size()) {
        if (loop) {
            frame %= count;
        }
        if (frame >= count) {
            std::fill(output.begin() + at, output.end(), bits == 8 ? std::byte{128} : std::byte{0});
            break;
        }
        const auto n = std::min(output.size() - at, (count - std::size_t(frame)) * stride);
        std::copy_n(samples.begin() + std::size_t(frame) * stride, n, output.begin() + at);
        at += n;
        frame += n / stride;
    }
    return true;
}

std::unique_ptr<Wave> Wave::load(StreamIO& stream, UInt32 size) {
    if (size < 12 || size > static_cast<UInt32>(std::numeric_limits<Int32>::max())) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!stream.readBytes(bytes)) {
        return {};
    }
    const std::span<const std::byte> b(bytes);
    if (!tag(b, 0, "RIFF") || !tag(b, 8, "WAVE")) {
        return {};
    }
    const UInt64 declared = UInt64(le(b, 4, 4)) + 8;
    // Shipped voice archives put the last odd data pad outside RIFF's length.
    if (declared < 12 || declared > size ||
        (declared != size && declared + (declared & 1U) != size)) {
        return {};
    }
    const auto end = std::size_t(declared);
    auto w = std::make_unique<Wave>();
    bool format = false, data = false;
    for (std::size_t p = 12; p < end;) {
        if (end - p < 8) {
            return {};
        }
        const auto n = le(b, p + 4, 4);
        const auto begin = p + 8;
        if (n > end - begin || (n & 1U) > b.size() - begin - n) {
            return {};
        }
        if (tag(b, p, "fmt ")) {
            if (format || data || n < 16 || le(b, begin, 2) != 1) {
                return {};
            }
            w->channels = UInt16(le(b, begin + 2, 2));
            w->rate = le(b, begin + 4, 4);
            w->bits = UInt16(le(b, begin + 14, 2));
            if ((w->channels != 1 && w->channels != 2) || (w->bits != 8 && w->bits != 16) ||
                w->rate < 1000 || w->rate > 192000) {
                return {};
            }
            const UInt32 align = w->channels * (w->bits / 8);
            if (le(b, begin + 12, 2) != align || le(b, begin + 8, 4) != w->rate * align) {
                return {};
            }
            format = true;
        } else if (tag(b, p, "data")) {
            if (!format || data || !n || n % (w->channels * (w->bits / 8))) {
                return {};
            }
            w->samples.assign(b.begin() + begin, b.begin() + begin + n);
            data = true;
        }
        p = begin + n + (n & 1U);
    }
    return data ? std::move(w) : nullptr;
}

bool registerWaveLoader(ResourceManager& m) {
    return m.registerLoader<Wave>(".wav", Wave::load);
}

bool valid(const TMat3F& t) {
    for (const auto& r : t.m) {
        for (float v : r) {
            if (!std::isfinite(v) || std::abs(v) > 1) {
                return false;
            }
        }
    }
    return std::isfinite(t.p.x) && std::isfinite(t.p.y) && std::isfinite(t.p.z) &&
           std::max({std::abs(t.p.x), std::abs(t.p.y), std::abs(t.p.z)}) <= 1e9f;
}

bool valid(const Profile& p) {
    return !(p.flags & ~UInt32(Loop | Stream | Pitch | Software3D | Hardware3D)) &&
           std::isfinite(p.baseVolume) && p.baseVolume >= 0 && p.baseVolume <= 1 &&
           std::isfinite(p.coneOutsideVolume) && p.coneOutsideVolume >= 0 &&
           p.coneOutsideVolume <= 1 && p.coneInsideAngle <= p.coneOutsideAngle &&
           p.coneOutsideAngle <= 360 && std::isfinite(p.minDistance) &&
           std::isfinite(p.maxDistance) && p.minDistance >= 0 && p.maxDistance > p.minDistance &&
           p.maxDistance <= 1e9f && std::isfinite(p.coneVector.x) &&
           std::isfinite(p.coneVector.y) && std::isfinite(p.coneVector.z) &&
           std::abs(p.coneVector.len() - 1) < .001f;
}

float convertFromLinear(float v) {
    return float(.069 * -float(std::log(std::max(v, .01f)) / std::log(2.0)));
}

Spatial
evaluate(const Profile& p, const TMat3F& transform, const Listener& listener, float master) {
    if (!valid(p) || !valid(transform) || !valid(listener.transform) || !std::isfinite(master) ||
        master < 0 || master > 1) {
        throw std::invalid_argument("invalid native audio state");
    }
    Spatial s;
    s.attenuation = p.baseVolume + convertFromLinear(master);
    if (p.flags & (Software3D | Hardware3D)) {
        auto relative = transform.p - listener.transform.p;
        float distance = relative.lenf();
        if (distance * distance < 1e-12f) {
            distance = .00001f;
        }
        const auto& r = listener.transform.m[0];
        s.pan = (r[0] * relative.x + r[1] * relative.y + r[2] * relative.z) / distance * .2f;
        if (p.coneOutsideVolume * p.coneOutsideVolume >= 1e-12f) {
            Point3F cone;
            m_mul(p.coneVector, static_cast<const RMat3F&>(transform), cone);
            const float x = 1.f - ((m_dot(cone, -relative) / distance + 1.f) / 2.f);
            const float inside = float(p.coneInsideAngle) / 360.f,
                        outside = float(p.coneOutsideAngle) / 360.f;
            if (x >= outside) {
                s.attenuation += p.coneOutsideVolume / 12.f;
            } else if (x > inside) {
                s.attenuation += (p.coneOutsideVolume / 12.f) * ((x - inside) / (outside - inside));
            }
        }
        if (distance >= p.maxDistance) {
            s.attenuation = 1;
        } else if (distance > p.minDistance) {
            const float f = (distance - p.minDistance) / (p.maxDistance - p.minDistance);
            s.attenuation += (1.f - s.attenuation) * std::max(f, 0.f);
        }
    } else {
        s.attenuation = std::min(s.attenuation, 1.f);
    }
    s.millibels = static_cast<Int32>(std::clamp(s.attenuation, 0.f, 1.f) * -10000.f);
    s.gain = s.millibels <= -10000 ? 0.f : std::pow(10.f, float(s.millibels) / 2000.f);
    return s;
}

UInt32 nativeBufferBytes(const Wave& w, bool stream) {
    if (!w.frames() || w.rate < 1000 || w.rate > 192000) {
        return 0;
    }
    const auto n = UInt32(w.samples.size());
    const auto ring = w.rate * w.channels * (w.bits / 8) * 2U;
    return stream && UInt64(n) * 2 >= ring ? ring : n;
}
} // namespace opent1::audio
