#include <opent1/gfx/software_rasterizer.hpp>
#include <opent1/gfx/render_limits.hpp>
#include <opent1/gfx/texture_cache.hpp>
#include "software_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

namespace opent1::gfx {
namespace {
using Float = detail::RasterFloat;
constexpr double fixedScale = 65536;

struct Span {
    Int32 y{}, left{}, right{}, originLeft{}, originRight{};
    Int64 u{}, v{}, shade{}, du{}, dv{};
    bool omitLast{};
};

struct Plane {
    double dx{}, dy{}, origin{};

    [[nodiscard]] double at(Int32 x, Int32 y) const {
        return ((Float(dx) * x + Float(dy) * y) + origin).stored();
    }
};

struct Primitive {
    std::vector<Span> spans;
    Int64 du{}, dv{}, ds{};
    Plane lightU{}, lightV{}, lightW{0, 0, 1};
};

UInt32 lightLevel(const Primitive& p, const LightMap& map, Int32 x, Int32 y, Int32 maxShade) {
    const auto w = std::max(p.lightW.at(x, y), 1. / 65536);
    const auto u = std::clamp(p.lightU.at(x, y) / w * map.size.x - .5, 0., double(map.size.x - 1));
    const auto v = std::clamp(p.lightV.at(x, y) / w * map.size.y - .5, 0., double(map.size.y - 1));
    const auto ix = Int32(u), iy = Int32(v), xx = std::min(ix + 1, map.size.x - 1),
               yy = std::min(iy + 1, map.size.y - 1);
    const auto sample = [&](Int32 a, Int32 b) {
        return double(map.samples[std::size_t(b) * map.size.x + a] >> 12);
    };
    const auto a = sample(ix, iy) * (1 - (u - ix)) + sample(xx, iy) * (u - ix);
    const auto b = sample(ix, yy) * (1 - (u - ix)) + sample(xx, yy) * (u - ix);
    return UInt32(
        std::clamp((a * (1 - (v - iy)) + b * (v - iy)) / 15 * maxShade, 0., double(maxShade)));
}

UInt8 shadeAt(const Span& span, Int64 step, UInt32 offset, bool textured) {
    // rn_nontx/nptex rotate 16.16 into a .32 accumulator and keep the
    // integer level in a separate byte. The step's low word is retained;
    // its carries differ from an ordinary signed 16.16 add at boundaries.
    const UInt32 initial =
        std::rotl(UInt32(span.shade), 16) & (textured ? 0xffff0000U : 0xffffffffU);
    const UInt32 increment = std::rotl(UInt32(step), 16);
    const UInt64 sum = UInt64(initial) + UInt64(increment) * offset;
    return UInt8((UInt32(span.shade) >> 16) + (UInt32(step) >> 16) * offset + UInt32(sum >> 32));
}

// Explicit floor for signed fixed-point values, independent of signed shifts.
Int64 whole(Int64 value) {
    return value >= 0 ? value / 65536 : -((-value + 65535) / 65536);
}

Int32 scanY(Flt32 y) {
    return Int32(whole(Int64(double(y) * fixedScale) + 65535));
}

double cross(Point2F a, Point2F b, Point2F c) {
    return (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
}

bool unit(Flt32 value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}

bool vertexValid(const PolygonVertex& v, bool wrap) {
    return std::isfinite(v.position.x) && std::isfinite(v.position.y) &&
           std::abs(v.position.x) <= Bitmap::MaxDimension &&
           std::abs(v.position.y) <= Bitmap::MaxDimension &&
           (wrap ? std::isfinite(v.texture.x) && std::isfinite(v.texture.y) &&
                       std::abs(v.texture.x) <= Bitmap::MaxDimension &&
                       std::abs(v.texture.y) <= Bitmap::MaxDimension
                 : unit(v.texture.x) && unit(v.texture.y)) &&
           unit(v.color.x) && unit(v.color.y) && unit(v.color.z) && unit(v.haze) &&
           std::isfinite(v.inverseDepth) && v.inverseDepth >= 1.f / 65536 &&
           v.inverseDepth <= 65536;
}

Plane plane(std::span<const PolygonVertex> vertices, std::array<Float, 3> values, double scale) {
    const auto a = vertices[0].position, b = vertices[1].position, c = vertices[2].position;
    // Same difference, determinant and origin order as DeltaCalcTriUVW/WS/WH.
    const double u1 = double(c.x) - b.x, u2 = double(c.y) - b.y;
    const double v1 = double(a.x) - b.x, v2 = double(a.y) - b.y;
    const Float u3 = values[2] - values[1], v3 = values[0] - values[1];
    const Float factor = (Float(-1) / (Float(u1) * v2 - Float(u2) * v1)) * scale;
    // Delta numerators are themselves stored to double before scaling.
    const double dx = (Float(u2) * v3 - u3 * v2).stored(), dy = (u3 * v1 - Float(u1) * v3).stored();
    Plane result{(Float(dx) * factor).stored(), (Float(dy) * factor).stored(), 0};
    result.origin =
        ((values[0] * scale - Float(result.dx) * a.x) - Float(result.dy) * a.y).stored();
    return result;
}

bool fixed(double value, Int64& output) {
    // Bound intermediates before conversion/addition; no floating-to-int UB.
    constexpr double limit = double(Int64{1} << 46);
    if (!std::isfinite(value) || value <= -limit || value >= limit) {
        return false;
    }
    output = Int64(value);
    return true;
}

bool buildSpans(std::span<const PolygonVertex> vertices, RectI clip, std::vector<Span>& output) {
    if (!clip.valid()) {
        return true;
    }

    // Each downward edge contributes one side of a convex polygon. Horizontal
    // edges and edges whose quantized endpoints share a row contribute nothing.
    // Reinitialize at every vertex; stepping remains anchored before clipping.
    struct Edge {
        Int32 first{}, last{};
        Int64 start{}, step{};
        bool left{};
    };

    std::vector<Edge> edges;
    edges.reserve(vertices.size());
    Int32 firstY = clip.bottom + 1, lastY = clip.top;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        auto a = vertices[i].position, b = vertices[(i + 1) % vertices.size()].position;
        const bool left = a.y > b.y;
        if (left) {
            std::swap(a, b);
        }
        const Int32 first = scanY(a.y), last = scanY(b.y);
        if (first == last || first > clip.bottom || last <= clip.top) {
            continue;
        }
        const Float slope = (Float(b.x) - a.x) / (Float(b.y) - a.y);
        Int64 start{}, step{};
        if (!fixed(((Float(a.x) + (Float(first) - a.y) * slope) * fixedScale).stored(), start) ||
            !fixed((slope * fixedScale).stored(), step)) {
            return false;
        }
        start += 65535;
        edges.push_back({first, last, start, step, left});
        firstY = std::min(firstY, std::max(first, clip.top));
        lastY = std::max(lastY, std::min(last, clip.bottom + 1));
    }
    for (Int32 y = firstY; y < lastY; ++y) {
        Int64 left = 0, right = 0;
        bool hasLeft = false, hasRight = false;
        for (const auto& edge : edges) {
            if (y < edge.first || y >= edge.last) {
                continue;
            }
            const auto x = whole(edge.start + edge.step * (y - edge.first));
            if (edge.left) {
                left = x;
                hasLeft = true;
            } else {
                right = x;
                hasRight = true;
            }
        }
        if (!hasLeft || !hasRight || right <= left) {
            continue;
        }
        if (left < INT32_MIN || left > INT32_MAX || right < INT32_MIN || right > INT32_MAX) {
            return false;
        }
        const Int32 originLeft = Int32(left), originRight = Int32(right);
        left = std::max(left, Int64(clip.left));
        right = std::min(right, Int64(clip.right) + 1);
        if (right > left) {
            output.push_back({y, Int32(left), Int32(right), originLeft, originRight});
        }
    }
    return true;
}

bool sourceValid(PolygonSource source) {
    return UInt8(source) <= UInt8(PolygonSource::Vertex);
}

// DeltaCalcPolyUVQW accumulates/stores every edge, then anchors the planes at
// vertex zero. This differs from a triangle fan for an unlit perspective n-gon.
Plane perspectivePlane(std::span<const PolygonVertex> vertices, unsigned component) {
    const auto value = [component](const PolygonVertex& vertex) -> Float {
        if (component == 2) {
            return vertex.inverseDepth;
        }
        return Float(component == 0 ? vertex.texture.x : vertex.texture.y) * vertex.inverseDepth;
    };
    const double scale = component == 2 ? 1 : fixedScale;
    if (vertices.size() == 3) {
        return plane(vertices, {value(vertices[0]), value(vertices[1]), value(vertices[2])}, scale);
    }
    double dx = 0, dy = 0, area = 0;
    auto previous = vertices.back();
    for (const auto& current : vertices) {
        dx = (((value(previous) + value(current)) *
               (Float(previous.position.y) - current.position.y)) +
              dx)
                 .stored();
        dy = (((value(previous) - value(current)) *
               (Float(previous.position.x) + current.position.x)) +
              dy)
                 .stored();
        area = ((Float(previous.position.x) - current.position.x) *
                    (Float(previous.position.y) + current.position.y) +
                area)
                   .stored();
        previous = current;
    }
    const Float factor = (Float(-1) / area) * scale;
    Plane result{(Float(dx) * factor).stored(), (Float(dy) * factor).stored(), 0};
    result.origin = ((value(vertices[0]) * scale - Float(result.dx) * vertices[0].position.x) -
                     Float(result.dy) * vertices[0].position.y)
                        .stored();
    return result;
}

bool fixed32(Float value, Int64& result) {
    return fixed(value.stored(), result) && result >= INT32_MIN && result <= INT32_MAX;
}

UInt32 carryAdd(UInt32 value, UInt32 step) {
    const UInt64 sum = UInt64(value) + step;
    return UInt32(sum) + UInt32(sum >> 32);
}

struct WrappedUV {
    UInt32 u{}, v{}, du{}, dv{}, uMask{}, vMask{};
    bool constant{};

