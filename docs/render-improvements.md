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

At that milestone, the remaining priorities followed the measurements: moving-camera terrain
cost about 0.4-1.5 ms per frame, while RPG map 1's local simulation cost about
5.4-5.9 ms and is outside the renderer's ownership. A GPU terrain/animated-shape path
needed to preserve native far-sphere clipping, first-writer geomorph/stitch rules,
per-vertex lighting and software callbacks. The follow-up below implements those
paths and explicit residency policies. Complete source residency was measured on
these installations; there is no universal source RAM cap
for arbitrary mods. Remote-client-only timing, GPU timestamps, full-process allocation
rates, exhaustive long traversal and target-device headroom remain open. No worker
threads or simulation/networking rule changes are introduced to mask those gaps.

## Retained terrain, animated TS and residency follow-up

The follow-up builds on `48c5e23`, which committed the native memory audit after
`5f9c0d6`. The historical captures above and their JSON files are unchanged.
Fresh comparisons and validation are recorded separately in
[render-retained-followup.json](data/render-retained-followup.json).

Native terrain now publishes compact selected patches. Each patch references a
shared stitch template and eight first-writer inputs: authored height, merge
height, morph fraction and authored distance. Selection, holes, row introduction,
seam ownership and texture callbacks retain their CPU rules. A single persistent
18-vertex GPU patch draws all 32 templates, with degenerate padding for shorter
templates. Camera-dependent inputs use a separately accounted reusable instance
buffer. The vertex shader morphs and transforms the patches; interpolated native
distances clip the far sphere and produce the deferred haze pass. Consecutive
compatible instances batch without changing primitive or transparency order.
Conservative CPU patch admission avoids converting textures for rejected patches.
Repeated block placements do not acquire expanded vertex or triangle meshes.
Software drawing and receiver queries reconstruct the selected triangles lazily.
Native software callback/ring timing continues through its original path.
The draw view's sphere limit and the terrain's prepared haze limit remain
independent, including when an immutable snapshot is drawn through a different
clip context.

Animated TS drawing retains immutable local mesh frames, UV frames and packed
normals, keyed by source identity and material shading policy. Evaluated poses
still provide shared CPU node transforms for collision, targeting and LOD; GPU
draws receive those transforms independently of source geometry. Native per-mesh
light preparation, including authored bounds and shadow attenuation, remains on
the CPU. Up to two active prepared directional, nonlinear, wrap or point lights
evaluate on the GPU. Flat shading with point lights, more active lights,
orthographic/two-sided views and explicit subpixel rejection use CPU projection.
Material remaps, cutouts, opacity and image-level transparency sorting survive
the retained submission path. A lazy projected frame supports software drawing
and silhouettes from the same immutable local meshes and prepared lighting scalars,
without cloning directional shadow maps.
Conservative CPU admission uses retained face planes and bounds computed from
actual mesh vertices, retaining compatibility with stale exported shape spheres.

Residency policies distinguish ownership and accounting domains:

| Domain | Policy and accounting |
| --- | --- |
| Decoded sources | `ResourceManager` defaults to a 256 MiB soft cache target. It evicts the least-recently released unused reloadable entries. Live handles, retired immutable versions and nonreloadable added objects retain ownership; oversized live sets report budget debt. Primary decoded formats report owned allocation capacities; custom types without an estimator report incomplete accounting. |
| Decoder input staging | A separate configurable 128 MiB aggregate limit covers nested loader input buffers, with current/peak counters and failure before publication. File validation limits remain independent. Caller-owned `read()`/`openStream()` results and archive backing are separate domains. |
| Derived CPU caches | TS local meshes have a configurable 32 MiB default budget and a 512-entry cap. Disabled or oversized TS entries use CPU projection before expanding local GPU geometry. Texture conversion has an independent configurable 64 MiB / 4096-entry budget, including compact comparison snapshots; conversion remains available without retention. Existing terrain tile and software texture budgets remain independent. Published snapshots survive cache eviction. |
| Published batches | `HardwareBatch::memoryStats()` counts batch allocation capacities and unique reachable CPU mesh/texture snapshots. Cache and batch reachable bytes overlap and must not be added as separate allocations. |
| GPU allocations | Existing mesh, texture, lightmap-page and array budgets remain configurable. Mesh payload, page/array capacity, physical textures, persistent vertex/instance capacities and allocation counters remain distinct from backend allocation/alignment usage measured by the native audit. |
| GPU staging | The configured transfer limit includes new mesh uploads, texture uploads and instance inputs together. Admission failure preserves the published frame. Trimming waits for the preceding fence and releases disposable buffers while retaining the color readback. |

