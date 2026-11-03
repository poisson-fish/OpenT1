#include <opent1/gui/terrain_tools.hpp>
#include <ranges>
#include <charconv>
#include <cmath>
#include <unordered_set>

namespace opent1::gui {
namespace {
Point2I gridSize(const world::TerrainDocument& d) {
    return {d.grid.size.x * Int32(d.blocks.front().width),
            d.grid.size.y * Int32(d.blocks.front().width)};
}

struct Cell {
    const world::TerrainGridBlock* block;
    UInt32 x, y;
};

std::optional<Cell> at(const world::TerrainDocument& d, Point2I p) {
    if (d.blocks.empty()) {
        return {};
    }
    const auto size = gridSize(d);
    const auto w = Int32(d.blocks.front().width);
    if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) {
        return {};
    }
    const auto id = d.grid.blockMap[std::size_t(p.y / w) * d.grid.size.x + p.x / w];
    auto entry = std::ranges::find(d.grid.blocks, id, &world::TerrainGridBlockEntry::id);
    if (entry == d.grid.blocks.end()) {
        return {};
    }
    return Cell{
        &d.blocks[std::size_t(entry - d.grid.blocks.begin())], UInt32(p.x % w), UInt32(p.y % w)};
}

bool configure(Control& control, Point2I p, Point2I e) {
    auto f = control.fields();
    f.position = p;
    f.extent = e;
    return control.configure(f);
}
}

TerrainView::TerrainView(std::shared_ptr<world::TerrainEditor> editor)
    : editor_(std::move(editor)) {
    if (!editor_) {
        throw std::invalid_argument("terrain editor model");
    }
    active = true;
}

bool TerrainView::projection(std::optional<sim::EnvironmentCamera> camera,
                             world::TsMaterialResources materials) {
    if (camera) {
        if (camera->viewport.left || camera->viewport.top ||
            camera->viewport.right != fields_.extent.x - 1 ||
            camera->viewport.bottom != fields_.extent.y - 1 ||
            !std::isfinite(camera->nearDistance) || !std::isfinite(camera->farDistance) ||
            camera->nearDistance <= 0 || camera->farDistance <= camera->nearDistance ||
            !std::isfinite(camera->focal.x) || !std::isfinite(camera->focal.y) ||
            camera->focal.x <= 0 || camera->focal.y <= 0 || !std::isfinite(camera->center.x) ||
            !std::isfinite(camera->center.y)) {
            return false;
        }
        try {
            auto inverse = camera->worldToCamera;
            inverse.inverse();
            for (const auto& row : inverse.m) {
                for (auto value : row) {
                    if (!std::isfinite(value)) {
                        return false;
                    }
                }
            }
        } catch (...) {
            return false;
        }
    }
    camera_ = std::move(camera);
    if (!materials.list().materials.empty()) {
        materials_ = std::move(materials);
    }
    return true;
}

namespace {
world::TsStaticView terrainCamera(const sim::EnvironmentCamera& c, Point2I extent) {
    world::TsStaticView v;
    v.shapeToCamera = c.worldToCamera;
    v.width = extent.x;
    v.height = extent.y;
    v.center = c.center;
    v.focal = c.focal;
    v.nearDistance = c.nearDistance;
    v.farDistance = c.farDistance;
    v.perspective = c.perspective;
    v.twoSided = true;
    return v;
}

world::TsMaterialResources flatTerrainMaterials() {
    world::TsMaterialResources::Data materials;
    materials.list.details = 1;
    materials.list.materialsPerDetail = 256;
    materials.list.materials.resize(256);
    materials.textures.resize(256);
    for (auto i : std::views::iota(0, 256)) {
        materials.list.materials[i].flags = 1;
        materials.list.materials[i].index = i;
    }
    return world::TsMaterialResources(std::move(materials));
}
}

std::shared_ptr<const world::TsStaticShapeFrame> TerrainView::projectedFrame() const {
    if (!camera_) {
        return {};
    }
    auto d = editor_->preview();
    if (!d) {
        return {};
    }
    auto resources =
        d->resources(materials_.list().materials.empty() ? flatTerrainMaterials() : materials_);
    if (!resources) {
        return {};
    }
    auto mesh = world::WorldSurfaceMesh::terrain(*resources, false);
    if (!mesh) {
        return {};
    }
    if (!display.materials) {
        mesh->materials = flatTerrainMaterials();
        for (auto& triangle : mesh->triangles) {
            triangle.material = UInt32(std::clamp(triangle.vertices[0].position.z, 0.f, 255.f));
        }
    }
    auto frame = mesh->prepare(terrainCamera(*camera_, fields_.extent));
    return frame ? std::make_shared<const world::TsStaticShapeFrame>(std::move(*frame)) : nullptr;
}