    WrappedUV(const Span& span, const Bitmap& texture, bool constantLight)
        : constant(constantLight) {
        const int shift = std::countr_zero(UInt32(texture.stride()));
        // UVWCR/UVWCTR leave U unrotated and feed its carry into the V step.
        // Preserve the actual assembly, including this constant-light quirk.
        u = constant ? UInt32(span.u) : std::rotl(UInt32(span.u), 16);
        v = std::rotl(UInt32(span.v), 16 + shift);
        du = std::rotl(UInt32(span.du), 16);
        dv = std::rotl(UInt32(span.dv), 16 + shift);
        uMask = UInt32(texture.stride() - 1);
        vMask = UInt32(texture.height - 1) << shift;
    }

    [[nodiscard]] std::size_t offset() const {
        return (u & uMask) | (v & vMask);
    }

    void advance() {
        if (constant) {
            const UInt64 sum = UInt64(u) + du;
            u = UInt32(sum);
            dv += UInt32(sum >> 32);
        } else {
            u = carryAdd(u, du);
        }
        v = carryAdd(v, dv);
    }
};

bool perspectiveSpans(const std::vector<Span>& rows,
                      Plane u,
                      Plane v,
                      Plane w,
                      Plane s,
                      bool vertexLight,
                      bool wrapped,
                      bool constantLight,
                      bool transparent,
                      bool clampTexture,
                      const Bitmap& texture,
                      std::vector<Span>& output) {
    const auto at = [vertexLight](Plane p, Int32 x, Int32 y) {
        return vertexLight ? (Float(p.dx) * x + Float(p.dy) * y) + p.origin
                           : (Float(p.dx) * x + p.origin) + Float(p.dy) * y;
    };
    for (const auto& row : rows) {
        const Int32 origin = row.originLeft, total = row.originRight - origin;
        Float startU = at(u, origin, row.y), startV = at(v, origin, row.y),
              depth = at(w, origin, row.y);
        if (!std::isfinite(depth.stored()) || depth.stored() <= 0) {
            return false;
        }
        const Float reciprocal = Float(1) / depth;
        Float dividedU = startU * reciprocal, dividedV = startV * reciprocal;
        Int64 shade{};
        if (!fixed32(at(s, origin, row.y), shade)) {
            return false;
        }
        // Original unlit spans avoid a first subspan shorter than four pixels.
        Int32 length = total <= 24 ? total : 24 - ((origin % 24 + 24) % 24);
        if (!vertexLight && total > 24 && length <= 3) {
            length += 24;
            length = std::min(length, total);
        }
        for (Int32 x = origin; x < row.originRight;) {
            const Int32 remaining = row.originRight - x;
            const Int32 distance = length == remaining ? length - 1 : length;
            // Native wrapping/unclamped spans use fpNewInfoTable[0] == 1.
            // Clamped world surfaces can project to a one-pixel sliver: its
            // nonexistent next pixel may lie behind the perspective plane.
            // Sample the sole visible endpoint without extrapolating past it.
            const Int32 count = clampTexture && remaining == 1 ? 0 : std::max(distance, 1);
            const Float inverse = float(1. / std::max(count, 1)); // original float lookup
            depth = depth + Float(count) * w.dx;
            if (!std::isfinite(depth.stored()) || depth.stored() <= 0) {
                return false;
            }
            const Float endReciprocal = Float(1) / depth;
            startU = startU + Float(count) * u.dx;
            startV = startV + Float(count) * v.dx;
            const Float endU = startU * endReciprocal, endV = startV * endReciprocal;
            Span span = row;
            span.left = std::max(x, row.left);
            span.right = std::min(x + length, row.right);
            span.shade = shade;
            if (!fixed32(dividedU, span.u) || !fixed32(dividedV, span.v) ||
                !fixed32((endU - dividedU) * inverse, span.du) ||
                !fixed32((endV - dividedV) * inverse, span.dv)) {
                return false;
            }
            const Int32 advance = span.left - x;
            bool finish = false;
            if (wrapped) {
                WrappedUV uv(span, texture, constantLight);
                // Preserve wrapping carries and UVWCTR's zero-at-subspan-end
                // early exit even when that texel is outside the current clip.
                for (Int32 i = 0; i < length; ++i) {
                    if (i == advance) {
                        span.u = uv.u;
                        span.v = uv.v;
                        span.du = uv.du;
                        span.dv = uv.dv;
                    }
                    if (i == length - 1 && constantLight && transparent) {
                        finish = texture.level()[uv.offset()] == std::byte{0};
                    }
                    uv.advance();
                }
            } else {
                span.u += Int64(advance) * span.du;
                span.v += Int64(advance) * span.dv;
                if (span.right > span.left && !clampTexture) {
                    for (const Int64 offset : {Int64{0}, Int64(span.right - span.left - 1)}) {
                        const auto tx = whole(span.u + offset * span.du),
                                   ty = whole(span.v + offset * span.dv);
                        if (tx < 0 || tx >= texture.stride() || ty < 0 || ty >= texture.height) {
                            return false;
                        }
                    }
                }
            }
            // Wrapped spans store the already rotated accumulators/steps.
            if (span.right > span.left) {
                output.push_back(span);
                if (output.size() > renderLimits().spans) {
                    return false;
                }
            }
            if (finish) {
                break;
            }
            dividedU = endU;
            dividedV = endV;
            x += length;
            length = std::min(24, row.originRight - x);
        }
    }
    return true;
}
}

bool SoftwareSurface::polygon(std::span<const PolygonVertex> vertices,
                              const PolygonMaterial& material) {
    if (material.blend && !material.texture && material.shadeSource == PolygonSource::None &&
        material.hazeSource == PolygonSource::None && blendRenderer_) {
        return blendRenderer_(*this, vertices, material);
    }
    std::size_t spanCount = 0;
    return polygonSpans(vertices, material, {}, false, spanCount);
}

bool SoftwareSurface::polygonSpans(std::span<const PolygonVertex> vertices,
                                   const PolygonMaterial& material,
                                   std::span<const VisibilitySpan> visible,
                                   bool sorted,
                                   std::size_t& spanCount) {
    if (vertices.size() < 3 || vertices.size() > 100 ||
        !std::ranges::all_of(vertices,
                             [&](const auto& v) {
                                 return vertexValid(v, material.wrap);
                             }) ||
        !sourceValid(material.shadeSource) || !sourceValid(material.hazeSource) ||
        !unit(material.shade) || !unit(material.haze)) {
        return false;
    }
    const bool shade = material.shadeSource != PolygonSource::None,
               haze = material.hazeSource != PolygonSource::None;
    const bool vertexShade = material.shadeSource == PolygonSource::Vertex,
               vertexHaze = material.hazeSource == PolygonSource::Vertex;
    if (material.lightMap &&
        (!material.lightMap->valid() || material.lightMap->format != LightMap::Format::Rgb444 ||
         shade || haze || material.blend || !std::ranges::all_of(vertices, [](const auto& v) {
             return std::isfinite(v.lightMap.x) && std::isfinite(v.lightMap.y) &&
                    std::abs(v.lightMap.x) <= 16384 && std::abs(v.lightMap.y) <= 16384;
         }))) {
        return false;
    }
    const UInt32 shadePrestep =
        material.perspective && material.transparent ? 1U : 0U; // UVWST shades after its carry step
    if ((material.perspective && !material.texture && !material.lightMap) ||
        (material.wrap && (!material.perspective || !material.texture || material.blend))) {
        return false;
    }
    if (material.clampTexture && (!material.texture || material.wrap)) {
        return false;
    }
    const bool wrapped =
        material.perspective &&
        (material.wrap || (material.transparent && !shade && !haze && !material.clampTexture));
    if ((vertexHaze && shade) || (material.blend && (shade || haze)) ||
        (material.transparent && (!material.texture || material.blend))) {
        return false;
    }
    if (material.texture && (!material.texture->valid() || material.texture->bitDepth != 8)) {
        return false;
    }
    if (wrapped && (!std::has_single_bit(UInt32(material.texture->stride())) ||
                    !std::has_single_bit(UInt32(material.texture->height)))) {
        return false;
    }
    const UInt32 index = material.texture ? material.texture->paletteIndex : material.paletteIndex;
    const SubPalette* sub = palette_ ? &palette_->find(index) : nullptr;
    if ((shade || haze || material.lightMap) &&
        (!sub || sub->type != PaletteType::ShadeHaze || palette_->shadeShift > 8 ||
         (haze && palette_->hazeLevels == 0))) {
        return false;
    }
    if (material.lightMap && sub->remap.size() < (std::size_t{1} << palette_->shadeShift) * 256) {
        return false;
    }
    if (material.blend &&
        (!sub ||
         (sub->type != PaletteType::Translucent && sub->type != PaletteType::Additive &&
          sub->type != PaletteType::Subtractive) ||
         sub->remap.size() != 65536)) {
        return false;
    }
    // Reject nonconvex and self-intersecting input, including a star polygon
    // whose consecutive cross products alone could all have the same sign.
    double area = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto a = vertices[i].position, b = vertices[(i + 1) % vertices.size()].position;
        area += (double(a.x) - b.x) * (double(a.y) + b.y);
    }
    const double sign = area >= 0 ? 1 : -1;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        if (area != 0 && std::ranges::any_of(vertices.subspan(i + 1), [&](const auto& v) {
                return v.position.x == vertices[i].position.x &&
                       v.position.y == vertices[i].position.y;
            })) {
            return false;
        }
        for (const auto& v : vertices) {
            if (sign * cross(vertices[i].position,
                             vertices[(i + 1) % vertices.size()].position,
                             v.position) <
                0) {
                return false;
            }
        }
    }
    if (area <= 0 || !clip_.valid() || (sorted && visible.empty())) {
        return true;
    }
    const Int32 maxShade = palette_ ? (Int32{1} << palette_->shadeShift) - 1 : 0;
    const Int32 maxHaze = palette_ ? palette_->hazeLevels - 1 : 0;
    const std::size_t shadeTable = std::size_t(256) * std::size_t(maxShade + 1);
    std::size_t mapBase = 0;
    if (shade) {
        if (haze) {
            mapBase += shadeTable * std::size_t(Int32(double(maxHaze) * material.haze));
        }
        if (!vertexShade) {
            mapBase += 256U * std::size_t(Int32(double(maxShade) * material.shade));
        }
    } else if (haze) {
        mapBase = shadeTable * std::size_t(palette_->hazeLevels);
        if (!vertexHaze) {
            mapBase += 256U * std::size_t(Int32(double(maxShade) * material.haze));
        }
    }
    // Original fill setup maps constant lighting immediately; unlit explicit
    // palette fills use that palette's brightest shade row instead of identity.
    UInt8 fillColor = material.color;
    if (!material.texture && (shade || haze) && !vertexShade && !vertexHaze) {
        fillColor = std::to_integer<UInt8>(sub->remap[mapBase + fillColor]);
    } else if (!material.lightMap && !material.texture && !material.blend && index != 0xffffffffU &&
               sub && sub->type == PaletteType::ShadeHaze) {
        fillColor = std::to_integer<UInt8>(sub->remap[std::size_t(maxShade) * 256 + fillColor]);
    }
    std::optional<Bitmap> snapshot;
    if (material.texture == &bitmap_) {
        snapshot = bitmap_;
    }
    const Bitmap* texture = snapshot ? &*snapshot : material.texture;
    const bool perspectiveTexture = material.perspective && texture;
    const bool fan =
        (texture && !material.perspective) || vertexShade || vertexHaze || material.lightMap;
    const auto primitiveCount = fan ? vertices.size() - 2 : 1;
    std::vector<Primitive> primitives;
    primitives.reserve(primitiveCount);
    for (std::size_t i = 0; i < primitiveCount; ++i) {
        const std::array<PolygonVertex, 3> triangle{
            vertices[0], vertices[fan ? i + 1 : 1], vertices[fan ? i + 2 : 2]};
        const std::span<const PolygonVertex> polygon =
            fan ? std::span<const PolygonVertex>(triangle) : vertices;
        if (fan && cross(triangle[0].position, triangle[1].position, triangle[2].position) <= 0) {
            continue;
        }
        Primitive primitive;
        if (material.lightMap) {
            const auto values = [&](bool vertical) {
                std::array<Float, 3> result{};
                for (std::size_t j = 0; j < 3; ++j) {
                    result[j] = Float(vertical ? polygon[j].lightMap.y : polygon[j].lightMap.x) *
                                (material.perspective ? polygon[j].inverseDepth : 1.f);
                }
                return result;
            };
            primitive.lightU = plane(polygon, values(false), 1);
            primitive.lightV = plane(polygon, values(true), 1);
            if (material.perspective) {
                primitive.lightW = perspectivePlane(polygon, 2);
            }
        }
        if (sorted) {
            // Batch fans have already been expanded before depth resolution.
            // Keep each fragment's original endpoints for interpolation while
            // applying the final surface clip only to its writes.
            for (const auto& span : visible) {
                if (span.y < clip_.top || span.y > clip_.bottom) {
                    continue;
                }
                const auto left = std::max(span.left, clip_.left),
                           right = std::min(span.right, clip_.right + 1);
                if (right > left) {
                    primitive.spans.push_back({span.y, left, right, span.left, span.right});
                }
            }
        } else if (!buildSpans(polygon, clip_, primitive.spans)) {
            return false;
        }
        if (primitive.spans.empty()) {
            continue;
        }
        spanCount += primitive.spans.size();
        if (spanCount > renderLimits().spans) {
            return false;
        }
        Plane u{}, v{}, w{}, s{};
        if (texture) {
            u = material.perspective
                    ? perspectivePlane(polygon, 0)
                    : plane(polygon,
                            {polygon[0].texture.x, polygon[1].texture.x, polygon[2].texture.x},
                            fixedScale);
            v = material.perspective
                    ? perspectivePlane(polygon, 1)
                    : plane(polygon,
                            {polygon[0].texture.y, polygon[1].texture.y, polygon[2].texture.y},
                            fixedScale);
            if (material.perspective) {
                w = perspectivePlane(polygon, 2);
            }
            const auto textureSpan = material.clampTexture ? texture->width : texture->stride();
            u.dx = (Float(u.dx) * textureSpan).stored();
            u.dy = (Float(u.dy) * textureSpan).stored();
            u.origin = (Float(u.origin) * textureSpan).stored();
            v.dx = (Float(v.dx) * texture->height).stored();
            v.dy = (Float(v.dy) * texture->height).stored();
            v.origin = (Float(v.origin) * texture->height).stored();
        }
        if (vertexShade || vertexHaze) {
            std::array<Float, 3> values{};
            for (std::size_t j = 0; j < 3; ++j) {
                const auto& vertex = polygon[j];
                values[j] = vertexHaze
                                ? Float(vertex.haze)
                                : (Float(vertex.color.x) * .22f + Float(vertex.color.y) * .67f) +
                                      Float(vertex.color.z) * .11f;
            }
            s = plane(polygon, values, double((maxShade << 16) | 0xff00));
            if (vertexShade) {
                // DeltaCalcTri*WS saves vertex zero's luminance as a float
                // under round-toward-zero before calculating the origin.
                const Flt32 saved = values[0].storedFloat();
                s.origin = ((Float(saved) * double((maxShade << 16) | 0xff00) -
                             Float(s.dx) * polygon[0].position.x) -
                            Float(s.dy) * polygon[0].position.y)
                               .stored();
            }
        }
        if (!fixed(s.dx, primitive.ds) ||
            (!perspectiveTexture && (!fixed(u.dx, primitive.du) || !fixed(v.dx, primitive.dv)))) {
            return false;
        }
        if (primitive.du < INT32_MIN || primitive.du > INT32_MAX || primitive.dv < INT32_MIN ||
            primitive.dv > INT32_MAX || primitive.ds < INT32_MIN || primitive.ds > INT32_MAX) {
            return false;
        }
        if (perspectiveTexture) {
            std::vector<Span> segments;
            if (!perspectiveSpans(primitive.spans,
                                  u,
                                  v,
                                  w,
                                  s,
                                  vertexShade || vertexHaze,
                                  wrapped,
                                  shade || haze,
                                  material.transparent,
                                  material.clampTexture,
                                  *texture,
                                  segments)) {
                return false;
            }
            spanCount += segments.size();
            if (spanCount > renderLimits().spans) {
                return false;
            }
            primitive.spans = std::move(segments);
        }
        for (auto& span : primitive.spans) {
            const auto shadeStart =
                texture
                    ? (Float(s.dx) * span.originLeft + (Float(s.dy) * span.y + s.origin)).stored()
                    : ((Float(s.dx) * span.originLeft + s.origin) + Float(s.dy) * span.y).stored();
            if (!perspectiveTexture) {
                if (!fixed(u.at(span.originLeft, span.y), span.u) ||
                    !fixed(v.at(span.originLeft, span.y), span.v) ||
                    !fixed(shadeStart, span.shade)) {
                    return false;
                }
                span.du = primitive.du;
                span.dv = primitive.dv;
            }
            if (span.shade < INT32_MIN || span.shade > INT32_MAX) {
                return false;
            }
            const Int64 advance = Int64(span.left) - span.originLeft;
            if (!perspectiveTexture) {
                span.u += advance * primitive.du;
                span.v += advance * primitive.dv;
            }
            const Int64 steps = span.right - span.left - 1;
            if (texture && !perspectiveTexture) {
                for (auto offset : {Int64{0}, steps}) {
                    const auto x = whole(span.u + primitive.du * offset),
                               y = whole(span.v + primitive.dv * offset);
                    if (!material.clampTexture &&
                        (x < 0 || x >= texture->stride() || y < 0 || y >= texture->height)) {
                        return false;
                    }
                }
                // Original UVT's transparent first byte in the final word
                // jumps to its one-byte tail, omitting a remaining even pixel.
                // Preserve it for this renderer only, anchored before clipping.
                const Int32 last = span.originRight - 1;
                if (material.transparent && !material.clampTexture && !shade && !haze &&
                    last >= span.left && last < span.right && last % 2 == 0 &&
                    span.originLeft <= last - 2) {
                    const Int64 offset = Int64(last) - 2 - span.left;
                    const auto x = whole(span.u + primitive.du * offset),
                               y = whole(span.v + primitive.dv * offset);
                    if (x < 0 || x >= texture->stride() || y < 0 || y >= texture->height) {
                        return false;
                    }
                    span.omitLast =
                        texture->level()[std::size_t(y) * std::size_t(texture->stride()) +
                                         std::size_t(x)] == std::byte{0};
                }
            }
            if (vertexShade || vertexHaze) {
                for (Int64 offset = 0; offset <= steps; ++offset) {
                    const auto level = shadeAt(span,
                                               primitive.ds,
                                               UInt32(advance + offset) + shadePrestep,
                                               texture != nullptr);
                    if (level > maxShade ||
                        mapBase + std::size_t(level) * 256 + 255 >= sub->remap.size()) {
                        return false;
                    }
                }
            }
        }
        primitives.push_back(std::move(primitive));
    }
    // All validation and self-texture snapshots precede writes, including later
    // fan triangles. Clipping advances the same original interpolation origins.
    const auto texels = texture ? texture->level() : std::span<const std::byte>{};
    const auto textureStride = texture ? texture->stride() : 0;
    const auto wrapMaskU = texture ? UInt32(textureStride - 1) : 0;
    const auto wrapMaskV =
        texture ? (UInt32(texture->height - 1) << std::countr_zero(UInt32(textureStride))) : 0;
    for (const auto& primitive : primitives) {
        for (auto span : primitive.spans) {
            auto row = bitmap_.row(span.y);
            for (Int32 x = span.left; x < span.right; ++x) {
                if (span.omitLast && x == span.originRight - 1) {
                    continue;
                }
                UInt8 value = fillColor;
                if (texture) {
                    const auto offset =
                        wrapped ? ((UInt32(span.u) & wrapMaskU) | (UInt32(span.v) & wrapMaskV))
                        : material.clampTexture
                            ? std::size_t(
                                  std::clamp(whole(span.v), Int64{0}, Int64(texture->height - 1))) *
                                      std::size_t(textureStride) +
                                  std::size_t(std::clamp(
                                      whole(span.u), Int64{0}, Int64(texture->width - 1)))
                            : std::size_t(whole(span.v)) * std::size_t(textureStride) +
                                  std::size_t(whole(span.u));
                    value = std::to_integer<UInt8>(texels[offset]);
                }
                auto& destination = row[std::size_t(x)];
                if (material.blend) {
                    destination =
                        sub->remap[std::size_t(value) * 256 + std::to_integer<UInt8>(destination)];
                } else if (!material.transparent || value != 0) {
                    if (material.lightMap) {
                        const auto level =
                            lightLevel(primitive, *material.lightMap, x, span.y, maxShade);
                        value =
                            std::to_integer<UInt8>(sub->remap[std::size_t(level) * 256 + value]);
                    } else if (vertexShade || vertexHaze) {
                        const auto level =
                            shadeAt(span,
                                    primitive.ds,
                                    UInt32(Int64(x) - span.originLeft) + shadePrestep,
                                    texture != nullptr);
                        value = std::to_integer<UInt8>(
                            sub->remap[mapBase + std::size_t(level) * 256 + value]);
                    } else if (texture && (shade || haze)) {
                        value = std::to_integer<UInt8>(sub->remap[mapBase + value]);
                    }
                    destination = std::byte(value);
                }
                if (wrapped) {
                    if (shade || haze) {
                        const UInt64 sum = UInt64(UInt32(span.u)) + UInt32(span.du);
                        span.u = UInt32(sum);
                        span.dv = UInt32(span.dv) + UInt32(sum >> 32);
                    } else {
                        span.u = carryAdd(UInt32(span.u), UInt32(span.du));
                    }
                    span.v = carryAdd(UInt32(span.v), UInt32(span.dv));
                } else {
                    span.u += span.du;
                    span.v += span.dv;
                }
            }
        }
    }
    return true;
}

