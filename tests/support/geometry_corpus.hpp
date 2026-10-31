#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

namespace geometry_corpus {
inline constexpr std::uint32_t magic = 0x4d54314f, version = 1;
enum class Accuracy { Exact, Arithmetic, Rotation, Normal };

struct Inputs {
    std::uint32_t state = 0x1b202611;

    std::uint32_t next() {
        state = 1664525U * state + 1013904223U;
        return state;
    }

    int integer() {
        return static_cast<int>(next() % 2049U) - 1024;
    }

    float scalar() {
        return static_cast<float>(integer()) / 64.0f;
    }

    float angle() {
        return static_cast<float>(integer()) / 1024.0f;
    }
};

template <class Math, class Word, class Float> void run(Math& math, Word word, Float real) {
    using P = typename Math::P;
    using I2 = typename Math::I2;
    using I3 = typename Math::I3;
    using P2 = typename Math::P2;
    using P4 = typename Math::P4;
    using M = typename Math::M;
    using T = typename Math::T;
    using E = typename Math::E;
    using Q = typename Math::Q;
    using A = typename Math::A;
    using Plane = typename Math::Plane;
    using B2 = typename Math::B2;
    using B3 = typename Math::B3;
    using Random = typename Math::Random;
    const auto pointI = [&](const auto& p) {
        word(static_cast<std::uint32_t>(p.x));
        word(static_cast<std::uint32_t>(p.y));
        if constexpr (requires { p.z; }) {
            word(static_cast<std::uint32_t>(p.z));
        }
    };
    const auto point = [&](const auto& p, Accuracy accuracy = Accuracy::Arithmetic) {
        real(p.x, accuracy);
        real(p.y, accuracy);
        if constexpr (requires { p.z; }) {
            real(p.z, accuracy);
        }
        if constexpr (requires { p.w; }) {
            real(p.w, accuracy);
        }
    };
    const auto matrix = [&](const auto& m) {
        word(static_cast<std::uint32_t>(m.flags));
        std::ranges::for_each(std::views::iota(0, 3), [&](auto i) {
            std::ranges::for_each(std::views::iota(0, 3), [&](auto j) {
                real(m.m[i][j], Accuracy::Rotation);
            });
        });
        if constexpr (requires { m.p; }) {
            point(m.p, Accuracy::Rotation);
        }
    };
    Inputs input;
    std::ranges::for_each(std::views::iota(0, 512), [&](auto) {
        const I3 a{input.integer(), input.integer(), input.integer()},
            b{input.integer(), input.integer(), input.integer()};
        const I2 a2(a.x, a.y), b2(b.x, b.y);
        pointI(a + b);
        pointI(a - b);
        pointI(a * b);
        pointI(a / 7);
        pointI(a << 3);
        pointI(a >> 3);
        I3 r;
        r.average(a, b);
        pointI(r);
        r = a;
        r.normalize(1000);
        pointI(r);
        r = a;
        r.normalizef(1000);
        pointI(r);
        r = a;
        r.normalizef(1000, 1500);
        pointI(r);
        word(static_cast<std::uint32_t>(math.dot(a, b)));
        word(static_cast<std::uint32_t>(math.dotSign(a, b)));
        math.cross(a, b, r);
        pointI(r);
        word(static_cast<std::uint32_t>(a.len()));
        word(static_cast<std::uint32_t>(a.lenf()));
        word(static_cast<std::uint32_t>(a.lenvf()));
        pointI(a2 + b2);
        pointI(a2 - b2);
        word(static_cast<std::uint32_t>(a2.len()));
        word(static_cast<std::uint32_t>(a2.lenf()));
        word(static_cast<std::uint32_t>(a2.lenvf()));
        I2 c;
        c.interpolate(a2, b2, 0.375f);
        pointI(c);
    });
    std::ranges::for_each(std::views::iota(0, 512), [&](auto) {
        const P a{input.scalar(), input.scalar(), input.scalar()},
            b{input.scalar(), input.scalar(), input.scalar()};
        const P2 a2(a.x, a.y), b2(b.x, b.y);
        point(a + b);
        point(a - b);
        point(a * b);
        point(a / 7.0f);
        real(math.dot(a, b), Accuracy::Arithmetic);
        P r;
        math.cross(a, b, r);
        point(r);
        real(a.len(), Accuracy::Arithmetic);
        real(a.lenf(), Accuracy::Exact);
        real(a.lenvf(), Accuracy::Exact);
        r = a;
        r.normalize();
        point(r);
        r = a;
        r.normalizef();
        point(r, Accuracy::Exact);
        r = a;
        r.normalizef(7.0f);
        point(r);
        r.interpolate(a, b, 0.375f);
        point(r);
        real(a2.len(), Accuracy::Arithmetic);
        real(math.dot(a2, b2), Accuracy::Arithmetic);
        P4 a4(a.x, a.y, a.z, 1), b4(b.x, b.y, b.z, 0), r4;
        r4.interpolate(a4, b4, 0.375f);
        point(r4);
        B2 box(P2(-2, -3), P2(4, 5));
        word(box.contains(a2));
        B3 box3(P(-2, -3, -4), P(4, 5, 6)), other(a, b);
        word(box3.contains(a));
        word(box3.overlaps(other));
        box3.intersect(other);
        point(box3.fMin);
        point(box3.fMax);
    });
    std::ranges::for_each(
        std::array<std::int16_t, 4>{0x027f, 0x067f, 0x0a7f, 0x0e7f}, [&](auto mode) {
            math.control(mode);
            std::ranges::for_each(
                std::array<float, 8>{
                    -2.5f, -1.5f, -0.5f, 0.5f, 1.5f, 2.5f, 2147483648.0f, -2147483648.0f},
                [&](auto value) {
                    I3 result;
                    math.convert(P(value, value * 0.5f, -value), result);
                    pointI(result);
                });
        });
    math.control(0x027f);
    // All single-axis fast paths plus mixed Z-X-Y rotations.
    std::ranges::for_each(std::views::iota(0, 384), [&](auto index) {
        E e{input.angle(), input.angle(), input.angle()},
            f{input.angle(), input.angle(), input.angle()};
        if (index < 4) {
            e = E(0, 0, 0);
            if (index == 1) {
                e.x = 0.5f;
            }
            if (index == 2) {
                e.y = -0.75f;
            }
            if (index == 3) {
                e.z = 1.0f;
            }
        }
        M a, b;
        a.flags = b.flags = 0;
        a.identity();
        b.identity();
        a.set(e);
        b.set(f);
        matrix(a);
        Q q(e), qb(f);
        point(q, Accuracy::Rotation);
        M qm;
        qm.flags = 0;
        qm.identity();
        q.makeMatrix(math.output(qm));
        matrix(qm);
        E angles;
        angles.set(a);
        point(angles, Accuracy::Rotation);
        Q roundtrip(a);
        point(roundtrip, Accuracy::Rotation);
        Q prod = q;
        prod *= qb;
        point(prod);
        Q inv = q;
        inv.inverse();
        point(inv);
        Q interp;
        interp.interpolate(q, qb, 0.375f);
        point(interp, Accuracy::Rotation);
        word(q.IsIdentity());
        if (!q.IsIdentity()) {
            const A axis(q);
            point(axis.axis, Accuracy::Rotation);
            real(axis.angle, Accuracy::Rotation);
        }
        M product;
        math.mul(a, b, product);
        matrix(product);
        M scaled = a;
        scaled.preScale(P(2, 3, 4));
        real(scaled.determinant(), Accuracy::Arithmetic);
        M inverse = scaled;
        inverse.inverse();
        matrix(inverse);
        scaled = a;
        scaled.preRotateX(0.25f);
        scaled.preRotateY(-0.5f);
        scaled.preRotateZ(0.75f);
        matrix(scaled);
        scaled = a;
        scaled.postRotateX(-0.25f);
        scaled.postRotateY(0.5f);
        scaled.postRotateZ(-0.75f);
        matrix(scaled);
        const P p{input.scalar(), input.scalar(), input.scalar()},
            t{input.scalar(), input.scalar(), input.scalar()};
        P r;
        math.mul(p, a, r);
        point(r, Accuracy::Rotation);
        math.mul(p, q, r);
        point(r, Accuracy::Rotation);
        T ta, tb, tr;
        ta.flags = tb.flags = 0;
        ta.set(e, t);
        tb.set(f, p);
        math.mul(ta, tb, tr);
        matrix(tr);
        math.mul(p, ta, r);
        point(r, Accuracy::Rotation);
        T ti = ta;
        ti.inverse();
        matrix(ti);
        math.mulInverse(p, ta, r);
        point(r, Accuracy::Rotation);
        ta.preTranslate(P(1, 2, 3));
        ta.postTranslate(P(-3, 1, -2));
        matrix(ta);
        B3 box(P(-1, -2, -3), P(2, 3, 4)), out;
        math.mul(box, ta, out);
        point(out.fMin, Accuracy::Rotation);
        point(out.fMax, Accuracy::Rotation);
    });
    T cancellation;
    cancellation.flags = 0;
    cancellation.set(E(0, 0, -0.7853981633974483f), P(1, -1, 0));
    P cancellationResult;
    math.mulInverse(P(100000000, 100000000, 0), cancellation, cancellationResult);
    point(cancellationResult);
    // Exact axis planes, segment endpoints, parallel lines, and polygon order.
    std::ranges::for_each(std::views::iota(0, 256), [&](auto index) {
        Plane plane;
        plane.setYZ(static_cast<float>(index % 7) - 3.0f);
        const P a{input.scalar(), input.scalar(), input.scalar()},
            b{input.scalar(), input.scalar(), input.scalar()};
        word(static_cast<std::uint32_t>(plane.whichSide(a)));
        P intersection(91, 92, 93);
        const auto hit = math.intersect(plane, a, b, intersection);
        word(hit);
        point(intersection);
        const std::array<P, 4> polygon{P(-4, -3, 0), P(4, -3, 0), P(4, 3, 0), P(-4, 3, 0)};
        const auto halves = math.split(plane, polygon);
        word(static_cast<std::uint32_t>(halves.first.size()));
        word(static_cast<std::uint32_t>(halves.second.size()));
        std::ranges::for_each(halves.first, [&](const auto& v) {
            point(v);
        });
        std::ranges::for_each(halves.second, [&](const auto& v) {
            point(v);
        });
        Plane normalized(a, P(1, 2, 3));
        point(normalized);
        real(normalized.d, Accuracy::Arithmetic);
        const P min(-2, -3, -4), max(2, 3, 4), normal(0, 0, 1);
        real(math.dot(normal, a), Accuracy::Arithmetic);
        word(math.triangle(a, normal, polygon[0], polygon[1], polygon[2]));
        word(math.poly(a, normal, polygon));
        float time = 13;
        word(math.line(a, b, min, max, time));
        real(time, Accuracy::Arithmetic);
        time = 13;
        word(math.tube(a, b, 0.5f, min, max, time));
        real(time, Accuracy::Arithmetic);
        float distance = 0;
        word(math.sphere(a, 2.25f, min, max, distance));
        real(distance, Accuracy::Arithmetic);
    });
    std::ranges::for_each(std::views::iota(0, 2048), [&](auto) {
        const P ar(0.5f + std::abs(input.scalar()) / 8, 1, 2),
            br(1, 0.5f + std::abs(input.scalar()) / 8, 1);
        const E e{input.angle(), input.angle(), input.angle()};
        const P translation{input.scalar() / 2, input.scalar() / 2, input.scalar() / 2};
        T t;
        t.flags = 0;
        t.set(e, translation);
        P ao, bo;
        const auto hit = math.obb(ar, br, t, ao, bo);
        word(hit);
        if (hit) {
            point(ao);
            point(bo);
        }
        const auto rect = math.rect(ar, br, t, ao, bo);
        word(rect);
        if (rect) {
            point(ao);
            point(bo);
        }
        const auto line = math.lineBox(ar, br, t, ao, bo);
        word(line);
        if (line) {
            point(ao);
            point(bo);
        }
        float overlap = 17;
        const auto plane = math.planeBox(br, t, P(0, 0, 1), P(0, 0, 1), overlap);
        word(plane);
        real(overlap, Accuracy::Arithmetic);
    });
    const std::array<std::uint32_t, 8> seeds{
        0, 1, 33655, 0xffffffffU, 0x80000000U, 0x12345678U, 250, 147};
    std::ranges::for_each(seeds, [&](auto seed) {
        Random random(seed);
        word(random.getSeed());
        std::ranges::for_each(std::views::iota(0, 2048), [&](auto) {
            word(random.getInt());
        });
        std::ranges::for_each(std::views::iota(0, 512), [&](auto i) {
            using U = decltype(random.getInt());
            word(random.getInt(static_cast<U>(i % 2 ? 0xffffffffU : static_cast<unsigned>(i))));
            word(static_cast<std::uint32_t>(random.getInt(-2000000000, 2000000000)));
            real(random.getFloat(), Accuracy::Exact);
            real(random.getFloat(-3.0f, 7.0f), Accuracy::Exact);
            real(random.getNormal(), Accuracy::Normal);
            real(random.getNormal(2, 0.25f), Accuracy::Normal);
        });
    });
}
} // namespace geometry_corpus