std::optional<Point2I> TerrainView::cell(Point2I p) const {
    if (pick) {
        return pick(p);
    }
    auto d = editor_->preview();
    if (!d || d->blocks.empty() || fields_.extent.x <= 0 || fields_.extent.y <= 0 || p.x < 0 ||
        p.y < 0 || p.x >= fields_.extent.x || p.y >= fields_.extent.y) {
        return {};
    }
    if (camera_) {
        const auto& c = *camera_;
        auto resources = d->resources();
        if (!resources) {
            return {};
        }
        auto collision = world::TerrainCollisionSnapshot::prepare(*resources);
        if (!collision) {
            return {};
        }
        const auto x = (float(p.x) - c.center.x) / c.focal.x,
                   z = (c.center.y - float(p.y)) / c.focal.y;
        auto inverse = c.worldToCamera;
        try {
            inverse.inverse();
        } catch (...) {
            return {};
        }
        Point3F origin, finish;
        m_mul(Point3F{x * (c.perspective ? c.nearDistance : 1),
                      c.nearDistance,
                      z * (c.perspective ? c.nearDistance : 1)},
              inverse,
              origin);
        m_mul(Point3F{x * (c.perspective ? c.farDistance : 1),
                      c.farDistance,
                      z * (c.perspective ? c.farDistance : 1)},
              inverse,
              finish);
        CollisionSurfaceList hit;
        if (!collision->line(origin, finish, hit, true) || hit.surfaces.empty()) {
            return {};
        }
        const auto square = float(1U << d->grid.scale);
        const auto position = hit.surfaces.front().position;
        Point2I result{Int32(std::floor(position.x / square)),
                       Int32(std::floor(position.y / square))};
        return at(*d, result) ? std::optional(result) : std::nullopt;
    }
    const auto size = gridSize(*d);
    return Point2I{Int32(Int64(p.x) * size.x / fields_.extent.x),
                   Int32(Int64(p.y) * size.y / fields_.extent.y)};
}

bool TerrainView::event(const Event& e) {
    if (e.type == EventType::Cancel) {
        close();
        return Active::event(e);
    }
    if (e.type == EventType::KeyDown && e.control && e.key == Key::Z) {
        return e.shift ? editor_->redo() : editor_->undo();
    }
    if (e.type == EventType::KeyDown && e.control && !e.shift && e.key == Key::C) {
        return editor_->action("copy");
    }
    if (e.type == EventType::KeyDown && e.control && !e.shift && e.key == Key::V) {
        auto restore = editor_->checkpoint();
        auto settings = editor_->settings();
        settings.paste = 3;
        if (editor_->settings(settings) && editor_->floatPaste()) {
            return true;
        }
        restore();
        return false;
    }
    if (e.type == EventType::KeyDown && e.key == Key::Escape) {
        close();
        if (canvas() && canvas()->capture() == this) {
            canvas()->unlockMouse();
        }
        return true;
    }
    if (e.type == EventType::Leave && !dragging_) {
        shadow_.clear();
        return true;
    }
    if (e.type == EventType::Move || e.type == EventType::Enter || e.type == EventType::Drag ||
        e.type == EventType::Down) {
        if (const auto hit = cell(globalToLocal(e.point))) {
            updateShadow(*hit);
        }
    }
    if (e.type == EventType::Down && (e.button == 1 || e.button == 3)) {
        const auto hit = cell(globalToLocal(e.point));
        if (!hit || !canvas() || !canvas()->makeFirstResponder(this) ||
            !canvas()->lockMouse(this)) {
            return false;
        }
        const auto index =
            e.button == 1 ? editor_->settings().leftAction : editor_->settings().rightAction;
        if (index >= world::TerrainEditor::actions().size()) {
            return false;
        }
        const auto action = world::TerrainEditor::actions()[index];
        if (!editor_->gesture(action, world::TerrainGesture::Begin, *hit)) {
            return false;
        }
        action_ = action;
        lastY_ = e.point.y;
        button_ = e.button;
        dragging_ = true;
        return true;
    }
    if (e.type == EventType::Drag && dragging_) {
        const auto hit = cell(globalToLocal(e.point));
        if (!hit) {
            return false;
        }
        if (!editor_->gesture(action_, world::TerrainGesture::Update, *hit, e.point.y - lastY_)) {
            return false;
        }
        lastY_ = e.point.y;
        return true;
    }
    if (e.type == EventType::Up && dragging_) {
        if (e.button != button_) {
            return false;
        }
        const auto hit = cell(globalToLocal(e.point));
        if (!hit) {
            return false;
        }
        canvas()->unlockMouse();
        if (!editor_->gesture(action_, world::TerrainGesture::End, *hit)) {
            return false;
        }
        dragging_ = false;
        return true;
    }
    return Active::event(e);
}

void TerrainView::close() {
    if (dragging_) {
        editor_->gesture(action_, world::TerrainGesture::Cancel, {});
        dragging_ = false;
    }
}

