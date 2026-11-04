# Vanilla bot behavior audit and corrections

Baseline audited 2026-11-03 against OpenT1 `ac33a9e` and
`../TribesRE/legacy/tribes-rebirth`. Corrections implement the native contracts
below without changing installed scripts or assets. The earlier
[lifecycle audit](vanilla-bot-lifecycle.md) covers representatives and ownership.

## Corrections

| Finding | Implemented correction and regression coverage |
| --- | --- |
| B01 | Spot-distance writes only assign the override. IQ/accuracy changes request the manager's global recomputation pulse. Repeated spot writes no longer starve LOS. |
| B02 | Each directive owns its LOS, acquisition and death flags. Replacement and immediate remove/readd create new state. Same-player directives have independent callbacks. |
| B03 | LOS/death callbacks are dispatched only on the actual player target directive. A laser owned by that player receives no player LOS callback. |
| B04 | Five-tick LOS work rotates through directives in order, including point and laser entries. |
| B05 | Target and waypoint callbacks execute synchronously before move submission. Stable directive identities and deferred AI destruction protect removal/replacement, including callbacks deleting their own AI. Routes are rebuilt after waypoint callbacks. |
| B06 | Native fixed pitch step, reduction and minimum are restored, independently of IQ. |
| B07 | AI bodies consume the previous movement input while trigger edges still consume the incoming input, matching other players and native code. |
| B08 | Automatic enrollment refreshes one cycling AI per server pass. All-bot pre-loop and post-loop phases surround per-bot movement. |
| B09 | Player lead center and velocity delegate to the mount. Follow, spotting and aim leading use lead centers, including the shooter's travel-distance origin. |
| B10 | A valid laser point suppresses route movement. Missing laser points resume normal route seeking and turning. A real TargetLaser plus mounted ballistic weapon exercises both branches. |
| B11 | SimMovement rotation uses the native collision-aware path even when passed force=true. Explicit ShapeBase pose setters retain forced placement. Ground-contact recovery and existing movement/projectile regressions cover the distinction. |

The live laser regression exposed an additional omission: `Player` inherited the
base implementation of `getWeaponAmmoType()`, which always returned false.
Mounted-image projectile reporting is now implemented from native
`player.cpp:1267-1279`. This restores actual projectile speed for predictive aim,
laser targeting and weapon consumers. The diagnostic checks a mounted 100-unit/s
projectile rather than relying on a zero-speed fallback.

Jet navigation now initializes saved velocity on its first flight update, uses
native interpolation without clamping, and selects accurate movement distances.
The initialized two-segment path is bounded at its endpoint; the legacy
experimental third segment references an uninitialized point and is not copied
as undefined behavior. No generic obstacle-avoidance planner was added.

The source-derived diagnostic is now a regular `ai_parity` CTest dependency of
`opent1_tests`. It includes actual grounded pursuit into melee range, callback
reentrancy, target replacement, mixed target scheduling, laser movement, pitch,
input delay, mounted lead data and collision-aware rotation. The installed RPG
hunt test uses normal application frames, moves its target during pursuit, and
requires acquisition, firing and damage.

### Corrected interpretation of the stationary hunter

The original audit's stalled scene spawned its bot at `2248 785 200`, directly
inside the map's StaticShape crystal named `2 500000`. Its authored position is
`2248.5 785.428 196.908` (`RPG/MISSIONS/rpgmap1.mis`, crystals group). Collision
tracing found repeated contact with that crystal and the crypt floor. This
reproduction did not demonstrate a separate AI movement defect; moving through
the crystal would itself violate collision behavior.

The corrected acceptance scene starts the bot at `2248 775 200` and the player at
`2248 755 200`, south of the crystal, with the original scripts selecting the
weapon, spotting distance and attack mode. In a full 1,250-frame run it pursues,
acquires, fires and damages the player. The synthetic terrain fixture separately
requires physical travel into three-unit range. The installed moving-target
regression also requires damage, rather than just a follow destination.

