# Render improvements: measurements and implementation plan

Measured 2026-11-01 on branch `render-improvements`, starting from `6ad5ea7`.
The machine-readable [baseline](data/render-memory-baseline.json) contains
per-mission results, catalog breakdowns, failures, and the capture executable hash.

The measurements support retaining the complete **compact mesh and terrain source
data for the current mission in CPU RAM**. Texture residency, generated meshes,
collision snapshots, and upload buffers need separate policies. Keeping an entire
world resident must preserve sharing between repeated terrain blocks and interior
instances; it must not mean expanding every placement into separate triangles.
These local-host runs already retain the decoded source resources after loading;
the main renderer work is retaining and updating their derived/GPU representations.

The original measurements below remain a historical baseline. Implementation
started on 2026-11-02; see the progress log below and the separate
[optimized timing captures](data/render-performance.json),
[native memory results](data/render-memory-after.json) and
[validation record](data/render-validation.json). Gameplay rules,
asset formats and the network protocol are preserved.

## What was measured

Both installations are user-supplied installations, including extra/custom maps.
The base-mode installation contains 194 mission files. The RPG installation has
72 base mission files plus `RPG/rpgmap1` and `RPG/rpgmap5`. These are not claimed
to be pristine retail distributions or equally sized catalogs.

The catalog traverses every `.vol` and terrain `.ted` archive in the selected
directories, plus loose files. It uses the real decoders for DTS, DML, DIS, DIG,
DIL, DTF, DTB, bitmaps, palettes, bitmap arrays, fonts, and PCM WAV. Each asset is
decoded separately, measured while alive, then released. Catalog sums include
shadowed sources, repeated content, all stored LODs and alternate light states;
they are **not a simultaneous runtime memory requirement**. Script/video/other
file sizes are inventoried without executing or decoding their contents.

| Complete installed catalog | Base-mode `base/` | RPG installation `base/` + `RPG/` |
| --- | ---: | ---: |
| VOL/TED archives | 396 | 289 |
| Asset entries including loose files | 13,259 | 11,445 |
| Archive file bytes, MiB | 176.7 | 191.6 |
| Sum of decoded C++ allocations, MiB | 217.3 | 244.7 |
| Mesh/terrain resource portion, MiB | 94.6 | 71.8 |
| Bitmap resource portion, MiB | 51.0 | 70.1 |
| Interior lighting resource portion, MiB | 27.7 | 24.6 |
| PCM WAV resource portion, MiB | 24.5 | 59.0 |

Every supported-format catalog decode succeeded. Every decoded allocation was
released in the individual-asset accounting check. Catalogs include all named
archive entries; the measured archives had no unnamed entries.

Each mission was loaded in a fresh process with an isolated output directory.
The game initialized a local server and client and advanced eight 32 ms steps.
All 194 base-mode missions were attempted: 179 loaded and 15 failed during mission
declaration/loading. Both RPG missions loaded. Failed cases remain in the data
and are excluded from successful-mission ranges.

## Complete mission sources and application RAM

All values below are MiB (1,048,576 bytes), measured after load/simulation warm-up,
before creating a GPU device. The mesh/terrain column counts live allocations
from decoded DTS, DIG, DTF and DTB resources. It includes animation/LOD data,
terrain lighting stored in DTBs, and retained resource versions. It excludes
separate collision structures and derived render meshes; those are included in
the whole application's C++ total. It is a conservative source-resource measure,
not a unique-vertex buffer size or a client-only minimum.

| Mission | Mesh/terrain sources | All tracked C++ | Windows private commit | Mounted archive file bytes |
| --- | ---: | ---: | ---: | ---: |
| 179 successful base-mode missions, min–max | 8.4–17.0 | 114.8–149.1 | 125.9–167.4 | 81.1–89.9 |
| Base Raindance | 10.2 | 125.8 | 138.0 | 85.6 |
| Base Broadside | 10.1 | 127.3 | 140.1 | 85.7 |
| RPG map 1 | 23.2 | 191.7 | 211.8 | 108.9 |
| RPG map 5 / Keldrin world | 27.0 | 242.2 | 267.8 | 130.5 |