bool TerrainView::highlight(std::span<const Point2I> points) {
    const auto d = editor_->preview();
    if (!d || points.size() > 1048576 || std::ranges::any_of(points, [&](auto p) {
            return !at(*d, p);
        })) {
        return false;
    }
    highlight_.assign(points.begin(), points.end());
    return true;
}

std::optional<std::pair<std::string, std::string>> TerrainView::brushStatus(Point2I point) const {
    const auto d = editor_->preview();
    if (!d || d->blocks.empty()) {
        return {};
    }
    const auto settings = editor_->settings();
    const auto dim = Int32(1U << settings.brushDetail);
    if (settings.snap) {
        point.x &= ~(dim - 1);
        point.y &= ~(dim - 1);
    }
    const auto corner = at(*d, point);
    if (!corner) {
        return {};
    }
    const auto cornerHeight =
        corner->block->heights[std::size_t(corner->y) * (corner->block->width + 1) + corner->x];
    float total{};
    Int32 count{};
    for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                      return x < dim;
                  })) {
        for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                          return y < dim;
                      })) {
            if (const auto c = at(*d, {point.x + x, point.y + y})) {
                total += c->block->heights[std::size_t(c->y) * (c->block->width + 1) + c->x];
                ++count;
            }
        }
    }
    if (!count) {
        return {};
    }
    const auto avg = total / float(count);
    const Point2I center{(point.x * 2 + dim) / 2, (point.y * 2 + dim) / 2};
    return std::pair{
        std::format("Corner - ( {}, {} )  Height: {:.3f}", point.x, point.y, cornerHeight),
        std::format("Center - ( {}, {} )  AvgHeight: {:.3f}", center.x, center.y, avg)};
}

void TerrainView::updateShadow(Point2I point) {
    const auto d = editor_->preview();
    if (!d) {
        shadow_.clear();
        return;
    }
    const auto settings = editor_->settings();
    const auto dim = Int32(1U << settings.brushDetail);
    if (settings.snap) {
        point.x &= ~(dim - 1);
        point.y &= ~(dim - 1);
    }
    std::vector<Point2I> next;
    if (editor_->floating()) {
        for (const auto& item : editor_->clipboard()) {
            const auto p = point + item.point;
            if (at(*d, p)) {
                next.push_back(p);
            }
        }
    } else {
        for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                          return y < dim;
                      })) {
            for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                              return x < dim;
                          })) {
                const Point2I p{point.x + x, point.y + y};
                if (at(*d, p)) {
                    next.push_back(p);
                }
            }
        }
    }
    shadow_ = std::move(next);
}

std::function<void()> TerrainView::checkpoint() {
    auto base = Active::checkpoint();
    auto model = editor_->checkpoint();
    const auto d = dragging_;
    const auto y = lastY_;
    const auto action = action_;
    const auto button = button_;
    const auto h = highlight_, s = shadow_;
    const auto camera = camera_;
    const auto materials = materials_;
    const auto options = display;
    return [this,
            base = std::move(base),
            model = std::move(model),
            d,
            y,
            action,
            button,
            h,
            s,
            camera,
            materials,
            options] {
        base();
        model();
        dragging_ = d;
        lastY_ = y;
        action_ = action;
        button_ = button;
        highlight_ = h;
        shadow_ = s;
        camera_ = camera;
        materials_ = materials;
        display = options;
    };
}