Pressure/background handling now reaches terrain tile/selection caches, interior
view caches and retained TS meshes as well as conversion/software caches and
unused decoded resources. Mission loading clears disposable TS source-frame
retention. Trimming does not change animation clocks, authoritative collision or
live immutable snapshots. Source budget debt explicitly distinguishes an
oversized live mission from an oversized unused cache; it is not an application
memory allowance or a measured mobile safety limit.
Transition stress testing also exposed a gameplay datablock lifetime bug:
reconnection replaced registry entries while surviving shapes kept raw derived
datablock pointers. Live objects now retain the original managed datablock across
reset, replacement and script rollback, releasing it when their ownership ends.

`OPENT1_PROJECTED_TERRAIN=1` and `OPENT1_PROJECTED_SHAPES=1` independently select
the compatibility paths. The benchmark runner records corresponding
`--projected-terrain` and `--projected-shapes` options. The existing
`--projected-interiors` comparison disables retained world hardware preparation.
CPU frame measurements include simulation, preparation, submission and the
preceding fence wait. Retained submission counts include source primitives that
the GPU later clips; they do not imply a change in visible geometry.

### Follow-up measurements and acceptance

Two serial reference/retained pairs use ordinary MSVC Release executables, D3D12,
800x600, 120 warm-up and 240 measured frames per case. Each pair uses the same
binary for both paths. The second pair adds only a datablock self-initialization
ownership guard; the render algorithms are unchanged. The JSON preserves both
pairs, executable hashes, revisions, distributions, readback hashes and independent
terrain-only/TS-only captures. Values below average the two run means, in ms.

| Mission / camera | CPU frame, projected -> retained | Preparation, projected -> retained | Preparation reduction |
| --- | ---: | ---: | ---: |
| Raindance stationary | 2.418 -> 2.196 | 1.751 -> 1.586 | 9.4% |
| Raindance rotate | 2.596 -> 2.460 | 1.883 -> 1.759 | 6.6% |
| Raindance travel | 3.731 -> 3.592 | 2.219 -> 2.085 | 6.0% |
| Broadside stationary | 3.704 -> 3.477 | 2.868 -> 2.644 | 7.8% |
| Broadside rotate | 3.320 -> 3.141 | 2.528 -> 2.347 | 7.2% |
| Broadside travel | 4.542 -> 4.393 | 2.879 -> 2.711 | 5.8% |
| RPG map 1 stationary | 7.632 -> 6.886 | 3.631 -> 2.877 | 20.8% |
| RPG map 1 rotate | 8.552 -> 7.224 | 4.112 -> 3.280 | 20.2% |
| RPG map 1 travel | 8.925 -> 8.213 | 3.828 -> 3.289 | 14.1% |
| RPG map 5 stationary | 7.009 -> 6.188 | 4.058 -> 3.332 | 17.9% |
| RPG map 5 rotate | 7.803 -> 7.318 | 4.780 -> 4.294 | 10.2% |
| RPG map 5 travel | 8.293 -> 7.745 | 4.761 -> 4.225 | 11.3% |

Full CPU frame means improve 3.3-15.5% in these comparisons. Terrain accounts for
most preparation savings. TS-only gains are smaller and scene-dependent; these
captures do not establish a benefit for every animated model or lighting setup.
The fallback policies retain the CPU path for incompatible lighting and views.
RPG map 1 travel reference run means vary from 8.532 to 9.319 ms; the individual
run results matter more than a precise universal speedup claim.

Upload traffic includes instance inputs as well as vertices. For stationary
Raindance, Broadside, RPG map 1 and RPG map 5 in the repeated pair, combined
vertex/instance bytes per frame fall from 82,532 / 140,160 / 254,862 / 251,491 to
61,460 / 87,040 / 190,176 / 197,152. All four have zero measured warm mesh-upload
bytes. Texture uploads and growing retained working sets remain separately
reported; this is not an allocation-free renderer.

