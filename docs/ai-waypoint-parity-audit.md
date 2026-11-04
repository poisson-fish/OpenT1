# AI waypoint parity and the Keldrin Mayor

Audited 2026-11-04 against OpenT1 `eb378bd`, recovered Tribes source
`../TribesRE/legacy/tribes-rebirth` (`1105fd0`), and the installed
`C:/Users/udidi/Documents/Code/Wasteland` content. This extends the
[bot behavior audit](vanilla-bot-behavior-audit.md) and
[lifecycle audit](vanilla-bot-lifecycle.md).

## Findings

### W01 — P1: The installed Mayor patrol was absent from normal startup — corrected in the mod

The map5 Mayor is a normal spawned combat AI, initially given scripted
`botAttackMode=1` (general wandering). The separate `RPG/Taurik_Mayor.cs` file
changes that script variable to 3 and supplies the town patrol destinations.
Before correction, it was not loaded by the installation's normal server
startup. The baseline diagnostic checked the actual running console:
`Taurik::Mayor` did not exist, and `$TaurikMayorRunning` was empty.

Consequently `AI::Periodic` calls `AI::SelectMovement`, which can call
`AI::moveToFurthest`. That routine casts horizontal rays and installs the
furthest hit as waypoint 99. These are long wandering destinations, not the
coordinates authored in the Mayor script. The engine faithfully walking toward
one of these destinations can take the Mayor far outside town.

Installed source:

- `RPG/MISSIONS/rpgmap5.mis:2413`: Mayor spawn marker at `-2343 -308 65`.
- `RPG/Ai.cs:81`: initial script movement mode 1.
- `RPG/Ai.cs:417`, `1157-1193`: periodic movement selection and mode dispatch.
- `RPG/Ai.cs:674-717`: furthest-visible-point wandering.
- `RPG/Taurik_Mayor.cs:48-106`: mode 3 and timed patrol destinations.
- `RPG/server.cs` and `RPG/Taurik_Server.cs:116-184`: startup script loading.

This is an installed-content finding. No original executable was run, and this
does not establish that vanilla would automatically load this otherwise
unreferenced file. No Mayor-specific engine exception was added.

Following the audit, the installed `RPG/Taurik_Server.cs` now executes the
unchanged `Taurik_Mayor.cs` from `Server::finishMissionLoad` on rpgmap5. It resets
the patrol's mission state after the old console scheduler has been replaced,
so a mission reload starts one fresh loop. The authored route and movement
function are unchanged. Loading `exec("Taurik_Mayor.cs");` in the server console
also enables the existing script in a session that was already running before
this startup edit. The original startup file is backed up at
`build/wasteland-mayor-startup/Taurik_Server.before.cs`.

### W02 — P2: Commands ignore a bot's display name — corrected

Native `AIObj::nameMatches` (`aiHelp.cpp:70-79`) matches both the internal AI name
and the body's displayed name when the names differed at spawn
(`aiObj.cpp:1688-1689`). OpenT1 only matched the internal name. For example,
an AI spawned internally as `AliasBot`, displayed as `PublicAlias`, ignored
`AI::directiveWaypoint(PublicAlias, ..., 99)` and wildcard variable commands.

`AIManager::spawn` now retains the native display-name eligibility flag, and
`buildExpressionList` checks the eligible current display name. A bot matching
both names is returned once. Exact internal-name lookup (`AI::getId`) keeps its
native semantics. The pre-fix regression observed zero directives instead of one.
This is a separate parity defect; it is not presented as the explanation for
the missing Mayor patrol.

### W03 — P2: Legacy single-target attack can reuse directive targets — corrected

Native setup clears its selected directive every frame and does not scan or
select directives while the legacy `AI::Attack` seek flag is set
(`aiObj.cpp:775-790`). OpenT1 skips the scan but reconstructs selection from
cached player directives and unconditional point/laser directives in
`updateDirectives`. Switching to an invalid legacy target can therefore keep
firing at the previous directive target instead of rethinking state.