Native attack mode 1 still has its intentional 17-unit keep-away rule. Native
follow still uses the script's global seek offset. A melee-range bot can therefore
alternate between scripted approach and native retreat; those rules have not
been replaced with mod-specific exceptions.

## Baseline audit record

The findings below describe the **pre-correction** code at `ac33a9e`; their source
line numbers refer to that baseline. Original timing traces are retained as
historical evidence. Current behavior and checks are described above.

## Method and limits

Compared the complete native AI decision, directive, callback, targeting, aiming,
movement, manager, spawn/death, console, graph, and jet-skill implementation:

| Legacy source | OpenT1 counterpart |
| --- | --- |
| `program/code/aiObj.cpp` | `src/game/ai.cpp`, `src/game/aiObjectives.cpp`, `src/game/aiManager.cpp` |
| `program/code/aiHelp.cpp` | Same files plus `include/opent1/game/ai.hpp` |
| `program/code/aiPlugin.cpp` | `src/game/aiPlugin.cpp`, `src/game/aiManager.cpp` |
| `program/code/aiSkill.cpp` | `AIObj::JetSkill` in `src/game/ai.cpp` |
| `program/code/aiGraph.cpp` | `AIGraph` in `src/game/aiObjectives.cpp` |
| `program/code/playerUpdate.cpp`, `player.cpp` | `src/game/playerUpdate.cpp`, `player.cpp`, player/base headers |
| `darkstar/Sim/code/simMovement.cpp` | `src/sim/sim_collision_movement.cpp` |
| `program/exe/base/scripts/ai.cs` | Script-facing API and installed RPG script acceptance |

All legacy line references below are relative to that checkout. Its AI headers
are absent, but the adjacent `../TribesRE/src/legacy_inc/aiobj.h` and `aiGraph.h`
provide the header contracts used to resolve the path-number and development
flag questions. No original executable was run. Synthetic expectations are translations of the
cited source, not recordings from vanilla. The installed RPG trace is acceptance evidence for
this port, not proof of what the original binary does at those coordinates.

## Findings

### B01 — P1: Updating spot distance can permanently prevent acquisition

OpenT1 `src/game/ai.cpp:397` calls `scaleIQVariables()` when setting `spotDist`.
That routine resets `losThinkCounter_` at line 302. `findTargets()` only performs
an LOS query once the counter reaches five (`aiObjectives.cpp:478`). Setting the
range every tick, even to the same value, therefore prevents every LOS query.
The bot never acquires an otherwise visible, nearby enemy.

Legacy `aiObj.cpp:187` binds `spotDist` directly to `iq.spotDistOverride`, with no
recomputation callback. IQ and weapon-accuracy tuning have separate recomputation
bindings; the spot-distance override does not reset LOS scheduling.

Reproduction: install one target, set `spotDist=50` before each of 20 ticks.
Observed **zero** acquisitions; the legacy source permits acquisition at tick 5.
Fix direction: assign the override without recomputing IQ or resetting LOS.
Also preserve the legacy distinction between local variable assignments and the
manager's deferred global recomputation pulse.

### B02 — P1: LOS and death history have the wrong lifetime

OpenT1 stores `visible`, `seen`, and `dead` in `targetVisibility_`, keyed only by
representative ID (`ai.hpp:429`, `aiObjectives.cpp:413`). Native flags belong to
each target directive (`aiObj.cpp:505-622`); directive construction/replacement
creates fresh state (`aiHelp.cpp:44`, `228`).

This changes three observable contracts:

- Replace a visible target directive at the same order: its fresh acquisition
  callback is suppressed.
- Remove and recreate that directive before the next AI tick: its old LOS history
  survives, and acquisition is suppressed again.
- Add two directives for the same player: OpenT1 checks one shared state and fires
  both callbacks together. Native checks one directive per LOS slice.

