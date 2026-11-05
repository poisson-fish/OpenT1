#pragma once

#include <opent1/gui/runtime.hpp>

namespace opent1::game {
// Native editor controls persist exactly their SimGui parent records. Runtime
// services and world references are installed only after resource preparation.
bool registerMissionEditorGuiClasses(Persistent::ClassRegistry&);
bool bindMissionEditorGuiControl(gui::Control&, sim::SimGame&, ResourceManager&);
void installMissionEditorGuiCommands(Console&, const std::shared_ptr<gui::Runtime>&, sim::SimGame&);
}

namespace opent1::world { class TerrainEditor; }
namespace opent1::game {
void installTerrainEditorFocus(sim::SimGame&, const std::shared_ptr<world::TerrainEditor>&);
void bindMissionEditorViewport(gui::Control&, sim::SimGame&, const std::shared_ptr<world::TerrainEditor>&);
}
