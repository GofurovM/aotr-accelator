# How AOTR Accelerator works - a reader's guide

This page is for someone who opens the repository for the first time: what the program is, how it works, what
this fork added, and which files to read in which order. The engineering detail (addresses, proofs,
measurements) is in [NOTES.md](NOTES.md); what each version changed is in [CHANGELOG.md](CHANGELOG.md).

## In one paragraph

Age of the Ring runs on EA's SAGE engine from 2006. The engine does almost everything on **one thread**: the game
logic (units, AI, pathfinding) and the drawing (Direct3D 9 calls) take turns on the same CPU core, while the other
cores mostly wait. In big battles that one core runs out of time, and the game stutters or falls below its 30 fps
cap. AOTR Accelerator is a DLL that is loaded into the running game. It moves work that does not have to be on that
thread onto other cores, and it makes some of the engine's slowest lookups cheaper. It does this in memory only,
for that session only, and without changing what the game computes.

## Where it came from

This is a fork of [BFME2 Accelerator](https://github.com/OhadBaehr/bfme2-accelerator) 2.0 by Ohad Baehr (MIT).
The foundations are his: the render thread, the texture workers, the file layer, the fast C runtime functions and
most of the engine patches. About 70% of the code here is still his (compared line by line with his 2.0 release).
This fork narrowed the target to Age of the Ring, made multiplayer safety the first rule, and added the parts listed
under "What this fork added". See [NOTICE](NOTICE) and [LICENSE](LICENSE).

## The big picture

There are two programs:

| Program | Source | What it does |
|---|---|---|
| `AOTR_ACCELERATOR.exe` (the **starter**) | `src/aotr_start.cpp` | Installs itself once, sits in the notification area as a ring, waits for AOTR's `game.dat` to start and loads the DLL into it. Raises the power plan while the game runs. Colours the ring from the DLL's status file. |
| `bfme2_accel.dll` (the **accelerator**) | `src/aotr_accel.cpp` + every `src/aotr_*.inc` (one translation unit) | Runs inside the game. Recognises the game build, installs its parts, shows the status light. |

What happens when the game starts:

1. The starter sees `game.dat` start and loads the DLL into it with the standard Windows calls (`VirtualAllocEx`,
   `WriteProcessMemory`, `CreateRemoteThread` on `LoadLibraryA`). This is also why some antivirus programs give a
   generic warning.
2. The DLL hashes the game's code section. If it is a build it knows (or a newer AOTR build where every address it
   uses still holds the verified bytes - the **anchor table**, `src/aotr_anchors.inc`), the address-based parts
   are allowed. Otherwise only the portable parts run, and the status light turns red.
3. Each part checks the exact bytes it is about to patch, then installs. A part that finds anything unexpected
   stays off and says so in `bfme2_accel.log`.
4. While the game runs, the parts that answer logic questions check themselves against the engine's own code. A
   single difference turns that part off for the rest of the session (the light turns yellow).

## The four kinds of work

| Kind | Examples (file) | Rule that keeps it safe |
|---|---|---|
| **Drawing on its own thread** | the render thread (`aotr_rt.inc`): every Direct3D call of the game thread is recorded in order and executed on a worker thread | The calls run in exactly the game's order, so the picture is the same. A call that must return data waits for the queue. A freeze guard steps the worker aside if it ever stalls. DXVK only. |
| **Work done ahead or on helper threads** | texture decoding (`aotr_texjobs.inc`), ground textures on all cores (`aotr_terrain.inc`), shadow mesh preparation (`aotr_shadownbr.inc`, `aotr_shadowweld.inc`) | The first results are computed both ways and compared byte for byte; one difference turns the part off. |
| **Files read before they are needed** | file read-ahead (`aotr_fdbuf.inc`), files of earlier battles and of this match's factions into the Windows file cache (`aotr_filewarm.inc`, `aotr_preload.inc`) | Read only, into the Windows file cache only; nothing is loaded into the game ahead of time. |
| **Exact logic shortcuts** | remembered answers and indexes for hot lookups: command buttons by name (`aotr_cbfind.inc`), an object's modules (`aotr_fmfind.inc`), flag masks in SSE (`aotr_maskfast.inc`), the whole-map query (`aotr_partfast.inc`) | **Self-proof:** for the first 20000 answers of every match the engine's own function runs too, and its answer is the one returned. One difference turns the part off. Logic speed, random numbers and game rules are never touched. |

## The ground rules

