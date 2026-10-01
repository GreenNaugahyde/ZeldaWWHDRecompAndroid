# WWHD ↔ GameCube decompilation: findings

Function names for WWHD (`cking.rpx`) come from
`python3 tools/decomp/match.py game/code/cking.rpx tww build/names.tsv` (about 1 minute). Needs
`tww/` (zeldaret/tww) built from your own GameCube disc image (`tww/build/GZLE01`). Outputs (all
in git-ignored `build/`):

- `names.tsv`: address, name, source file, evidence, score (match probability for `graph`/`tu`).
- `wwhd_to_gc.tsv`: WWHD address → GameCube mangled symbol, source file, module, GameCube
  address, evidence, both sizes. Stage 2 (checking each function against the source) builds on it.
- `coverage.tsv`: per translation unit, GameCube functions vs. matched.
- `regions.tsv`: long unmatched stretches (HD-only code).

Helper tools: `layout.py` (structure offsets, below), `timers.py` (frame timers, below),
`datamap.py` (WWHD addresses of GameCube globals), `heldout.py` (precision test), `train.py`
(refits the pair-scoring model).

### Evidence and measured precision

| Evidence | Meaning | Count | Held-out precision |
|---|---|---|---|
| `manual` | hand-verified (`tools/decomp/manual_names.tsv`) | 11 | — |
| `assert` | assert text (file + condition) | 191 | near-certain |
| `profile` | actor profile method tables (process-name IDs) | 1,477 | near-certain |
| `strings` | rare string/float literals + file neighbourhood | 353 | high (used as test truth) |
| `vtable` | virtual tables and other function-pointer tables (method tables, PTMF tables) aligned slot by slot; also tables identified through the constructors that store them | 623 | 100% (1,689/1,689) |
| `graph` | call-graph neighbourhood + content model, mutual best, p ≥ 0.6 | 3,732 | 97.1% (271/279); 94.3% strict |
| `tu` | unmatched functions between matched functions of the same file, or between two files (alphabetical link order), same model, p ≥ 0.6 | 6,024 | 99.7% (998/1,001); 97.5% strict |
| `tu-margin` | same windows, decided relative to the file's other unmatched functions (score ≥ −1.5, ≥ 2.0 above the runner-up), after the stages above converge | 755 | 96.8% (30/31) held-out; 98.8% (80/81) offline; **medium confidence** |
| `dup`, `dup-graph`, `dup-file` | WWHD's per-file copies of inline functions: named only when that file's GameCube object has a copy of the same function | 40 | not measured (too few) |
| `callgraph-legacy` | the first call-graph matcher, run last, only where nothing else matched | 785 | ~85–90% (older test); **lower confidence** |

The content model scores a pair with 25 features. It is a conditional logit (`train.py`), fitted
on 8,900 trusted pairs against decoys from the same file, from call-graph neighbours and random.
Since round 2 it includes:
- register-agnostic instruction shape (`shape.py`): opcode histogram cosine, opcode-bigram overlap,
  branch/loop/compare/float-op counts, with prologue/epilogue bookkeeping removed;
- per-class field maps (`field_map`): GameCube field offsets accessed through `this` (or the first
  pointer argument) are translated into WWHD offsets with maps learned from matched pairs (as in
  `layout.py`; actor classes fall back to the fopAc_ac_c map) and compared with the WWHD accesses.

Cross-validated, it names 1,527 of 2,500 test pairs at p ≥ 0.7 with 97.6% precision.

How the held-out test works (`heldout.py`): trusted names (assert/strings/profile + tables) are
hidden at random, the matcher runs from the rest, and each hidden function it names again is
checked. Round-2 figures are summed over five runs: three hide 50% of the `assert`/`strings` names
(functions without table evidence, the harder case), two hide 50% of everything. "Strict" counts
naming a wrapper after its own callee as wrong. In WWHD, an actor's `daXxx_Create` often has
`_create` inlined, so its body is the callee's. The "offline" `tu-margin` figure re-decides 2,714
trusted pairs with each one hidden in turn and the rest known.

Known wrong names are listed with `!` in `manual_names.tsv`. For example, `025200D4` is a tiny
static-instance accessor, not `JAIZelBasic::seStart` (and not `__ptmf_scall`). The compiler runtime
(`PowerPC_EABI_Support/Runtime`) is never used as a match target.

### Structural facts used by the matcher

- WWHD links translation units in **alphabetical order per directory** (`SSystem/c_*`,
  `JAZelAudio`, `d/` with the actors in `d/actor` sorted into it, `f_op`, `f_pc`, `m_Do`, ...,
  JSystem, `dolphin/mtx`). Each unit is contiguous. Function order inside a unit is often
  preserved (c_lib) but not always (d_camera).
- Green Hills vtables: 8 bytes per slot (`delta:index` word, function pointer). A zero header slot
  starts each table. Actor classes gained a virtual destructor in slot 0, so WWHD actor tables
  have one more slot than the GameCube ones. `028F036C` (`li r3,13; b ...`) is the
  pure-virtual stub.
- Identical code folding is not done: a GHS build keeps a copy of each out-of-line inline
  function per translation unit (≈12,300 functions in ≈1,000 identical-body groups; 1,504 copies
  of one 20-byte trivial deleting destructor).
- Function discovery in `tools/recomp` merges functions that are only reached by tail calls
  (`b`) into their predecessor. The matcher splits 1,932 of them (`binmodel.refine_functions`).
  The recompiler's own list is unchanged.

## Camera (for frame interpolation)

| WWHD | Function | Notes |
|---|---|---|
| `024FFA3C` | `camera_execute` | per logic tick |
| `024FFC40` | `camera_draw` | builds the matrices each frame from `view.mLookat` (eye, center, up), `mFovy`, `mAspect`, `mNear`, `mFar`, `mBank` |
| `025F1EAC` | `mDoMtx_lookAt` | view matrix (called from `camera_draw`) |
| `028E9948` | `C_MTXPerspective` | projection; called with view_class fovy/aspect/near/far (manual) |
| `028E91EC` | `PSMTXInverse` | inverse view |

Interpolation hook: the inputs of `camera_draw` (eye/center/up/fovy/bank) are a handful of
values; blending them between two ticks gives the in-between camera.

## Models

| WWHD | Function | Notes |
|---|---|---|
| `025E2DE0` | `mDoExt_modelUpdateDL` | per-model update from actor Draw |
| `027F4FE4` | `J3DModel::update` (structure) | 52 bytes: calls `calc` then `entry`; direct calls, no vtable in WWHD |
| `027F4D5C` | `J3DModel::calc` (structure) | joint/world matrices; called from actor Execute (e.g. `daBoko_c::execute`) |
| `027F4F1C` | `J3DModel::entry` (structure) | |
| `025E2BF4` → `027F55FC` → `027F53CC` → `027FDA54` | likely `viewCalc` → `calcDrawMtx` chain | world × view → draw matrices; to confirm |

| `027F55FC` | `J3DModel::viewCalc` | world → view draw matrices (`027DE8A0`, per screen); from `025E2BF4` (mDoExt_modelEntryDL part) and `daShip_c` |
| `027F5018` | J3DModel UBO update (WWHD) | job on the `update_ubo` thread: world matrices → uniform buffers, concurrent with the main thread's draw |
| `027F273C` | shape packet entry | stores `&world[joint]` for rigid shapes (called from `entry`) |

Layout (WWHD J3DModel): `+0x2C` → joint matrix block (`+0x10` world matrices, 3x4 row-major,
`+0x2C` u16 count), `+0xBC` base scale, `+0xC8` base transform (3x4), `+0x6C` draw-buffer index,
`+0xAC` model data. The painter does not read the view-space draw matrices for skinned/rigid
models seen so far; what reaches the screen is what `027F5018` copies from the world matrices.