The fresh native audit is in
[render-memory-followup.json](data/render-memory-followup.json), separately from
the unchanged preceding audit. These are requested C++ bytes and DXGI local usage,
in MiB; they are overlapping domains and must not be added.

| Mission | Tracked C++, preceding -> follow-up | DXGI local, preceding -> follow-up |
| --- | ---: | ---: |
| Raindance | 52.69 -> 52.81 | 79.20 -> 79.86 |
| Broadside | 54.62 -> 54.74 | 82.67 -> 83.66 |
| RPG map 1 | 102.32 -> 102.53 | 140.99 -> 142.21 |
| RPG map 5 | 136.40 -> 136.68 | 150.37 -> 151.77 |

Follow-up process private commit is 233.17 / 241.45 / 366.88 / 419.45 MiB for
those four maps. Fresh Raindance/Broadside repeats are 235.44 / 240.63 MiB, showing
variation larger than their tracked C++ change. Final-interval render-tag allocation
rates are about 2,431 / 3,995 / 5,657 / 7,187 per frame, slightly above the preceding
audit. Source, batch/cache reachable bytes, physical GPU allocations and staging
capacities remain separate counters. None of these measurements establishes
mobile headroom or a universal application RAM limit.

All 13 Windows Release CTest suites and all 11 Clang 23.1.1 Linux headless suites
pass. Real-device D3D12 and Windows Vulkan platform suites cover all 32 terrain
templates, independent sphere/haze limits and 315 TS light/material/depth cases.
Compact terrain reconstruction tests preserve first-writer values, holes,
repeated/unequal seams and software callback policy. Frozen TS lighting, source
budgets, retired owners, staging rejection and live datablock replacement/reset/
rollback/self-initialization have regression coverage.

Four real-device mission acceptance cases cover base and RPG transitions in both
directions, software fallback, published-frame preservation and immutable batch
re-submission after trimming. All four trim re-submissions have zero channel error.
After fixing the discovered datablock lifetime bug, twelve additional fresh RPG
transition stress processes pass. Installed base fixpack, RPG and RPG-model suites
pass. Both complete catalogs again have zero supported decode failures, entry errors
or individual-allocation release mismatches; six GPU audit missions render world
geometry and allocator self-checks pass.

The twelve final projected/retained readbacks differ at 813-8,299 of 480,000 pixels,
mostly by one or two channel levels. Only 1-8 pixels per image exceed two levels,
with an isolated maximum of 190 at an edge. Full images and magnified edge crops
were reviewed. GPU/CPU transform rounding is not pixel-identical. Metal compilation
and runtime, Linux desktop Vulkan, Android/iOS devices, exhaustive traversal,
remote-client-only timing and GPU timestamps remain unverified or unmeasured.

## Scene scratch and shared inputs (2026-11-03)

Scene capture, container selection, shape clipping and hardware submission reuse
thread-local scratch capacities. Nested calls receive independent workspaces;
every exit clears borrowed pointers and retained resource handles. Individual
scratch vectors above 8 MiB release their allocation on exit. Container and batch
lookup nodes use fixed 128/256 KiB arenas, with scoped heap overflow for larger
inputs. Published frames and batches own their output independently of scratch.

Material snapshots now share immutable material tables and texture handles.
Skinning and terrain builders publish replacement tables after construction.
Surface queries share one immutable scene-light snapshot, and simulation/TS light
adapters share immutable directional shadow maps. Per-mesh lighting still samples
those maps into owned prepared scalars. Map-content equality remains available for
independently constructed equivalent maps. Scene and shape polygons append into
the same submission vector, and translucent sorting borrows source geometry until
synchronous batch preparation finishes.

Fresh 240-frame D3D12 RPG audits compare the clean baseline with this change.
Final-interval render allocations/frame (last 60 frames, excluding simulation):

| Mission | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| RPG map 1 | 5,656.42 | 3,820.43 | 32.46% |
| RPG map 5 | 7,188.37 | 4,829.07 | 32.82% |