1. **Multiplayer first.** The game's results must be bit-for-bit the same as without the DLL, so players with and
   without it play in the same match. Logic rate, frame cap and game speed are left alone.
2. **No broken pictures.** A part that could change what is drawn is not shipped.
3. **game.dat and the game folder are never modified on disk.** Uninstall it, and the game is exactly as before.
4. **Every part can be turned off** with an empty file named in its header comment (for example `FMFIND_OFF`),
   placed next to the DLL. The list is in [README.md](README.md).
5. **Open-BFME-2, the EA Generals source and OpenSAGE were used as maps only** (names, structure). No code was
   copied from them.

## What this fork added

On top of the upstream 2.0 code (new files are named after the feature):

- **Age of the Ring 10 support that survives updates:** the build check and the anchor table
  (`aotr_anchors.inc`, generated by `tools/gen_anchors.py`).
- **The starter:** one-click install, a tray ring (gold / yellow / red), start with Windows, single instance,
  power plan while playing, laptop clock levels (`aotr_start.cpp`, `aotr_guard.inc`).
- **The in-game status light** and the refresh after every fifth match (`aotr_badge.inc`).
- **Exact logic shortcuts with self-proof:** `aotr_cbfind.inc`, `aotr_fmfind.inc`, `aotr_maskfast.inc`,
  `aotr_amfind.inc`, `aotr_partfast.inc`, `aotr_hashfind.inc`, and the particle system lookups
  (`aotr_psfind.inc`, `aotr_psremove.inc`, `aotr_pevict.inc`).
- **Fewer first-appearance hitches:** the match preload from the mod's own INI files (`aotr_preload.inc`), parked
  ground textures (`aotr_terrainkeep.inc`), shadow mesh preparation in n log n instead of n²
  (`aotr_shadownbr.inc`, `aotr_shadowweld.inc`).
- **Lighter waits:** the W3D render lock without a system call (`aotr_w3dlock.inc`), the game thread on a core of
  its own and a sleeping frame limiter (`aotr_sched.inc`).
- **Small PCs:** every part sizes itself by memory and cores; on a 4 GB, two-core PC the ones that need spare
  capacity stay off.
- **The diagnostics build** (`build.bat diag`): an in-game panel, a session report, a frame profiler and a
  "glitch hunt" that lets a tester mark a broken picture with the right Ctrl key (`aotr_overlay.inc`,
  `aotr_frameprof.inc`, `aotr_vmprobe.inc`). Every change is measured on a real PC with it before it ships.
- **Turned off:** upstream features that change timing or pictures (in-between frames, a higher logic rate, merged
  draws) stay in the tree but are never installed in this fork.

## Reading order

| # | File | Why |
|---|---|---|
| 1 | [README.md](README.md) | What the player gets, the build scripts, the off switches. |
| 2 | this page | The shape of the whole thing. |
| 3 | [NOTES.md](NOTES.md), first sections | The rules, the target build, the threads, and the table of every part with its proof. |
| 4 | `src/aotr_start.cpp` | The starter: install, tray, how the game is found and the DLL is loaded. |
| 5 | `src/aotr_accel.cpp`, `initThread` | How the DLL starts: build hash, anchors, which parts are installed and in which order. |
| 6 | `src/aotr_fmfind.inc` | A short, complete example of a logic shortcut: the header comment explains why the remembered answer is the engine's answer, and the code shows the self-proof. Every logic part follows the same pattern. |
| 7 | `src/aotr_rt.inc` | The render thread - the largest and most delicate part. Read its header and `rtApplyToggle` first. The hooks themselves are generated (`aotr_rt_gen.inc` from `gen_rt_hooks.py`). |
| 8 | `src/aotr_badge.inc` | How green / yellow / red are decided. |
| 9 | [CHANGELOG.md](CHANGELOG.md) | What each version changed and why. |

Every `.inc` file starts with a comment that says what was slow, what the part does, why the result stays the
same, and its off switch. Read that comment before the code.

## How changes are checked

- **Before a commit:** a clang syntax check of both builds (the commands are in NOTES.md, "Working in this repo");
  the real build is MSVC (`build.bat`).
- **At run time:** the byte checks at install, the self-proof of logic parts, the comparisons of helper-thread
  results, and the freeze guard.
- **On a real PC:** a test game with the diagnostics build; the report and the log decide whether a part stays,
  is fixed, or is turned off by default.

## Known issues

- At full graphics, rare dark stretched shapes have been seen in big battles. The logic slicer and the particle
  spread have been ruled out; the render thread is being tested next (see NOTES.md, "Open issues").