WWHD restructured J3DModel (Hexa Drive's HD renderer): the GameCube virtual calls are direct
calls here, so names must be confirmed by call structure rather than vtables.

## Interpolation (runtime/src/interp.cpp)

Logic stays at 30 Hz; the per-frame function runs at 60. Hold passes draw everything at step N+1
and record each model's world matrices; logic passes draw with the camera inputs and every model's
world matrices halfway between N and N+1 (viewCalc and the UBO update run on the halfway values,
the exact ones are restored right after each). Models without a previous step (spawned, not drawn
last time) or that jumped further than 400 units use the current matrices.

Logic stays at 30 Hz. For each rendered tick, record per model (J3DModel pointer) the world
matrices produced by `calc` and the camera inputs of `camera_draw`. For the in-between frame,
replay the frame's GX2 command list with draw matrices recomputed from blended world matrices and
the blended camera. Models without a previous tick (spawned, teleported) use the current matrices.

## True 60 fps (runtime/src/true60.cpp)

Game logic at 60 steps per second ("true 60", Graphics menu / key 7 / `WWHD_TRUE60=1`; exclusive
with interpolation). Experimental, off by default.

### Design

- **Passes.** The interpolation pass structure is reused: the per-frame function runs every vsync
  (swap interval halved). *Full passes* run everything as a normal 30 Hz frame. *Half passes*
  (interpolation's hold passes) skip scene management, counters, process creation/deletion and the
  HD menus, but now also run `fpcEx_Handler`.
- **Per-process gate.** `fpcM_Execute` (`025DF940`) classifies every process before its execute:
  - *60 Hz processes* execute on every pass with the step length `dt = 0.5`.
  - *30 Hz processes* execute on full passes only (`dt = 1`). They are drawn interpolated with
    interpolation's camera and model blending (`fpcM_Draw` `025DF904` tracks the drawing process;
    models and the camera of 60 Hz processes are not blended).
- **Conversion per process and per state.** Link runs at 60 Hz only in the procedures in
  `kDefault` (`true60.cpp`); every other procedure stays at 30 Hz and is interpolated, so
  unconverted actions keep their exact original behaviour. The camera runs at 60 Hz while it uses
  the follow camera.
- **Units.** Every quantity keeps its per-30-Hz-step unit (speeds, rates, thresholds), so the game's
  own comparisons stay valid. Only the places that *accumulate* per step are scaled by `dt`:
  - *Shared primitives* are hooked: they read the step length of the execute in progress
    (`true60::dt()`, thread-local, 1 outside a 60 Hz execute).
  - *Inline per-step code* in converted functions is patched with instruction-level hooks:
    `@ADDR` in `hooks.txt` → `site_ADDR(c)` runs before the instruction at ADDR. This is new in the
    recompiler.
- **Integer per-step amounts** (s16 angle steps, chase steps) are split so that the two half steps
  add up to exactly one original step: the half pass takes the rounded-up half.
- **Step counters** are held: a field counted down (Link) or up/down (camera) by exactly one on a
  half pass is put back. So timers count on full passes only.
- **Input.** Buttons change on full passes only, so every press reaches the 30 Hz processes and the
  menus. Sticks are read on every pass: 60 Hz processes get fresh analog input, and a stick change
  takes effect at the same time as in the 30 fps game.
- **Sounds.** Sound starts on half passes are allowed only inside a 60 Hz execute; drawing code
  stays suppressed as in interpolation. Animation sounds track the frame themselves (each key plays
  once).

### Hooked functions

All are hooks in `true60.cpp` / `true60_link.cpp`, active only while `dt < 1`.

| WWHD | Function | Conversion |
|---|---|---|
| `025DF940` | `fpcM_Execute` | per-process gate, dt, timer and counter hold |
| `025DF904` | `fpcM_Draw` | drawing process (interpolation only for 30 Hz processes) |
| `0200ECD4`…`0200F268` | `cLib_addCalc`, `addCalc2`, `addCalc0`, `addCalcPos(XZ)(2)` | ratio `1-(1-s)^dt`, max/min step × dt |
| `0200F378` / `0200F428` / `0200F474` | `cLib_addCalcAngleS` / `S2` / `L` | native re-implementation: fraction `1-(1-1/scale)^dt`, split integer steps |
| `0200F4FC`…`0200F8D0` | `cLib_chaseUC/S/F/Pos/PosXZ/AngleS` | step × dt (integer steps split) |
| `027F2FC4` | `J3DFrameCtrl::update` | frame += rate × dt |
| `025E3EC8` | `mDoExt_MtxCalcOldFrame::decOldFrameMorfCounter` | counter −= dt (blend from the previous pose stays linear in time) |
| `025D67A8` / `025D6800` | `fopAcM_calcSpeed` / `fopAcM_posMove` | gravity × dt; pos += speed × dt + Δv·(1−dt)/2, where Δv is the gravity the actor's own calcSpeed just applied (see below) |
| `0207A9A0`, `022ED850`, `02055B64`, `0211D2F8`, `025AAE08` | `cLib_calcTimer<u8/u8/s16/s32/s32>` | count on full passes only |
| `02416230` | `daPy_lk_c::setNormalSpeedF` | acceleration argument × dt |
| `025028B8` | `dCamera_c::followCamera` | marks the camera as following (60 Hz) |
| `0282167C` | `JPAEmitterManager::calc` | counted only (statistics) |

The exact-arc term: the 30 Hz game integrates `v += g; p += v`. With half steps,
`p += v·dt + g·dt·(1−dt)/2` (v after the half step's gravity) lands exactly on the 30 Hz positions
at every full step, so jump heights and lengths match. For Link the same is done inline (below).

### Layouts used (WWHD; generated code)

| Structure | Field | Offset | Evidence |
|---|---|---|---|
| J3DFrameCtrl (no vtable in WWHD) | rate, frame, start s16, end s16, loop s16, attribute u8, state u8 | +0x0, +0x4, +0x8, +0xA, +0xC, +0xE, +0xF | `027F2FC4` loads rate +0 / frame +4, switches on +0xE, writes state +0xF, sets the rate to 0 on stop |
| mDoExt_MtxCalcOldFrame | counter, 1/morf, rate, +0x10, +0x14 | as on GameCube (+0x4 … +0x14) | `025E3EC8` |
| fopAc_ac_c | `sub_method` | +0xF0 (GameCube 0xEC) | `fpcM_Execute` → method table +8 = execute |
| daPy_lk_c | `mFrameCtrlUnder[0]` / `mNormalSpeed` / `mCurProc` | +0x5898 / +0x6A14 / +0x65F0 | layout.py; traces |
| daPy_lk_c | `m3522` (combo timer) | +0x6972 | execute `0240D638`: `if (m3522 > 0) m3522--` |
| daPy_lk_c | HD-only s32 countdown, clamped at 0 | +0x8260 | execute `0240CE60` |
| camera_class | `mpMtd` | +0x228 (GameCube 0x224) | method table +8 = `camera_execute` `024FFA3C` |
| camera_process_class | `mCamera` (dCamera_c) | +0x248 | notes above |
| dCamera_c | follow-camera work (`mWork.follow`) | +0x37C | followCamera `02502A44`: `addi r30, r31, 0x37C` |
| dCamera_c | follow work: bezier counter (GameCube m388), turn counter (m38C), charge counter (m392) | +0x380 s32, +0x384 s32, +0x38A s16 | followCamera `0250316C` (m388++ then /80) |
| daPy_lk_c execute | `daPy_Execute` | `0240EBB0` | tail-calls `daPy_lk_c::execute` `0240CDD0` |
| process manager | `fpcM_Execute` → `fpcEx_Execute` | `025DF940` → `025DE58C` | `fpcM_Management` passes `025DF940` to `fpcEx_Handler` |

### Link (`true60_link.cpp`)

Instruction hooks in `daPy_lk_c::posMoveFromFootPos` (`023FCB9C`, GameCube d_a_player_main.cpp:2352):

| Site | Code | Conversion |
|---|---|---|
| `023FCEB0` | `f31_2 = |toe movement|` (f1, from PSVECSquareMag+sqrt) | ÷ dt (the animation advanced dt frames) |
| `023FD338` | `speed.y += gravity * 2.25f` (heavy boots, fmadds) | gravity (f9) × dt |
| `023FD35C` | `speed.y += gravity` | gravity (f9) × dt |
| `023FD39C` | `current.pos += speed` (PSVECAdd) | speed × dt + Δv·(1−dt)/2 (exact arc) |

Timers held on half passes (s16 unless noted): the per-procedure union `m34D0..m34DA`
(+0x6916…+0x6920), `m3522` +0x6972, `m3526` +0x6976, `mTinkleHoverTimer` +0x699C, `m355C` +0x69AC,
`m355E` +0x69AE, HD s32 +0x8260; eye timers through `cLib_calcTimer<u8>` (`0207A9A0`).

Procedures at 60 Hz by default (measured): WAIT, FREE_WAIT, MOVE, ATN_MOVE, SIDE_STEP,
FRONT_ROLL, BACK_JUMP, BACK_JUMP_LAND, AUTO_JUMP, LAND, FALL.

While `dBgS::MoveBgCrrPos` (`024EF968`) carries Link on moving collision (platforms, rafts), Link
runs at 30 Hz together with the platform, for 8 passes after the last carry.
`WWHD_TRUE60_LINK=audited` adds 101 more:
- `tools/true60/proc_audit.py` finds them free of inline per-step code in the GameCube source of
  the procedure itself, or with only timer countdowns.
- Ship, rope, hookshot, carrying and swimming procedures are left out.
- Measured with it: side hop and Z-target movement match; SWIM_UP did not (27.5 steps instead of
  35), so swimming stays at 30 Hz.

`WWHD_TRUE60_LINK=all|none|n,n,..` is for testing.

### Camera (`tools/true60/sites_camera.txt`)

- **Inline smoothing.** `tools/true60/smooth_sites.py` finds per-step smoothing `x += (t - x) * r`
  in the generated code:
  - an `fsubs` feeding an `fmadds` (or an `fmuls`+`fadds`);
  - the addend loaded from, and the result stored back to, the same object field.
  
  `gen_sites.py` turns the list into instruction hooks that use `1-(1-r)^dt` for that instruction
  (and restore the register after it).
- **Converted sites:**
  - 26 sites in `dCamera_c::followCamera`;
  - 12 vector/angle smoothing calls through `cXyz::operator*` (`0201AE48`) and
    `cSAngle::operator*(f32)` (`0200693C`). For these, f1 is the per-step ratio, recognised by the
    `__mi` → `__ml` → add/`PSVECAdd` call pattern;
  - 2 calls in `dCamera_c::Run` (m148 forward-check angle, bank decay).
- **Step counters.** The camera's step counters (`m07C`, `m080`, `m108`, `m118`, `m11C`,
  `mForceLockTimer`, and the follow-work counters) are held on half passes.
- **Other camera modes** (lock-on, talk, event, …) run at 30 Hz. All of d_camera.cpp has 134 inline
  smoothing sites by the same scan.

### Measured against the 30 fps game

Setup:
- Copy of the user's save, Outset pier.
- Scripted input on *game time*: `WWHD_TEST_*` env vars. Scenario time is full logic steps / 30,
  so frame-time hitches don't shift the input.
- Traces: `WWHD_LINK_TRACE`, `WWHD_CAM_TRACE`.
- The 30 fps and interpolation runs are deterministic: interpolation reproduces the 30 fps
  trajectory to 0.00 units.

| Quantity | 30 fps | true 60 | Notes |
|---|---|---|---|
| Running speed (steady) | 16.98 units/step (509 units/s) | 8.496 per half step (509 units/s) | identical |
| Start of a run (2 s) | — | Link ≤ 1.8 units from the 30 fps path | |
| Run animation cycle | 14.0 steps | 14.0 steps | |
| Auto jump off the pier | peak 67.8, 26 steps | peak 67.9, 26 steps | height profile within 1 unit (takeoff point differs by half a step) |
| Side hop (Z-target) | heights 14.4 26.4 36.0 43.2 48.0 50.4 … | identical to 0.1 unit | 16 steps in both |
| Back flip (Z-target + back + A) | heights 16 29 39 46 50 51 49 44 36 25 11, lands after 12 steps | identical heights, lands after 11.5 steps | the landing is found half a step earlier, so the flip ends 10 units shorter (266.6 vs 256.8): the 30 Hz step overshoots into the ground |
| Fall | speedF −1.0 per step | −1.0 per step | |
| Front roll | 16 steps, 22.09 units/step | 15.5 steps, 22.09 units/step | ends half a step earlier |
| Follow camera | — | eye ≤ 4.7, center ≤ 3.9 units from 30 fps while running | with the camera at 30 Hz: ≤ 1.9. Sampling a moving target at 60 Hz changes a discrete smoothing lag by about half a step of motion |
| Logic rates | — | Link 59.9 steps/s; camera 60 while following; ≈5050 executes/s of the other ≈170 processes at 30 Hz | `[true60]` log line every 10 s |
| Pause menu | — | opens and closes; walking afterwards works | |

### Known differences and limits

- **Phase.** A converted action that starts on a half pass ends half a step earlier than at 30 fps
  (roll: 15.5 vs 16 steps). Per-step changes are applied in halves, so ramps lag or lead by up to
  half a step.
- **30 Hz processes are drawn 1/60 s behind** (interpolated between their last two steps), while
  Link and the camera are current. Constant offsets don't jitter. Where Link rides or carries a 30 Hz
  actor (ship, carried objects, moving platforms), he must stay at 30 Hz: those procedures are
  excluded, and Link drops to 30 Hz while moving collision carries him.
- **Collisions with 30 Hz actors** are only checked on full passes: their colliders are only set in
  their execute.
- **Random chances per step** (`cM_rnd() < p` in converted code, e.g. procWait's demo voice) fire
  twice as often.
- **Not converted inside Link's 60 Hz procedures:**
  - wind, whirlpool, ice and conveyor pushes (`m3644`, `m3610`, `m36A0`, `m3730` in `posMove`);
  - the CC push (`mStts.GetCCMoveP`), a per-step correction.
- **Particles** are not converted. Emitters step wherever the game steps them; the rate is in the
  `[true60]` log line.
- **Camera:** only the follow camera is converted; the other modes stay at 30 Hz.
- **Integer angle smoothing** (`cLib_addCalcAngleS` with small divisors) cannot be split exactly.
  Link's heading after a turn differed by ≤ 44 units (0.24°).

### Plan for the rest (estimates for one person with these tools)

- **Global systems, about 1 week.**
  - *Particles at 60 Hz* (JPABaseEmitter calc/calcParticle/calcCreatePtcls: emission rate,
    velocity, lifetime and key frames per dt; WWHD's JPA is partly restructured): 2–3 days. Today
    they step at 30 Hz without interpolation.
  - *World systems in the scene draw* (grass/tree/flower sway, magma, ice): 1–2 days. Today they
    are held at 30 Hz.
  - *Other camera modes* (lock-on, talk, event, …): the same site tooling, 134 inline sites in
    d_camera.cpp, 1–2 days with a trajectory comparison per mode.
- **The rest of Link, about 1 week.**
  - 59 procedures with inline per-step code (`proc_audit.py`: speed changes, angle steps, rope and
    climb movement);
  - swimming buoyancy and the swim meter;
  - Link's items (boomerang, hookshot, arrows are separate actors);
  - riding the ship, which needs the ship (d_a_ship: 97 primitive calls, 94 position/speed stores)
    at 60 Hz first.
- **Enemies, NPCs, objects.** Per actor: the survey line, a site list for its inline physics and
  timers (like `sites_camera.txt`), and a comparison run.
  - Objects and tags with no per-step logic (108 files) can be switched on with a list.
  - Typical NPCs and enemies need 10–40 sites: about 1–3 hours each with the tools, and the
    comparison needs a reproducible way to meet them.
  - Bosses and minigames: 1–2 days each.
  - About 300 files with real per-step logic: roughly 2–3 months for everything. Less if a generic
    pass handles the common `pos += speed` / `speed.y -= g` forms and timer arrays automatically
    (the struct-snapshot timer hold that Link uses, applied to whole actors).
- **Cutscenes and events** (event manager, JStudio `forward(1)`): about 1 week, but only useful
  once the actors that appear in them run at 60 Hz. Until then they stay at 30 Hz with
  interpolation, which is already correct and smooth.

### Per-step logic in the other actors (survey for the next conversions)

`tools/true60/actor_survey.py [d_a_]` scans the generated code of every named actor function and
writes `build/true60_survey.tsv`, one line per GameCube source file. It reports:
- calls to the primitives that true60 already scales;
- inline step counters: a 16/32-bit field loaded, ±1, stored back. The offset is from the base
  register, usually `this`; Link's code often uses `this+0x448`. Example: Link's `-0x652A`
  is `m3522` at +0x6972;
- inline exponential smoothing (smooth_sites.py);
- stores to the position/speed fields of `fopAc_ac_c` outside the primitives.

The counter fields cross-check with the timer catalogue where both exist. Example: d_a_bk
(Bokoblin) `m034C` → +0x464 in both.

| Category | Files | Functions | Primitive calls | Inline step counters | Pos/speed stores | Files with none of these |
|---|---|---|---|---|---|---|
| Link (d_a_player_*) | 3 | 804 | 248 | 27 | 338 | 0 |
| NPCs (d_a_npc_*) | 60 | 3223 | 805 | 51 | 627 | 0 |
| Objects (d_a_obj_*) | 142 | 2062 | 363 | 84 | 211 | 61 |
| Enemies, bosses, effects, items (other d_a_*) | 218 | 3363 | 3393 | 335 | 2351 | 34 |
| Tags/triggers (d_a_tag_*) | 20 | 247 | 5 | 7 | 0 | 13 |
| d_camera.cpp (for comparison) | 1 | 87 | 5 | 6 | 6 | — (134 inline smoothing sites) |

What this means for converting them:
- **Smoothing is solved generically.** Actor code smooths almost only through the cLib primitives:
  0 inline smoothing sites in all d_a_* files, against 134 in d_camera.cpp.
- **Inline physics.** The work per actor is its inline physics: `pos += speed`, `speed.y -= g` and
  similar written out instead of calling `fopAcM_posMove`/`calcSpeed` (3,527 stores, part of them
  initialisation in create/init functions).
- **Timer arrays and phase checks.** Examples: `mTimers[4]`, `g_Counter.mCounter0 & 0x3F`,
  per-step random chances.
- **Seagull example.** d_a_kamome has 56 primitive calls, but also about ten inline position/speed
  updates (`pos += speed`, `pos.y += 5`, `speed.y -= 3`, `speed.y -= 0.8`):
  - d_a_kamome.cpp:206–212, 245–249, 575–577, 827–829;
  - a timer array `mTimers[4]` plus `mRiseTimer`;
  - a timer bit test (`mGlobalTimer & 0x3F`);
  - several per-step random chances.

Most active files (score = primitive calls + 5 × counters + pos/speed stores):

| File | Functions | Primitive calls (already scaled) | Inline step counters (WWHD offsets) | Pos/speed stores | Catalogued timers (field, WWHD offset) |
|---|---|---|---|---|---|
| d_a_player_main.cpp | 786 | 246 | s16-0x3B0, s16+0x3FC, s16-0x424, s32+0x458, s16-0x5B60, s16-0x5B68, s16-0x5B6C, s16-0x652A… | 335 | m34D0 0x6916, m34D2 0x6918, m34D6 0x691C, m34DA 0x6920, m3526 0x6976, m355C 0x69… |
| d_a_bpw.cpp | 32 | 225 | s16+0x562, s16+0x568, s16+0x57E, s16+0x59A, s16-0x59A | 131 | — |
| d_a_gm.cpp | 19 | 90 | s16+0x3F6, s16+0x3F8, s16+0x40C, s16+0x40E, s16+0x410, s16+0x412, s16+0x43A, s16+0x43E | 117 | — |
| d_a_bdk.cpp | 27 | 135 | s16+0x3DC, s16-0x40A, s16-0x40C, s16+0x122C, s16-0x1234, s16+0x269C, s16+0x26A0, s16+0x26A… | 64 | — |
| d_a_bk.cpp | 50 | 93 | s32+0x410, s32+0x414, s16-0x424, s16-0x42C, s16-0x44E, s16-0x464, s16+0x4A0, s16-0x95A, s1… | 90 | m034C 0x0464 |
| d_a_bst.cpp | 25 | 161 | s16+0x1308, s16+0x130E, s16-0x133C, s16-0x30B8, s16+0x30D0 | 43 | — |
| d_a_ship.cpp | 77 | 97 | s16-0x662, s16+0x662, s16+0x66E, s16-0x5B6C | 94 | — |
| d_a_mo2.cpp | 43 | 75 | s16+0x594, s32+0x840, s32+0x844, s16-0x856, s16-0x85A, s16-0x85E, s16-0x860, s16-0x882, s1… | 49 | — |
| d_a_mt.cpp | 19 | 75 | s16-0x466, s16-0x580, s16+0x582, s16+0x586, s16+0x588, s16+0x5AC, s16+0xD1E, s16-0x1A18, s… | 63 | — |
| d_a_ph.cpp | 23 | 110 | s16+0x462, s16-0x46C, s16+0x47A, s16-0x48E | 57 | — |
| d_a_bgn.cpp | 24 | 125 | s16-0x284, s16-0x4BC, s16+0xA32, s16-0xAA2, s16-0xD4C, s16+0xD50, s16+0x2296 | 21 | — |
| d_a_npc_md.cpp | 135 | 60 | s16-0x4260, s16-0x4262, s16-0x4266 | 105 | m3144 0x4260, m314A 0x4266 |
| d_a_tn.cpp | 46 | 55 | s16+0x43A, s32+0x4F0, s16-0x502, s16-0x506, s16-0x536, s16-0x54C, s16+0x594, s16+0x8C0, s1… | 62 | — |
| d_a_bgn2.cpp | 15 | 72 | s16-0x62A, s16-0x648, s32+0x2028, s16-0x3058 | 49 | — |
| d_a_npc_so.cpp | 87 | 44 | s32+0xCEC, s32+0xCF0, s32+0xCF4, s32+0xCF8, s32+0xD34 | 70 | — |
| d_a_kb.cpp | 26 | 45 | s16+0x768, s16+0x776, s16+0x780, s16-0x782, s16-0x78C, s16+0x790, s16-0x792 | 58 | — |
| d_a_nz.cpp | 24 | 37 | s16-0x3DA, s16+0x3DC, s16-0x3E8, s16+0x3EC, s16+0x3EE, s16+0x3FE | 69 | — |
| d_a_am2.cpp | 15 | 62 | — | 71 | — |
| d_a_bwd.cpp | 11 | 83 | s32+0x580, s16+0x1B30, s16-0x1B5A, s16+0x3E98 | 23 | — |
| d_a_pw.cpp | 28 | 45 | s16+0x484, s16-0x4A0, s16-0x4B4, s16+0x57E | 57 | — |
| d_a_fganon.cpp | 21 | 59 | s16+0x5B8, s16+0x5BC, s16+0xD88, s16+0xD8A | 37 | — |
| d_a_kamome.cpp | 17 | 56 | s32+0x3BC, s16+0x3CC | 50 | — |
| d_a_btd.cpp | 24 | 85 | s16+0x40A, s16+0x40E, s16-0x41C, s16-0x6208, s16+0x705A | 4 | — |
| d_a_wz.cpp | 18 | 41 | s16+0x4FA, s16-0x510, s16+0x512 | 56 | — |
| d_a_cc.cpp | 24 | 30 | s16+0x40E | 77 | — |
| d_a_kanban.cpp | 10 | 20 | s16+0x3DC | 83 | — |
| d_a_npc_ji1.cpp | 100 | 70 | s32-0xE50, s32-0xE54, s32+0xF90, s32+0xF98, s32+0xF9C | 11 | field_0xC30 0x0E54 |
| d_a_ks.cpp | 14 | 17 | s16-0x43A, s16+0x56C, s16-0x57C, s16+0x57C, s16+0x57E, s16+0x69EE, s32+0xFFFF8840, s32-0xF… | 49 | — |
| d_a_bb.cpp | 23 | 46 | s16-0x49A, s16+0x4C6, s16+0x4C8 | 44 | — |
| d_a_bl.cpp | 21 | 26 | s16+0x422 | 69 | — |

## Effects interpolation (runtime/src/interp_fx.cpp)

Per-step work that the game runs from drawing code (so on hold passes too, i.e. twice per logic
step with interpolation on) and how it is handled:

| WWHD | What | Hold pass | Logic pass (halfway) |
|---|---|---|---|
| `025AF8A0` `dScnPly_Draw` | calls particle calc, grass/tree/wood/flower calc, `g_Counter.mTimer++` (`101FF560`, right after `025A8148`), Bgsp Move, dSnap | (see rows below) | |
| `0282167C` `JPAEmitterManager::calc(group)` | particles (from `025A81A0` groups 0-6, `025A8148` 7-8, `025A81F8` 9-12) | skipped | particle position `+0x28` and size `+0x9C` halfway, restored at the next pass start |
| `027DF40C` key animator evaluation | btk/brk/... material animations; called from Draw on every pass (btk entry `025E7FC4`, brk entry `025E83FC`, daBg, items, ...) | frame recorded | evaluated at the halfway frame |
| `027F2FC4` `J3DFrameCtrl::update` | frame advance (e.g. `daSalvage_c::_draw` plays its btk/brk in Draw) | skipped | |
| `0246C5E8` sea vertex grid, `0246C7C8` sea material | heights/origin from `execute` (logic only); scroll counter `+0x22C` +1 per Draw | counter held | halfway heights/origin; scroll `(n-0.5)/300` |
| `0256A448` `wave_move` | wave crest sprites, environment Execute | | sprite position/scale/alpha halfway |
| `0254C6C4` / `025C9948` / `02548370` grass/tree/flower calc | sway from `g_Counter.mTimer` | mTimer increment undone | calc with mTimer+1, slots halfway |
| `025D0994` `dWood::Packet_c::calc` | bush sway (72 `Anm_c` at packet `+0x1C8D8`, 0x8C apart, sway matrix `+0`, trunk `+0x30`) | (world block) | animation matrices halfway |
| `0281FE40` `JPABaseEmitter::calc` | per emitter: centre/scale from the static emitter info (`104B5730` `+0xE0` / `+0xEC`) | | particles born this step: half a step back (velocity × scale + emitter movement) |
| rain `02566B88`, snow `02568BD4`, ash `02569408`, spores `02567F68`, fog `0256BB6C`, poison `0256CA54`, sky clouds `0256DDF8`, stars `0256A388` | environment Execute (logic only); packets from `g_env_light` (`10475A68`) `+0xA44/+0xA50/+0xA78/+0xA84/+0xA6C/+0xA94/+0xA60` (GameCube offset + 0x80); record arrays at GameCube offset + 0x84/0x88 | | every float field of records that did not respawn, halfway |
| `0251D864` cloth vertex fill | `dCloth_packet_c` (`+0x98/+0x9C` grid, `+0xB0/+0xB8/+0xC0` pos/nrm/back-nrm buffers [2], `+0x1C0` current) | | halfway between the two buffers after a simulation step |
| `024EDDF8` `dAttention_c::Draw`, `024EC1C8` `dAttDraw_c::draw` | attention arrow: a 4-joint J3D model at the target's attention position, facing the camera (inverse camera rotation); animation (`mDoExt_McaMorf` at `dAttDraw_c +0`, frame control `+0x98`, frame `+0x9C`) played by `dAttention_c::Run` in the play scene's execute (30 Hz) | | interpolation: model blending (viewCalc). True 60: full passes draw it unblended from the halfway target position and frame and the current (60 Hz) camera |

Still run on hold passes from `dScnPly_Draw` (with true60.cpp's site_025B00B0): only drawing
(actor draws, grass/tree/wood/flower/magma draw, attention draw), `MassClear`, the menu particle
calc (held back by the particle hook) and the HD call `0271DF30` (only when `+0xB64 == 2`).

Particle fields (measured, WWHD = GameCube): `+0x8C` axis, `+0x98` scale out, `+0x9C/+0xA0` size,
`+0xAC` alpha, `+0xB0` alpha wave, `+0xB4` loop offset, `+0xB8` prim colour, `+0xBC` env colour,
`+0xC0` rotation angle (u16), `+0xC2` rotation speed. Emitter: `+0x22C` global translation,
`+0x238` global particle scale.

Layouts used:

- `JPAEmitterManager`: `+0x50 + 12*group` emitter list (JSUPtrList; link `+0` object, `+0xC`
  next). `JPABaseEmitter`: `+0x1AC` particles, `+0x1B8` children, `+0x194` tick. `JPABaseParticle`
  matches the GameCube layout up to 0xD0 (`+0x10` offset, `+0x1C` local, `+0x28` global position,
  `+0x34` velocity, `+0x78` age, `+0x8C` draw params (`+0x9C/+0xA0` size), `+0xC8` callback,
  `+0xCC` status); WWHD adds a delay byte at `+0x10C`.
- Key animator: `*(this+0)` time block: `+0` frame, `+4` start, `+8` end, `+0x10` frame-mapping
  function (`027DA9E8` loop, `027DAAA0` clamp); `this+0x28` cached frame.
- `J3DFrameCtrl` (no vtable in WWHD): `+0` rate, `+4` frame, `+8` start, `+0xA` end, `+0xE`
  attribute, `+0xF` state. `mDoExt_baseAnm` starts with it; btk anm at `+0x68`, brk at `+0x10`.
- `daSea_packet_c` (static instance `1046D8D0`): `+0x130` flat inter, `+0xF0` wave info
  (`+0x114` cur scale), `+0x1FC/+0x200` grid min x/z, `+0x20C` height table (65x65 f32),
  `+0x22C` scroll counter, `+0x250` double-buffered vertex buffers (index `+0x4A8`).
  Draw `0246CAEC` (actor Draw) builds the vertices (`0246C5E8`) and the material (`0246C7C8`).
- `dKankyo_wave_Packet`: `WAVE_EFF[300]` at `+0xA0`, 0x38 each (GameCube layout).
- Sway slots: grass `+0x18F0C` (0x38 apart, s16 `+4`), tree `+0x2A9C` (0x84, `+4/+6`), flower
  `+0x35BC` (0x38, `+4`).
- Billboards: J3D billboard shapes get their view-space matrices in viewCalc (`027DE8A0` →
  `027DB590`, view matrix from `104B45F8`), i.e. from the blended camera and world matrices.

## True 60 fps: per-step primitives

The game advances its logic one step per 30 Hz frame. Running logic at 60 steps/s means halving
every per-step rate below (or calling it with half steps). These are the shared primitives that
actors use everywhere. "Evidence" is the matcher's tag (p = model probability). All are in
`build/names.tsv`.

### Smoothing and chasing (`SSystem/SComponent/c_lib.cpp`)

`addCalc` moves `*value` by a fraction (`scale`) of the remaining distance per step, so at 60
steps/s the fraction needs `1-(1-s)^(1/2)`. `chase` moves by a fixed `step` per step, so it
needs half the step.

| GameCube | WWHD | Evidence | Source |
|---|---|---|---|
| `cLib_addCalc` | `0200ECD4` | graph (p=0.78) | src/SSystem/SComponent/c_lib.cpp:22 |
| `cLib_addCalc2` | `0200ED84` | tu (p=0.99) | c_lib.cpp:56 |
| `cLib_addCalc0` | `0200EDC8` | tu (p=0.65) | c_lib.cpp:69 |
| `cLib_addCalcPos` | `0200EE00` | graph (p=0.79) | c_lib.cpp:80 |
| `cLib_addCalcPosXZ` | `0200EF78` | graph (p=0.96) | c_lib.cpp:105 |
| `cLib_addCalcPos2` | `0200F164` | graph (p=0.99) | c_lib.cpp:133 |
| `cLib_addCalcPosXZ2` | `0200F268` | tu (p=0.64) | c_lib.cpp:145 |
| `cLib_addCalcAngleS` | `0200F378` | tu (p=0.99) | c_lib.cpp:160 |
| `cLib_addCalcAngleS2` | `0200F428` | tu (p=1.00) | c_lib.cpp:192 |
| `cLib_addCalcAngleL` | `0200F474` | graph (p=0.67) | c_lib.cpp:205 |
| `cLib_chaseUC` | `0200F4FC` | graph (p=0.88) | c_lib.cpp:235 |
| `cLib_chaseS` | `0200F564` | graph (p=0.99) | c_lib.cpp:259 |
| `cLib_chaseF` | `0200F5C8` | graph (p=0.82); checked by hand against the source | c_lib.cpp:276 |
| `cLib_chasePos` | `0200F62C` | graph (p=0.97) | c_lib.cpp:293 |
| `cLib_chasePosXZ` | `0200F764` | graph (p=0.97) | c_lib.cpp:309 |
| `cLib_chaseAngleS` | `0200F8D0` | graph (p=1.00) | c_lib.cpp:326 |
| `cLib_calcTimer<T>` | one copy per actor TU; named: `<Uc>` `0207A9A0` (+1 copy), `<s>` `02055B64`, `<i>` `0211D2F8` (+1 copy), `<Us>` `0223556C`, `<Sc>` `0220CCB4` (legacy) | dup-graph / graph (p=1.00) / dup-graph / not named / callgraph-legacy | include/SSystem/SComponent/c_lib.h (template: `if (*t != 0) --*t`) |

The c_lib unit keeps the GameCube function order in WWHD (`0200ECD4`–`0200FCD8`), which
cross-checks the names above. Many `cLib_calcTimer` calls are inlined by GHS, so timers also appear
as plain decrements (see the catalogue below).

### Movement and per-step physics

| GameCube | WWHD | Evidence | Source |
|---|---|---|---|
| `fopAcM_calcSpeed` (speedF + gravity → speed, clamps at maxFallSpeed) | `025D67A8` | graph (p=0.85) | src/f_op/f_op_actor_mng.cpp:454 |
| `fopAcM_posMove` (pos += speed) | `025D6800` | graph (p=0.87) | f_op_actor_mng.cpp:469 |
| `fopAcM_posMoveF` (calcSpeed + posMove) | `025D6870` | tu (p=1.00) | f_op_actor_mng.cpp:482 |
| `dBgS_Acch::dBgS_Acch` | `024F0474` | graph (p=0.93) | src/d/d_bg_s_acch.cpp:50 |
| `dBgS_Acch::Set` | `024F06B4` | graph (p=0.99) | d_bg_s_acch.cpp:92 |
| `dBgS_Acch::CrrPos` (per-step ground/wall/roof correction) | `024F08A8` | assert | d_bg_s_acch.cpp:209 |
| `dBgS_Acch::GroundCheckInit` | `024F034C` | graph (p=0.77) | d_bg_s_acch.cpp:109 |
| `dBgS_Acch::GroundCheck` | `024EFF50` | graph (p=0.95) | d_bg_s_acch.cpp:122 |
| `dBgS_Acch::GroundRoofProc` | `024F03A0` | graph (p=0.78) | d_bg_s_acch.cpp:156 |
| `dBgS_Acch::LineCheck` | `024F00CC` | callgraph-legacy | d_bg_s_acch.cpp:175 |
| `dBgS_Acch::CalcWallBmdCyl` | `024F1134` | not named in the final run (earlier runs: graph (p=0.68)) | d_bg_s_acch.cpp:350 |
| `dBgS_Acch::SetGroundUpY` | `024F12A8` | callgraph-legacy | d_bg_s_acch.cpp:378 |
| `daObj::posMoveF_*` (object physics: resist, grade, stream) | `02311AB8`, `023121C4`, `023123C0` | graph (p=0.65) / tu (p=0.65) / graph (p=0.65) | src/d/actor/d_a_obj.cpp |

The fields involved are in fopAc_ac_c and moved by +0x11C in WWHD (see layouts): `speedF`
0x254→**0x370**, `gravity` 0x258→**0x374**, `maxFallSpeed` 0x25C→**0x378**, `speed` (cXyz)
0x220→**0x33C**, `current.pos` 0x1F8→**0x314**, `current.angle` 0x204→**0x320**, `shape_angle`
0x20C→**0x328**, `old` 0x1E4→**0x300**.

### Animation frame advance

| GameCube | WWHD | Evidence | Source |
|---|---|---|---|
| `J3DFrameCtrl::init` | `027F2BC0` | graph (p=0.83) | src/JSystem/J3DGraphAnimator/J3DAnimation.cpp:13 |
| `J3DFrameCtrl::checkPass` | `027F2BF8` | graph (p=0.82) | J3DAnimation.cpp:24 |
| `J3DFrameCtrl::update` (frame += rate, loop/stop modes) | `027F2FC4` | callgraph-legacy | J3DAnimation.cpp:143 |
| `mDoExt_baseAnm::initPlay` | `025E72D4` | tu (p=0.90) | src/m_Do/m_Do_ext.cpp:85 |
| `mDoExt_baseAnm::play` | `025E742C` | graph (p=0.80) | m_Do_ext.cpp:116 |
| `mDoExt_McaMorf::setAnm` | `025E4A98` | tu (p=1.00) | m_Do_ext.cpp:1414 |
| `mDoExt_McaMorf::setMorf` (morf length → per-step morf rate) | `025E4A54` | graph (p=0.74) | m_Do_ext.cpp:1452 |
| `mDoExt_McaMorf::play` (frame advance + morf counter) | `025E535C` | graph (p=1.00) | m_Do_ext.cpp:1463 |
| `mDoExt_McaMorf::calc()` | `025E55A0` | graph (p=0.77) | m_Do_ext.cpp |
| `mDoExt_McaMorf2::setMorf` / `play` | `025E5D1C` / `025E65FC` | tu (p=0.60) / callgraph-legacy | m_Do_ext.cpp:1799 / 1810 |
| `daPy_lk_c::setFrameCtrl` (Link's per-anime rate) | `023DE788` | tu (p=0.79) | src/d/actor/d_a_player_main.cpp:2937 |

Not identified: `mDoExt_McaMorf::calc(u16)` and `mDoExt_McaMorf2::calc(u16)`, the per-joint morf
blend. They are J3DMtxCalc callbacks on GameCube, and WWHD restructured J3D. The candidates
`025E410C` (1448 bytes) and `025E46B4` sit between `mDoExt_MtxCalcOldFrame::initOldFrameMorf`
and `setMorf` and use 0.5/1.5/1e-4 constants (quaternion blend), but are not confirmed. The
J3D math they call (`JMAEulerToQuat`, `JMAQuatLerp`, `PSMTXQuat` on GameCube) lives at
`027E0710`/`027ED3AC`, inside the HD library block.

### Particles

| GameCube | WWHD | Evidence | Source |
|---|---|---|---|
| `JPAEmitterManager::calc` (all emitters, per step) | `0282167C` | tu (p=0.62) | src/JSystem/JParticle/JPAEmitterManager.cpp:84 |
| `JPABaseEmitter::calc` | `0281FE40` | tu (p=0.97) | src/JSystem/JParticle/JPAEmitter.cpp:230 |
| `JPABaseEmitter::calcEmitterInfo` | `0281F688` | graph (p=0.64) | JPAEmitter.cpp:200 |
| `JPABaseEmitter::calcCreatePtcls` (emission rate per step) | `0281F878` | tu (p=0.99) | JPAEmitter.cpp:251 |
| `JPABaseEmitter::calcParticle` | `0281FBD8` | not named in the final run (earlier runs: callgraph-legacy) | JPAEmitter.cpp:319 |
| `JPABaseEmitter::calcChild` | `0281FD14` | not named in the final run (earlier runs: callgraph-legacy) | JPAEmitter.cpp:345 |
| `JPABaseEmitter::calcKey` | `0281F42C` | tu (p=0.96) | JPAEmitter.cpp:370 |
| `JPAEmitterManager::JPAEmitterManager` / `createSimpleEmitterID` | `02820DE4` / `02821448` | assert / assert | JPAEmitterManager.cpp |

### Camera

| GameCube | WWHD | Evidence | Source |
|---|---|---|---|
| `camera_execute` | `024FFA3C` | manual | src/d/d_camera.cpp:5426 |
| `camera_draw` | `024FFC40` | manual | d_camera.cpp:5453 |
| `dCamera_c::Run` (per-step camera update; mode dispatch) | `024FE3E8` | graph (p=0.99) | d_camera.cpp:676 |
| `dCamera_c::NotRun` | `024FF6C0` | graph (p=0.90) | d_camera.cpp:917 |
| `dCamera_c::followCamera` (main smoothing) | `025028B8` | graph (p=1.00) | d_camera.cpp:2618 |
| `dCamera_c::lockonCamera` | `025052E8` | tu (p=0.94) | d_camera.cpp:3291 |
| `dCamera_c::talktoCamera` | `02508A60` | tu (p=1.00) | d_camera.cpp:3677 |
| `dCamera_c::eventCamera` | `024FF164` | strings | d_camera.cpp:4679 |
| `dCamera_c::shakeCamera` / `StartShake` | `024FC108` / `02515298` | strings / callgraph-legacy | d_camera.cpp:4989 / 5118 |

The camera's smoothing goes through the cLib_addCalc* family above and through `d_cam_param.cpp`
(14 of 28 matched).

### Events, demos, cutscene timelines

| GameCube | WWHD | Evidence | Source |
|---|---|---|---|
| `dEvt_control_c::check` / `checkStart` / `order` | `0253FF34` / `0253FD1C` / `0253EC0C` | tu (p=0.89) / graph (p=0.91) / graph (p=0.90) | src/d/d_event.cpp |
| `dEvent_manager_c::runProc` | `02544374` | callgraph-legacy | src/d/d_event_manager.cpp |
| `dDemo_manager_c::create` (`mControl->forward(0)`) | `02528CEC` | callgraph-legacy | src/d/d_demo.cpp:659 |
| `dDemo_manager_c::update` (`mControl->forward(1)`, `mFrame++`) | not identified, probably inlined into its caller in WWHD | | d_demo.cpp:695 |
| `JStudio::stb::TControl::forward` | not identified | | JSystem/JStudio |

### Global counters and the main loop

| GameCube | WWHD | Evidence | Notes |
|---|---|---|---|
| `cCt_Counter` | `0200E6EC` | manual | `g_Counter` is at **`101FF558`**: `mCounter0` +0 (always ++), `mCounter1` +4 (reset or ++), `mTimer` +8. Same logic as c_counter.cpp:12 |
| `fpcM_Management` | `025DF948` | manual | runs every process (actor) step |
| `fapGm_Execute` | `025D42EC` | graph (p=0.74) | game main loop step |
| `dScnPly_Execute` | `025B0314` | vtable | play scene step |

Many actors use `g_Counter.mCounter0` as a frame clock (for example `daBomb_c`, `daBoko_c`,
`dKyr_wind_move`). On 60 steps/s these phase checks (`mCounter0 & 1`, `% N`) change rate too.

## Link (daPy_lk_c) and camera coverage

- `d_a_player_main.cpp`: 783 of 934 GameCube functions matched (84%). Link's procedures come
  mostly from his PTMF procedure tables (`vtable` evidence) plus `tu`/`graph`. Some key ones:
  `execute` `0240CDD0`, `posMove` `023FDA70`, `autoGroundHit` `023FE2F0`, `setNormalSpeedF`
  `02416230`, `setSpeedAndAngleNormal` `0241650C`, `setSpeedAndAngleAtn` `02417538`,
  `setSpeedAndAngleAtnActor` `02416B70`, `setSpeedAndAngleSwim` `0242ECFC`, `commonProcInit`
  `023DFDD8`, `setFrameCtrl` `023DE788`, `setBlendMoveAnime` `023E14A0`, `setWorldMatrix`
  `023FF6A0`, `draw` `02443EC4`. The full list is in `build/names.tsv`.
- Camera: `d_camera.cpp` 87/176, `d_ev_camera.cpp` 28/46, `d_cam_param.cpp` 14/28.

Key daPy_lk_c fields (GameCube → WWHD): `mCurProc` 0x31D8→0x65F0, `mStickDistance` 0x35B0→0x6A08,
`mNormalSpeed` 0x35BC→0x6A14, `mModeFlg` 0x3618→0x6A70, `mpSeAnmFrameCtrl` 0x363C→0x6A94,
`mFrameCtrlUnder[2]` 0x302C→0x5898, `mEquipItem` 0x3560→0x69B0, `mTinkleHoverTimer`
0x354C→0x699C, the timer union `m34D0..m34DA` 0x34D0→0x6916 (+0x3446). The fopAc_ac_c base fields
(speedF, gravity, ...) are listed above.

## Frame-count timers (catalogue)

`tools/decomp/timers.py` lists fields that are decremented by one (load, `addi -1`, store back)
or passed to `cLib_calcTimer`. It covers code reachable from an actor's Execute (method table,
`execute` methods, PTMF procedure tables of the same file) in the GameCube build. Each entry gets
its WWHD offset when the matched WWHD function has the same number of decrement sites.

Current output (`build/timers.tsv`): 510 sites in 164 classes; 131 have a WWHD offset.
Examples:

| Class | Field (GameCube → WWHD) | Where |
|---|---|---|
| daPy_lk_c | `m34D0` 0x34D0→0x6916 (proc timer, 19 procs) | procAutoJump, procCutEA, procGuardSlip, procLadderMove, ... |
| daPy_lk_c | `m34D2` 0x34D2→0x6918 | procFall, procAutoJump, procVomitJump |
| daPy_lk_c | `m34D6` 0x34D6→0x691C, `m34DA` 0x34DA→0x6920 | procTactPlayOriginal, procTactPlay |
| daPy_lk_c | `m3526` 0x3526→0x6976 | changeSlideProc |
| daPy_lk_c | `mTinkleHoverTimer` 0x354C→0x699C | autoGroundHit |
| daPy_lk_c | `m355C` 0x355C→0x69AC, `m355E` 0x355E→0x69AE | checkNextActionHookshotReady / BowReady |
| dCamera_c | `mBlureTimer` 0x590→0x594 | shakeCamera |
| sub_meter_class (HUD) | `mAdjustHp` 0x300A, `mCurrHP` 0x301F, alpha timers 0x2FD8/0x3014 | dMeter_lifeChange, dMeter_heartAlpha (HUD rewritten in HD) |

Limits: timers updated through other patterns (`if (t) t--` through a temporary pointer, or
through the cLib_calcTimer template with the address in a non-argument register) are missed.
Free functions on `xxx_class*` count as that class.

## Structure layouts (GameCube → WWHD)

`tools/decomp/layout.py` aligns the loads/stores through `this` (or the first argument) of every
matched pair. Each aligned pair votes GameCube offset → WWHD offset. Field names come from the
`/* 0x... */` comments in the decompilation headers.

**fopAc_ac_c** (3,942 pairs; all actors' base class):

| GameCube | WWHD | Shift | Fields |
|---|---|---|---|
| 0x000–0x0C0 | same | 0 | leafdraw_class base; `actor_type` 0xC0 |
| 0x0C4–0x10C | +4 | +0x4 | `actor_tag`; `draw_tag` 0xD8→0xDC; `heap` 0xF0→0xF4; `eventInfo` 0xF4→0xF8; `tevStr` 0x10C→0x110 |
| tevStr | 0xB0 → 0x1C8 bytes | | dKy_tevstr_c grew by 0x118 (HD lighting) |
| 0x1BC–0x290 | +0x11C | +284 | `setID` 0x1BC→0x2D8, `demoActorID` 0x1C0→0x2DC, `actor_status` 0x1C4→0x2E0, `actor_condition` 0x1C8→0x2E4, `parentActorID` 0x1CC→0x2E8, `home` 0x1D0→0x2EC, `old` 0x1E4→0x300, `current` 0x1F8→0x314, `shape_angle` 0x20C→0x328, `scale` 0x214→0x330, `speed` 0x220→0x33C, `cullMtx` 0x22C→0x348, `cullSizeFar` 0x248→0x364, `jntHit` 0x250→0x36C, `speedF` 0x254→0x370, `gravity` 0x258→0x374, `maxFallSpeed` 0x25C→0x378, `eyePos` 0x260→0x37C |
| sizeof | 0x290 → ~0x3AC | | derived actor members start ~0x11C later |

**daPy_lk_c** (485 pairs). The shift grows piecewise through the class: +0x11C (base) →
+0x3A0 (from about 0x3DC: daPy_py_c/early members grew) → +0x15A8 (0x2E98 `mpEquipItemModel`) →
+0x286C (0x2FDC `m_anm_heap_under`) → +0x3414..+0x3458 (0x317C onwards: `mActorKeepEquip`
0x317C→0x6590, `mCurProc` 0x31D8→0x65F0, `mDirection` 0x34B8→0x68D4, `mItemButton`
0x34C9→0x690E, the s16 timers 0x34D0→0x6916, `mEquipItem` 0x3560→0x69B0, `mNormalSpeed`
0x35BC→0x6A14, `mModeFlg` 0x3618→0x6A70). Run `layout.py ... daPy_lk_c` for the full list.

**view_class / camera_process_class** (from `camera_draw`, by hand): +4 from 0xC4 on.
`mNear` 0xC8→0xCC, `mFar` 0xCC→0xD0, `mFovy` 0xD0→0xD4, `mAspect` 0xD4→0xD8, `mLookat` 0xD8→0xDC
(eye 0xDC, center 0xE8, up 0xF4), `mBank` 0xFC→0x100. camera_process_class `mCamera`
(dCamera_c) 0x244→0x248.

**dCamera_c** (43 pairs): 0x000–0x130 unchanged (`mFovy` 0x38, `mPadId` 0x124,
`mpPlayerActor` 0x128, `mLockOnActorId` 0x130); +4 around 0x50C (`mEventFlags` 0x50C→0x510,
`mBlureTimer` 0x590→0x594).

**dBgS_Acch**: unchanged where measured (`m_flags` 0x28, `pm_pos` 0x2C, `m_tbl_size` 0x84,
`m_ground_h` 0x94).

**mDoExt_McaMorf**: `mpModel` 0x50→0x90 (+0x40). **JPABaseEmitter**: `mGlobalRotation`
0x1A8→0x1F0 (+0x48).

**J3DModel / J3DModelData / J3DJoint**: no matched methods. Hexa Drive rewrote J3D for GX2
(devirtualised; most of J3DMatBlock/J3DMaterial/J3DShape has no counterpart found), so their
layouts can't be measured this way.

## Coverage against the GameCube build

`build/coverage.tsv` has the per-file table. Totals (round 2):

- WWHD: 39,698 functions (37,766 from the recompiler's discovery plus 1,932 tail-call-only
  functions). **13,991 are named (35%)**: 12,451 from stages measured at ≥ 97%, 755 `tu-margin`
  (medium) and 785 `callgraph-legacy` (lower). Round 1 had 12,271.
- GameCube: 27,041 functions (weak copies merged); 13,844 matched (51.2%; round 1: 44.8%).
- **No GameCube counterpart (HD-only), ≈16,600 functions**, in long unmatched blocks
  (`build/regions.tsv`):
  - `025F8C38`–`0273A9D4`: new UI (Layout `.bflyt`/`.bflan` panes, GamePad/TV screens, save,
    staff roll, Miiverse, "PictureAccessor"), interleaved with the few GameCube `d_menu_*`,
    `d_metronome` and `d_a_agb` pieces that survived.
  - `0273B2D0`–`027EAC44`: Nintendo `sead`/`agl` (task manager, render layers, bloom, DOF,
    shadows, shader programs).
  - `02843374`–`028E8B04`: nw4f `lyt`/`font`/`snd`, ErrEula/swkbd wrappers, GX2 utilities.
- That leaves ≈23,100 WWHD functions that may have a GameCube counterpart. **≈61% of them are
  named** (round 1: ≈51%). Of the ≈9,100 still unnamed there:
  - ≈4,200 are identical per-file copies of inline functions (destructors, collider virtuals,
    vector operators);
  - ≈5,900 are unique functions, mostly mid-size without literals in `d_*`, `m_Do` and the
    JSystem block.

By area (GameCube functions matched):

| Area | GameCube functions | Round 1 | Round 2 |
|---|---|---|---|
| actors (d_a_*) | 15,374 | 9,593 (62%) | 10,820 (70%) |
| d_* (game) | 4,833 | 1,544 (32%) | 1,751 (36%) |
| JSystem/JAudio | 4,015 | 477 (12%) | 633 (16%) |
| SDK/MSL/runtime (dolphin, TRK, MSL; replaced by Cafe OS) | 1,221 | 43 (4%) | 48 (4%) |
| f_op/f_pc | 621 | 171 (28%) | 216 (35%) |
| m_Do | 515 | 81 (16%) | 97 (19%) |
| SSystem c_* | 462 | 216 (47%) | 279 (60%) |

What blocks 80%:

- **Inline copies** (≈4,200). GHS keeps one per file, and identical bodies belong to different
  functions (for example all trivial destructors). A copy is named only when its own file's
  GameCube object has a copy of a known candidate and the choice is unique. Most collider and
  destructor copies sit in per-file copies of vtables. Those can be identified only through
  their file's constructor, and constructors reference many tables at once.
- **Rewritten or replaced code on the GameCube side**: J3DGraphBase/Loader (0%), JKernel and
  JUtility (≈2%, replaced by sead), the Dolphin SDK/MSL/TRK (gone; Cafe OS), most of
  `d_menu_*`/`d_meter`/`f_op_msg_mng`/JMessage (HD UI). These GameCube functions have no WWHD
  counterpart to find, so the GameCube-side percentage is capped well below 80%.
- **Absolute-score limit**: ≈10,500 unnamed WWHD functions score p < 0.1 against every
  candidate in their window. Their counterpart is outside the window (moved file, or a file with
  no anchor in a block that isn't alphabetical, like the JSystem libraries), already taken by
  a wrapper/inline twin, or absent.
- **JSystem block** (`027EAC44`–`02843374`, 1,963 functions, ≈25% named). Libraries are
  alphabetical inside but not with each other. JAudio, JParticle and JStudio are the parts that
  survived.

Tried without gain: raising the file-window cap from 400 to 1,000 candidates (+0 names, 3× the
runtime).


## Gameplay mods (runtime/src/mods/, Gameplay menu)

All off by default; hooks in `tools/recomp/hooks_mods.txt`. Test switches `WWHD_MOD_*`, traces
`WWHD_MODS_TRACE`, test aids `WWHD_TEST_RSTICK` / `WWHD_RSTICK`, `WWHD_TEST_MOUSE`,
`WWHD_TEST_WHEEL`, `WWHD_TEST_GOTO` (see the sources).

| WWHD | What | Used for |
|---|---|---|
| `0250FDC8` `dCamera_c::manualCamera` | right-stick camera: bezier-shaped stick → `cLib_chaseF` on the turning speed `this+0x154` (+0.25/step) → yaw += speed × 0.92 × f25 (param, 8) → drawn U/V smoothed (≈0.66) | sites `@02510448`/`@025104A0` (speed), `@0251053C` (f28 = vertical rate), `@02510EA0`/`@02510EE4` (V/U smoothing) |
| `025071FC` `dCamera_c::subjectCamera` | first-person camera | mouse → right stick (rate) |
| `0252A684` `dDoor_info_c::getDemoAction`, `021C0078` `daMbdoor_c::getDemoAction` | door event cut (action table: 16 = TALK) | quick doors |
| `101F36CC` | `l_fopOvlpM_overlap[0]` (request: `+0x20` task) | fast scene changes |
| `101F5088` | game pad state (sead controller): `+0x124` held, `+0x18` pressed, `+0x1C` released, `+0x40` hold counter, `+0x130/0x134` main stick, `+0x138/0x13C` right stick | cleared triggers in extra steps |
| `daPy_lk_c+0x68D9` | `mReadyItemBtn` (0 X, 1 Y, 2 R; from `itemTrigger` `023EAB40`) | left click while aiming |

Measured (Outset, copy of the user's save): the original right-stick camera ramps 0.48, 1.13, 1.84 …
to 7.36°/step over 10 steps and eases out over 4 steps plus a tail; the direct camera turns
7.36°/step × stick × speed from the first step and stops on release (same in interpolation and true
60, 30 steps/s: 220.8°/s at full deflection). Link's house door, A press → control inside: 170
frames; quick doors 78; fast scene changes 166; both 74 (the ≈34-frame wait for the new scene's
archives is not shortened).
