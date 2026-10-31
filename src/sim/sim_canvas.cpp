#include <opent1/sim/sim_world_game.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace opent1::sim {
bool SimCanvas::onAdd() {
    if (!getManager()) {
        return false;
    }
    auto* set = dynamic_cast<SimCanvasSet*>(getManager()->findObject(CanvasSetId));
    if (!set || !set->addObject(*this)) {
        return false;
    }
    return setPalette(set->getPalette(), -1) && !isRemoved();
}

bool SimCanvas::setPalette(Resource<gfx::Palette> palette, Int32 transition) {
    if (transition < -1 || transition > 255 || isRemoved()) {
        return false;
    }
    palette_ = std::move(palette);
    transition_ = transition;
    const auto apply = [&] {
        paletteChanged();
    };
    if (getManager()) {
        getManager()->dispatch(apply);
    } else {
        apply();
    }
    return true;
}

bool SimCanvasSet::addObject(SimObject& object) {
    return dynamic_cast<SimCanvas*>(&object) && SimSet::addObject(object);
}

bool SimCanvasSet::setPalette(Resource<gfx::Palette> palette, Int32 transition) {
    if (transition < -1 || transition > 255 || isRemoved() || (palette && !palette->valid())) {
        return false;
    }
    if (palette.get() == palette_.get() && transition >= 0) {
        return bool(palette);
    }
    if (paletteRevision_ == std::numeric_limits<UInt64>::max()) {
        throw std::overflow_error("canvas palette revision exhausted");
    }
    palette_ = palette;
    const auto revision = ++paletteRevision_;
    const auto snapshot = objects_;
    const auto apply = [&] {
        std::ranges::for_each(snapshot, [&](auto* object) {
            if (!isRemoved() && revision == paletteRevision_ &&
                std::ranges::find(objects_, object) != objects_.end() && !object->isRemoved()) {
                static_cast<SimCanvas*>(object)->setPalette(palette, transition);
            }
        });
    };
    if (getManager()) {
        getManager()->dispatch(apply);
    } else {
        apply();
    }
    return bool(palette);
}

bool SimWorld::setPalette(std::string_view name, Int32 transition) {
    auto palette = SimResource::loadByName<gfx::Palette>(manager_, name);
    return palette && setPalette(std::move(palette), transition);
}

bool SimWorld::setPalette(Resource<gfx::Palette> palette, Int32 transition) {
    auto* set = dynamic_cast<SimCanvasSet*>(manager_.findObject(CanvasSetId));
    return set && set->setPalette(std::move(palette), transition);
}

Resource<gfx::Palette> SimWorld::getPalette() const {
    const auto* set = dynamic_cast<SimCanvasSet*>(manager_.findObject(CanvasSetId));
    return set ? set->getPalette() : Resource<gfx::Palette>{};
}

void SimWorld::delPalette() {
    if (auto* set = dynamic_cast<SimCanvasSet*>(manager_.findObject(CanvasSetId))) {
        set->setPalette({}, -1);
    }
}

void SimCanvasSet::render() {
    const auto snapshot = objects_;
    const auto draw = [&] {
        std::ranges::for_each(snapshot, [&](auto* object) {
            if (!isRemoved() && std::ranges::find(objects_, object) != objects_.end() &&
                !object->isRemoved()) {
                static_cast<SimCanvas*>(object)->renderFrame();
            }
        });
    };
    if (getManager()) {
        getManager()->dispatch(draw);
    } else {
        draw();
    }
}
}