The route update now receives the setup pass's directive-target eligibility.
Cached player targets and point/laser entries cannot bypass the legacy seek
flag. Capturing the decision before callbacks preserves native setup timing.
Before correction, three assertions reproduced selection/firing at the old
player and firing at a point after setting an invalid legacy target.

### W04 — P2: Installed AI regression silently skips loose-script RPG installs — corrected

The installed hunt regression required `RPG/scripts.vol`. Wasteland supplies
loose scripts and has no such archive, so setting `OPENT1_TEST_ASSETS` to this
installation previously skipped the AI acceptance case. The hunt test now
accepts an RPG directory and lets normal startup validate the resources.
The explicit Mayor diagnostic also loads this installation directly.

Enabling the test also exposed a fixture conflict: Wasteland's `PlayerCage`
wraps every player's X above 1022 to -5118, including on map1. The map1 crypt
hunt now stops that map5-specific boundary loop before creating its character.
Bot AI, armor, weapons, spotting, attack mode and movement remain controlled by
the original scripts. The Mayor map5 traces leave the boundary loop enabled.

### W05 — P2: The mod patrol has additional limits

These are script constraints, not engine parity corrections:

- `Taurik_Mayor.cs:16-25` searches only representative IDs 2049 through 2099 and
  recognizes two exact display names. Native supports a larger representative
  pool. A Mayor outside that range or with a duplicate-name fallback is missed.
- The script advances by elapsed time, not waypoint arrival. Its one-second
  schedule changes desired positions; the AI's five-second periodic callback
  installs them later. Short six-second legs can have substantially less time
  for actual travel. This can cut corners even with source-matching steering.
- `AI::moveToFurthest` uses visibility, not walkability. Neither it nor the native
  ordinary waypoint follower promises avoidance of water, cliffs, or obstacles.

The timing and representative-range consequences are source-derived risks;
they are not claims of original-binary reproduction.

## Coverage and compatibility boundaries

| Area | Result |
| --- | --- |
| Directive parsing, sorting, replacement, removal, callbacks | Rechecked `aiPlugin.cpp`, `aiHelp.cpp`, OpenT1's plugin and objectives. Native callbacks are synchronous, additions reset callbacks, and omitted-order insertion retains the historical hundred bucket. |
| Path modes | Header ABI is 0 one-way, 1 two-way, 2 circular. Contrary script comments do not change that ABI. Three-point transitions and endpoint behavior have executable checks. |
| Arrival and speed | Native 1.2-unit XY waypoint arrival, 1.0-unit seek stop, 1.7-unit slowdown, 0.2-unit movement threshold, IQ speed scaling and armor axis speeds are retained. Authored radius is not used as walking arrival tolerance by native. |
| Steering and physics | Eight physical travel tests include unequal forward/back/side speeds, turning, ground contact and the delayed Player input pipeline. Each must reach its goal and remain within a two-unit corridor. |
| Route edits | Repeated removal/recreation of order 99 works before arrival and after completion. Existing checks cover lower-order follow priority and synchronous callbacks deleting/replacing routes. |
| Follow and targets | Live owned-player lead centers, global seek offset, LOS scheduling, acquisition/loss/death, attack modes, movement compensation, leading and triggers were rechecked against the prior audit. Legacy single-target suppression is W03. |
| Manager and lifecycle | All-bot pre/post phases, cycling target enrollment, global recomputation, representative ownership, death renaming and body deletion remain covered by the existing source-derived and installed tests. |
| Script utilities | Vector rotation and LOS transforms/masks match the recovered implementations. `GameBase::getLOSInfo` takes an object reference in native; it does not independently resolve a representative ID. |
| Graph navigation | Recovered `aiGraph.h` disables `INCLUDE_AI_GRAPH_CODE`; ordinary native walking does not invoke graph search. OpenT1 graph/Dijkstra helpers are extensions. |
| Jet navigation | Opt-in development path. The port bounds the two initialized flight segments rather than reading the legacy uninitialized tail. No retail flight-equivalence certification is claimed. |

