# Built-in map editor compatibility audit

## Viewport and terrain repair — 2026-11-04

The unchanged installed editor now has a working scene/input bridge. The shipped
F2/F3/F4/F5 workflow passes an automated installed-Raindance acceptance sequence,
including a second session after server replacement. This establishes the tested
workflow below; it does not claim exhaustive vanilla parity for every tool.

Implemented in this follow-up:

- The native `TSControl` renders the server scene through the client edit camera.
  Runtime-owned GUI graphs receive symbolic `postAction` commands even though
  they are not simulation-manager objects. Camera lookup retains its owning
  world independently of the rendered scene. Late player-control/GUI packets
  preserve the active editor. Server replacement clears stale scene snapshots.
- Viewport picking, projected grab handles, modifier selection, double-click
  inspection, movement, axis rotation, snapping, capture cancellation, and
  right-button camera look are connected. Downward LOS rays preserve endpoint
  order, and editor terrain queries include holes. Group transforms avoid
  repeated children; selection snapping preserves relative offsets.
- `TedObject` is created in the client, handles typed focus events, and cancels
  unfinished terrain gestures on focus loss. F4 uses live terrain LOS and the
  existing undoable gesture model. Selection, highlight, floating-paste shadow,
  and block overlays use the shipped display commands. `Ted::GetConsoleOptions`
  applies the terrain options panel's values. An empty mouse action disables
  that button, as required by `MEShowTed()`.
- `ME::ObjectToSC` and `ME::PasteFile` work. Prefab declarations are loaded through
  the active resource search path into a temporary group, then placed and
  selected in the server world. Failed imports preserve the prior selection.
  Command-map rebuilds temporarily suppress editor rendering and edit events.
- `ME::MissionLight(quick)` provides a supported native **terrain** lighting
  workflow: save, bake low-resolution or detailed terrain lighting, publish the
  result, and save again. Failures return `False`; `ME::LightingStatus` reports
  `TerrainBaked` only after success. Authored interior lightmaps are retained.
  This does not invoke the legacy offline interior-lighting compiler.
- Saved missions and terrain volumes mirror the selected resource directory
  inside the configured write root, including the active mod prefix. A search
  rescan makes new files discoverable. Remounting a changed archive refreshes its
  source while preserving overlapping client/server references and retained
  resource snapshots. `ME::LastSavedMission` reports the successful destination.

The `editor_acceptance/installed_editor_acceptance` case drives the installed
scripts and input maps through F2/F3/F4/F5, the shipped terrain-button callback,
a terrain mouse stroke, object picking/movement/undo, screen-center placement,
clipboard paste, file paste, deletion, save, quick lighting, return to play,
re-entry, and mission/server reload. The saved mission text is checked against
the exporter; reloaded transforms allow Euler/float round-trip precision and
exclude the Item `originalposition` field recomputed by mission rules. Terrain
heights and light samples compare exactly. Mode switching is exercised again
in the replacement server session. F4 rendering was also visually inspected.

Final validation passed all **16 Debug CTest entries**, the **six Release core,
console, simulation, world, GUI, and game suites**, and the installed editor
acceptance case in **both Debug and Release**. Regression coverage includes
archive replacement with overlapping owners and retained decoded resources,
viewport gestures/cancellation, terrain focus/options, and failed prefab imports.

Run the optional installed test with `OPENT1_TEST_ASSETS` set to the game root:

```powershell
build/x64-debug/tests/opent1_tests.exe --suite editor_acceptance
```

`OPENT1_EDITOR_SCREENSHOT` optionally names a BMP output for the F4 viewport.
The fixture writes and reloads disposable copies; installed assets are not
modified. Native desktop mouse capture and every terrain action/placement-mask
combination are outside this automated acceptance case. The editor scene is
currently rendered by the viewport's software path.

## First repair pass — 2026-11-04

The audit below records the original baseline. The first repair pass opened
the unchanged installed `gui/editor.gui` through `exec("editor.cs");`
after hosting Raindance. At that point this was partial compatibility; the
world viewport and interactive terrain path still needed integration.

Implemented in this repair pass:

- All nine persistent editor GUI classes and their native parent record formats.
  Editor fonts, popup commands and callbacks, popup-button selection, object-tree
  branches, recursive selection, modifier selection, and inspector fields/actions
  are connected. Inspector application uses the existing schema- and
  incarnation-checked inspection API. Controls retain world revisions and object
  generations instead of borrowing pointers across server replacement.
- Client-owned editor lookup with a separate server edit target. Creation,
  selection, clipboard paste, placement, and mission export target the server;
  bookmarks and camera operations use the client camera. `ME::Create` validates
  its canvas and replaces the previous editor. Server teardown invalidates and
  removes the editor, including worlds without a scheduler object.
