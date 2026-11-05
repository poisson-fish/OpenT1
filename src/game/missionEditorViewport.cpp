#include <opent1/game/mission_editor_controls.hpp>
#include <opent1/game/fear_mission_editor.hpp>
#include <opent1/world/terrain_editor.hpp>
#include <opent1/world/world_mission.hpp>
#include <opent1/core/text.hpp>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <set>

namespace opent1::game {
namespace {
class TedFocus final : public sim::SimObject {
public:
    explicit TedFocus(std::weak_ptr<world::TerrainEditor> model) : model_(std::move(model)) {}

    bool processEvent(const sim::SimEvent& event) override {
        const bool gain = dynamic_cast<const sim::SimGainFocusEvent*>(&event) != nullptr;
        const bool lose = dynamic_cast<const sim::SimLoseFocusEvent*>(&event) != nullptr;
        if (auto model = model_.lock(); model && (gain || lose)) {
            model->focused = gain;
            if (lose)
                model->gesture({}, world::TerrainGesture::Cancel, {});
            return true;
        }
        return SimObject::processEvent(event);
    }

    void onRemove() override {
        if (auto model = model_.lock()) {
            model->focused = false;
            model->gesture({}, world::TerrainGesture::Cancel, {});
        }
        SimObject::onRemove();
    }

private:
    std::weak_ptr<world::TerrainEditor> model_;
};

struct EditorViewport {
    gui::TSControl& view;
    sim::SimGame& game;
    std::weak_ptr<const void> gameLife;
    std::shared_ptr<world::TerrainEditor> terrain;
    bool dragging{}, terrainDrag{}, changed{}, looking{}, toggleOnRelease{};
    UInt8 button{};
    UInt64 revision{}, editorNumber{};
    Point2I lastMouse{}, cell{}, hover{};
    bool hasHover{};
    Point3F anchor{}, lastPlane{}, placeOffset{};
    float rotationSum{}, rotationApplied{};
    std::string action{};

    FearMissionEditor* editor() const {
        auto* client = !gameLife.expired() ? game.world(sim::WorldType::Client) : nullptr;
        return client ? dynamic_cast<FearMissionEditor*>(
                            client->manager().findObject(sim::MissionEditorId))
                      : nullptr;
    }

    sim::SimWorld* scene() const {
        auto* me = editor();
        return me && me->targetManager() ? game.world(sim::WorldType::Server) : nullptr;
    }

    float option(std::string_view name, float fallback) const {
        const auto text = game.console().getVariable(name);
        char* end{};
        const float value = std::strtof(text.c_str(), &end);
        return !text.empty() && end == text.c_str() + text.size() && std::isfinite(value)
                   ? value
                   : fallback;
    }

    std::optional<Point2I> project(Point3F position) const {
        const auto& camera = view.cameraView();
        if (!camera)
            return {};
        Point3F p;
        m_mul(position, camera->worldToCamera, p);
        if (p.y < camera->nearDistance || p.y > camera->farDistance)
            return {};
        const auto depth = camera->perspective ? p.y : 1.f;
        const float x = camera->center.x + p.x * camera->focal.x / depth;
        const float y = camera->center.y - p.z * camera->focal.y / depth;
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 16000 || std::abs(y) > 16000)
            return {};
        return view.localToGlobal({Int32(std::lround(x)), Int32(std::lround(y))});
    }

    template <class F> void objects(F&& f) const {
        auto* me = editor();
        auto* root =
            me && me->targetManager() ? me->targetManager()->findObject("MissionGroup") : nullptr;
        const auto visit = [&](auto&& self, sim::SimObject* object) -> void {
            if (!object)
                return;
            sim::SimObjectTransformQuery transform;
            if (object->processQuery(transform) && (object->getType() & me->grabMask()))
                f(*object, transform.transform);
            if (auto* group = dynamic_cast<sim::SimGroup*>(object))
                for (auto* child : group->objects())
                    self(self, child);
        };
        visit(visit, root);
    }

    std::optional<Point3F>
    hit(const gui::Event& event, UInt32 mask, sim::SimObjectId exclude = 0) const {
        auto* world = scene();
        const auto ray = view.ray(event);
        auto* root = world ? dynamic_cast<sim::SimContainer*>(
                                 world->manager().findObject(sim::RootContainerId))
                           : nullptr;
        if (!root || !ray)
            return {};
        sim::SimContainerQuery query;
        query.id = exclude;
        query.mask = mask;
        query.box.fMin = ray->origin;
        query.box.fMax = ray->origin + ray->direction * view.cameraView()->farDistance;
        sim::SimCollisionInfo collision;
        if (!root->findLOS(query, collision, sim::SimCollisionImageQuery::Editor) ||
            collision.surfaces.surfaces.empty())
            return {};
        Point3F p;
        m_mul(collision.surfaces.surfaces.front().position, collision.surfaces.tWorld, p);
        return p;
    }

