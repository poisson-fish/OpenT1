#include <opent1/sim/common_irc_shadow.hpp>
#include <opent1/sim/sim_environment_render.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <ranges>
#include <stdexcept>

namespace opent1::sim {
namespace {
bool finite(Flt32 v) {
    return std::isfinite(v) && std::abs(v) <= 1e12f;
}

bool finite(const Point3F& p) {
    return finite(p.x) && finite(p.y) && finite(p.z);
}

bool unit(const Point3F& p) {
    return finite(p) && std::abs(m_dot(p, p) - 1.f) <= 1e-4f;
}

Point3F cross(const Point3F& a, const Point3F& b) {
    Point3F out;
    return m_cross(a, b, out);
}

void require(bool condition) {
    if (!condition) {
        throw std::invalid_argument("invalid environment render input");
    }
}

constexpr std::array<Point2F, 8> octagon{{{0, 1},
                                          {.707f, .707f},
                                          {1, 0},
                                          {.707f, -.707f},
                                          {0, -1},
                                          {-.707f, -.707f},
                                          {-1, 0},
                                          {-.707f, .707f}}};
constexpr std::array<Int32, 4> weights2{1, 1, 1, 1};
constexpr std::array<Int32, 9> weights3{1, 2, 1, 2, 3, 2, 1, 2, 1};
constexpr std::array<Int32, 25> weights5{1, 2, 3, 2, 1, 2, 4, 5, 4, 2, 3, 5, 6,
                                         5, 3, 2, 4, 5, 4, 2, 1, 2, 3, 2, 1};

std::vector<Point2D>
clip(std::vector<Point2D> polygon, std::size_t axis, Flt64 boundary, bool lower) {
    if (polygon.empty()) {
        return {};
    }
    std::vector<Point2D> result;
    result.reserve(polygon.size() + 1);
    auto previous = polygon.back();
    const auto inside = [&](const Point2D& p) {
        return lower ? p[axis] >= boundary : p[axis] <= boundary;
    };
    std::ranges::for_each(polygon, [&](const auto& current) {
        const bool previousIn = inside(previous), currentIn = inside(current);
        if (previousIn != currentIn) {
            // The legacy upper-edge expression omitted parentheses around
            // (1-previous). Compute the intersection on the boundary itself.
            const auto t = (boundary - previous[axis]) / (current[axis] - previous[axis]);
            auto point = previous + (current - previous) * t;
            point[axis] = boundary;
            result.push_back(point);
        }
        if (currentIn) {
            result.push_back(current);
        }
        previous = current;
    });
    return result;
}

Flt32 hazeAt(const Point3F& point, const VolumetricView& view) {
    const auto distance = (point - view.cameraPosition).len();
    if (distance <= view.hazeDistance) {
        return .0001f;
    }
    const auto delta = view.visibleDistance - view.hazeDistance;
    const auto factor = (distance - view.hazeDistance) / (delta + delta * .02f) - 1.f;
    return std::clamp(1.f - factor * factor, 0.f, 1.f);
}
}

VolumetricGeometry buildVolumetricGeometry(const VolumetricState& state,
                                           const VolumetricView& view) {
    require(state.valid() && finite(view.cameraPosition) && finite(view.visibleDistance) &&
            view.visibleDistance > 0 && finite(view.hazeDistance) && view.hazeDistance >= 0 &&
            view.hazeDistance < view.visibleDistance);
    VolumetricGeometry result;
    result.materialName = state.materialName;
    result.opacity = state.transparency;
    const auto& p = state.dimensions;
    const Point3F a{}, b{0, 0, p.z}, c{p.x, 0, p.z}, d{p.x, 0, 0}, e{0, p.y, 0}, f{0, p.y, p.z},
        h{p.x, p.y, 0};

    struct Face {
        UInt16 flag;
        UInt8 color;
        std::size_t u, v;
        Point3F a, b, d;
    };

    // Original drawCube order, winding, texture axes and fallback colors.
    const std::array<Face, 6> faces{{{16, 246, 2, 0, a, b, d},
                                     {32, 246, 2, 0, e, h, f},
                                     {4, 245, 1, 2, a, e, b},
                                     {8, 245, 1, 2, d, c, h},
                                     {2, 244, 0, 1, a, d, e},
                                     {1, 244, 0, 1, b, f, c}}};
    const auto camera = view.cameraPosition - state.position;
    std::ranges::for_each(faces, [&](const auto& face) {
        if (!(state.faces & face.flag)) {
            return;
        }
        const auto forward = face.b - face.a, right = face.d - face.a;
        const auto forwardLength = forward.len(), rightLength = right.len();
        if (forwardLength == 0 || rightLength == 0) {
            return;
        }
        const auto forwardUnit = forward / forwardLength, rightUnit = right / rightLength;
        const auto normal = cross(rightUnit, forwardUnit);
        const auto distance = m_dot(camera - face.a, normal);
        // Both sides need the same test: the legacy one-sided test could take
        // sqrt of a negative value for a distant camera behind the face.
        if (std::abs(distance) >= view.visibleDistance) {
            return;
        }
        const auto radius = std::sqrt(
            std::max(0.f, view.visibleDistance * view.visibleDistance - distance * distance));
        if (radius < 1) {
            return;
        }
        // Double intermediates keep normalized coordinates finite for very
        // thin, nonzero volumes; the published geometry remains float.
        const Point2D center{Flt64(m_dot(rightUnit, camera - face.a)) / rightLength,
                             Flt64(m_dot(forwardUnit, camera - face.a)) / forwardLength};
        const Point2D scale{Flt64(radius) / rightLength, Flt64(radius) / forwardLength};
        std::vector<Point2D> polygon;
        polygon.reserve(12);
        std::ranges::transform(octagon, std::back_inserter(polygon), [&](const auto& point) {
            return Point2D{point.x * scale.x + center.x, point.y * scale.y + center.y};
        });
        polygon = clip(std::move(polygon), 0, 0, true);
        polygon = clip(std::move(polygon), 0, 1, false);
        polygon = clip(std::move(polygon), 1, 0, true);
        polygon = clip(std::move(polygon), 1, 1, false);
        polygon.erase(std::unique(polygon.begin(), polygon.end()), polygon.end());
        if (polygon.size() > 1 && polygon.front() == polygon.back()) {
            polygon.pop_back();
        }
        require(polygon.size() <= 12);
        // Boundary contact can yield duplicate/collinear points and no area.
        if (polygon.size() < 3) {
            return;
        }
        Flt64 area{};
        auto previous = polygon.back();
        std::ranges::for_each(polygon, [&](const auto& point) {
            area += Flt64(previous.x) * point.y - Flt64(previous.y) * point.x;
            previous = point;
        });
        if (area == 0) {
            return;
        }
        VolumetricFace output;
        output.face = face.flag;
        output.colorIndex = face.color;
        output.vertices.reserve(polygon.size());
        std::ranges::transform(
            polygon, std::back_inserter(output.vertices), [&](const auto& point) {
                const auto local = right * static_cast<Flt32>(point.x) +
                                   forward * static_cast<Flt32>(point.y) + face.a;
                const auto world = local + state.position;
                return VolumetricVertex{
                    world,
                    {local[face.u] * state.textureScale, local[face.v] * state.textureScale},
                    hazeAt(world, view)};
            });
        result.faces.push_back(std::move(output));
    });
    return result;
}

std::size_t volumetricAnimationFrame(Flt32 time, Flt32 scale, std::size_t frames) {
    require(finite(time) && time >= 0 && finite(scale) && scale >= 0 && frames <= 32768);
    if (!frames) {
        return 0;
    }
    const auto cycle = time * scale;
    const auto fraction = cycle - std::floor(cycle);
    return static_cast<std::size_t>(fraction * static_cast<Flt32>(frames)) % frames;
}

ShadowSourceWindow makeShadowSourceWindow(const Point3F& light,
                                          const Point3F& center,
                                          Flt32 radius,
                                          const Box3F& box,
                                          const RMat3F& rotation,
                                          const Point3F& position) {
    require(unit(light) && finite(center) && finite(radius) && radius >= 0 && finite(box.fMin) &&
            finite(box.fMax) && box.isValid() && finite(position));
    require(std::ranges::all_of(rotation.m, [](const auto& row) {
        return std::ranges::all_of(row, [](auto v) {
            return finite(v);
        });
    }));
    ShadowSourceWindow result;
    result.direction = light;
    if (light.z * light.z < .99f) {
        result.right = cross(light, Point3F{0, 0, 1});
        result.right.normalize();
        result.up = cross(result.right, light);
    } else {
        result.up = cross(Point3F{1, 0, 0}, light);
        result.up.normalize();
        result.right = cross(light, result.up);
    }
    const auto radii = (box.fMax - box.fMin) * .5f;
    Point3F x, y, z;
    rotation.getRow(0, x);
    rotation.getRow(1, y);
    rotation.getRow(2, z);
    const auto extent = [&](const Point3F& axis) {
        return radii.x * std::abs(m_dot(x, axis)) + radii.y * std::abs(m_dot(y, axis)) +
               radii.z * std::abs(m_dot(z, axis));
    };
    result.radius = std::max(extent(result.right), extent(result.up));
    require(finite(result.radius));
    auto origin = light * (-radius - 2);
    origin += center;
    const auto right = result.right * result.radius, up = result.up * result.radius;
    result.points = {up - right, up + right, -up + right, -up - right};
    std::ranges::for_each(result.points, [&](auto& point) {
        point += origin;
        point += position;
        require(finite(point));
    });
    return result;
}

std::optional<ShadowProjection>
projectShadow(const ShadowSourceWindow& window, const Point3F& normal, Flt32 constant, Flt32 lift) {
    require(unit(window.direction) && unit(normal) && finite(constant) && finite(lift) &&
            lift >= 0 && std::ranges::all_of(window.points, [](const auto& point) {
                return finite(point);
            }));
    const auto dot = m_dot(window.direction, normal);
    if (dot >= -.01f) {
        return std::nullopt;
    }
    ShadowProjection result;
    result.lightPlaneDot = -dot;
    const auto inverse = 1.f / dot;
    std::ranges::transform(window.points, result.points.begin(), [&](const auto& point) {
        const auto t = (constant - m_dot(point, normal)) * inverse;
        auto projected = window.direction * t;
        projected += point;
        projected -= window.direction * lift;
        require(finite(projected));
        return projected;
    });
    return result;
}

Box3F shadowQueryBox(const Point3F& center, Flt32 radius, const Point3F& light, Flt32 length) {
    require(finite(center) && finite(radius) && radius >= 0 && unit(light) && finite(length) &&
            length >= 0);
    Box3F result{center - radius, center + radius};
    std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{3}), [&](auto i) {
        if (light[i] > 0) {
            result.fMax[i] += light[i] * length;
        } else {
            result.fMin[i] += light[i] * length;
        }
    });
    require(finite(result.fMin) && finite(result.fMax));
    return result;
}