The runner observes zero new acquisitions after both replacement and immediate
remove/readd, versus one expected. Two same-player directives both acquire at
tick 5, versus native acquisition at ticks 5 and 10. The shared death flag has the
same lifetime problem on reauthored targets, although that branch was not given a
separate runtime probe. Fix direction: keep all target flags on the directive,
including its identity across replacement, instead of a player-wide cache.

### B03 — P2: Player LOS events also invoke laser-directive callbacks

The callback fanout in `aiObjectives.cpp:435-451` matches all target directives
with the same player ID, without checking `targetMode`. A player target plus a
laser target owned by that same player produces two acquisition callbacks for
one player LOS query. Native `aiObj.cpp:548-568` deliberately invokes these
callbacks only for player targets; point and laser targets just set their flags.

The diagnostic observes two callbacks at tick 5, versus one. This can duplicate
script follow, weapon, sound, or target-bookkeeping side effects.

### B04 — P2: LOS scheduling skips point/laser directives and directive order

Native `scanTargetList()` cycles through target **directives** in order, including
point and laser entries (`aiObj.cpp:505-576`). OpenT1 constructs an eligible list
of unique player IDs and cycles through that instead (`aiObjectives.cpp:413-506`).
A point target at order 100 and player target at order 200 should spend the first
five-tick slice on the point. OpenT1 acquires the player during that slice. Its
state-vector insertion order can also survive directive reordering. Consequently,
the statement that LOS runs every five ticks is insufficient to establish parity;
the unit and order of scheduling differ.

### B05 — P2: Acquisition and waypoint callbacks run after this bot's movement

Native `checkCallback()` executes immediately (`aiHelp.cpp:505-532`). Target
callbacks execute during setup, before path destination selection and the move
(`aiObj.cpp:775-827`, `1798-1808`). OpenT1 queues them while `processing_` is true
and flushes after `AIObj::doServerProcesses()` has already submitted the move
(`aiManager.cpp:298-322`, `348-361`). Waypoint callbacks are likewise recorded
after route traversal (`aiObjectives.cpp:796`).

When acquisition installs a follow directive, its destination is available in
that same native setup pass. In the runner, the target's follow Y destination is
23, but OpenT1 still reports its old destination of 0 at the acquisition tick.
LOS-loss callbacks similarly cannot cancel the move already submitted that tick.
Waypoint scripts cannot affect the remaining traversal during their callback.

Reentrancy needs a safe implementation, but deferring the effects until after
movement is an observable behavior change. Native only reserves the separate
post-loop phase for death and periodic callbacks.

### B06 — P2: Pitch aiming is substantially slower and now varies with IQ

Native `pitchAtTarget()` uses fixed pitch constants: increment 0.084, reduction
threshold 0.168, minimum 0.03 (`aiObj.cpp:81-85`, `1118-1140`). OpenT1 `aimAt()`
uses `iqVars_.pitchInc` (`ai.cpp:649-654`), with increments 0.008 to 0.03.

For a point 20 units ahead and 5 above the bot's center, the runner obtains a
pitch step of **0.030 at IQ 100** and **0.008 at IQ 0**, versus **0.084** in both
native cases. This delays trigger eligibility when targets change elevation.
Both ordinary and laser targeting use the changed helper. The existence of an
IQ pitch increment in the old data structure does not mean the active native
firing code used it.

### B07 — P2: AI movement bypasses the native one-input delay

Native `Player::serverUpdateMove()` updates triggers from the incoming move,
simulates movement with `lastPlayerMove`, then saves the incoming move
(`playerUpdate.cpp:143-187`). There is no AI exception.

OpenT1 saves the incoming move **before** movement when `aiControlled_` is true
(`src/game/playerUpdate.cpp:56-65`). A first submitted yaw of 0.25 immediately
rotates the bot by 0.25; vanilla would apply neutral movement first. This affects
turning, path arrival, aiming, and the interaction with `moveTowardLoc()`'s turn
compensation. Trigger handling must retain its separate incoming-move timing.

### B08 — P2: Automatic target enrollment runs for every bot every tick