Final polygon, surface, shape, draw, texture and mesh-upload counts match.
Ordinary scratch capacity retention raises tracked live C++ heap by 1.60/2.15 MiB;
the fixed lookup arenas additionally occupy 384 KiB per thread outside that heap
counter. Both 800x600 rotating RPG readbacks match the baseline exactly. All 13
Release CTest suites, real-device D3D12 platform tests and allocator self-checks
pass. Sharing/replacement isolation, resource-manager destruction, nested scratch,
exception cleanup, stable translucent order and repeated/failing preparations have
regression coverage. These audits measure allocation churn, not a timing benefit.
Capture hashes, executable hashes and counters are in
[`render-allocation-followup.json`](data/render-allocation-followup.json).

## Moving-camera terrain selection

The November 3 follow-up profiles window/visibility construction, traversal and
validation, edge preparation, and patch/texture emission separately. The ordinary
benchmark also reports node/material/height work, first-writer point requests,
unique reached points, collected edge entries, and temporary arena allocations.
The visibility measurement includes window selection; traversal includes source
validation. These phases are within `terrain_lod_ms`, not additional frame costs.

Native stitching previously collected every introduced point in two edge lists,
added selected corners again, sorted/deduplicated both lists, and searched them
for each selected tile. The native boundary filter ultimately admitted only
corners and reached edge midpoints. Selection now queries those four midpoints
directly in the existing first-writer point table. Scan-row order, transformCross
introduction order, child visitation, corner-distance accumulation, geomorph
inputs, and topology index order are preserved. Points introduced before a child
fails visibility still contribute to neighboring stitches. General non-native
stitch fans keep their sorted edge path.

Traversal uses a directly recursive lambda instead of `std::function`. Selected
tiles use the frame arena, and published patch/ID vectors reserve from the known
selection size. A nested-call-safe scratch lease supplies a fixed 384 KiB buffer
per render thread. Arena objects are destroyed before releasing the lease;
published meshes own their data. Larger preparations spill into frame-local heap
blocks, which are released at return. The buffer retains capacity without
retaining resource handles, and recursive projected fallbacks use an independent
lease.

Two serial 120-warm-up/240-measured-frame D3D12 runs per binary at 800x600 give
the following mean CPU terrain LOD times. Both binaries include matching phase
diagnostics and no global allocation overrides. These local baselines are higher
than the historical 1.3-1.5 ms RPG travel captures; use the paired comparisons.

| Travel mission | Before, ms | After, ms | Reduction |
| --- | ---: | ---: | ---: |
| Raindance | 0.683 | 0.470 | 31.3% |
| Broadside | 1.194 | 0.755 | 36.7% |
| RPG map 1 | 1.996 | 1.297 | 35.0% |
| RPG map 5 | 2.323 | 1.499 | 35.5% |

Rotating cases improve 36.6-39.4%. RPG travel visibility/window work stays near
0.011 ms, so its rasterization was left unchanged. Traversal/validation falls
from 0.320/0.432 to 0.269/0.367 ms; native edge preparation falls from
0.317/0.345 ms to under 0.001 ms. Patch/texture emission falls from 1.327/1.511
to 0.999/1.102 ms, including the avoided per-tile edge searches.

RPG travel previously collected 9,804/10,699 edge entries and allocated about
662/664 KiB in six upstream arena blocks per frame. All measured candidate cases
have zero upstream arena allocations, with 384 KiB retained per thread. These
counters describe terrain arena traffic, not all renderer allocations; the old
selected-tile vector was outside that arena, while the new vector is included.
Node visits, validated heights, material checks, first-writer requests/reached
points, polygon/draw counts and upload bytes match across all four runs of each
case. All 12 final mission/camera readbacks match byte for byte.

The 27-view regression corpus fingerprints selected square order, topology keys,
and exact authored/target first-writer heights across repeated blocks with unequal
seams, holes, pins, moving/rotated cameras, orthographic footprints, and clipped
regions. It also compares reconstructed compact and projected geometry, checks
native software geometry, and exercises failure cleanup and nested fallbacks.
All 13 Release and 11 headless CTest checks pass, as do the real-device D3D12 and
Vulkan platform cases and installed RPG acceptance suites. The initial Debug
run exposed a preexisting Windows archive replacement bug, reproduced in a
core-only executable: the rename fallback lacked an explicit filename terminator
and could create a garbage-suffixed sibling instead of replacing the archive.
A subsequent fix reserves the terminator and tests four filename lengths;
all 13 Debug CTest checks now pass, along with Release and headless rechecks.
The timing captures predate that independent archive fix.
Raw per-run mean/p95/p99, work counters, executable/capture/readback hashes and
the validation history are in
[`render-terrain-selection.json`](data/render-terrain-selection.json).

