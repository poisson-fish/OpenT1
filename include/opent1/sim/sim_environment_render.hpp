#pragma once
#include <opent1/math/matrix.hpp>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace opent1::sim {
// Invalid parameters raise std::invalid_argument. Results own their storage;
// these stateless operations may run concurrently on independent snapshots.
struct VolumetricState;

struct VolumetricView {
    Point3F cameraPosition; // world space
    Flt32 visibleDistance{1500}, hazeDistance{750};
};

struct VolumetricVertex {
    Point3F position; // world space
    Point2F texture;  // repeating local-space coordinates
    Flt32 haze{};
};

struct VolumetricFace {
    UInt16 face{};                          // original TOP/DOWN/LEFT/RIGHT/FRONT/BACK bit
    UInt8 colorIndex{};                     // original palette fallback: 244, 245 or 246
    std::vector<VolumetricVertex> vertices; // convex, at most 12
};

struct VolumetricGeometry {
    std::vector<VolumetricFace> faces;
    std::string materialName;
    Flt32 opacity{};
};

// Clips each enabled face to the original octagonal visibility footprint. No
// material resource or simulation object is borrowed by the resulting snapshot.
// Degenerate faces and faces outside visibility produce no polygon.
[[nodiscard]] VolumetricGeometry buildVolumetricGeometry(const VolumetricState&,
                                                         const VolumetricView&);
// One material-list cycle per second * scale, as in SimVolumetric. Zero frames
// returns zero; negative/nonfinite time or scale and counts >32768 are rejected.
[[nodiscard]] std::size_t volumetricAnimationFrame(Flt32 time, Flt32 scale, std::size_t frames);

struct ShadowSourceWindow {
    Point3F direction, right, up;
    std::array<Point3F, 4> points; // upper left/right, lower right/left, world space
    Flt32 radius{};
};

// shapeCenter includes any root-node translation. shapeRotation follows the
// row-vector convention; box dimensions determine the square light-space window.
// Light must be unit length. The caller supplies shape/receiver data from TS or
// terrain when available; no TS resource or graphics state is required here.
[[nodiscard]] ShadowSourceWindow makeShadowSourceWindow(const Point3F& light,
                                                        const Point3F& shapeCenter,
                                                        Flt32 shapeRadius,
                                                        const Box3F& objectBox,
                                                        const RMat3F& shapeRotation,
                                                        const Point3F& position = {});

struct ShadowProjection {
    std::array<Point3F, 4> points;
    Flt32 lightPlaneDot{}; // -dot(light direction, receiver normal)
};

// Plane equation dot(normal,p)=constant, matching TS::Shadow. Back-facing and
// near-parallel planes return nullopt. Lift translates against the light.
[[nodiscard]] std::optional<ShadowProjection>
projectShadow(const ShadowSourceWindow&, const Point3F& normal, Flt32 constant, Flt32 lift = 0);

struct ShadowReceiver {
    Point3F normal;
    Flt32 constant{};              // dot(normal,p)=constant
    std::vector<Point3F> vertices; // convex and coplanar, either winding, 3..32
};

struct ShadowVertex {
    Point3F position;
    Point2F texture;
};

struct ShadowPolygon {
    std::size_t receiverIndex{};
    std::vector<ShadowVertex> vertices;
};

struct ShadowGeometry {
    std::vector<ShadowPolygon> polygons;
    Flt32 maxPlaneDot{}; // max(0,-dot(cameraDirection,normal)), for shadow detail
};

// Projects/clips the source quad to at most 100 convex receivers and retains
// interpolated UVs. Results own all vertices and may be cached by the caller.
// Receivers describe a non-overlapping surface; world/TS adapters remain
// responsible for visibility, occlusion and collecting those surfaces.
[[nodiscard]] ShadowGeometry buildShadowGeometry(const ShadowSourceWindow&,
                                                 std::span<const ShadowReceiver>,
                                                 const Point3F& cameraDirection,
                                                 Flt32 lift = .1f);
// Bounded original TS::Shadow cache partition, including native 0.05/0.1
// plane/edge tolerances. The single-plane path intentionally projects the full
// source quad. Exact authored-edge projection remains buildShadowGeometry().
[[nodiscard]] ShadowGeometry nativeShadowGeometry(const ShadowSourceWindow&,
                                                  std::span<const ShadowReceiver>,
                                                  const Point3F& cameraDirection,
                                                  Flt32 lift = .1f);
[[nodiscard]] Box3F
shadowQueryBox(const Point3F& center, Flt32 radius, const Point3F& light, Flt32 length = 100);

enum class ShadowBlur : Int32 { None = 0, Box2 = 2, Weighted3 = 3, Legacy5 = 5, Oversampled3 = 23 };

struct ShadowBitmap {
    std::size_t dimension{};
    std::vector<UInt8> pixels; // row-major, one byte per pixel
    bool translucent{};        // None retains indexed silhouette values instead of alpha
};

// Input is a complete square silhouette including padding, analogous to the
// original 256x256 scratch bitmap. Nonzero means covered for blurred modes.
// The centered crop must include the filter's padding; 23 uses 2x input pixels.
// Dimensions are bounded to 256, and output to 16/32/64. Mode 5 deliberately
// preserves the original grey-table truncation (only full coverage reaches 1).
[[nodiscard]] ShadowBitmap filterShadowBitmap(std::span<const UInt8> silhouette,
                                              std::size_t sourceDimension,
                                              std::size_t outputDimension,
                                              ShadowBlur,
                                              Flt32 alpha = .5f);
} // namespace opent1::sim