bool TerrainView::draw(gfx::SoftwareSurface& s, Point2I p, RectI clip) const {
    if (camera_) {
        return projectedDraw(s, p, clip);
    }
    const auto d = editor_->preview();
    if (!d) {
        return true;
    }
    if (d->blocks.empty() || fields_.extent.x <= 0 || fields_.extent.y <= 0) {
        return false;
    }
    const auto size = gridSize(*d);
    if (Int64(size.x) * size.y > 1048576) {
        return false;
    }
    s.fill({p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1}, 0);
    for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                      return y < size.y;
                  })) {
        for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                          return x < size.x;
                      })) {
            const auto c = at(*d, {x, y});
            if (!c) {
                continue;
            }
            const auto& b = *c->block;
            const auto material = b.materials[std::size_t(c->y) * b.width + c->x];
            const auto height = b.heights[std::size_t(c->y) * (b.width + 1) + c->x];
            const auto color =
                display.materials ? UInt8(material.index) : UInt8(std::clamp(height, 0.f, 255.f));
            RectI r{p.x + Int32(Int64(x) * fields_.extent.x / size.x),
                    p.y + Int32(Int64(y) * fields_.extent.y / size.y),
                    p.x + Int32(Int64(x + 1) * fields_.extent.x / size.x) - 1,
                    p.y + Int32(Int64(y + 1) * fields_.extent.y / size.y) - 1};
            if (!gfx::intersect(clip, r)) {
                continue;
            }
            s.fill(r, material.hole() ? 0 : color);
            if (display.grid) {
                s.rectangle(r, 249);
            }
        }
    }
    const auto overlay = [&](std::span<const Point2I> points, UInt8 mask, UInt8 fill, UInt8 frame) {
        if (!(mask & 3)) {
            return;
        }
        const auto key = [](Point2I cell) {
            return (UInt64(UInt32(cell.x)) << 32) | UInt32(cell.y);
        };
        std::unordered_set<UInt64> membership;
        if (mask & 4) {
            membership.reserve(points.size());
            for (const auto q : points) {
                membership.insert(key(q));
            }
        }
        const auto contains = [&](Point2I cell) {
            return membership.contains(key(cell));
        };
        for (const auto q : points) {
            if (!at(*d, q)) {
                continue;
            }
            const RectI r{p.x + Int32(Int64(q.x) * fields_.extent.x / size.x),
                          p.y + Int32(Int64(q.y) * fields_.extent.y / size.y),
                          p.x + Int32(Int64(q.x + 1) * fields_.extent.x / size.x) - 1,
                          p.y + Int32(Int64(q.y + 1) * fields_.extent.y / size.y) - 1};
            // Native Outline selects edge cells before applying Frame/Fill.
            if ((mask & 4) && contains(q + Point2I{1, 0}) && contains(q + Point2I{-1, 0}) &&
                contains(q + Point2I{0, 1}) && contains(q + Point2I{0, -1})) {
                continue;
            }
            if (mask & 2) {
                s.fill(r, fill);
            }
            if (mask & 1) {
                s.rectangle(r, frame);
            }
        }
    };
    if (display.blockOutline) {
        const auto width = Int32(d->blocks.front().width);
        for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                          return y < d->grid.size.y;
                      })) {
            for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                              return x < d->grid.size.x;
                          })) {
                s.rectangle({p.x + Int32(Int64(x * width) * fields_.extent.x / size.x),
                             p.y + Int32(Int64(y * width) * fields_.extent.y / size.y),
                             p.x + Int32(Int64((x + 1) * width) * fields_.extent.x / size.x) - 1,
                             p.y + Int32(Int64((y + 1) * width) * fields_.extent.y / size.y) - 1},
                            display.blockFrameColor);
            }
        }
    }
    overlay(editor_->selection(),
            display.selection,
            display.selectionColor,
            display.selectionFrameColor);
    overlay(highlight_, display.highlight, display.highlightColor, display.highlightFrameColor);
    overlay(shadow_, display.shadow, display.shadowColor, display.shadowFrameColor);
    return drawChildren(s, p, clip);
}