    sim::SimObject* pick(const gui::Event& event) const {
        auto* world = scene();
        auto* me = editor();
        if (!world || !me)
            return nullptr;
        const auto collision = view.pick(*world, event, me->grabMask());
        auto* result = collision ? collision->object.resolve(world->manager()) : nullptr;
        float nearest = std::numeric_limits<float>::max();
        if (me->flags() & FearMissionEditor::ShowHandles)
            objects([&](sim::SimObject& object, const TMat3F& t) {
                const auto p = project(t.p);
                if (!p || std::abs(p->x - event.point.x) > 6 || std::abs(p->y - event.point.y) > 6)
                    return;
                Point3F camera;
                m_mul(t.p, view.cameraView()->worldToCamera, camera);
                if (camera.y < nearest) {
                    nearest = camera.y;
                    result = &object;
                }
            });
        return result;
    }

    void tree(std::string_view command, sim::SimObject* object = nullptr) {
        auto& console = game.console();
        const auto name = "MissionObjectList::" + std::string(command);
        if (!console.isFunction(name))
            return;
        if (object)
            (void)console.execute(name,
                                  std::array<std::string, 2>{"1", std::to_string(object->getId())});
        else
            (void)console.execute(name);
    }

    std::optional<Point3F> plane(const gui::Event& event) const {
        const auto ray = view.ray(event);
        if (!ray || std::abs(ray->direction.z) < 1.e-6f)
            return {};
        const auto distance = (anchor.z - ray->origin.z) / ray->direction.z;
        return distance >= 0 ? std::optional(ray->origin + ray->direction * distance)
                             : std::nullopt;
    }

    std::optional<float> vertical(const gui::Event& event) const {
        const auto ray = view.ray(event);
        if (!ray)
            return {};
        const auto normal = ray->origin - anchor;
        const auto divisor = normal.x * ray->direction.x + normal.y * ray->direction.y;
        if (std::abs(divisor) < 1.e-6f)
            return {};
        const auto distance =
            (normal.x * (anchor.x - ray->origin.x) + normal.y * (anchor.y - ray->origin.y)) /
            divisor;
        return distance >= 0 ? std::optional(ray->origin.z + ray->direction.z * distance)
                             : std::nullopt;
    }

    std::optional<Point2I> terrainCell(const gui::Event& event) const {
        auto* world = scene();
        auto* object = world ? dynamic_cast<world::SimMissionTerrain*>(
                                   world->manager().findObject(sim::TerrainId))
                             : nullptr;
        const auto p = hit(event, sim::TerrainObjectType);
        const auto document = terrain->snapshot();
        if (!p || !object || !document || document->blocks.empty())
            return {};
        const auto& s = object->state();
        TMat3F inverse(EulerF(s.rotation.x, s.rotation.y, s.rotation.z), s.position);
        inverse.inverse();
        Point3F local;
        m_mul(*p, inverse, local);
        const float square = std::ldexp(1.f, document->grid.scale);
        const Point2I point{Int32(std::floor(local.x / square)),
                            Int32(std::floor(local.y / square))};
        const auto width = Int32(document->blocks.front().width);
        return point.x >= 0 && point.y >= 0 && point.x < document->grid.size.x * width &&
                       point.y < document->grid.size.y * width
                   ? std::optional(point)
                   : std::nullopt;
    }

    void cancel(bool restore = true) {
        auto* me = editor();
        if (dragging && terrainDrag)
            terrain->gesture(action, world::TerrainGesture::Cancel, {});
        else if (dragging && changed && restore && me && me->getNumber() == editorNumber &&
                 game.worldRevision(sim::WorldType::Server) == revision)
            me->restoreInfo();
        dragging = terrainDrag = changed = false;
        looking = false;
        if (!gameLife.expired())
            game.console().setVariable("ME::MouseLook", "False");
        if (view.canvas() && view.canvas()->capture() == &view)
            view.canvas()->unlockMouse();
    }