Median tracked C++ residency across successful base-mode missions was 122.6 MiB;
Freezer was the largest at 149.1 MiB. RPG map 5 retained about 11.9 MiB of decoded
interior geometry versus about 0.9 MiB in Raindance. Its decoded interior records
contain 319,162 source triangles across live LOD/resource versions, versus 15,720
for Raindance. These counts explain why visible polygon counts alone are a poor
measure of the difference between the worlds.

At the historical baseline, raw archive backing was a substantial separate cost:
`VolumeRStream` owned complete archive buffers. Canonical asset sharing and archive I/O lifetime
deserve measurement alongside the renderer. Full source geometry residency does
not require retaining multiple copies of unchanged sources or every archive's
bytes indefinitely.

Terrain sharing is particularly important. Both Raindance and RPG map 5 have
132,098 loaded height samples and 18 block placements across retained grid
versions. A 256×256 block has 131,072 full-resolution triangles. Expanding it into
the current 192-byte `StaticSurfaceTriangle` representation costs 24 MiB before
materials/maps; copying that representation into nine placements costs 216 MiB.
Keep heightfields, shared topology and placements compact instead.

## GPU and derived cache memory

Separate fresh-process runs prepared and submitted 240 offscreen frames at
800×600 through the actual D3D12 backend on an RTX 5090. Observer cameras rotate
through two full turns when the mission provides both control objects. This
measures a warmed spawn-view working set, not an exhaustive traversal of every
location or a full-world GPU residency requirement. Audio playback, presentation
swapchains and video playback are not initialized.

| Valid world-rendering capture | Maximum textures | RGBA payload MiB | DXGI local usage MiB | Tracked C++ at end MiB | Private commit at end MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Raindance | 1,607 | 10.8 | 146.9 | 152.8 | 408.9 |
| Broadside | 1,467 | 9.2 | 137.0 | 153.9 | 405.9 |
| Stonehenge | 132 | 1.4 | 46.3 | 125.7 | 266.8 |
| Scarabrae | 1,604 | 9.9 | 146.8 | 162.8 | 425.7 |
| RPG map 5 | 4,511 | 15.4 | 342.3 | 294.9 | 786.5 |

DXGI values are per-process usage summed across adapters. They include driver
resource costs, buffers and render targets. The shared/nonlocal segment was
about 26.3 MiB in these captures. Private commit, tracked C++ bytes and GPU usage
describe different accounting domains and must not simply be added together.

The small-texture allocation problem is material: RGBA payload alone understates
the GPU working set. The current bundled D3D12 backend uses committed textures;
thousands of tiny lightmaps incur allocation/alignment/descriptor costs. Portable
lightmap pages and compatible texture arrays should reduce both binding work and
allocation count. Raising an entry cap alone is not the long-term design.

At baseline, RPG map 1 never acquired a server/client control object and rendered
no world geometry. Its GPU case is explicitly rejected and excluded above. Its
catalog and complete mission load-only RAM measurements remain valid. The cause
was a local connection hardcoded to an ID already allocated to a startup NPC.
The engine now selects local and remote player IDs from the shared vanilla
representative pool and keeps AI separate from the connected-client list. A follow-up
240-frame D3D12 capture rendered world geometry successfully; those results are
separate from this historical baseline. Collect fresh measurements before using
this mission for GPU or frame-time comparisons.

## Measurement limits

The optional executable replaces C++ `new`/`delete` only within itself. Requested
allocation sizes include vector capacity and object storage but exclude allocator
metadata and shared-library `malloc`, stacks, code, GPU resources and mapping
costs. Tracking adds a 32-byte header plus alignment padding to each allocation
and small process-lifetime metadata records. Thus process commit includes some
instrumentation overhead; tracked requested bytes do not include that overhead.