- Signed decimal mask conversion; class-name registration and per-class
  placement masks; no-argument `CreateGroup`; selection callbacks; the singular
  inspector camera commands; terrain-grid sizing; and vanilla's explicitly
  unimplemented `ME::Redo` registration. Existing plural camera commands remain.
- Vanilla option names/defaults and creator drop modes, including preservation
  of object rotation for `DropAtCamera`. Clipboard paste retains copied world
  positions independently of the creator's drop mode, as vanilla does.
- Recursive undo snapshots with instance checks and one-shot consumption.
  Clearing selection preserves undo deletion notifications; reused object IDs
  cannot receive an old transform. Selecting a group and its child does not
  move the child twice or dereference it after group deletion.
- Shared typed focus events and editor edit-mode broadcasts. EditCamera's
  factory now resolves maps built by `move.cs` from `InputMaps`, and replacing
  its map removes the previous action-handler binding.
- No-argument terrain attachment against the server, `$TED::success` publication,
  and `$TED::currFile` referring to the enclosing terrain volume. Mission and
  terrain writes use the configured write root. The mission exporter handles
  the runtime palette subclass through its native `SimPalette` descriptor.
- Failed editor GUI loads clear `$ME::Loaded`, retain the previous GUI, and
  report failure. Popup callbacks may replace their GUI without continuing
  through a destroyed control.

Validation includes six new game cases in addition to the original editor
options case: console/world contracts and server replacement; undo/deletion/ID
reuse; focus and camera maps; all nine persistence round trips and popup/load
failure behavior; terrain status; and mission save/reload with failed-write
preservation. The game suite contains 46 cases. The compatibility fixture uses
an ephemeral server port so it can run beside a real dedicated server.

Final validation passed all **16 Debug CTest entries** and the **five Release
console, simulation, world, GUI, and game suites**. The installed GUI was also
rendered and visually checked; the inspector uses the native black field
background rather than the generic text editor's white background.

The installed-asset probe in `build/editor-audit/` confirms successful GUI
deserialization/startup, server-side group creation, signed masks, editor focus,
terrain attachment/status, and mission/terrain saves under its disposable write
root. Selecting Raindance's `Start1` populates three inspector fields and applies
them unchanged successfully. A terrain height edit through the model saves and
decodes with identical block heights, and the exported mission parses successfully.
Queued camera focus changes are checked after event delivery. A newly
created object must be resolved by ID or its `MissionGroup/...` path: root name
lookup does not search nested groups.

Work remaining after the first pass (superseded by the follow-up above):

1. Bind the editor viewport to the server scene and client camera; connect
   symbolic `postAction`, picking, drag/rotate gestures, and grab-handle rendering.
   Registering the GUI controls alone does not provide that scene/input bridge.
2. Complete `TedObject` initialization/focus, terrain display commands, and the
   F4 terrain UI. Terrain document/status support is available, but the shipped
   terrain mouse-editing workflow is not connected.
3. Implement `ME::ObjectToSC`, `ME::PasteFile`, and a supported
   `ME::MissionLight` workflow. Complete return-to-play/rebuild render suppression
   and verify all option/placement combinations against vanilla.
4. Run the complete interactive acceptance sequence at the end of this document,
   including F2/F3/F4/F5, saved mission/terrain reload comparisons, and a second
   interactive session after server replacement. Current checks do not establish
   that acceptance result.

## Original audit

Verified 2026-11-04 against:

- OpenT1 source revision `e74180feea0036a939773e9fa399302f20f7489f`.
- `../TribesRE/legacy/tribes-rebirth` revision
  `1105fd0890c19c13f816b91e51b9cf0658ffc63c`.
- The installed base game assets, hosting Raindance in an isolated
  `FearGameApp` with its write root under `build/editor-audit/runtime`.

**Result: the vanilla launch command is correct, but the integrated map editor
is not functional in OpenT1.** The first visible blocker is deserialization of
`gui/editor.gui`. Further independent gaps affect world ownership, input,
selection, inspection, placement, terrain editing, saving, and teardown.

This is a source and runtime audit. It does not change runtime implementation
or claim parity with a running vanilla executable. Installed game assets were
read externally and are not included in this document or the repository.

## Vanilla entry and mode switching

`program/exe/base/scripts/editor.cs:771` runs `ME::init()` only if
`focusServer()` succeeds. A local server must therefore exist before the first
`exec("editor.cs");`.

The initialization path is:

1. Initialize editor options and switch to the client manager.
2. Set `$ME::Loaded = true`; execute `move.cs` and build `editor.sae`.
3. Execute `med.cs`, which creates the client `editCamera` and calls
   `ME::Create(MainWindow)`.
4. Execute `ted.cs`, terrain/object registrations, and `editorconfig.cs`.
5. Call `ted()`, including `Ted::initTed()` and `Ted::attachToTerrain()`.
6. Call `MEMode()`, which loads `gui/editor.gui`, installs display roots for
   server world `1`, and enters inspector mode.
7. Inspector/creator mode focuses `MissionEditor` and `editCamera` and calls
   `postAction(EditorGui, Attach, editCamera)`.

References: vanilla `editor.cs:707`, `editor.cs:476`, `editor.cs:341`,
`med.cs:4`, and `move.cs:3`.

The installed `editor.cs`, `med.cs`, `move.cs`, and `editorconfig.cs` are
byte-identical to those in the reference tree.

Vanilla also guards initialization with `$ME::Loaded`. Re-executing
`editor.cs` after initialization does not itself reopen the UI. `MEMode();`
or F5 is the normal re-entry path; F2/F3/F4 select inspector/creator/terrain,
and `MEGameMode();` returns to play. This script behavior is distinct from
OpenT1's initial GUI load failure.

## Confirmed runtime failures

A diagnostic executable linked against freshly built Debug libraries loaded
Raindance and evaluated the installed scripts through the normal console.
The complete output and probe source are local in `build/editor-audit/`.

| Check | Observed result |
| --- | --- |
| Initialize app and load Raindance | Both succeed |
| Read `editor.cs`, `med.cs`, `move.cs`, `ted.cs`, `editorconfig.cs`, `RegisterObjects.cs`, `gui/editor.gui` | All found |
| `exec("editor.cs");` | Returns `True`, sets `$ME::Loaded = true`, leaves `LoadingGui` visible |
| Open `gui/editor.gui` directly | Fails; persistent loader reports `UnregisteredClassError` |
| Nine editor-specific GUI tags in that file | All present in the asset; all absent from the class registry |
| `MissionObjectList::ClearDisplayGroups`, `AddDisplayGroup`, `SetSelMode` during startup | Unknown commands |
| `ME::SetGrabMask(-8)` | Returns `True`, stores mask `0` |
| Editor's default add group | Client manager root, rather than the server's `MissionGroup` |
| `MissionCreateObject(AuditGroup, SimGroup)` | Creates `AuditGroup1` in the client world only |
| `ME::CreateGroup()` | Returns `False` |
| `ME::RegisterType(StaticShape, 8388608)` | Returns `False` because the class-name argument is parsed as a number |
| Deliver typed gain-focus event to `FearMissionEditor` | Unhandled |
| `unfocus(editCamera)` | Returns `True`; camera remains focused |
| `Ted::attachToTerrain()` | Returns `False` |
| `Ted::attachToTerrain("Terrain")` | Returns `True`; `$TED::success` remains `false`, `$TED::currFile` remains empty |
| `ME::Save()` in the editor's normal client focus | Returns `False` |
| Manually register the standalone object-list helper, then `MissionObjectList::SetSelMode(1)` | Still returns `False` |

The standard `game` suite also passes: **40 cases, 0 failures**. Those checks
do not exercise the installed editor startup path. The dedicated editor test
currently checks only a few console option flags.

## Findings by subsystem

### 1. GUI deserialization and inspector controls — blocking

The shipped editor GUI requires these vanilla classes and persistent tags:

| Class | Tag |
| --- | --- |
| `METextList` | `MetL` |
| `MEButton` | `MebT` |
| `MEPopup` | `MepL` |
| `MEPopupButton` | `Mepb` |
| `MECheck` | `MecK` |
| `MERadio` | `Merb` |
| `METextEdit` | `MeTe` |
| `MEInspectTagList` | `MeIT` |
| `MEObjectList` | `MeOL` |

Vanilla implements them in `program/code/missionObjectList.cpp:31` onward.
OpenT1 has no corresponding persistent GUI controls or registrations.
`src/gui/runtime.cpp:798` consequently cannot load the graph.

OpenT1's `game::MEObjectList` is a standalone model, not a GUI control.
Its `registerConsoleCommands` helper has no production caller. Generic
`gui::MissionView`/`MissionTree` and the native GUI layout editor do not supply
these vanilla controls. `Popup::*` and `PopupButton::makeCurrent` are also
missing from the game runtime.

### 2. Client/server ownership and editor lifecycle — blocking