    bool input(const gui::Event& e) {
        auto* me = editor();
        if (!me || !scene()) {
            cancel(false);
            return false;
        }
        const bool ted = terrain->focused;
        if (dragging && (revision != game.worldRevision(sim::WorldType::Server) ||
                         editorNumber != me->getNumber() || (terrainDrag ? !ted : !me->focused())))
            cancel();
        if (e.type == gui::EventType::Cancel ||
            (e.type == gui::EventType::KeyDown && e.key == gui::Key::Escape)) {
            if (!dragging && !looking)
                return false;
            cancel();
            return true;
        }
        if (!ted && !me->focused())
            return false;
        if (looking && e.type == gui::EventType::Up && e.button == 3) {
            cancel(false);
            return true;
        }
        if (e.type == gui::EventType::Move && ted) {
            const auto point = terrainCell(e);
            hasHover = bool(point);
            if (point)
                hover = *point;
            return true;
        }
        if (e.type == gui::EventType::Down && (e.button == 1 || e.button == 3)) {
            cancel();
            me->getConsoleOptions();
            if (e.button == 3 && (!ted || terrain->settings().rightAction >=
                                              world::TerrainEditor::actions().size())) {
                looking = true;
                game.console().setVariable("ME::MouseLook", "True");
                if (view.canvas()) {
                    (void)view.canvas()->makeFirstResponder(&view);
                    (void)view.canvas()->lockMouse(&view);
                }
                return true;
            }
            if (ted) {
                const auto point = terrainCell(e);
                const auto index = e.button == 1 ? terrain->settings().leftAction
                                                 : terrain->settings().rightAction;
                if (!point || index >= world::TerrainEditor::actions().size())
                    return false;
                action = world::TerrainEditor::actions()[index];
                if (!terrain->gesture(action, world::TerrainGesture::Begin, *point))
                    return false;
                cell = hover = *point;
                hasHover = true;
                terrainDrag = true;
            } else {
                auto* object = pick(e);
                if (!object) {
                    tree("ClearSelection");
                    me->selection().clearSet();
                    return true;
                }
                const bool selected = std::ranges::find(me->selection().objects(), object) !=
                                      me->selection().objects().end();
                toggleOnRelease = selected && e.shift;
                if (!selected && !e.shift) {
                    tree("ClearSelection");
                    me->selection().clearSet();
                }
                if (!selected) {
                    tree("SelectObject", object);
                    me->selection().addObject(*object);
                }
                if (e.clicks >= 2) {
                    tree("ExpandToObject", object);
                    tree("Inspect", object);
                }
                if (me->selection().objectLocked())
                    return true;
                me->selection().calcCentroid();
                anchor = me->selection().centroid();
                lastPlane = plane(e).value_or(anchor);
                const auto surface = hit(e, me->defaultPlaceMask(), object->getId());
                placeOffset = anchor - surface.value_or(anchor);
                rotationSum = rotationApplied = 0;
            }
            if (!view.canvas() || !view.canvas()->lockMouse(&view)) {
                if (terrainDrag)
                    terrain->gesture(action, world::TerrainGesture::Cancel, {});
                terrainDrag = false;
                return false;
            }
            (void)view.canvas()->makeFirstResponder(&view);
            dragging = true;
            changed = false;
            button = e.button;
            lastMouse = e.point;
            revision = game.worldRevision(sim::WorldType::Server);
            editorNumber = me->getNumber();
            return true;
        }
        if (e.type == gui::EventType::Drag && dragging) {
            if (terrainDrag) {
                const auto point = terrainCell(e);
                if (point) {
                    cell = hover = *point;
                    if (!terrain->gesture(
                            action, world::TerrainGesture::Update, cell, e.point.y - lastMouse.y)) {
                        cancel();
                        return true;
                    }
                }
            } else if (e.point != lastMouse) {
                if (!changed)
                    me->saveInfo();
                if (e.shift || e.alt) {
                    rotationSum +=
                        float(e.point.x - lastMouse.x) * option("ME::RotateSensitivity", .02f);
                    float target = rotationSum;
                    if ((me->flags() & FearMissionEditor::SnapRotations) &&
                        me->rotationSnap() > 0) {
                        const auto step = me->rotationSnap() * std::numbers::pi_v<float> / 180;
                        target -= std::fmod(target, step);
                    }
                    const auto axes = (me->flags() & FearMissionEditor::SnapRotations) ? 0U
                                      : e.alt ? FearMissionEditor::RotateY
                                              : FearMissionEditor::RotateZ;
                    me->rotateObject(&me->selection(), anchor, target - rotationApplied, axes);
                    rotationApplied = target;
                } else {
                    Point3F delta{};
                    if (e.control) {
                        if (me->flags() & FearMissionEditor::UsePlaneMovement) {
                            auto previous = e;
                            previous.point = lastMouse;
                            const auto a = vertical(previous), b = vertical(e);
                            if (a && b)
                                delta.z = *b - *a;
                        } else
                            delta.z = -float(e.point.y - lastMouse.y) *
                                      option("ME::MoveSensitivity", .2f);
                    } else if (me->flags() & FearMissionEditor::UsePlaneMovement) {
                        if (const auto point = plane(e))
                            delta = *point - lastPlane;
                    } else {
                        const auto selected = me->selection().objects();
                        const auto mask = selected.size() == 1 ? me->placementMask(selected.front())
                                                               : me->defaultPlaceMask();
                        const auto point =
                            hit(e, mask, selected.empty() ? 0 : selected.front()->getId());
                        if (point)
                            delta = *point + placeOffset - me->selection().centroid();
                    }
                    const auto before = me->selection().centroid();
                    me->moveObjectRelative(&me->selection(), delta);
                    lastPlane += me->selection().centroid() - before;
                    if (!e.control && !(me->flags() & FearMissionEditor::UsePlaneMovement) &&
                        (me->flags() & FearMissionEditor::DropToGround))
                        me->dropObjectDown(&me->selection());
                }
                changed = true;
            }
            lastMouse = e.point;
            return true;
        }
        if (e.type == gui::EventType::Up && dragging && e.button == button) {
            if (terrainDrag) {
                if (const auto point = terrainCell(e))
                    cell = *point;
                if (!terrain->gesture(action, world::TerrainGesture::End, cell))
                    terrain->gesture(action, world::TerrainGesture::Cancel, {});
            } else if (!changed && toggleOnRelease && e.shift) {
                if (auto* object = pick(e)) {
                    tree("UnselectObject", object);
                    me->selection().removeObject(*object);
                }
            }
            dragging = terrainDrag = changed = false;
            if (view.canvas())
                view.canvas()->unlockMouse();
            return true;
        }
        return false;
    }