This is a memory instrument, not the frame-time benchmark: its atomic counters
and allocation headers perturb CPU cost. Use the ordinary optimized application
and GPU timings for performance claims. All native memory/GPU measurements here
are Windows x64 measurements. macOS/Android/iOS allocator and GPU behavior has
not been measured. Heap layout and vector growth can differ across C++ libraries.

## Platform policy

Use one portable residency interface with device-dependent budgets. Keep file
validation limits separate from cache budgets. Track source bytes, derived CPU
bytes, GPU payload, allocation count, staging capacity and backend allocation
usage separately. Retain a whole compact mission source set where it fits;
keep an eviction/streaming fallback available for future larger mods.

| Platform | Initial policy and required validation |
| --- | --- |
| Windows / Linux desktop | Full compact mission geometry in RAM. Budget GPU caches and transient uploads independently. Validate D3D12 and Vulkan; this baseline covers Windows D3D12 only. |
| macOS / Metal | Full compact mission geometry where the device budget permits. Account for CPU/GPU shared memory on unified-memory machines. Measure Metal allocations and process footprint on actual target Macs. |
| Android / Vulkan | Start with the same compact geometry representation. Use smaller measured texture/staging budgets and release dispensable derived data on backgrounding. Verify low/mid/high supported device tiers under sustained play, travel and app switching. |
| Possible iOS / Metal | Use the same source/derived separation. Recreate disposable GPU caches after lifecycle events as needed. Establish foreground/background headroom on the lowest supported device before setting defaults. |