Vanilla `program/code/FearMissionPlugin.cpp:63` always locates the editor in
the client manager, then explicitly sets its edit target to the server
manager. It validates the canvas, replaces an existing editor, and observes
the server's `ConsoleScheduler` for teardown.

OpenT1 `src/game/fearMissionPlugin.cpp:103` uses the currently focused manager
for lookup, creation, object creation, selection lookup, and saving.
`src/game/FearMissionEditor.cpp:287` and `:310` also set `targetManager_` to
the editor's owning manager. Since `med.cs` runs on the client, modifications
target the client. Switching console focus to the server loses the cached
editor instead of preserving a client editor with a server edit target.

`ME::Create` ignores its canvas argument and does not recreate an existing
editor or install the vanilla server teardown notification.

### 3. Console API contracts — incompatible

These ten vanilla command registrations are absent:

```text
ME::ObjectToCamera       ME::CameraToObject       ME::ObjectToSC
ME::onSelected           ME::onUnselected         ME::onSelectionCleared
ME::onUseTerrainGrid     ME::PasteFile            ME::MissionLight
ME::Redo
```

Vanilla's `ME::Redo` is itself an unimplemented branch; its missing
registration does not represent a working vanilla redo feature.

OpenT1's plural `ObjectsToCamera`/`CameraToObjects` and `SelectObject` names
do not satisfy the scripts' singular inspector commands and world/object
selection callbacks. The inspector commands also operate on the inspected
object in vanilla, so restoring names alone would not restore their contracts.

Other differences in `src/game/fearMissionPlugin.cpp`:

- `RegisterType` reverses the class-name and placement-mask arguments.
  Vanilla identifies the entry by class name; OpenT1 instead matches an object
  type bit mask. Its bitmap parameters also have different meanings.
- `SetPlaceMask(className, mask)` and `SetPlaceMask(className)` should update
  or remove a class entry. OpenT1 treats the first argument as a global mask.
- `CreateGroup()` should work without a name; OpenT1 requires one.
- The unsigned mask parser rejects negative decimal strings. The complement
  expression in vanilla `med.cs` produces `-68`, so startup also stores a zero
  grab mask. The direct `-8` probe independently confirms the parsing failure.

References: vanilla `FearMissionPlugin.cpp:13`, `:124`, `:155`, `:467`;
OpenT1 `fearMissionPlugin.cpp:53`, `:72`, `:165`, `:352`, `:373`.

### 4. Object tree, selection callbacks, and inspection — disconnected

Vanilla `missionObjectList.cpp:1105` installs the tree's command methods when
the GUI control is added. Selection events call the script callbacks in
`editor.cs:122`, which in turn update inspection state and call
`ME::onSelected(world, object)` and its counterparts.

OpenT1 has no UI that invokes this chain. Even manually installing the helper
in `src/game/MissionObjectList.cpp:224` leaves contract errors:

- `SetSelMode(1)` expects one argument in vanilla; the helper requires two.
- `ClearSelection` and `UnselectObject` are absent; `UnselectAll` is a
  different exported name.
- World arguments are ignored in favor of one captured manager.
- The helper changes selected flags without delivering the vanilla script
  callbacks or synchronizing `FearMissionEditor::selection()`.
- `Inspect(world, -1)` should clear the inspector; the helper instead tries
  to find an object named `-1` and fails.

### 5. Camera input, viewport, mouse editing, and handles — disconnected

Vanilla `FearMissionEditor.cpp:465`, `:905`, `:1203`, and `:1231` handle mouse,
focus, edit-mode broadcasts, and render queries. OpenT1's editor declares no
event/query overrides for those responsibilities. Its selection transform
helpers therefore do not provide interactive picking, dragging, modifiers,
or grab-handle rendering.

Additional independent integration gaps:

- `move.cs` builds `editor.sae` in `world::InputMaps`, but
  `EditCamera::setActionMap` loads from `SimResource` instead. The probe finds
  the map in `InputMaps` and no `editor.sae` resource. The gameplay input
  binder targets `PlayerPSC`; it does not bridge this map to the edit camera.
- `SimGame` sends a plain event with a focus type number
  (`src/sim/sim_world_game.cpp:935`). `EditCamera::processEvent` requires the
  typed focus event subclasses (`src/game/editCamera.cpp:304`). This explains
  the ineffective `unfocus(editCamera)` probe.
- Vanilla `postAction` accepts symbolic action names and object-name values
  (`darkstar/Sim/code/simGame.cpp:537`). OpenT1 requires a numeric action and
  numeric value (`src/sim/sim_world_game.cpp:1076`), so the script's
  `postAction(EditorGui, Attach, editCamera)` is incompatible independently
  of the missing GUI.