ShadowGeometry buildShadowGeometry(const ShadowSourceWindow& window,
                                   std::span<const ShadowReceiver> receivers,
                                   const Point3F& cameraDirection,
                                   Flt32 lift) {
    require(receivers.size() <= 100 && finite(lift) && lift >= 0 && unit(window.direction) &&
            unit(cameraDirection) && std::ranges::all_of(window.points, [](const auto& point) {
                return finite(point);
            }));
    ShadowGeometry result;
    constexpr std::array<Point2F, 4> textures{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    std::ranges::for_each(
        std::views::iota(std::size_t{0}, receivers.size()), [&](auto receiverIndex) {
            const auto& receiver = receivers[receiverIndex];
            require(unit(receiver.normal) && finite(receiver.constant) &&
                    receiver.vertices.size() >= 3 && receiver.vertices.size() <= 32);
            const auto& vertices = receiver.vertices;
            const auto origin = vertices.front();
            require(std::ranges::all_of(vertices, [&](const auto& point) {
                return finite(point) &&
                       std::abs(m_dot(receiver.normal, point) - receiver.constant) <=
                           1e-4f * std::max(1.f, std::abs(receiver.constant));
            }));
            // Work relative to a vertex to avoid cancellation when the receiver
            // is translated far from the world origin.
            Flt32 area{};
            std::ranges::for_each(
                std::views::iota(std::size_t{1}, vertices.size() - 1), [&](auto i) {
                    area += m_dot(cross(vertices[i] - origin, vertices[i + 1] - origin),
                                  receiver.normal);
                });
            require(std::isfinite(area) && area != 0);
            const auto sign = area > 0 ? 1.f : -1.f;
            struct Edge {
                Point3F start, inward;
            };
            std::vector<Edge> edges;
            edges.reserve(vertices.size());
            std::ranges::for_each(std::views::iota(std::size_t{0}, vertices.size()), [&](auto i) {
                auto inward =
                    cross(receiver.normal, vertices[(i + 1) % vertices.size()] - vertices[i]) *
                    sign;
                const auto length = inward.len();
                require(length > 0 && std::isfinite(length));
                inward /= length;
                require(std::ranges::all_of(vertices, [&](const auto& p) {
                    return m_dot(p - vertices[i], inward) >= -1e-4f;
                }));
                edges.push_back({vertices[i], inward});
            });
            const auto projected = projectShadow(window, receiver.normal, receiver.constant);
            if (!projected) {
                return;
            }
            std::vector<ShadowVertex> polygon;
            polygon.reserve(4 + vertices.size());
            std::ranges::for_each(std::views::iota(std::size_t{0}, std::size_t{4}), [&](auto i) {
                polygon.push_back({projected->points[i], textures[i]});
            });
            std::ranges::for_each(edges, [&](const auto& edge) {
                if (polygon.empty()) {
                    return;
                }
                std::vector<ShadowVertex> clipped;
                clipped.reserve(polygon.size() + 1);
                auto previous = polygon.back();
                auto previousDistance = m_dot(previous.position - edge.start, edge.inward);
                std::ranges::for_each(polygon, [&](const auto& current) {
                    const auto distance = m_dot(current.position - edge.start, edge.inward);
                    if ((previousDistance >= 0) != (distance >= 0)) {
                        const auto t = previousDistance / (previousDistance - distance);
                        clipped.push_back(
                            {previous.position + (current.position - previous.position) * t,
                             previous.texture + (current.texture - previous.texture) * t});
                    }
                    if (distance >= 0) {
                        clipped.push_back(current);
                    }
                    previous = current;
                    previousDistance = distance;
                });
                polygon = std::move(clipped);
                require(polygon.size() <= 36);
            });
            polygon.erase(std::unique(polygon.begin(),
                                      polygon.end(),
                                      [](const auto& a, const auto& b) {
                                          return a.position == b.position;
                                      }),
                          polygon.end());
            if (polygon.size() > 1 && polygon.front().position == polygon.back().position) {
                polygon.pop_back();
            }
            if (polygon.size() < 3) {
                return;
            }
            Flt32 projectedArea{};
            const auto start = polygon.front().position;
            std::ranges::for_each(
                std::views::iota(std::size_t{1}, polygon.size() - 1), [&](auto i) {
                    projectedArea +=
                        m_dot(cross(polygon[i].position - start, polygon[i + 1].position - start),
                              receiver.normal);
                });
            if (projectedArea == 0) {
                return;
            }
            std::ranges::for_each(polygon, [&](auto& vertex) {
                vertex.position -= window.direction * lift;
                require(finite(vertex.position));
            });
            result.maxPlaneDot = std::max(
                result.maxPlaneDot, std::clamp(-m_dot(cameraDirection, receiver.normal), 0.f, 1.f));
            result.polygons.push_back({receiverIndex, std::move(polygon)});
        });
    return result;
}

ShadowBitmap filterShadowBitmap(std::span<const UInt8> source,
                                std::size_t sourceDimension,
                                std::size_t dimension,
                                ShadowBlur method,
                                Flt32 alpha) {
    require(sourceDimension > 0 && sourceDimension <= 256 &&
            source.size() == sourceDimension * sourceDimension &&
            (dimension == 16 || dimension == 32 || dimension == 64) && finite(alpha) &&
            alpha >= 0 && alpha <= 1);
    std::span<const Int32> weights;
    std::size_t width{}, oversampling = 1, padding{};
    switch (method) {
        case ShadowBlur::None:
            break;
        case ShadowBlur::Box2:
            weights = weights2;
            width = 2;
            padding = 1;
            break;
        case ShadowBlur::Weighted3:
            weights = weights3;
            width = 3;
            padding = 1;
            break;
        case ShadowBlur::Legacy5:
            weights = weights5;
            width = 5;
            padding = 2;
            break;
        case ShadowBlur::Oversampled3:
            weights = weights3;
            width = 3;
            padding = 2;
            oversampling = 2;
            break;
        default:
            require(false);
    }
    require(sourceDimension >= dimension * oversampling + 2 * padding);
    const auto inset = (sourceDimension - dimension * oversampling) / 2;
    ShadowBitmap result{
        dimension, std::vector<UInt8>(dimension * dimension), method != ShadowBlur::None};
    if (method == ShadowBlur::None) {
        std::ranges::for_each(std::views::iota(std::size_t{0}, dimension), [&](auto row) {
            std::ranges::copy(source.subspan((inset + row) * sourceDimension + inset, dimension),
                              result.pixels.begin() + static_cast<std::ptrdiff_t>(row * dimension));
        });
        return result;
    }
    const auto total = std::accumulate(weights.begin(), weights.end(), Int32{0}) *
                       static_cast<Int32>(oversampling * oversampling);
    std::vector<UInt8> greys(static_cast<std::size_t>(total) + 1);
    std::ranges::transform(std::views::iota(Int32{0}, total + 1), greys.begin(), [&](auto value) {
        const auto multiplier = method == ShadowBlur::Legacy5 ? 1.f : 255.f;
        return static_cast<UInt8>(multiplier * static_cast<Flt32>(value) /
                                  static_cast<Flt32>(total));
    });
    const auto kernelDimension = width * oversampling;
    std::ranges::for_each(std::views::iota(std::size_t{0}, result.pixels.size()), [&](auto pixel) {
        const auto x = inset + (pixel % dimension) * oversampling - padding,
                   y = inset + (pixel / dimension) * oversampling - padding;
        Int32 tally{};
        std::ranges::for_each(
            std::views::iota(std::size_t{0}, kernelDimension * kernelDimension), [&](auto kernel) {
                const auto row = kernel / kernelDimension, column = kernel % kernelDimension;
                if (source[(y + row) * sourceDimension + x + column]) {
                    tally += weights[(row / oversampling) * width + column / oversampling];
                }
            });
        result.pixels[pixel] = static_cast<UInt8>(alpha * greys[static_cast<std::size_t>(tally)]);
    });
    return result;
}
} // namespace opent1::sim