bool TerrainView::projectedDraw(gfx::SoftwareSurface& destination, Point2I p, RectI clip) const {
    auto frame = projectedFrame();
    auto d = editor_->preview();
    if (!frame || !d) {
        return false;
    }
    auto surface = gfx::SoftwareSurface::create(fields_.extent.x, fields_.extent.y);
    if (!surface) {
        return false;
    }
    if (destination.palette() && !surface->setPalette(*destination.palette())) {
        return false;
    }
    surface->clear(0);
    if (!frame->draw(*surface)) {
        return false;
    }
    const auto& camera = *camera_;
    const auto square = float(1U << d->grid.scale);
    const auto view = terrainCamera(camera, fields_.extent);
    const auto polygon =
        [&](std::array<Point3F, 4> corners, UInt8 mask, UInt8 color, UInt8 border) -> bool {
        if (mask & 2) {
            std::array<world::StaticSurfaceTriangle, 2> triangles;
            for (auto i : std::views::iota(std::size_t{0}, std::size_t{2})) {
                for (auto j : std::views::iota(std::size_t{0}, std::size_t{3})) {
                    triangles[i].vertices[j].position =
                        corners[std::array<std::size_t, 6>{0, 2, 3, 0, 3, 1}[i * 3 + j]];
                }
            }
            auto materials = flatTerrainMaterials();
            for (auto& t : triangles) {
                t.material = color;
            }
            auto overlay =
                world::TsStaticShapeFrame::prepareSurfaces(std::move(materials), triangles, view);
            if (!overlay || !overlay->draw(*surface)) {
                return false;
            }
        }
        if (mask & 1) {
            for (auto edge : std::array<std::array<int, 2>, 4>{{{0, 1}, {1, 3}, {3, 2}, {2, 0}}}) {
                Point3F a, b;
                m_mul(corners[edge[0]], camera.worldToCamera, a);
                m_mul(corners[edge[1]], camera.worldToCamera, b);
                // Clip the complete segment against depth and four side planes
                // before integer projection, including near-plane crossings.
                const auto distance = [&](Point3F q, int plane) {
                    const float depth = camera.perspective ? q.y : 1;
                    switch (plane) {
                        case 0:
                            return q.y - camera.nearDistance;
                        case 1:
                            return camera.farDistance - q.y;
                        case 2:
                            return q.x * camera.focal.x + depth * camera.center.x;
                        case 3:
                            return -q.x * camera.focal.x +
                                   depth * (float(fields_.extent.x) - camera.center.x);
                        case 4:
                            return -q.z * camera.focal.y + depth * camera.center.y;
                        default:
                            return q.z * camera.focal.y +
                                   depth * (float(fields_.extent.y) - camera.center.y);
                    }
                };
                bool visible = true;
                for (auto plane : std::views::iota(0, 6)) {
                    const auto da = distance(a, plane), db = distance(b, plane);
                    if (da < 0 && db < 0) {
                        visible = false;
                        break;
                    }
                    if (da < 0 || db < 0) {
                        const auto point = a + (b - a) * (da / (da - db));
                        if (da < 0) {
                            a = point;
                        } else {
                            b = point;
                        }
                    }
                }
                if (!visible) {
                    continue;
                }
                const auto project = [&](Point3F q) {
                    const auto depth = camera.perspective ? q.y : 1;
                    return Point2I{Int32(camera.center.x + q.x * camera.focal.x / depth),
                                   Int32(camera.center.y - q.z * camera.focal.y / depth)};
                };
                surface->line(project(a), project(b), border);
            }
        }
        return true;
    };
    const auto overlay = [&](std::span<const Point2I> points,
                             UInt8 mask,
                             UInt8 color,
                             UInt8 border) {
        std::unordered_set<UInt64> set;
        const auto key = [](Point2I q) {
            return UInt64(UInt32(q.x)) << 32 | UInt32(q.y);
        };
        if (mask & 4) {
            for (auto q : points) {
                set.insert(key(q));
            }
        }
        for (auto q : points) {
            const auto cell = at(*d, q);
            if (!cell) {
                continue;
            }
            if ((mask & 4) && set.contains(key(q + Point2I{1, 0})) &&
                set.contains(key(q + Point2I{-1, 0})) && set.contains(key(q + Point2I{0, 1})) &&
                set.contains(key(q + Point2I{0, -1}))) {
                continue;
            }
            const auto& block = *cell->block;
            std::array<Point3F, 4> corners;
            for (auto i : std::views::iota(UInt32{0}, UInt32{4})) {
                corners[i] = {float(q.x + Int32(i & 1)) * square,
                              float(q.y + Int32(i >> 1)) * square,
                              block.heights[std::size_t(cell->y + (i >> 1)) * (block.width + 1) +
                                            cell->x + (i & 1)]};
            }
            if (!polygon(corners, mask, color, border)) {
                return false;
            }
        }
        return true;
    };
    if (display.grid) {
        const auto size = gridSize(*d);
        std::vector<Point2I> cells;
        for (auto y : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 y) {
                          return y < size.y;
                      })) {
            for (auto x : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 x) {
                              return x < size.x;
                          })) {
                cells.push_back({x, y});
            }
        }
        if (!overlay(cells, 1, 0, 249)) {
            return false;
        }
    }
    if (display.blockOutline) {
        const auto width = Int32(d->blocks.front().width);
        for (auto by : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 by) {
                           return by < d->grid.size.y;
                       })) {
            for (auto bx : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 bx) {
                               return bx < d->grid.size.x;
                           })) {
                const auto cell = at(*d, {bx * width, by * width});
                if (!cell) {
                    return false;
                }
                const auto& block = *cell->block;
                const auto point = [&](Int32 x, Int32 y) {
                    return Point3F{float(bx * width + x) * square,
                                   float(by * width + y) * square,
                                   block.heights[std::size_t(y) * (block.width + 1) + x]};
                };
                const auto edge = [&](Point3F a, Point3F b) {
                    return polygon({a, b, a, b}, 1, 0, display.blockFrameColor);
                };
                for (auto i : std::views::iota(Int32{0}) | std::views::take_while([&](Int32 i) {
                                  return i < width;
                              })) {
                    if (!edge(point(i, 0), point(i + 1, 0)) ||
                        !edge(point(i, width), point(i + 1, width)) ||
                        !edge(point(0, i), point(0, i + 1)) ||
                        !edge(point(width, i), point(width, i + 1))) {
                        return false;
                    }
                }
            }
        }
    }
    if (!overlay(editor_->selection(),
                 display.selection,
                 display.selectionColor,
                 display.selectionFrameColor) ||
        !overlay(
            highlight_, display.highlight, display.highlightColor, display.highlightFrameColor) ||
        !overlay(shadow_, display.shadow, display.shadowColor, display.shadowFrameColor) ||
        !destination.blit(surface->bitmap(), p)) {
        return false;
    }
    return drawChildren(destination, p, clip);
}