Native manager refreshes automatic enrollment for one cycling AI per server pass
(`aiObj.cpp:1788-1796`, `aiHelp.cpp:140-188`), after the all-bot pre-loop phase.
OpenT1 performs the full representative scan inside every bot's `findTargets()`
each tick (`aiObjectives.cpp:388-411`). With two automatic bots, the runner sees
both lists populated in the first tick; native refreshes one list.

This changes enrollment/team-change latency and callback order, and multiplies
the enrollment scan cost by the bot count. Pre-loop, enrollment, per-bot movement,
and post-loop phase ordering should be restored together.

### B09 — P2: Mounted-player follow and projectile leading use body data

Native `Player::getLeadCenter()` returns the mount position while mounted, and
`getLeadVelocity()` delegates to the mount (`player.cpp:1359-1372`). Follow,
distance tests, selection, and aim leading use those methods. OpenT1 has no player
lead-center equivalent, repeatedly uses body box center, and inherits body linear
velocity (`aiObjectives.cpp:470`, `516`, `562`; `ai.cpp:556-569`;
`include/opent1/game/game_base.hpp:289`).

In a fixture with a moving mount, observed target lead X velocity is 0 rather
than 10, and the follow destination differs from mount position plus offset by
58.321 units. The fixture sets a mount link without running mounted-pose updates
to expose which API is used; 58.321 is not a claim about the steady offset of a
normally animated passenger. The missing delegation remains a source difference.

Even unmounted, `leadPosition()` measures travel distance from the bot's linear
origin, rather than its lead center (`ai.cpp:564`; native `aiObj.cpp:1088-1093`).
Fix direction: restore lead-center/velocity accessors and use them consistently.

### B10 — P2: Laser attack movement follows the player-target policy

Native `fireAtLaserTarget()` only resumes path seeking when it could **not** aim
at a laser point (`aiObj.cpp:1221-1333`). When aiming, it remains still. OpenT1's
generic `attack` lambda applies attack-mode-0 route movement after the laser call
whether aiming succeeded or not (`aiObjectives.cpp:720-742`). When no usable laser
exists, it moves toward the path without the native `seekLocation()` turn.

This is a source-confirmed branch difference. It was not exercised with a live
target laser and ballistic weapon in this audit; that integration case remains
necessary before declaring laser parity.

### B11 — P2: Collision-failure rotation fallback changes native semantics

Both player implementations fall back to `setRotation(..., true)` when stepping
fails (`src/game/playerUpdate.cpp:509`; native `playerUpdate.cpp:814`). Native
`SimMovement::setRotation()` explicitly ignores that `force` argument and calls
collision-aware `setPosition()` (`darkstar/Sim/code/simMovement.cpp:446-452`).
OpenT1 propagates `force` (`sim_collision_movement.cpp:1456-1472`), forcing the
transform and clearing contact instead of doing the native collision pass. It
therefore changes contact/traction recovery after failed movement.

This is a source-confirmed difference, **not a proven complete explanation** of
the installed stall below. An exploratory collision-aware rotation recheck did
not dislodge that hunter. Changing this branch alone must not be presented as a
verified fix for the reported symptom.

## Initial installed RPG reproduction (before diagnosis)

Run original installed `RPG` scripts with `rpgmap1`; create a normal local
character, put it at `2248 765 200`, and spawn `Raider998` at `2248 785 200`
using `SpawnAI(..., "TempSpawn 2248 785 200 1")`. Remove other AI to isolate the
case and disable the script's smart FOV option. Do not override this bot's
weapon, attack mode, spotting range, forces, or movement. Advance 1,250 normal
32 ms application frames, including player/server processing and script schedules.

The script chooses `spotDist=5` and `attackMode=1`. A follow directive appears at
tick 160. By tick 224 the bot remains at approximately
`2247.583 784.984 196.909`, with the player at `2248 765 196.918`:

| Observation | Result |
| --- | --- |
| Distance through tick 1248 | 19.988 units |
| Generated movement after turning | 1.000 |
| Active follow directives | 1 |
| Attacked representative | -1 |
| Trigger / player damage | 0 / 0 |
| Bot linear velocity at ticks 192 and 640 | `0 0 0` |
| Ground contact at those snapshots | false |
| Armor ground force / traction / forward speed / mass | 675 / 3 / 7 / 9 |

These measurements describe the bot embedded in the authored crystal. They
were initially inconclusive; collision tracing subsequently identified that
obstruction, as described in the correction section. The clear-spawn regression
replaces this scene as a pursuit acceptance test.

## Behaviors that match, and boundaries on the claim

| Behavior family | Audit disposition |
| --- | --- |
| Spawn and identity | Same script spawn inputs, AI body flag, representative ownership, cleanup membership, variable bindings; shared-pool/roster details remain covered by the lifecycle audit. Extra OpenT1 `SpawnAtMarker`/respawn helper paths are extensions, not vanilla APIs. |
| Default targeting | Scripted targeting, IQ 0, attack-follow-target, seek offset `3 3 0`, named `Circular` path default match source construction. Numeric path values are unresolved below. |
| Enemy eligibility | Self excluded; team inequality, or at most one team, matches `isEnemy`. Ownership lookup uses the base representative's owned player. |
| States | NeedToThink, DoNothing, FollowingPath, AttackStill, AttackTarget and their broad transitions match; callbacks, phases, and move delay differ as listed. |
| Legacy `AI::Attack` | Uses seekFlag, representative IDs over 2048, think wait, living target, range and eye-to-eye LOS; bypasses directive scanning while active, as native does. |
| Ordinary LOS | Eye-to-eye endpoints; source excluded; projectile/camera/item types ignored; no hit or the target itself counts as visible. Range exit silently forgets LOS history, and return acquires afresh. The runner confirms that last rule. |
| Target choice | Nearest visible player, directive-order processing, point/laser replacement without resetting best player distance broadly match. Per-directive state and scheduling do not. |
| Follow | Live destination rather than a spawn snapshot; `seekOff` applies globally. Parsed follow point/radius do not become an arrival radius in the actual native implementation. Follow directives do not automatically complete. Mounted center differs. |
| Directive priority | Adding a lower-order follow does not preempt the current higher-order waypoint; runner confirms this. Native path index retains the current/last order. An acquisition follow is not an unconditional command to abandon the current path. |
| Attack modes | 0 moves along the current route; 1 follows the target with native 17-unit keep-away; other values stand while attacking. All three synthetic checks match. Laser has a distinct policy and currently differs. |
| Walking | Speed endpoints 0.4/1.0 by IQ, slowdown at 1.7, arrival at 0.2, ordinary seek stop at 1.0, waypoint reach at 1.2 in XY, axis signs, and attack turn compensation match in structure. Physical travel is not proven by those input calculations. |
| Directive handling | Sorted positive orders replace existing entries, default callbacks are reset on add, wildcard removal and automatic-order reservation broadly match. Omitted-order insertion retains the existing hundred bucket in the actual legacy code; do not "fix" it based only on its contrary comment. |
| Aiming accuracy | IQ interpolation, rotating dope, close-range correction, target-relative velocity, and trigger windup/percentage broadly match formulas. Pitch, lead center, and exact-vs-fast distance call sites still differ. No statistical or original-binary accuracy certification was performed. |
| Point targets | Aim and trigger without route movement matches; pitch helper differs. |
| Periodic and death | Explicit periodic enabling/minimum delay, post-move callbacks, corpse renaming, variable removal, and body-deletion cleanup broadly match. Existing installed combat/death tests are narrow acceptance cases. Immediate respawn from native death callbacks is itself documented as unsupported. |
| Guard/bored/ambient/evasion | Guard and ambient are unimplemented/unregistered in native; no active bored-dispatch loop was found. OpenT1 accepting extra setters/commands is not evidence those behaviors should be invented as native functionality. |
| Graph | Native graph connection code is unfinished and compile gated; no graph route search is called by the ordinary native hunt/follow loop. OpenT1's nearest-neighbor graph/Dijkstra implementation is an extension, not evidence of vanilla obstacle avoidance. |
| Jet navigation | Opt-in development code, compile gated in native. OpenT1 rewrites it as two explicit flight segments, clamps interpolation, and saves velocity at a different transition. Source-level full parity is not established; retail enablement cannot be determined without the missing header. Ordinary hunters default to jetNavigation 0. |

