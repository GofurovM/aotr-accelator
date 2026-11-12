# AOTR accelerator

A performance DLL for **Age of the Ring** (BFME2: Rise of the Witch-king 2.02). It is loaded into the running game
at start-up. It makes the game's own work cheaper or moves it off the game's main thread, so big battles stutter
less and stay closer to the game's 30 fps cap.

- **Multiplayer-safe.** The simulation is untouched: the logic rate, frame cap and game rules stay as they are.
  The few parts that answer logic questions faster prove themselves against the engine's own code (see
  [NOTES.md](NOTES.md)). Other players do **not** need the DLL.
- **Nothing on disk changes.** game.dat and the game folder are never modified, so the AotR launcher's checks
  still pass. Everything happens in memory, for that session only.
- **Age of the Ring only.** The starter loads the DLL into AOTR's game.dat and no other game.
- **Graceful fallback.** Every part checks the exact game bytes it patches first. On an unknown build, the parts
  tied to fixed addresses stay off.

New here? [OVERVIEW.md](OVERVIEW.md) explains how it works and which files to read first.

## The continuation of the first version

This project continues the first version, [BFME2 Accelerator](https://github.com/OhadBaehr/bfme2-accelerator)
2.0 by Ohad Baehr (MIT, see [LICENSE](LICENSE) and [NOTICE](NOTICE)). Its foundations - the render thread, the
texture workers, the file layer and most engine patches - are his work, and most of the code is still his. This
fork narrows the target to Age of the Ring 10, puts multiplayer safety first, and adds an installer with a tray
icon, an in-game status light, exact logic speed-ups that check themselves, read-ahead from earlier battles and
sizing for small PCs (the full list is in [OVERVIEW.md](OVERVIEW.md)). Not affiliated with Electronic Arts, the
Age of the Ring team or Open-BFME.

> **Work in progress.** The accelerator is tested by playing on a few PCs, not on every setup. Problems can
> happen - see [Known problems right now](#known-problems-right-now). If something goes wrong, every part can be
> turned off with a file (see [Switches](#switches-empty-files-beside-the-dll)), and *Uninstall* in the ring's
> menu returns the game exactly to how it was.

## Quick start: which file does what

You need Windows and the **Visual Studio Build Tools** with "Desktop development with C++" (free). See
[BUILD.md](BUILD.md).

| Double-click | What it does |
|---|---|
| **`PLAY.bat`** | **Builds, then starts AOTR with the accelerator from `out\` (nothing is installed). Start here.** |
| `PLAY_DIAG.bat` | The same with the **diagnostics build** (F7 panel, `aotr_report.txt`, measurement lines in the log). |
| `build.bat` | Only builds. The results go into `out\`. |
| `PACKAGE.bat` | Builds and makes `AOTR_accelerator.zip` for players: one click installs it. |

What the build makes, in `out\`:

| File | What it is |
|---|---|
| `aotr_start.exe` | **The starter.** It sits in the notification area (a ring), opens the AOTR launcher on request and loads the DLL into the game when it starts. Run this one, or let `PLAY.bat` run it. From the package it installs itself. |
| `bfme2_accel.dll` | **The accelerator itself.** It is never run by hand; the starter loads it into game.dat. It must stay next to `aotr_start.exe`. |
| `bfme2_accel.log` | Written by the DLL while the game runs: what was installed, and any problem. |

In the package made by `PACKAGE.bat`, the starter is named `AOTR_ACCELERATOR.exe`. The package also holds the
players' guides, `README_EN.txt` and `QOLLANMA_UZ.txt` (Uzbek); they are kept in `guide/`.

**From the package (one click):** unpack `AOTR_accelerator.zip` and run `AOTR_ACCELERATOR.exe`, then answer *Yes*
to the administrator question once. It then:
1. copies itself and the DLL to `%ProgramData%\AOTR Accelerator`;
2. makes a desktop shortcut "AOTR Accelerator";
3. registers a Task Scheduler task that starts it hidden at every logon, with the rights the game needs and no
   prompt each time.

From then on the player just opens the AOTR launcher. The starter waits in the notification area and loads the
DLL when AOTR's game.dat starts. A second start, or the shortcut, never runs a second copy: it asks the running
one to open the AOTR launcher. Running a newer package exe updates the installed copy. *Uninstall* is in the
ring's menu.

While the game runs, the starter:
- switches Windows to the *High performance* power plan and *Best performance* slider, and puts both back when
  the game closes (an empty `POWER_OFF` file in the install folder leaves them alone);
- opens the game's files once when the AOTR launcher appears, so the virus scanner has already looked at them
  (`WARM_OFF` turns this off);
- colours its ring from the DLL's `aotr_status.txt`: gold when all is well, yellow when a part switched itself
  off, red when the DLL is not running. Red is the only way to see that an antivirus blocked the DLL. A balloon
  says why, in English and Uzbek.

## What it does (release build)

The only thing on the screen is a small light in the top right corner of the picture. It is green when
everything runs, yellow when a part switched itself off after a self-check, and red when the main part (the
render thread) is not running. F7 hides and shows it, and the choice is remembered. The DLL writes a short
`bfme2_accel.log` beside itself. It records what was
installed at start-up and, after that, only problems: a part that switched itself off, the freeze guard, a crash.

| Area | What changes |
|---|---|
| Drawing | Direct3D 9 calls run in order on their own thread (DXVK only). Texture decoding runs on worker threads. Ground patch textures are built on all cores. Shadow mesh preparation uses fast exact algorithms. The render list sort is replaced with an exact one. The minimap and shroud go through the render queue. The particles' own update on the spare cores is available with `PXPAR_ON` (it measured slower than the engine on the test PC). |
| Files | The engine's unbuffered reads are served from read-ahead windows, and saves use a write buffer. Files of earlier sessions, and the files this match's factions can use (found from the mod's INI files), are read into the Windows file cache ahead of time. |
| Logic (exact) | The logic step's six engine calls run unchanged and in order; only where the frames fall between them changes. Several hot lookups are memoised or indexed (command buttons by name and an object's modules among them), and each proves itself against the engine's own code. |
| System | The game's thread gets a physical core to itself. Windows power throttling (EcoQoS) is off for the game. On a PC with enough memory Windows is asked not to trim the game's memory (fewer page faults in battles). |
| Long sessions | After every fifth match, back in the menu, the DLL lets go of its own short-cut tables and parked ground textures. The game's loaded art, the file cache and the learned list stay, so later matches stay fast. After 2.5 hours in one session the starter suggests restarting the game. |

[NOTES.md](NOTES.md) lists each part with its file, its multiplayer argument and the file name that turns it off. [CHANGELOG.md](CHANGELOG.md) says what each version changed.

## Switches (empty files beside the DLL)

`VANILLA` keeps the DLL idle for that session. `SLICER_OFF`, `TERRAINKEEP_OFF`, `RADARHOOKS_OFF`, `PRELOAD_OFF`,
`CORES_OFF`, `PARTFAST_OFF`, `AMFIND_OFF`, `EQUIV_OFF`, `FILTER_OFF`, `AUDIOLIMIT_OFF`, `PSFIND_OFF`, `PSREMOVE_OFF`,
`PEVICT_OFF`, `CBFIND_OFF`, `FMFIND_OFF`, `MASKFAST_OFF`, `WSFLOOR_OFF`, `SHADOWNBR_OFF`, `SHADOWWELD_OFF`, `W3DLOCK_OFF` and `PXBATCH_OFF` each turn off one part. Use them to bisect a problem. `PXPAR_ON`, `TASKWARM_ON` and `HASHFIND_ON` turn on parts that are off by default (they measured no gain on the test PC).

## Testing it yourself

`build.bat diag` (or `PLAY.bat diag`) makes the **diagnostics build**. It adds:
- an in-game panel (F7 shows and hides it): fps, logic steps a second, hitches, and the multiplayer self-check;
- `aotr_report.txt`, a session summary rewritten every minute;
- detailed `perf:` / `frame:` / `rt:` lines every 5–30 s in the log;
- continuous sampling of every self-check for the whole session (the release build checks only the first answers).
- the **glitch hunt**: in play the render thread is switched off and on in turns (every 3 minutes). Press the **right Ctrl** whenever a broken picture is on the screen; the log says which state
  each press fell in.

To check multiplayer bit-exactness without a second PC:
1. Play a skirmish with the DLL and save the replay.
2. Put an empty `VANILLA` file beside the DLL.
3. Watch the replay. It must play out exactly the same.

## Measured (the author's laptop, AOTR, DXVK)

- **Large 4v4 battles:** 28–30 fps average (median 29–30). The first builds measured about 23 fps average in
  the same kind of battle.
- **Multiplayer:** 2.5 hours in network games (5 matches) ran at 29.2 fps and 4.9 logic steps a second. The
  self-checks ran 208 million times with 0 differences.
- **The heaviest battles** still dip to about 18–22 fps. That is the cost of the logic and the drawing together
  on the game's single main thread.

## Known problems right now

What we are facing at the moment, most important first:

- **Dark stretched shapes at full graphics.** In big battles, dark columns stretched upward from units and flat
  dark "walls" near trees and buildings can appear for a moment. They look like the game's shadow volumes drawn
  into the picture. The logic slicer and the particle spread have been ruled out by test games; the render thread
  is being tested next, then the shadow mesh parts (`SHADOWNBR_OFF`, `SHADOWWELD_OFF`), then the game without the
  DLL (`VANILLA`) to rule out the game itself. If you see it, press the **right Ctrl** in the diagnostics build
  and send the log.
- **Antivirus warnings.** Some antivirus programs (on VirusTotal about 12 of 70, most of them built on the same
  BitDefender engine) flag the package as a generic or heuristic "trojan". The starter loads the DLL into the game
  with the standard Windows calls that trainers and overlays use, asks for administrator rights and starts with
  Windows - together that looks suspicious to them. The code is here for anyone to read.
- **AOTR updates.** The DLL checks every place in game.dat it uses. Since v85 an update that changes some of them
  turns off only the parts that use those places; the rest keeps running (before, AOTR 10.01 turned everything off
  because of a mistake in that check). A big update can still change the core, and then the light is red until a
  new version of the accelerator knows it.
- **DXVK is needed for the render thread.** With Windows' own Direct3D 9 instead of DXVK, the render thread is
  not used and the status light is red. Turn DXVK on in the AOTR launcher's graphics settings.
- **The heaviest battles still dip to about 18-22 fps.** The rest of the cost is the engine's own logic and
  drawing on its single main thread, which cannot be split without changing the game's results or pictures.
- **Laptops slow themselves down.** On battery, on a loose charger, or when hot, a laptop can drop its processor
  to 12-20% of its speed for a while. The game stutters then whatever the DLL does; the diagnostics report says
  when this happened. Keep the charger plugged in firmly.
- **Rare broken geometry after a long session.** On one PC, stretched lines and broken spell effects appeared
  once, in the fifth match of a 2.5-hour session; a second PC in the same games did not see it. The cause is not
  identified (all self-checks clean, memory fine). The DLL now clears its own tables after every fifth match, and
  the starter suggests a restart after 2.5 hours. If you see it, send `bfme2_accel.log` and DXVK's
  `game_d3d9.log` (in the `rotwk` folder).
- **32-bit address space.** The game is a 32-bit process (4 GB with Large Address Aware) and Age of the Ring
  alone uses 2-2.5 GB of it. That is why nothing is loaded into the game ahead of time - read-ahead goes only to
  the Windows file cache.
- **Fixed recently:** AOTR 10.01 showed a red light because of a mistake in the update check (v85); the diagnostics build could freeze the game when its glitch hunt switched the render thread
  (v84); a module lookup could miss a module that Age of the Ring added to a living unit (v83). See
  [CHANGELOG.md](CHANGELOG.md).