std::shared_ptr<const world::WorldSurfaceMesh>
TerrainView::surface(world::TsMaterialResources materials) const {
    auto d = editor_->preview();
    if (!d) {
        return {};
    }
    auto resources = d->resources(std::move(materials));
    if (!resources) {
        return {};
    }
    auto mesh = world::WorldSurfaceMesh::terrain(*resources, false);
    return mesh ? std::make_shared<const world::WorldSurfaceMesh>(std::move(*mesh)) : nullptr;
}

std::shared_ptr<const gfx::Bitmap> LandscapeView::snapshot() const {
    if (!editor_) {
        return {};
    }
    auto bitmap = editor_->landscape.bitmap(true);
    return bitmap ? std::make_shared<const gfx::Bitmap>(std::move(*bitmap)) : nullptr;
}

bool LandscapeView::draw(gfx::SoftwareSurface& s, Point2I p, RectI) const {
    auto bitmap = snapshot();
    return !bitmap ||
           s.stretch(*bitmap, {p.x, p.y, p.x + fields_.extent.x - 1, p.y + fields_.extent.y - 1});
}

TerrainTools::TerrainTools(std::shared_ptr<world::TerrainEditor> editor)
    : view(editor), preview(editor), editor_(std::move(editor)) {
    if (!editor_) {
        throw std::invalid_argument("terrain tools model");
    }
    ted.keyCommand = [this](const Event& e) {
        return shortcut(e);
    };
    if (!tedRoot_.addObject(view) || !landscapeRoot_.addObject(preview) ||
        !landscapeRoot_.addObject(script)) {
        throw std::runtime_error("terrain tools hierarchy");
    }
    script.maxBytes = 65536;
    script.multiline = true;
    const auto button = [&](Control& parent, std::string text, UInt32 message, Point2I p) {
        auto b = std::make_unique<Button>();
        b->text = std::move(text);
        b->message = message;
        configure(*b, p, {88, 24});
        if (!parent.addObject(*b)) {
            throw std::runtime_error("terrain toolbar");
        }
        buttons_.push_back(std::move(b));
    };
    button(tedRoot_, "Undo", 1, {0, 0});
    button(tedRoot_, "Redo", 2, {88, 0});
    button(tedRoot_, "Display", 3, {176, 0});
    button(landscapeRoot_, "Generate", 4, {0, 0});
    button(landscapeRoot_, "Apply", 5, {88, 0});
    button(landscapeRoot_, "Textures", 6, {176, 0});
    button(displayRoot_, "OK", 7, {16, 110});
    button(displayRoot_, "Cancel", 8, {116, 110});
    ted.onAction = [this](Control&, UInt32 message, std::string_view) {
        if (message >= 65536) {
            return uiMessage(message, ted);
        }
        if (message == 0) {
            return true;
        }
        if (message == 1) {
            return editor_->undo();
        }
        if (message == 2) {
            return editor_->redo();
        }
        if (message == 3) {
            ted.afterDispatch([this] {
                displayDialog();
            });
            return true;
        }
        if (message == 7) {
            auto next = displayDraft_.value_or(view.display);
            std::array<UInt8, 3> modes{};
            for (auto i : std::views::iota(std::size_t{0}, std::size_t{9})) {
                if (displayChecks_[i]->checked) {
                    modes[i / 3] |= UInt8(1U << (i % 3));
                }
            }
            next.selection = modes[0];
            next.highlight = modes[1];
            next.shadow = modes[2];
            next.blockOutline = displayChecks_[9]->checked;
            ted.afterDispatch([this, next] {
                acceptDisplay(next);
            });
            return true;
        }
        if (message == 8) {
            ted.afterDispatch([this] {
                cancelDialog();
            });
            return true;
        }
        if (message == 9) {
            ted.afterDispatch([this] {
                acceptDialog();
            });
            return true;
        }
        if (message == 10) {
            ted.afterDispatch([this] {
                cancelEditorDialog();
            });
            return true;
        }
        return false;
    };
    landscape.onAction = [this](Control&, UInt32 message, std::string_view) {
        if (message >= 65536) {
            return uiMessage(message, landscape);
        }
        if (message == 4) {
            return runScript(script.text);
        }
        if (message == 5) {
            return editor_->applyLandscape();
        }
        if (message == 6) {
            return editor_->applyTextures();
        }
        return false;
    };
    initializeUi();
    if (!resize(false, {640, 480}) || !resize(true, {640, 480}) || !ted.content(&tedRoot_) ||
        !landscape.content(&landscapeRoot_)) {
        throw std::runtime_error("terrain tools canvas");
    }
}

TerrainTools::~TerrainTools() {
    view.close();
    ted.content(nullptr);
    landscape.content(nullptr);
    ui_.reset();
}