## Header and compatibility resolution

Stock `base/scripts/ai.cs:19-22` comments describe 0 circular, 1 one-way and
2 two-way, while OpenT1 uses 0 one-way, 1 two-way and 2 circular. The adjacent
`../TribesRE/src/legacy_inc/aiobj.h` agrees with the implementation's mapping.
The enum was retained; a contradictory script comment does not justify changing
its ABI. That header enables `_JETNAVDEV_` by default, and `aiGraph.h` disables
`INCLUDE_AI_GRAPH_CODE` by default.

Original-binary interoperability, statistical aim equivalence and the undefined
experimental jet tail are outside the validation claim. The corrected runtime
cases exercise the defined source contracts and installed content directly.

## Reproduce and validate

The [audit runner](../tools/ai_behavior_audit.cpp) is excluded from production
builds without testing. With `BUILD_TESTING`, building `opent1_tests` also builds
it, and `ai_parity` runs its synthetic checks. Supplying the installed asset root
adds a normal-frame RPG hunt trace. Exit 0 means all checks match, 1 means a
comparison failed, and 2 means setup failed.

```powershell
cmake --build --preset headless --target opent1 opent1_tests --parallel 8
.\build\headless\opent1_ai_behavior_audit.exe
.\build\headless\opent1_ai_behavior_audit.exe ..\TribesRE\PCRPG
ctest --preset headless --output-on-failure
$env:OPENT1_TEST_ASSETS = (Resolve-Path ..\TribesRE\PCRPG).Path
.\build\headless\tests\opent1_tests.exe --suite trpg
.\build\headless\tests\opent1_tests.exe --suite fixpack
```

A third argument after the asset root limits the trace's frame count for diagnosis;
its full pursuit/damage assertions run for traces of at least 1,000 frames.

The original 20 comparisons had 13 differences. All of those comparisons now
match. Added checks cover the missing weapon query, physical travel, laser
movement, callback deletion and rotation contact. The full clear-spawn trace is
in `build/ai-fixes-clear-hunt-final.log`; it fired and damaged the player during 40
simulated seconds. The installed hunt regression also passed with a moving player.
The old input-only acquisition fixture now repays body move debt and checks
movement in world coordinates as the bot turns.

Validation logs are under `build/ai-fixes-*`. Installed files are unchanged.

### Final validation

- Headless: all 12 CTest suites passed.
- Windows x64 Debug and Release: both applications built; all 14 CTest suites
  passed in each configuration.
- Source-derived diagnostic: 34 synthetic comparisons passed. With the installed
  RPG assets, all 37 comparisons passed, including pursuit, firing and damage
  over 1,250 normal application frames.
- Installed `fixpack`: all four cases passed.
- Installed `trpg`: hunt, regressions and map rendering passed. The combat case
  passed its acquisition, firing and damage assertions, then failed at
  `trpg_behavior_test.cpp:623` while rendering the play GUI. Its runtime error is
  `World scene draw failed`; this result was reproduced in headless and Release.
  The broader installed suite therefore remains 3/4, with that rendering failure
  outside the bot behavior assertions. Concurrent rendering work is not changed
  by these fixes.

Logs: `build/ai-fixes-headless-tests.log`, `build/ai-fixes-debug-tests.log`,
`build/ai-fixes-release-tests.log`, `build/ai-fixes-clear-hunt-final.log`,
`build/ai-fixes-installed-fixpack.log`,
`build/ai-fixes-installed-trpg-release.log` and
`build/ai-fixes-render-diagnosis.log`.