## Draw submission follow-up (2026-11-03)

The later fog correction applies haze in the material fragment shader and removes
the separate haze draws described in the historical measurements below. A late
terrain haze overlay could pass the depth test over alpha-blended foliage because
those leaves intentionally do not write depth. Fog now follows alpha discard and
precedes blending, and translucent TS faces retain their own haze. Additive and
subtractive effects fog toward black; multiply effects fog toward white. Native
terrain distances, clipping, depth modes, image sorting and layer order are
unchanged. GPU regression readbacks cover foreground foliage, cutout holes,
nearer occluders, sky, partial/full haze and all five blend modes in both projected
and retained paths. The measurements below predate this correction.

Native terrain patch instances now carry their lightmap-page rectangles and
texture-array layers. Flat shader outputs retain those values per primitive,
including source-UV clamping before the page transform. Adjacent compatible
patches can share an instanced draw even when their logical textures or maps
differ. Physical texture, sampler, material, blend and depth changes still split
runs; base/haze passes and composed layer boundaries retain their original order.

All retained mesh transforms now come from the existing instance storage buffer.
This also permits compatible adjacent instances of ordinary retained meshes to
batch. Vertex uniforms shrink from 272 to 32 bytes. Each mesh instance grows from
256 to 288 bytes for sampling metadata; staging limits and resource accounting
include that increase. Projected and other retained materials keep their existing
per-material sampling controls.

A fresh serial Windows Release D3D12 pair measured 240 frames after 120 warm-up
frames at 800x600, using matching diagnostics in both executables:

| RPG case | Mean draws, before -> after | Mean submission ms, before -> after |
| --- | ---: | ---: |
| Map 1 stationary | 1,931 -> 1,495 | 1.221 -> 1.005 |
| Map 1 rotate | 1,029 -> 873 | 1.501 -> 1.298 |
| Map 1 travel | 1,008 -> 906 | 3.333 -> 3.208 |
| Map 5 stationary | 2,038 -> 1,578 | 1.205 -> 1.049 |
| Map 5 rotate | 1,442 -> 1,320 | 2.049 -> 1.895 |
| Map 5 travel | 1,394 -> 1,309 | 3.267 -> 3.078 |

Map 5 stationary vertex-uniform pushes fall from 799 to 339 per frame;
fragment-uniform pushes fall from 1,858 to 1,180. Vertex-uniform payload falls
about 95% when accounting for the smaller block. Input draws, polygons, sampler
and pipeline binds, and vertex/mesh/texture upload bytes match. Submission means
improve in all twelve base/RPG cases, but full-frame results are mixed, including
a stationary Broadside regression. These single-pair CPU measurements include
the preceding fence wait and do not establish a universal full-frame speedup.

All twelve final mission/camera readbacks match byte for byte. All 13 Release
CTest checks and both real-device platform cases pass on D3D12 and Vulkan.
Six new submission scenarios cover array/page batching, overlapping alpha,
cutout depth writes, alternating depth policies, composed haze layers, and
ordinary retained mesh instances with adjacent index ranges and a later reused
transform. They compare against separately submitted or standalone-texture
references and check actual draw/uniform counts; patch cases also exercise cache
recreation. Updated DXIL/SPIR-V artifacts are generated from HLSL, and matching
MSL sources are maintained. Metal device validation remains outstanding.

Per-case mean/p95/p99 timings, counters, executable provenance and readback hashes
are recorded in [`render-submission.json`](data/render-submission.json). The
benchmark now reports input draws, pipeline binds and both uniform-push counts.

## Terrain seam precision (2026-11-04)

The retained terrain path introduced in `8207997` folded each patch's origin into
its camera translation on the CPU. Adjacent patches then transformed a shared
endpoint through different floating-point expressions. At some camera angles,
the projected endpoints rounded to different rasterizer subpixels, exposing
single background pixels through otherwise continuous terrain.