- The game host does not bind an editor viewport's world, camera, or mouse
  callback. Generic `TSControl` requires those connections; its available
  ray/picking helpers do not install them automatically.

### 6. Options, placement, and undo — partial model only

`src/game/FearMissionEditor.cpp:809` reads `ME::ShowHandles` and
`ME::DropToGround`, whereas vanilla uses `ME::ShowGrabHandles` and
`ME::ObjectsSnapToTerrain`. Defaults also differ, including rotation snap
(15 versus 90 degrees) and Z granularity (1 versus 0.001).

`onDrop` at `:640` implements only `DropAtCamera` and copies the complete
camera transform. Vanilla distinguishes position-only placement, placement
with camera rotation, below-camera terrain placement, screen-center placement,
and placement at the selected object. OpenT1 stores the other flags without
implementing those drop paths. Its drop-to-ground function uses one global
placement mask instead of vanilla's class-specific/default placement mask.

Undo saves only the directly selected objects that answer a transform query;
group children are not snapshotted. Restore resolves by ID without vanilla's
object-instance number check and does not consume the undo record. These are
source-level differences, not exercised by the launch probe.

### 7. Terrain editor initialization and UI — incompatible

Vanilla `darkstar/Ted/Code/tedPlugin.cpp:280` creates a focusable `TedObject`.
Its command wrapper publishes `$TED::success`; no-argument attach searches the
server for terrain, and successful attach sets `$TED::currFile`
(`simTed.cpp:3217`). The shipped `ted.cs` relies on those behaviors.

OpenT1 has a useful terrain document/editing model and live publication hooks
(`src/game/FearPlugin.cpp:1746`), but its vanilla adapter is incomplete:

- `initTed` returns success without creating `TedObject`
  (`src/world/terrain_editor.cpp:1661`).
- `attachToTerrain` requires an explicit name (`:1435`) and resolves against
  the currently focused world. Vanilla initialization supplies no argument
  while the client is focused.
- Command installation at `:2036` forwards return values without maintaining
  `$TED::success` or the current filename. A successful explicit attach in
  the probe leaves both script variables unusable.
- The selection/highlight/shadow display setters called by the installed
  script are missing, including `setSelectShow`, `setHilightShow`,
  `setShadowShow`, their frame/fill color setters, and `setBlockOutline`.
- The standalone `TerrainView`/`TerrainTools` controls are not connected to
  the built-in editor's F4 mode, popups, or `TedObject` focus path.

### 8. Saving, lighting, exit, and reload — incomplete

`ME::Save` at `src/game/fearMissionPlugin.cpp:228` searches the focused
manager for `MissionGroup`, so it fails in the normal client editor context.
If a terrain filename is supplied, it also requires `$TED::success` from a
terrain command wrapper that never publishes that status.

The mission exporter and temporary-file replacement code already exist.
However, the destination is constructed directly from `$missionFile`,
bypassing the installation/write-root resolution used by other file commands.
Vanilla resolves the mission resource's full physical path. This path behavior
was inspected in source; no installed mission was overwritten during the audit.

`ME::MissionLight` is absent. Vanilla saves first, invokes the external
`missionlighting.exe`, and reloads the mission. An implementation needs an
explicit supported lighting workflow rather than a success stub.

OpenT1's `rebuildCommandMap` editor method omits vanilla's temporary suppression
of editor render flags and server edit-mode broadcasts. Since editor focus and
terrain focus are also disconnected, the return-to-play and reload lifecycle
cannot yet be considered verified editor behavior.

## Repair and acceptance order

1. Implement and register the nine persistent GUI controls and their command
   methods. Load the unchanged installed `gui/editor.gui` successfully.
2. Keep the editor/camera/UI in the client world while resolving edit targets,
   display roots, terrain, and saves in the server world. Validate teardown
   against world replacement and object deletion.
3. Restore vanilla console names, argument shapes, signed mask conversion,
   class placement entries, and selection/inspection callbacks.
4. Connect camera input maps, typed focus events, symbolic `postAction`, the
   viewport's server scene/camera, and mouse/render editor events.
5. Complete terrain initialization/status variables and the shipped terrain UI
   command surface; implement placement/options/undo contracts.
6. Verify save/reload and supported lighting on disposable copies of missions.

Acceptance should execute the installed `editor.cs` after hosting Raindance,
then exercise F2/F3/F4/F5; pick, inspect, create, move, rotate, copy/paste,
delete and undo an object; edit terrain; save; reload; and compare the server
mission and terrain data. Include a second editor session after server
replacement and a failure test that cannot report a successful editor load
when GUI deserialization fails.
