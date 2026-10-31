#pragma once
#include "test_support.hpp"
#include <opent1/world/ts_shape.hpp>
#include <opent1/core/mem_stream.hpp>
#include <algorithm>
#include <bit>
#include <ranges>

namespace opent1::test::ts {
// Hand-authored file-format layouts, independent of the decoder. Padding is
// deliberately nonzero to catch accidental native-layout/zero-padding assumptions.
struct Bytes {
    std::vector<std::byte> data;

    template <class T> void put(T value) {
        using U = std::conditional_t<sizeof(T) == 4,
                                     UInt32,
                                     std::conditional_t<sizeof(T) == 2, UInt16, UInt8>>;
        const auto bits = std::bit_cast<U>(value);
        std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(T)), [&](auto i) {
            data.push_back(std::byte(UInt64(bits) >> (i * 8) & 255));
        });
    }

    void integer(Int32 v) {
        put(v);
    }

    void real(Flt32 v) {
        put(v);
    }

    void point(Point3F p) {
        real(p.x);
        real(p.y);
        real(p.z);
    }

    void text(std::string_view s, std::size_t size) {
        std::ranges::transform(s, std::back_inserter(data), [](char c) {
            return std::byte(c);
        });
        data.insert(data.end(), size - s.size(), std::byte{});
    }

    void append(const Bytes& b) {
        data.insert(data.end(), b.data.begin(), b.data.end());
    }

    Bytes pers(std::string_view name, Int32 version) const {
        Bytes body;
        body.put(Int16(name.size()));
        body.text(name, (name.size() + 1) & ~std::size_t{1});
        body.integer(version);
        body.append(*this);
        Bytes result;
        result.put(makeFourCC('P', 'E', 'R', 'S'));
        result.put(UInt32(body.data.size()));
        result.append(body);
        if (body.data.size() & 1) {
            result.put(UInt8{0xa5});
        }
        return result;
    }
};

inline Bytes material(Int32 version = 4) {
    Bytes b;
    b.integer(1);
    b.integer(1);
    b.put(UInt32{0x103});
    b.real(1);
    b.integer(0);
    b.put(UInt32{0x00302010});
    b.text("test.bmp", 32);
    if (version >= 3) {
        b.integer(3);
        b.real(.5f);
        b.real(.25f);
    }
    if (version >= 4) {
        b.put(UInt32{1});
    }
    return b.pers("TS::MaterialList", version);
}

inline Bytes mesh(Int32 version = 3, Int32 faceMaterial = 0) {
    Bytes b;
    b.integer(5);
    b.integer(5);
    b.integer(3);
    b.integer(1);
    b.integer(1);
    if (version >= 2) {
        b.integer(3);
    }
    if (version < 3) {
        b.point({.01f, .01f, .01f});
        b.point({-1, 0, -1});
    }
    b.real(2);
    // bounds min/max followed by one clockwise screen-space triangle (+Y camera).
    for (auto v : {std::array<UInt8, 4>{0, 0, 0, 0},
                   {200, 0, 200, 0},
                   {0, 0, 0, 1},
                   {100, 0, 200, 2},
                   {200, 0, 0, 3}}) {
        std::ranges::for_each(v, [&](auto n) {
            b.put(n);
        });
    }
    for (auto uv : {Point2F{0, 1}, {.5f, 0}, {1, 1}}) {
        b.real(uv.x);
        b.real(uv.y);
    }
    for (auto pair : {std::array<Int32, 2>{2, 0}, {3, 1}, {4, 2}}) {
        b.integer(pair[0]);
        b.integer(pair[1]);
    }
    b.integer(faceMaterial);
    b.integer(0);
    if (version >= 3) {
        b.point({.01f, .01f, .01f});
        b.point({-1, 0, -1});
    }
    return b.pers("TS::CelAnimMesh", version);
}

inline void transform(Bytes& b, Int32 version, Point3F translation = {}) {
    if (version < 7) {
        b.real(0);
        b.real(0);
        b.real(0);
        b.real(1);
    } else {
        b.put(Int16{0});
        b.put(Int16{0});
        b.put(Int16{0});
        b.put(Int16{32767});
    }
    b.point(translation);
    if (version < 8) {
        b.point({1, 1, 1});
    }
}

