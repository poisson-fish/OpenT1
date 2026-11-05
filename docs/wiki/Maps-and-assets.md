# Maps and assets

OpenT1 loads legacy game data through resource search directories and mounted VOL
archives. Missions refer to terrain, interiors, shapes, materials and other
resources by name. Keep names and dependencies consistent across platforms and
test from a clean copy of the mod rather than an installation containing unrelated
loose files that can hide missing resources.

## Edit a mission

Use a backed-up game/mod installation with its original editor scripts and GUI
resources. Host a local mission using those scripts, then use their F2/F3/F4/F5
editor workflow. Key maps come from the installation; custom mods can change them.
The editor supports scene picking/transforms, inspection, groups/prefabs, terrain
gestures and save/reload workflows. Save to your own mod and verify by restarting
the mission. Inspect `ME::LastSavedMission` after a successful save.

Terrain lighting has a supported native path through `ME::MissionLight(quick)`.
`ME::LightingStatus` reports `TerrainBaked` only after success. Existing authored
interior lightmaps are retained; this does not invoke a legacy offline interior
lighting compiler. See the
[editor audit](https://github.com/poisson-fish/OpenT1/blob/main/docs/editor-parity.md)
for tested workflows and remaining limits.

## Inspect or author resources

Binary packages include `opent1_asset_tool` (`.exe` on Windows). Run it with no
arguments to print usage. Commands include:

| Command | Purpose |
| --- | --- |
| `volume-list INPUT` | List a VOL archive |
| `volume-extract ARCHIVE ENTRY OUTPUT` | Extract one named entry |
| `volume-create OUTPUT` | Create a new volume |
| `volume-put ARCHIVE INPUT ENTRY [none\|rle\|lzh] [ID]` | Add/update an entry |
| `volume-remove ARCHIVE ENTRY` | Remove an entry |
| `palette-info INPUT` | Inspect a palette |
| `dml-make OUTPUT BITMAP...` | Construct a material list |
| `dts-convert INPUT OUTPUT` | Rewrite a supported shape |
| `dts-import TARGET SOURCE SEQUENCE OUTPUT` | Import an animation sequence |
| `interior-rewrite INPUT OUTPUT` | Rewrite a supported interior |

For example, `./opent1_asset_tool volume-list /path/to/example.vol` inspects an
archive without starting the game. Operations that write archives/assets should
use disposable copies first. These tools inspect/rewrite supported formats;
they are not general-purpose mesh/interior editors.

Ship all resources your original mission/mod requires and document its base-data
dependencies. Do not add original installation archives, missions, scripts, GUI
definitions, textures, models, sounds or videos to OpenT1 release packages.
See [game modding](Game-modding.md) for layout and distribution guidance.