bool TerrainTools::resize(bool ls, Point2I size) {
    if (size.x < 264 || size.y < 80 || size.x > 16384 || size.y > 16384) {
        return false;
    }
    if (ls) {
        const auto middle = size.x / 2;
        return configure(landscapeRoot_, {}, size) &&
               configure(script, {middle, 24}, {size.x - middle, size.y - 24}) &&
               configure(preview, {0, 24}, {middle, size.y - 24}) && resizeUi(ls, size);
    }
    return configure(tedRoot_, {}, size) && configure(view, {0, 24}, {size.x, size.y - 24}) &&
           resizeUi(ls, size);
}

bool TerrainTools::displayDialog() {
    if (ted.dialogCount()) {
        return false;
    }
    displayChecks_.clear();
    const auto modes =
        std::array{view.display.selection, view.display.highlight, view.display.shadow};
    for (auto row : std::views::iota(0, 3)) {
        for (auto bit : std::views::iota(0, 3)) {
            auto c = std::make_unique<Check>();
            c->checked = (modes[row] & (1 << bit)) != 0;
            c->text = std::array{"Frame", "Fill", "Outline"}[bit];
            c->font = c->highlightFont = font_;
            configure(*c, {16 + bit * 100, 16 + row * 30}, {96, 24});
            displayRoot_.addObject(*c);
            displayChecks_.push_back(std::move(c));
        }
    }
    auto block = std::make_unique<Check>();
    block->checked = view.display.blockOutline;
    block->text = "Block frame";
    block->font = block->highlightFont = font_;
    configure(*block, {216, 110}, {116, 24});
    displayRoot_.addObject(*block);
    displayChecks_.push_back(std::move(block));
    configure(displayRoot_, {8, 8}, {340, 240});
    if (!ted.pushDialog(displayRoot_)) {
        return false;
    }
    displayDraft_ = view.display;
    return true;
}

bool TerrainTools::acceptDisplay(const TerrainDisplayOptions& value) {
    if (ted.top() != &displayRoot_ || (value.selection | value.highlight | value.shadow) > 7) {
        return false;
    }
    if (!ted.popDialog()) {
        return false;
    }
    view.display = value;
    displayDraft_.reset();
    return true;
}

bool TerrainTools::cancelDialog() {
    if (uiOpen()) {
        return cancelUi();
    }
    if (dialog_) {
        return cancelEditorDialog();
    }
    if (ted.dialogCount() && ted.popDialog()) {
        displayDraft_.reset();
        return true;
    }
    return false;
}