inline Bytes shape(Int32 version = 8,
                   Int32 meshVersion = 3,
                   Int32 materialVersion = 4,
                   Int32 faceMaterial = 0,
                   bool animated = false) {
    Bytes b;
    for (auto n : {2, Int32(animated), Int32(animated), Int32(animated), 2, 2, 2, 1, 1}) {
        b.integer(n);
    }
    if (version >= 2) {
        b.integer(0);
    }
    if (version >= 4) {
        b.integer(0);
    }
    b.real(2);
    b.point({});
    if (version >= 8) {
        b.point({-1, 0, -1});
        b.point({1, 0, 1});
    }
    for (auto parent : {-1, 0}) {
        for (auto n : {parent + 1, parent, Int32(animated && parent == 0), 0, parent + 1}) {
            if (version >= 8) {
                b.put(Int16(n));
            } else {
                b.integer(n);
            }
        }
    }
    if (animated) {
        b.integer(1);
        b.integer(1);
        b.real(1.25f);
        b.integer(7);
        if (version >= 4) {
            b.integer(0);
            b.integer(0);
        }
        if (version >= 5) {
            b.integer(0);
            b.integer(0);
        }
        for (auto n : {0, 1, 0}) {
            if (version >= 8) {
                b.put(Int16(n));
            } else {
                b.integer(n);
            }
        }
        b.real(.25f);
        if (version < 3) {
            b.put(UInt32{0xc0000001});
        } else if (version < 8) {
            b.integer(1);
            b.put(UInt32{0xd0000000});
        } else {
            b.put(UInt16{1});
            b.put(UInt16{0xd000});
        }
    }
    transform(b, version, {5, 0, 0});
    transform(b, version);
    b.text("bounds", 24);
    b.text("detail", 24);
    for (auto node : {0, 1}) {
        b.put(Int16(node));
        b.put(Int16{0});
        b.integer(0);
        if (version >= 8) {
            b.put(Int16(node));
            b.put(UInt16{0xa5a5});
        } else {
            b.integer(node);
            std::ranges::for_each(std::views::iota(0, 10), [&](auto) {
                b.integer(0);
            });
        }
        b.point({});
        if (version >= 8) {
            b.put(Int16{0});
            b.put(Int16{0});
        } else {
            b.integer(0);
            b.integer(0);
        }
    }
    b.integer(1);
    b.real(1);
    if (version >= 5) {
        b.integer(1);
    }
    if (version >= 6) {
        b.integer(-1);
    }
    b.append(mesh(meshVersion, faceMaterial));
    b.integer(1);
    b.append(material(materialVersion));
    return b.pers("TS::Shape", version);
}

inline std::unique_ptr<world::TsShape> load(const Bytes& b) {
    MemRWStream stream{std::span<const std::byte>(b.data)};
    return world::TsShape::load(stream, UInt32(b.data.size()));
}

inline std::unique_ptr<world::TsMaterialList> loadMaterial(const Bytes& b) {
    MemRWStream stream{std::span<const std::byte>(b.data)};
    return world::TsMaterialList::load(stream, UInt32(b.data.size()));
}

struct Hash {
    UInt32 value{2166136261U};

    template <class T> void put(T v) {
        using U = std::conditional_t<sizeof(T) == 4,
                                     UInt32,
                                     std::conditional_t<sizeof(T) == 2, UInt16, UInt8>>;
        const auto bits = std::bit_cast<U>(v);
        std::ranges::for_each(std::views::iota(std::size_t{0}, sizeof(T)), [&](auto i) {
            value = (value ^ UInt32(UInt64(bits) >> (i * 8) & 255)) * 16777619U;
        });
    }

    void point(Point3F p) {
        put(p.x);
        put(p.y);
        put(p.z);
    }

    void text(std::string_view s) {
        std::ranges::for_each(s, [&](char c) {
            put(UInt8(c));
        });
        put(UInt8{});
    }
};

inline UInt32 hash(const world::TsMaterialList& list) {
    Hash h;
    std::ranges::for_each(list.materials, [&](const auto& m) {
        h.put(m.flags);
        h.put(m.alpha);
        h.put(m.index);
        std::ranges::for_each(m.rgb, [&](auto c) {
            h.put(c);
        });
        h.text(m.mapFile);
        h.put(m.surfaceType);
        h.put(m.elasticity);
        h.put(m.friction);
        h.put(UInt32(m.useDefaultProperties));
    });
    return h.value;
}

inline UInt32 hash(const world::TsMesh& m) {
    Hash h;
    std::ranges::for_each(m.vertices, [&](const auto& v) {
        h.put(v.x);
        h.put(v.y);
        h.put(v.z);
        h.put(v.normal);
    });
    std::ranges::for_each(m.textureVertices, [&](const auto& v) {
        h.put(v.x);
        h.put(v.y);
    });
    std::ranges::for_each(m.faces, [&](const auto& f) {
        std::ranges::for_each(f.corners, [&](const auto& c) {
            h.put(c.vertex);
            h.put(c.texture);
        });
        h.put(f.material);
    });
    std::ranges::for_each(m.frames, [&](const auto& f) {
        h.put(f.firstVertex);
        h.point(f.scale);
        h.point(f.origin);
    });
    return h.value;
}
} // namespace opent1::test::ts