Metal exposes [`hasUnifiedMemory`](https://developer.apple.com/documentation/metal/mtldevice/hasunifiedmemory),
[`currentAllocatedSize`](https://developer.apple.com/documentation/metal/mtldevice/currentallocatedsize)
and [`recommendedMaxWorkingSetSize`](https://developer.apple.com/documentation/metal/mtldevice/recommendedmaxworkingsetsize).
The latter is a performance guidance value, not an application RAM allowance.
On Apple mobile platforms, [`os_proc_available_memory`](https://developer.apple.com/documentation/os/os_proc_available_memory)
reports current application headroom; its value is advisory and changes during
the app lifecycle. Do not treat physical RAM as the application's allocation limit.

Android's [memory management guidance](https://developer.android.com/games/optimize/memory-overview)
describes changing process budgets, including foreground/background distinctions
on Android 17+. Its [LMK guidance](https://developer.android.com/topic/performance/issues/lmk)
also explains supported background trimming and checking previous exit reasons.
Do not base foreground pressure handling on deprecated trim callbacks. Concrete
mobile budgets require device measurements; this Windows baseline does not
establish a safe universal mobile memory cap.

## Implementation sequence

1. **Establish permanent resource accounting and repeatable comparisons.** Keep
   the base/RPG cases separate. Add renderer-side allocation/dirty-update counters
   with negligible overhead when disabled. Refresh the RPG map 1 baseline after
   the vanilla representative/client allocation fix.
   Track mean/p95/p99 full frame time, allocations/frame, uploads, binds, resident
   and peak bytes, and load times. Measure remote-client-only operation separately
   from this local server/client baseline. Keep native memory tests separate from timing.

2. **Introduce retained immutable render resources.** Give meshes, material tables,
   palettes, textures, poses and lightmaps stable handles and explicit revisions.
   Validate assets before publication. Preserve resource ownership across mount
   changes while distinguishing genuinely changed resolution/overrides. Keep
   software rendering as a compatibility reference and independently supported
   consumer rather than imposing its projected triangles on the hardware path.

3. **Prototype retained static interiors plus independent lighting.** Upload local
   vertices/indices once per canonical mesh; pass instance/camera transforms to
   the vertex shader. Keep authored visibility, material, haze, depth, cutout and
   transparency rules. Change only affected lightmap regions when lights animate.
   Compare the same cameras and scenes against the current renderer before
   enabling the new path by default.

4. **Share evaluated poses and collision data.** Cache by shape identity,
   pose/override revision and detail. World placement alone should not require
   rebuilding local geometry. HUD/physics queries reuse the same valid snapshots.
   Reuse BSP clipping scratch storage. Preserve authoritative collision, movement,
   networking and targeting behavior; exercise moving and animated targets.

5. **Retain terrain as heightfields, shared patches and LOD topology.** Rebuild
   selections/index data only when relevant camera/terrain inputs change. Keep
   authored stitching, geomorphing, holes, repeated blocks and lighting behavior.
   Separate native/software-only preparation from hardware mesh submission.

6. **Page lightmaps and batch compatible texture bindings.** Use portable bounded
   pages/arrays with correct gutters, mips and sampling behavior. Preserve image
   and transparency order. Measure allocation cost on Metal/Vulkan as well as
   D3D12; do not assume desktop descriptor counts or bindless support on mobile.

7. **Reduce archive/duplicate CPU retention, then consider worker jobs.** Measure
   canonical decoded-resource sharing and bounded archive reads/mapping through
   portable I/O. Render from immutable snapshots. Introduce bounded worker jobs
   only where profiling still shows useful parallel work, keeping mutable
   simulation and device API ownership explicit.

After each stage, repeat stationary, rotating, travel, animated-light, collision,
palette/skin-change and mission-transition cases. Require image/readback checks,
gameplay regressions, bounded peaks and stable warm-cache behavior. Review any
precision/coverage differences before replacing existing behavior. Establish a
measured frame-time improvement before proceeding with further restructuring.

The first implementation milestone is **retained static interiors with separately
updated lightmaps**, backed by accounting and visual comparisons. The measured
geometry sizes do not justify starting with a complex world streamer.

## Implementation progress (2026-11-02)

Implementation spans focused commits on `render-improvements`, from `90a4091`
through the texture-array conformance follow-up. The original uncommitted measurement files and loader-decoration
work are preserved. The data below comes from ordinary Release timing binaries
and separate native memory audit processes; neither instrument's time is substituted
for the other's measurement.

| Stage | Implemented | Remaining scope |
| --- | --- | --- |
| Accounting / comparisons | Serial base/RPG stationary, rotating and travel runs; mean/p95/p99, load time, dirty work, uploads/binds, separate GPU capacities, capture hashes, native allocation rates | GPU timestamps, remote-client-only operation, whole-process allocations/frame |
| Immutable resources | Canonical decoded source versions, owning immutable render handles, unique mesh/pose revisions, explicit dirty lighting, palette/content invalidation | A global source-residency budget for arbitrary larger mods |
| Static interiors / lighting | Compact shared indexed CPU geometry, persistent GPU vertex/index buffers, GPU instance/camera transforms, authored CPU visibility, independent affected-surface lightmap updates | Translucent interiors and orthographic views use the supported projected path |
| Poses / collision | Shared evaluated poses; at most two local collision details per pose; bounded BSP scratch and no copies of unchanged polyhedra | Animated TS draw geometry is still transformed/projected on the CPU |
| Terrain | Compact source heightfields and block instances retained; exact unchanged-camera selected/morphed mesh reuse; 32 shared native stitch index templates | Moving-camera selection, geomorphing and projection remain on the CPU; no complete GPU heightfield renderer |
| Textures / lifecycle | Bounded 512-square lightmap pages with replicated gutters; compatible 16-layer arrays; standalone fallback; explicit GPU/conversion/source cache trimming | Target-device allocation/lifecycle/headroom measurements |
| Source / archive retention | Opt-in pure-decoder identity across unchanged archive resolutions; read-only mappings with buffered fallback; unused source purge on mount changes and trimming | Bounded worker jobs remain conditional on further profiling |

Interior geometry is canonical across instances, with independent immutable lighting
snapshots and per-instance transforms. Animated slots rebuild only dependent maps;
scene-light changes retain unaffected surfaces. The gameplay InteriorShape adapter
and mission SimInteriorSurface both use this mechanism. Software rendering lazily
reconstructs its supported reference representation from the same compact sources.
`OPENT1_PROJECTED_INTERIORS=1` selects CPU projection for hardware comparisons.

Evaluated TS poses share data until an override/mutation receives a new revision.
Local collision meshes are shared across physics/targeting queries and remain
independent of placement. BSP box clipping uses at most 32 KiB of fixed scratch per
query thread, with ordinary allocation lifetimes on overflow and independent storage
for nested queries. A deep 96-plane regression exercises overflow and reuse. A
separate ordinary Clang Release synthetic corpus benchmark reduced 514,000 box
queries from 863-874 ms to 553-569 ms; this is not a whole-frame speedup claim.

Lightmap pages preserve clamp behavior through explicit source UV clamps and
replicated gutters. Dynamic maps recycle only unpinned slots; static maps retain
their working set. Texture arrays group identical RGBA dimensions and authored mip
layouts, retaining filtering, cutout/alpha and primitive order. There is a portable
standalone fallback. Defaults independently budget 64 MiB retained meshes, 64 MiB
logical texture payload, 16 MiB lightmap pages, 16 MiB array allocation payload and
128 MiB staging capacity. These are configurable cache limits, **not mobile safety
limits** or a deduction from dedicated VRAM. Logical payload and allocated page/array
capacity overlap; do not add all reported fields as if they were disjoint totals.

Read-only archive mappings replace whole-archive private copies where replacement
semantics are supported. Windows mappings deny in-place writes and use POSIX atomic
replacement for authoring saves. Unsupported filesystems and Linux 9p use buffered
snapshots. Native POSIX mappings retain their file object and reject detected in-place
modifications before opening an entry; mounted archives must not be edited in place.
Old handles keep their source bytes after an atomic replacement. A map's virtual byte
count is separate from private heap and is **not a measurement of resident pages**.
Pure decoders preserve unchanged source identity across archive mount changes;
overrides, new source versions, loose-file rescans and dependency-loading custom
loaders preserve their existing invalidation rules.

The ordinary Release benchmark uses 120 warm-up frames followed by 240 measured
frames at 800x600, with stationary, full rotating and 64-unit travel paths. No audio
playback or presentation is initialized. CPU frame samples include local simulation,
preparation, submission and the preceding submission's fence wait; they are **not GPU
timestamps**. RPG observers remain at the class-selection spawn view, so these runs
do not replace a complete gameplay traversal. Raw stage distributions and capture
provenance are in [render-performance.json](data/render-performance.json).

The final column below is the average of two consecutive serial run means, with the
range of those two means shown. The original is one earlier capture; this is a local
comparison with run variance, not a portable performance guarantee.

| Mean CPU frame time, ms | Original | Final (run range) |
| --- | ---: | ---: |
| Base Raindance, stationary | 2.33 | 2.32 (2.29-2.34) |
| Base Raindance, rotating | 3.47 | 2.54 (2.53-2.54) |
| Base Raindance, travel | 5.04 | 3.62 (3.61-3.62) |
| Base Broadside, stationary | not captured | 3.66 (3.59-3.74) |
| Base Broadside, rotating | not captured | 3.27 (3.26-3.27) |
| Base Broadside, travel | not captured | 4.42 (4.31-4.52) |
| RPG map 1, stationary | 15.06 | 8.91 (8.89-8.92) |
| RPG map 1, rotating | 13.45 | 9.72 (9.52-9.92) |
| RPG map 1, travel | 13.17 | 10.50 (10.34-10.67) |
| RPG map 5, stationary | 12.89 | 7.65 (7.59-7.71) |
| RPG map 5, rotating | 12.66 | 8.85 (8.85-8.86) |
| RPG map 5, travel | 13.61 | 10.12 (9.63-10.62) |

Warm retained interior mesh uploads are zero in the stationary and rotating cases.
RPG map 5 rotating lightmap builds fell from about 84,000 to 7,600 per 240 frames
after connecting the gameplay adapter to independent lighting. Compact CPU interior
sampling initially regressed Broadside; conservative per-surface query bounds reduced
its stationary mean from about 7.1 ms to 3.5 ms before continuing. Sharing native
terrain templates and BSP scratch preserves all 12 readbacks exactly; no significant
whole-frame speedup is claimed for those two small changes.

Fresh native audit results are in [render-memory-after.json](data/render-memory-after.json).
The following rotating 240-frame working sets include a local server/client. The
source column is compact decoded DTS/DIG/DTF/DTB allocations. Tracked C++ includes
sources plus derived/runtime data; process private commit also includes native
libraries, drivers and instrumentation overhead. Neither includes the same thing as
DXGI usage, and these columns must not be added together.

| Mission | Source mesh/terrain MiB | Tracked C++ at GPU end MiB | Process private MiB | DXGI local MiB | Physical cached textures |
| --- | ---: | ---: | ---: | ---: | ---: |
| Base Raindance | 5.09 | 52.69 | 231.99 | 79.20 | 308 |
| Base Broadside | 5.07 | 54.62 | 239.15 | 82.67 | 374 |
| RPG map 1 | 16.46 | 102.32 | 364.19 | 140.99 | 1,138 |
| RPG map 5 | 18.36 | 136.40 | 413.69 | 150.37 | 1,239 |

For comparison, the historical RPG map 5 audit recorded 294.9 MiB tracked C++,
786.5 MiB private commit, 342.3 MiB DXGI local usage and 4,511 texture resources.
Raindance recorded 152.8 / 408.9 / 146.9 MiB and 1,607 resources. Historical RPG map 1
GPU output was invalid; its current source/load measurements and fresh ordinary
world-rendering timing baseline are kept separate from that rejected GPU capture.

All six fresh audit cases have zero owned whole-archive buffer bytes on this Windows
filesystem. Mapped backing is 85.6-130.5 MiB in the four primary cases; it remains a
separate file-backed virtual accounting domain. GPU transfer capacities are 8 MiB in
Raindance/Broadside and 16 MiB in the RPG cases. Final-interval render-tag allocations
remain roughly 2,389 / 3,790 / 5,506 / 6,922 per frame respectively, excluding simulation.
This remaining churn is recorded rather than described as allocation-free rendering.

Validation details are in [render-validation.json](data/render-validation.json):

- All 13 Windows MSVC Release CTest suites pass. CTest uses the dummy video driver;
  separate real-device platform suites pass on D3D12 and Windows Vulkan.
- Synthetic readbacks cover near/far clipping, both windings, cutouts, palette/gamma
  changes, authored mip filtering, array/standalone behavior, NPOT lightmap clamping,
  gutters, dynamic reuse and budget rejection. Immutable batches survive trimming;
  the published frame stays byte-identical, and re-rendering after a new page layout
  differs by at most one channel level in the mission acceptance cases.
- Four fresh real-device acceptance cases cover Raindance -> Broadside, rpgmap1 ->
  rpgmap5 on D3D12 and their reverse transitions on Vulkan, plus software rendering
  and cache trimming. Installed fixpack and RPG suites pass all six cases, covering
  gameplay/targeting, model rendering, RPG paths and rpgmap1 world rendering.
- Both complete catalogs pass again: 396 base-mode archives / 13,259 entries and
  289 RPG-installation archives / 11,445 entries, with zero supported decode failures,
  entry errors or individual-allocation release mismatches. Six fresh GPU audit
  missions render world geometry; allocation/alignment self-checks pass.
- Linux WSL Clang 23.1.1 headless Release builds and passes all 11 suites. Core tests
  exercise native Linux mappings under /tmp and 9p buffered fallback under /mnt/c.
  GCC 16.2.1 Release instead stops at an existing maybe-uninitialized warning promoted
  to an error in net_transport.cpp:202; networking was not changed for that warning.
- Pose sharing, compact query bounds, arrays, source sharing, archive mappings,
  shared terrain topology and BSP scratch preserve their preceding 12 mission/camera
  final readbacks byte-for-byte. Lightmap pages changed at most five pixels by one
  channel level. GPU versus CPU transforms have isolated coverage/interpolation
  differences up to 94/255 in RPG map 1 travel. Magnified edge crops were reviewed;
  the projected reference remains available. The two paths are not pixel-identical.
- HLSL, DXIL, SPIR-V and MSL representations are synchronized. Actual macOS Metal,
  Linux desktop Vulkan, Android Vulkan and iOS Metal devices are unavailable; native
  Metal shader compilation and those runtime/lifecycle measurements are unverified.

The next priorities follow the measured remaining work: moving-camera terrain still
costs about 0.4-1.5 ms per frame, while RPG map 1's local simulation costs about
5.4-5.9 ms and is outside the renderer's ownership. A GPU terrain/animated-shape path
needs to preserve native far-sphere clipping, first-writer geomorph/stitch rules,
per-vertex lighting and software callbacks. It is not complete here. Complete source
residency is measured on these installations; there is no universal source RAM cap
for arbitrary mods. Remote-client-only timing, GPU timestamps, full-process allocation
rates, exhaustive long traversal and target-device headroom remain open. No worker
threads or simulation/networking rule changes are introduced to mask those gaps.

## Reproduction

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --target opent1_memory_audit --parallel 8
.\build\x64-release\opent1_memory_audit.exe self-test
python tools/run_memory_audit.py --executable build/x64-release/opent1_memory_audit.exe --base C:/Games/TRIBES --rpg C:/Games/PCRPG --output build/profiles/memory-audit --all-missions --gpu --frames 240
python tools/summarize_memory_audit.py build/profiles/memory-audit docs/data/render-memory-baseline.json --date 2026-11-01
```

The target is excluded from the normal build. A headless configuration supports
catalog/load-only measurement without SDL/OpenAL; `--gpu` requires a platform
build. Omit `--all-missions` or `--gpu` to skip those groups. Every run includes
fresh-process logs and isolated runtime outputs under the chosen directory.
Failed missions and non-world GPU captures are recorded without aborting the
remaining cases. `ResourceManager` permits explicit same-type loader decoration
for this instrumentation; normal duplicate registration remains rejected and
existing cached handles are preserved.

Validation completed for this baseline: Release and headless tool builds,
allocation/alignment/release self-checks, a headless real-mission load, all 13
Release CTest checks, all catalog decodes, and the successful/failed mission
captures described above. No renderer optimization or mobile support is claimed
as completed by this measurement work.

### Ordinary optimized comparisons

Use the asset roots for the two installations. Keep the allocation audit out of
these timing runs:

```powershell
cmake --build --preset x64-release --target opent1 opent1_tests opent1_render_benchmark
python tools/run_render_benchmarks.py --executable build/x64-release/opent1_render_benchmark.exe --base C:/path/to/TRIBES --rpg C:/path/to/PCRPG --output build/profiles/render-current --driver direct3d12
python tools/compare_render_readbacks.py build/profiles/render-reference build/profiles/render-current
# Add --projected-interiors for the CPU projection compatibility comparison.
$env:SDL_GPU_DRIVER = 'vulkan'
./build/x64-release/tests/opent1_tests.exe --suite platform
Remove-Item Env:SDL_GPU_DRIVER
```

The runner records executable SHA-256, commit, working-tree status and capture time.
Optional `OPENT1_VALIDATE_RENDER=1` runs trim/readback/software acceptance work after
all timed samples. `OPENT1_VALIDATE_TRANSITION` adds a named mission transition after
that acceptance work. For native audit summaries, run the original measurement
runner with `--gpu --frames 240`, then:

```powershell
python tools/summarize_render_memory.py build/profiles/render-memory-current docs/data/render-memory-after.json
```