Terrain instances now carry their origins separately. The vertex shader first
reconstructs shared mesh coordinates, then applies the common camera transform;
CPU patch admission uses the same coordinates. HLSL, committed DXIL/SPIR-V and
the Metal source use the matching layout. The instance stride grows from 288 to
304 bytes, including existing sampling metadata.

A real-device coverage regression renders an opaque tiled plane at map-sized
coordinates through 12 rotated downward views. The preceding path leaves six
uncovered pixels at the first failing angle. The corrected path has no uncovered
pixels in all 12 views on Windows D3D12 and Vulkan. Matching `rpgmap5` captures
from the Wasteland installation also fill a previously uncovered terrain pixel.
All 14 Release CTest checks pass.

This coverage fix does not remove bright texels authored into the terrain artwork.
A follow-up `rpgmap5` capture at 1600x1200 identifies such a speck in `lnnnn.bmp`,
source coordinate (82,72): index 248 in subpalette 506 is RGB (255,253,254).
After lighting, the corresponding screen pixel is RGB (143,141,141) in the
retained path and (143,141,140) in the projected compatibility path. Both the
bitmap in `lushDML.vol` and the palette in `lushWorld.vol` match the original
Tribes installation; the complete respective archive SHA-256 values match too.
Terrain-only sweeps through 360 moving/rotating views at both 800x600 and
1600x1200 find no isolated uncovered pixels after the seam fix. The reproduced
texture speck remains visible with solid terrain coverage, so eliminating it
would require changing the artwork or its filtering rather than seam projection.

## Transient terrain coverage during rotation (2026-11-04)

Issue #15's large, brief missing regions were a separate CPU visibility defect.
When a footprint vertex lay on intersecting camera planes, `PlaneF::split` could
classify it outside but reject the corresponding segment intersections because
the independently rounded float calculations disagreed. Dropping both crossings
removed a whole corner of the visible ground footprint. A tiny camera rotation
restored it, explaining why fast flight or looking around exposed the background.

Terrain footprint clipping now computes signed distances in double precision and
uses those same distances to interpolate crossings. It does not discard a crossing
after rounding its position. Projecting the height range onto the ground plane
also consistently negates the selected height extremum, including negative minima.
The general-purpose plane/collision implementation remains unchanged.

An asset-free CPU regression checks ground-ray admission through nearby rotated
views at every LOD. A real-device regression independently intersects screen rays
with a known plane, then checks both projected and retained terrain readbacks.
The original implementation exposes 60,400 expected ground pixels in a 400x240
frame at the failing angle. The fixed implementation has zero uncovered expected
ground pixels throughout the tested rotations on D3D12 and Vulkan. A diagnostic
sweep of 40,000 camera poses also reports zero rejected in-view ground samples.

All 16 Release CTest checks pass. These cases exercise terrain selection, unlike
the earlier seam coverage regression, which constructs already-selected patches.

## Reproduction

```powershell
cmake --preset x64-release
cmake --build --preset x64-release --target opent1_memory_audit --parallel 8
.\build\x64-release\opent1_memory_audit.exe self-test
python tools/run_memory_audit.py --executable build/x64-release/opent1_memory_audit.exe --base C:/Games/TRIBES --rpg C:/Games/PCRPG --output build/profiles/memory-audit --all-missions --gpu --frames 240
python tools/summarize_memory_audit.py build/profiles/memory-audit docs/data/render-memory-baseline.json --date 2026-11-01
```

The target is excluded from the normal build. The same build supports
catalog/load-only measurement and GPU measurement. Omit `--all-missions` or
`--gpu` to skip those groups. Every run includes
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

### Projected player shadows

The November 4 live investigation measured 17.9 FPS (55.8 ms/frame). Two
1,200-stack captures attributed 48.6-50.0% of sampled main-thread time to shadow
receiver generation and 15.7-17.3% to interior movement collision. The shadow
rework leaves simulation/collision behavior unchanged.

`ShadowReceiverScene` prepares one local-space BVH per immutable interior mesh,
terrain patch set, or explicit mesh, shared by every caster in a pass. The
session cache weakly identifies geometry and retains at most 64 indexes / 16 MiB
of derived index storage. PVS selection and instance placement are applied at
query time. First-input coplanar ties remain deterministic despite BVH traversal.