bool SoftwareSurface::sortedPolygons(std::span<const SortedPolygon> polygons, DepthOrder order) {
    const auto maxPrimitives = renderLimits().polygons;
    if (polygons.size() > maxPrimitives || UInt8(order) > UInt8(DepthOrder::SmallerInFront)) {
        return false;
    }

    struct Input {
        std::vector<PolygonVertex> vertices;
        std::vector<Point3F> positions;
        PolygonMaterial material;
        bool behind;
    };

    std::vector<Input> inputs;
    for (const auto& polygon : polygons) {
        const auto vertices = polygon.vertices;
        if (vertices.size() < 3 || vertices.size() > 100) {
            return false;
        }
        const auto& material = polygon.material;
        std::size_t unused = 0;
        // Validate the entire convex n-gon before fan expansion can hide an
        // invalid boundary. An empty sorted span list cannot write pixels.
        if (!polygonSpans(vertices, material, {}, true, unused)) {
            return false;
        }
        const bool fan = (material.texture && !material.perspective) ||
                         material.shadeSource == PolygonSource::Vertex ||
                         material.hazeSource == PolygonSource::Vertex || material.lightMap;
        const auto count = fan ? vertices.size() - 2 : 1;
        if (count > maxPrimitives - inputs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            Input input{{}, {}, material, polygon.alwaysBehind};
            if (fan) {
                input.vertices = {vertices[0], vertices[i + 1], vertices[i + 2]};
            } else {
                input.vertices.assign(vertices.begin(), vertices.end());
            }
            std::ranges::transform(
                input.vertices, std::back_inserter(input.positions), [](const auto& v) {
                    return Point3F{v.position.x, v.position.y, v.inverseDepth};
                });
            inputs.push_back(std::move(input));
        }
    }
    std::vector<VisibilityPolygon> geometry;
    geometry.reserve(inputs.size());
    std::ranges::transform(inputs, std::back_inserter(geometry), [](const auto& p) {
        return VisibilityPolygon{p.positions, p.material.transparent || p.material.blend, p.behind};
    });
    // Resolving against framebuffer bounds retains depth-fragment origins under
    // clip changes (including perspective subspan and transparent-tail policy).
    const auto spans = sortVisibility(geometry, bitmap_.bounds(), order);
    if (!spans) {
        return false;
    }
    if (spans->empty() || !clip_.valid()) {
        return true;
    }
    // Staging bounds atomicity to one framebuffer and palette snapshot. All
    // self-textures still refer to the unchanged original surface during this
    // batch, including after an earlier polygon has written to the staging copy.
    SoftwareSurface staged(bitmap_);
    staged.clip_ = clip_;
    staged.palette_ = palette_;
    std::size_t cursor = 0, spanCount = 0;
    // sortVisibility returns opaque primitives first, then transparent ones.
    for (const bool transparent : {false, true}) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            if ((input.material.transparent || input.material.blend) != transparent) {
                continue;
            }
            const auto first = cursor;
            while (cursor < spans->size() && (*spans)[cursor].polygon == i) {
                ++cursor;
            }
            if (!staged.polygonSpans(input.vertices,
                                     input.material,
                                     std::span(*spans).subspan(first, cursor - first),
                                     true,
                                     spanCount)) {
                return false;
            }
        }
    }
    // Preserve outstanding framebuffer spans/addresses across a successful draw.
    std::ranges::copy(staged.bitmap_.pixels, bitmap_.pixels.begin());
    return true;
}
} // namespace opent1::gfx
