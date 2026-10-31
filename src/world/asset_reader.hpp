#pragma once
#include <opent1/core/stream_io.hpp>
#include <opent1/math/point.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <optional>
#include <vector>

namespace opent1::world::asset_detail {
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t position{};
    bool ok{true};

    std::span<const std::byte> take(std::size_t count) {
        if (!ok || count > bytes.size() - position) {
            ok = false;
            return {};
        }
        auto result = bytes.subspan(position, count);
        position += count;
        return result;
    }

    template <class T> T scalar() {
        const auto raw = take(sizeof(T));
        if (!ok) {
            return {};
        }
        UInt32 bits{};
        std::ranges::for_each(std::views::iota(std::size_t{}, sizeof(T)), [&](auto i) {
            bits |= UInt32(std::to_integer<UInt8>(raw[i])) << (8 * i);
        });
        if constexpr (sizeof(T) == 4) {
            return std::bit_cast<T>(bits);
        } else if constexpr (sizeof(T) == 2) {
            return std::bit_cast<T>(UInt16(bits));
        } else {
            return std::bit_cast<T>(UInt8(bits));
        }
    }

    Flt32 real() {
        const auto v = scalar<Flt32>();
        if (!std::isfinite(v) || std::abs(v) > 1e12f) {
            ok = false;
        }
        return v;
    }

    Point3F point() {
        const auto x = real(), y = real(), z = real();
        return {x, y, z};
    }

    UInt32 count(UInt32 maximum = 65536) {
        const auto n = scalar<UInt32>();
        if (n > maximum) {
            ok = false;
            return 0;
        }
        return n;
    }

    template <class T, class Fn>
    void array(std::vector<T>& values, std::size_t n, std::size_t stride, Fn fn) {
        if (!ok || n > (bytes.size() - position) / stride) {
            ok = false;
            return;
        }
        values.resize(n);
        std::ranges::for_each(values, [&](auto& v) {
            if (ok) {
                v = fn();
            }
        });
    }

    bool done() const {
        return ok && position == bytes.size();
    }

    Reader object(UInt32 expected, std::string_view name, Int32& version) {
        const auto tag = scalar<UInt32>(), flags = scalar<UInt32>(), n = flags & 0x7fffffffU;
        const auto raw = take(n);
        const std::size_t mask = (flags & 0x80000000U) ? 3 : 1;
        // Standalone native files may omit the final block alignment bytes.
        if (position < bytes.size()) {
            take(((std::size_t(n) + mask) & ~mask) - n);
        }
        Reader child{raw};
        child.ok = ok;
        if (tag == makeFourCC('P', 'E', 'R', 'S')) {
            const auto length = child.scalar<Int16>();
            if (length <= 0 || length > 128) {
                child.ok = false;
                return child;
            }
            auto text = child.take(std::size_t(length));
            if (length & 1) {
                child.take(1);
            }
            if (text.size() != name.size() || !std::ranges::equal(text, name, {}, [](auto b) {
                    return char(std::to_integer<UInt8>(b));
                })) {
                child.ok = false;
            }
        } else if (!expected || tag != expected) {
            child.ok = false;
        }
        version = child.scalar<Int32>();
        return child;
    }
};

inline std::optional<std::vector<std::byte>> file(StreamIO& stream, UInt32 size) {
    if (size < 12 || size > 64U * 1024U * 1024U) {
        return {};
    }
    std::vector<std::byte> bytes(size);
    if (!stream.readBytes(bytes)) {
        return {};
    }
    return bytes;
}

inline std::optional<std::string> name(std::span<const std::byte> bytes, UInt32 at) {
    if (at >= bytes.size() || (at && bytes[at - 1] != std::byte{})) {
        return {};
    }
    const auto raw = bytes.subspan(at);
    const auto end = std::ranges::find(raw, std::byte{});
    if (end == raw.end() || end - raw.begin() > 255) {
        return {};
    }
    std::string result;
    std::ranges::transform(raw.begin(), end, std::back_inserter(result), [](auto b) {
        return char(std::to_integer<UInt8>(b));
    });
    return result;
}
} // namespace opent1::world::asset_detail
