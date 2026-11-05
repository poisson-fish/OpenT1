# Test fixtures

These 19 files contain regression inputs and expected results. Default test
suites use the bundled records without a game installation. The records are
development inputs and are not needed by the game executable.

Fixtures include numeric and wire-format results, serialized test objects, GUI
pixel output, and short audio/video streams. Files whose names end in
`assets-v1.bin` do not all contain the same kind of data: the terrain and TS
asset inventories contain names, counts, checksums, and expected properties,
whereas the interior fixture embeds serialized objects. Do not infer contents
from the filename or `.bin` extension alone.

## Contents and consumers

Paths below are relative to `tests/`. Format versions refer to the fixture
container unless a payload version is explicitly named. Versioned containers
start with a four-byte tag and version. Some numeric corpora instead use a tag
followed directly by a record count.

| File | Contents | Consumer |
| --- | --- | --- |
| `audio-v1.bin` | AUD1 v1: spatial/lifecycle results, 16 WAV test streams, sample and streaming results | `audio/audio_test.cpp` |
| `game-player-v1.bin` | T1PL v1: player/control results and serialized update/persistence records | `game/player_test.cpp` |
| `game-psc-wire-v1.bin` | T1PC v1: player-control packet bytes and a demo record | `game/network_parity_test.cpp` |
| `game-weapons-v1.bin` | GWV1 v1: weapon/projectile results and packet records | `game/weapons_test.cpp` |
| `gui-canvas-v1.bin` | B7GC v1: 64 canvas histories, state/event traces, and 64-by-48 indexed test frames | `gui/canvas_test.cpp` |
| `gui-editor-failures-v1.bin` | B7EF v1: editor file-operation failure results | `gui/editor_test.cpp` |
| `gui-editor-v1.bin` | B7GE v2: 64 editor histories, control state, and run-length-encoded 160-by-100 test frames | `support/gui_editor_support.hpp`, used by `gui/editor_test.cpp` |
| `gui-routing-v1.bin` | B7WH v1: text-wrap routing/visibility results for 256 histories | `gui/routing_test.cpp` |
| `interior-assets-v1.bin` | O1TI v1: five serialized objects covering room geometry, shape, and three lighting cases | `world/mission_test.cpp`, `game/image_lighting_test.cpp` |
| `material-v1.dml` | One serialized TS material-list payload, version 1, with a filename-length test value | `world/ts_shape_test.cpp` |
| `smacker-v1.bin` | SMK1 v1: 14 short SMK2/SMK4 streams, each eight frames, plus expected frame/audio hashes | `video/video_test.cpp` |
| `software-2d-v1.bin` | Indexed pixel results for deterministic line and bitmap operations | `graphics/software_rasterizer_test.cpp` |
| `terrain-assets-v1.bin` | O1TA v1: 71 asset-name/property/checksum records; no terrain file payloads | Installation-check helper in `world/terrain_grid_test.cpp` |
| `terrain-grid-v1.bin` | O1TG v1: eight small serialized grid-block test bodies and expected height/material/light arrays | `world/terrain_grid_test.cpp` |
| `ts-assets-v1.bin` | O1TS v1: 475 asset-name, mesh-count, dependency, and checksum records; no shape or texture payloads | Installation-check helper in `world/ts_shape_test.cpp` |
| `ts-formats-v1.bin` | O1TF v1: expected sizes/checksums for 37 shapes constructed by the test helpers | `world/ts_shape_test.cpp` |
| `world-children-v1.bin` | O1HC v1: child-object update records and expected state | `world/mission_test.cpp` |
| `world-collision-v1.bin` | Collision query results for deterministic terrain, convex room, and shape test geometry | `world/collision_test.cpp` |
| `world-mission-v1.bin` | O1WM v1: mission-object update/persistence records and expected state | `world/mission_test.cpp` |

The default runner does not invoke the installation-check helpers. Those
helpers read actual volumes from a separately supplied installation and compare
them with inventory records. Bundled names and checksums do not provide the
corresponding game files.

## Integrity

The following sizes and SHA-256 digests cover the complete fixture files. Keep
them synchronized when a fixture changes, and describe any changed test contract
in the corresponding reader. Expected results should remain independent of the
implementation being checked; do not regenerate them from that implementation
merely to make a failing regression pass.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `audio-v1.bin` | 1,030,816 | `8e0344fc0d11df83979ba1f2a5180cf4416a4d6561d002654ad1dc8b20c964bc` |
| `game-player-v1.bin` | 1,371 | `ba653597e0388d9507eeb31cf57ffd59fbe21d9f9b9b3b23f6aa9ed98a7e70c0` |
| `game-psc-wire-v1.bin` | 123 | `0b405f6c557edf34aa3c1cc03ba355fa9233c59ba7c7b70bad294971f8c811a9` |
| `game-weapons-v1.bin` | 3,177 | `0789373cb664d33aae90ce60873601b426185747b7bf3beae23f52ff3b9cb05c` |
| `gui-canvas-v1.bin` | 9,921,108 | `08ec5258d8de7cf8d8e6142495a1c453afe8e29fb3261f28f683becc27d0f8de` |
| `gui-editor-failures-v1.bin` | 140 | `ea096af270775d97bb2d9541b1ff53faefe39878975665575e37e74df1e5aaea` |
| `gui-editor-v1.bin` | 24,469,324 | `ba9f6920c740bd9f448882311d7b5df279ef90cf53aa8b2380da17c8bd7c28f4` |
| `gui-routing-v1.bin` | 15,372 | `0e5e778702263cbd6aa4d9eb2bdf27db5f9b64eb4361235445c0b8eff226ed85` |
| `interior-assets-v1.bin` | 2,522 | `9b9cba1db8df90f9368ec399041f5a242685b35db324acbd1c71c82cc430f9bc` |
| `material-v1.dml` | 86 | `17a910e1c9df9a690723aff463fe5030fcde09b5d33c71a16924af2f76c3c7b1` |
| `smacker-v1.bin` | 103,729 | `90f00c7ad7fcbc5be80b5ed8fb638259e5bb136104c884e1495f3fb1e307dfaa` |
| `software-2d-v1.bin` | 338,192 | `d49e260fa970ffd9cd645436c403ad976db675fd6c22b0e9f2065e5fce6f2f27` |
| `terrain-assets-v1.bin` | 8,569 | `faf6f0462f9476458ec78064763b7c62d55902e2a4f0d3442da049fdaeb304f2` |
| `terrain-grid-v1.bin` | 3,784 | `ffde7dc48e221d3872bd22b58976ba8c4d6ea6c9abdc82cc665a2d30a24601fe` |
| `ts-assets-v1.bin` | 197,226 | `b510c4a0cbd897d82c85f508f5f092a17e681b9f958bc9c54b4af556b51fd9d3` |
| `ts-formats-v1.bin` | 308 | `f3e1b8c151eb221e34444416e8ed11f371278d71aca6810af0c27f94065570c4` |
| `world-children-v1.bin` | 64,524 | `c5d2899222d7b37c955bd0b6d2dbe48e20ed781e5f55f7acd7c87a437ae3817c` |
| `world-collision-v1.bin` | 35,904 | `98372b9e03dffdadba1889a60cfab54c31236a3168780e598cf06f814e84cc21` |
| `world-mission-v1.bin` | 92,342 | `b999c07334d22e159dd45bba597a5eaf369e41c8fc69683faa75953f9593a995` |