The two-way fallback after deleting an active waypoint uses the filtered route.
Legacy accidentally indexes the unfiltered directive list in two boundary cases
(`aiObj.cpp:349-360`), which can select a non-waypoint with interleaved target
orders. This historical bug is not copied into the safe route representation.

Synthetic expectations are translations of recovered source, not recordings
from vanilla. Installed traces demonstrate this port and this content. Broader
terrain, interior collision and arbitrary mod-script correctness are not proven
by straight-path or town-route tests.

## Reproduction

The existing `ai_parity` CTest invokes the expanded
`tools/ai_behavior_audit.cpp` synthetic suite. Mayor traces use normal frames,
normal zone-based spawning and an isolated writable profile. The optional
`patrol` argument explicitly loads the installed patrol into that test server;
it does not edit the installation.

```powershell
cmake --preset x64-release -B build/ai-waypoint-audit
cmake --build build/ai-waypoint-audit --target opent1 opent1_tests --parallel 8
ctest --test-dir build/ai-waypoint-audit --output-on-failure
.\build\ai-waypoint-audit\opent1_ai_behavior_audit.exe --mayor ..\Wasteland 4000
.\build\ai-waypoint-audit\opent1_ai_behavior_audit.exe --mayor ..\Wasteland 4000 patrol
$env:OPENT1_TEST_ASSETS = (Resolve-Path ..\Wasteland).Path
.\build\ai-waypoint-audit\tests\opent1_tests.exe --suite trpg
```

## Validation

Built the headless Release application, tests and diagnostic under
`build/ai-waypoint-audit`, independently of the shared Debug/Release builds.

- All 12 regular CTest suites passed, including `game`, `simulation`, and
  `ai_parity`.
- All 74 synthetic comparisons passed, including 40 additional comparisons
  beyond the earlier behavior audit. Walking tests simulate 250 ticks in each
  of eight bearings, rather than just inspecting input signs.
- The installed Wasteland moving-target hunt passed: live follow tracking,
  physical pursuit, firing and player damage were all required.
- The separate installed hunt diagnostic passed all 77 comparisons (74
  synthetic plus pursuit/range, firing and damage). The player took 0.461538
  damage over the trace.
- The other three `trpg` cases still have their own archive-only asset gates
  and skip this loose-script installation. Their printed PASS results are not
  counted here as executed installed checks.

Before the startup correction, both Mayor traces ran 4,000 ordinary 32 ms
frames (128 simulated seconds), with natural zone spawning and one Mayor at a
time:

| Measurement | Normal startup | Explicitly loaded patrol |
| --- | --- | --- |
| `Taurik::Mayor` loaded | No | Yes |
| `$TaurikMayorRunning` | Empty | 1 |
| Script movement mode | 1: wandering | 3: requested patrol position |
| Greatest XY distance from spawn | 234.199 units | 128.869 units |
| Lowest body Z | 5.593 | 52.317 |

At frame 704 in normal startup the engine destination was
`-1954.029 -319.764 66.817`, almost 389 units east of the spawn marker; the patrol
script's destinations stay between X=-2364 and X=-2263. Loading the script
removed those distant wandering goals. This is acceptance evidence for the
installed patrol, not a guarantee of arbitrary obstacle avoidance or timing.

After adding the mod startup call, a fresh 1,000-frame trace without the
diagnostic's `patrol` argument reported `mayorFunction=1 running=1` at startup.
The naturally spawned Mayor selected script movement mode 3 and reached the
authored `-2337 -299 65` destination. This is recorded in
`build/ai-mayor-startup-hook.log`.

Logs:

- `build/ai-waypoint-ctest-final.log`
- `build/ai-waypoint-isolated-before.log` (the reproduced legacy-attack defects)
- `build/ai-waypoint-new-before.log` (the reproduced display-name defect)
- `build/ai-waypoint-wasteland-hunt.log`
- `build/ai-wasteland-hunt-trace.log`
- `build/ai-mayor-natural-final.log`
- `build/ai-mayor-patrol-final.log`