Receiver projection uses double-precision transforms, planes and light-space coordinates, convex unions
of coplanar polygons sharing exact edges, and an 8x8 projected-bounds index.
Depth and separating-edge tests reject unrelated occluders before clipping.
Polygons use fixed vertex storage; a reentrant thread-local workspace retains
query vectors and uses a 128 KiB arena for the edge table. Exceptional arena
spill allocations are released after each query. Output owns its vertices.
Overlapping geometry can still require quadratic work; candidate, fragment,
vertex and output limits remain enforced, with one 4,194,304-unit work budget
shared across the pass rather than renewed for every caster.

`WorldShadowContext`, owned by the game rendering session, independently caches
receiver geometry and silhouettes using object generations/image slots, resource
and pose revisions, transforms, light direction, receiver selection and occupied
mask bounds. Mission changes and explicit cache trimming clear the context.
Projected size selects 16/32/64-pixel silhouettes and the existing update cadence;
visibility and resolution have hysteresis. The existing box filter is retained.
The receiver footprint is cropped to the filtered silhouette's occupied bounds
plus a zero-texel sampling border. UVs map back into the full bitmap, preserving
the shadow and its blur fringe. The stateless snapshot API retains full-footprint,
64-pixel rendering for exact comparisons.

The asset-free Release benchmark measures 16 receiver queries per frame. A paired
12-frame repeat, after four warm-up frames, gives:

| Receiver workload | Before mean, ms | After mean, ms | Reduction | Failed shadows, before / after |
| --- | ---: | ---: | ---: | ---: |
| Sparse, 8,192 source triangles | 11.34 | 2.07 | 81.8% | 0 / 0 |
| Dense, 512 source triangles | 45.72 | 3.53 | 92.3% | 192 / 0 |
| Stacked, 1,536 source triangles | 2,043.67 | 13.95 | 99.3% | 192 / 0 |

The old dense/stacked path spent its time clipping before rejecting the result.
The new output uses a different triangulation; correctness is checked against an
independent nearest-ray oracle, including holes, crossing slopes, rotated receivers and large world
coordinates, rather than requiring identical triangle lists. A separate allocation
audit records about 2,530 / 2,670 / 3,508 allocations per 16-query frame, including
owned receiver/output publication. Its timings are not performance measurements.

A separate `rpgmap5` roof test adds 32 render-only casters with unchanged poses
and moves them laterally, at 800x600 after 120 warm-up frames. The final D3D12
120-frame capture measures 23.75 ms full-frame mean / 24.88 ms p95 / 25.16 ms p99,
including simulation, preparation, submission and the preceding fence wait.
Receiver work averages 2.91 ms, with 24.41 emitted shadows/frame and no limit
exhaustions. The 60-frame Vulkan capture measures 24.31 / 25.75 / 26.31 ms and
3.04 ms receiver work, also without limit exhaustion. This fixture is distinct
from the live player's session; these numbers do not establish its new FPS.
Raw numeric results and provenance are in [render-shadow-rework.json](data/render-shadow-rework.json).

Validation includes all 14 Release CTest suites, a newly linked standalone smoke
run, native D3D12/Vulkan GPU tests, cropped/full-footprint software image comparison,
cache invalidation/update-cadence checks, and native crowd readback/trim/software
acceptance. Cache trimming preserved both backend readbacks exactly; D3D12 also
completed a transition to `rpgmap1`.

```powershell
cmake --build --preset x64-release --target opent1_shadow_benchmark opent1_shadow_allocation_audit opent1_render_benchmark
./build/x64-release/opent1_shadow_benchmark.exe build/profiles/shadow-current.json 60
./build/x64-release/opent1_shadow_allocation_audit.exe build/profiles/shadow-allocations.json 8
```

For installed-content crowd runs, set `OPENT1_SHADOW_BENCHMARK_ORIGIN` to three
space-separated world coordinates and `OPENT1_SHADOW_BENCHMARK_SHAPE` to an
installed DTS filename. `OPENT1_SHADOW_BENCHMARK_MOVING=1` moves the casters while
the stationary camera remains fixed. The ordinary benchmark reports full-frame
p50/p95/p99, shadow stage timings, attempted/emitted casters, cache hits and limit
exhaustions. Allocation fields are null in the timing executable. `--render-stats`
exposes corresponding counters in interactive gameplay.