    bool draw(gfx::SoftwareSurface& surface, Point2I, gui::RectI) {
        auto* me = editor();
        if (!me || !scene()) {
            cancel(false);
            return true;
        }
        if (dragging && (terrainDrag ? !terrain->focused : !me->focused()))
            cancel();
        if (me->focused() && (me->flags() & FearMissionEditor::ShowHandles))
            objects([&](sim::SimObject& object, const TMat3F& transform) {
                const auto point = project(transform.p);
                if (!point)
                    return;
                const auto color = UInt8(object.isLocked() ? 249 : object.isSelected() ? 255 : 254);
                surface.rectangle({point->x - 4, point->y - 4, point->x + 4, point->y + 4}, color);
                surface.line({point->x - 6, point->y}, {point->x + 6, point->y}, color);
                surface.line({point->x, point->y - 6}, {point->x, point->y + 6}, color);
            });
        if (!terrain->focused)
            return true;
        auto document = terrain->preview();
        auto* object =
            dynamic_cast<world::SimMissionTerrain*>(scene()->manager().findObject(sim::TerrainId));
        if (!document || document->blocks.empty() || !object)
            return true;
        const auto& state = object->state();
        const TMat3F transform(EulerF(state.rotation.x, state.rotation.y, state.rotation.z),
                               state.position);
        const auto width = Int32(document->blocks.front().width);
        const auto vertex = [&](Point2I p) -> std::optional<Point2I> {
            const auto bx = std::clamp(p.x / width, 0, document->grid.size.x - 1);
            const auto by = std::clamp(p.y / width, 0, document->grid.size.y - 1);
            if (p.x < 0 || p.y < 0 || p.x > width * document->grid.size.x ||
                p.y > width * document->grid.size.y)
                return {};
            const auto id = document->grid.blockMap[std::size_t(by) * document->grid.size.x + bx];
            const auto entry =
                std::ranges::find(document->grid.blocks, id, &world::TerrainGridBlockEntry::id);
            if (entry == document->grid.blocks.end())
                return {};
            const auto& block =
                document->blocks[std::size_t(entry - document->grid.blocks.begin())];
            const auto h =
                block.heights[std::size_t(p.y - by * width) * (width + 1) + p.x - bx * width];
            Point3F world;
            const auto square = std::ldexp(1.f, document->grid.scale);
            m_mul(Point3F{p.x * square, p.y * square, h + .05f}, transform, world);
            return project(world);
        };
        const auto cells = [&](std::span<const Point2I> points,
                               const world::TerrainEditorDisplay& display) {
            if (!display.show)
                return;
            std::set<std::pair<Int32, Int32>> region;
            if ((display.show & 4) && !(display.show & 1))
                for (const auto p : points)
                    region.emplace(p.x, p.y);
            for (const auto p : points) {
                const std::array corners{vertex(p),
                                         vertex({p.x + 1, p.y}),
                                         vertex({p.x + 1, p.y + 1}),
                                         vertex({p.x, p.y + 1})};
                if (std::ranges::any_of(corners, [](auto p) {
                        return !p;
                    }))
                    continue;
                if (display.show & 2) {
                    std::array<gfx::PolygonVertex, 4> polygon{};
                    for (std::size_t i = 0; i < 4; ++i)
                        polygon[i].position = {float(corners[i]->x), float(corners[i]->y)};
                    gfx::PolygonMaterial material;
                    material.color = display.fillColor;
                    surface.polygon(polygon, material);
                }
                const std::array neighbors{Point2I{p.x, p.y - 1},
                                           Point2I{p.x + 1, p.y},
                                           Point2I{p.x, p.y + 1},
                                           Point2I{p.x - 1, p.y}};
                for (std::size_t i = 0; i < 4; ++i)
                    if ((display.show & 1) ||
                        ((display.show & 4) && !region.contains({neighbors[i].x, neighbors[i].y})))
                        surface.line(*corners[i], *corners[(i + 1) % 4], display.frameColor);
            }
        };
        cells(terrain->selection(), terrain->selectionDisplay);
        if (hasHover) {
            std::vector<Point2I> brush;
            const auto size = Int32(1U << terrain->settings().brushDetail);
            auto start = hover;
            if (terrain->settings().snap) {
                start.x &= ~(size - 1);
                start.y &= ~(size - 1);
            }
            for (Int32 y = 0; y < size; ++y)
                for (Int32 x = 0; x < size; ++x)
                    brush.push_back({start.x + x, start.y + y});
            cells(brush, terrain->highlightDisplay);
            if (terrain->floating()) {
                std::vector<Point2I> shadow;
                for (const auto& entry : terrain->clipboard())
                    shadow.push_back({hover.x + entry.point.x, hover.y + entry.point.y});
                cells(shadow, terrain->shadowDisplay);
            }
        }
        if (terrain->blockOutline) {
            for (Int32 y = 0; y < document->grid.size.y; ++y)
                for (Int32 x = 0; x < document->grid.size.x; ++x) {
                    const Point2I p{x * width, y * width};
                    for (Int32 i = 0; i < width; ++i) {
                        for (const auto& edge :
                             {std::pair{Point2I{p.x + i, p.y}, Point2I{p.x + i + 1, p.y}},
                              std::pair{Point2I{p.x, p.y + i}, Point2I{p.x, p.y + i + 1}}}) {
                            const auto a = vertex(edge.first), b = vertex(edge.second);
                            if (a && b)
                                surface.line(*a, *b, terrain->selectionDisplay.frameColor);
                        }
                    }
                }
        }
        return true;
    }
};
}