std::optional<std::string> TerrainTools::command(std::string_view name,
                                                 std::span<const std::string> args) {
    const auto lower = [](std::string_view v) {
        std::string out(v);
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = char(c + 32);
            }
        }
        return out;
    };
    if (lower(name).starts_with("ted::")) {
        name.remove_prefix(5);
    }
    const auto a = lower(name);
    const auto yes = [](bool value) -> std::optional<std::string> {
        return value ? "True" : "False";
    };
    const auto byteVal = [](std::string_view v) -> std::optional<UInt8> {
        Int32 n{};
        const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
        return ec == std::errc{} && end == v.data() + v.size() && n >= 0 && n <= 255
                   ? std::optional(UInt8(n))
                   : std::nullopt;
    };
    const auto boolVal = [&](std::string_view v) -> std::optional<bool> {
        const auto s = lower(v);
        if (s == "true" || s == "t" || s == "1") {
            return true;
        }
        if (s == "false" || s == "f" || s == "0") {
            return false;
        }
        return {};
    };
    if (name == "dialogSettings") {
        return yes(args.empty() && dialog(TerrainDialog::Settings));
    }
    if (name == "dialogNew") {
        return yes(args.empty() && dialog(TerrainDialog::NewTerrain));
    }
    if (name == "dialogDisplay") {
        return yes(args.empty() && displayDialog());
    }
    if (a == "setselectframecolor" || a == "setselectfillcolor" || a == "sethilightframecolor" ||
        a == "sethilightfillcolor" || a == "setshadowframecolor" || a == "setshadowfillcolor" ||
        a == "setblockframecolor") {
        const auto c = args.size() == 1 ? byteVal(args[0]) : std::nullopt;
        if (!c || !editor_->snapshot() || ted.dispatching()) {
            return yes(false);
        }
        const auto apply = [&](TerrainDisplayOptions& d) {
            if (a == "setselectframecolor") {
                d.selectionFrameColor = *c;
            } else if (a == "setselectfillcolor") {
                d.selectionColor = *c;
            } else if (a == "sethilightframecolor") {
                d.highlightFrameColor = *c;
            } else if (a == "sethilightfillcolor") {
                d.highlightColor = *c;
            } else if (a == "setshadowframecolor") {
                d.shadowFrameColor = *c;
            } else if (a == "setshadowfillcolor") {
                d.shadowColor = *c;
            } else {
                d.blockFrameColor = *c;
            }
        };
        apply(view.display);
        if (displayDraft_) {
            apply(*displayDraft_);
        }
        return yes(true);
    }
    if (a == "setselectshow" || a == "sethilightshow" || a == "setshadowshow") {
        if (args.size() != 3 || !editor_->snapshot() || ted.dispatching()) {
            return yes(false);
        }
        const auto f = boolVal(args[0]), fi = boolVal(args[1]), o = boolVal(args[2]);
        if (!f || !fi || !o) {
            return yes(false);
        }
        const auto mask = UInt8((*f ? 1U : 0U) | (*fi ? 2U : 0U) | (*o ? 4U : 0U));
        const auto apply = [&](TerrainDisplayOptions& d) {
            (a == "setselectshow"    ? d.selection
             : a == "sethilightshow" ? d.highlight
                                     : d.shadow) = mask;
        };
        apply(view.display);
        if (displayDraft_) {
            apply(*displayDraft_);
        }
        return yes(true);
    }
    if (a == "setblockoutline") {
        const auto b = args.size() == 1 ? boolVal(args[0]) : std::nullopt;
        if (!b || !editor_->snapshot() || ted.dispatching()) {
            return yes(false);
        }
        view.display.blockOutline = *b;
        if (displayDraft_) {
            displayDraft_->blockOutline = *b;
        }
        return yes(true);
    }
    if (a == "getconsoleoptions" || a == "focus" || a == "unfocus" || a == "updatetoolbar") {
        return yes(args.empty() && bool(editor_->snapshot()));
    }
    if (a == "window") {
        return yes(args.size() <= 1 && (args.empty() || !args[0].empty()) &&
                   bool(editor_->snapshot()));
    }
    if (a == "setstatustext") {
        const auto pane = args.size() == 2 ? byteVal(args[0]) : std::nullopt;
        if (!pane || *pane >= status_.size() || !editor_->snapshot()) {
            return yes(false);
        }
        status_[*pane] = args[1];
        return yes(true);
    }
    if (a == "getworldname") {
        if (args.size() > 1 || !editor_->snapshot()) {
            return yes(false);
        }
        auto v = lower(args.empty() ? std::string_view(scriptPath_.generic_string())
                                    : std::string_view(args[0]));
        const auto pos = v.find("world");
        return pos != v.npos && pos > 0 ? std::optional<std::string>(v.substr(0, pos)) : yes(false);
    }
    if (a == "ls::editor") {
        return yes(args.size() <= 1 && (args.empty() || !args[0].empty()) &&
                   bool(editor_->snapshot()) && invoke(1043));
    }
    // Internal value-dialog route; original toolbar ID 706 retains its behavior.
    if (name == "forceTerrainType") {
        return args.empty() ? yes(valueDialog(65558)) : editor_->command(name, args);
    }
    if (name == "assignMatList") {
        return yes(args.size() == 1 && file(TerrainFileAction::LoadMaterials, args[0]));
    }
    constexpr std::array<std::pair<std::string_view, TerrainFileAction>, 13> files{
        {{"open", TerrainFileAction::Open},
         {"save", TerrainFileAction::Save},
         {"loadSelection", TerrainFileAction::LoadSelection},
         {"saveSelection", TerrainFileAction::SaveSelection},
         {"LS::loadScript", TerrainFileAction::LoadScript},
         {"LS::saveScript", TerrainFileAction::SaveScript},
         {"LS::Rules", TerrainFileAction::LoadRules},
         {"LS::saveRules", TerrainFileAction::SaveRules},
         {"LS::loadBitmap", TerrainFileAction::LoadBitmap},
         {"LS::saveBitmap", TerrainFileAction::SaveBitmap},
         {"loadPalette", TerrainFileAction::LoadPalette},
         {"loadMatList", TerrainFileAction::LoadMaterials},
         {"LS::Textures", TerrainFileAction::LoadTextures}}};
    for (auto [command, action] : files) {
        if (name == command) {
            return yes(args.empty()
                           ? fileDialog(action)
                           : (args.size() == 1 ||
                              (args.size() == 2 && action == TerrainFileAction::LoadTextures)) &&
                                 file(action, args[0], args.size() == 2 ? args[1] : std::string{}));
        }
    }
    return editor_->command(name, args);
}

void TerrainTools::font(std::shared_ptr<const gfx::BitmapFont> font) {
    font_ = font;
    for (auto& b : buttons_) {
        b->font = b->highlightFont = font;
    }
    for (auto& c : displayChecks_) {
        c->font = c->highlightFont = font;
    }
    for (auto& e : edits_) {
        e->font = e->highlightFont = font;
    }
    for (auto& l : labels_) {
        l->font = font;
    }
    for (auto& b : dialogButtons_) {
        b->font = b->highlightFont = font;
    }
    script.font = script.highlightFont = std::move(font);
    fontUi();
}
} // namespace opent1::gui