void installTerrainEditorFocus(sim::SimGame& game,
                               const std::shared_ptr<world::TerrainEditor>& model) {
    const auto life = game.lifetime();
    model->initialize = [&game, life, weak = std::weak_ptr(model)] {
        auto* client = !life.expired() ? game.world(sim::WorldType::Client) : nullptr;
        if (!client || weak.expired())
            return false;
        if (auto* object = client->manager().findObject("TedObject"))
            return dynamic_cast<TedFocus*>(object) != nullptr;
        auto* object = client->manager().addObject(std::make_unique<TedFocus>(weak));
        if (!object)
            return false;
        object->assignName("TedObject");
        return true;
    };
}

void bindMissionEditorViewport(gui::Control& control,
                               sim::SimGame& game,
                               const std::shared_ptr<world::TerrainEditor>& model) {
    auto* root = &control;
    while (root->parent())
        root = root->parent();
    auto* view = dynamic_cast<gui::TSControl*>(root);
    // Runtime names a loaded root after resource preparation. The native object
    // tree is also a reliable discriminator while the graph is still unnamed.
    if (!view || !model || view->sceneWorld ||
        (!equalIgnoreCase(root->getName(), "EditorGui") && !root->findNamed("MissionObjectList")))
        return;
    auto state =
        std::make_shared<EditorViewport>(EditorViewport{*view, game, game.lifetime(), model});
    view->sceneWorld = [state] {
        auto* scene = state->scene();
        if (!scene)
            state->cancel(false);
        return scene;
    };
    view->input = [state](const gui::Event& event) {
        return state->input(event);
    };
    view->overlay = [state](gfx::SoftwareSurface& surface, Point2I p, gui::RectI clip) {
        return state->draw(surface, p, clip);
    };
}
}
