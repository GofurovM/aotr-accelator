# Changelog

What each version changed, newest first. The engineering detail (addresses, proofs, measurements) is in
[NOTES.md](NOTES.md); the off switches are in [README.md](README.md). Every part that touches the game's logic proves
itself against the engine's own code, and turns itself off (yellow light) on any difference.

## v85 (build 135) - works on AOTR updates (10.01), fewer antivirus warnings
- **AOTR 10.01 showed a red light**: the check for a newer game.dat compared 10 places in the game's import table,
  which Windows fills in when the game starts - so every update looked changed, on every PC. The import table is
  never compared now. The anchor table: 664 places, all equal on AOTR 10.0.
- **An update no longer turns everything off.** Each place the DLL uses is tied to the source files that use it;
  when an update really changes one, only the parts in those files stay off and the rest runs. Only a change in the
  core (the frame and logic plumbing) turns all the address-tied parts off, as before. A newer game.dat no longer
  needs the same size, checksum and entry point - the same 2.02 build stamp is enough to look further.
- **Antivirus**: the starter no longer enables the debug privilege (it was never needed - the game runs as the same
  user) and asks only for the rights the load needs; the DLL now has a version resource, and both files say who made
  them. Heuristic warnings may still appear (the DLL is still loaded into the game), but these were the strongest
  signals.
- The crash log no longer writes Windows' own memory probes (IsBadStringPtr and friends fault on purpose and handle
  it themselves) as crashes.

## v84 (build 134) - the v83 test's freeze fixed
- **The game hung after 15 minutes because of the diagnostics build's glitch hunt**: its switch from FULL to CORE
  told the game thread to call the graphics driver directly before the render worker had finished the calls already
  handed to it - two threads in the driver at once, and DXVK crashed. The switch now goes the safe way the freeze
  guard has used since v33: the worker's queue is emptied first, then the mode changes, at the start of the next frame.
  The release build never switched (it has no hunt).
- The glitch hunt counts markers by the mode really in effect, and never turns the render thread back on after the
  freeze guard has turned it off for good.

## v83 (build 133) - what the v82 test showed (26 min, full graphics, 22 glitch markers)
- **The long upward shapes are not the logic slicer and not the particle spread**: markers came with both off as
  with both on. They look like stencil shadow volumes drawn into the picture. The diagnostics build's glitch hunt now
  switches the **render thread** (FULL / CORE every 3 minutes) instead; new file switches `SHADOWNBR_OFF` and
  `SHADOWWELD_OFF` for the next steps.
- **fmfind fixed**: its one self-check difference showed that AOTR adds modules to living objects (the constructor
  leaves room for 7 more). "No such module" is no longer remembered; a found module is used only while the array still
  holds it at its place and it still answers that key.
- **Off by default, because the test measured no gain** (each turns back on with a file): the particle spread
  (`PXPAR_ON`; its trial measured 139% of the engine's time), the render task read-ahead (`TASKWARM_ON`; 102%), the
  id map find without a divide (`HASHFIND_ON`; same share as before). The flag mask test (kept) left the profile.

## v82 (build 132)
- **Starter: PLAY.bat / PLAY_DIAG.bat always load the build they made.** Without administrator rights a start from the
  source tree used to hand the launch to an installed copy running in the tray (which loads its own, release DLL);
  now it asks for the rights and takes over. And it is never silent: when the game or the AOTR launcher is already
  running it says so (the launcher's window is brought to the front), and when the launcher is not found it says so.
- **Id map find without a divide** (`aotr_hashfind.inc`, logic, self-proving): the engine's unsigned-key hash map find
  (findObjectByID's, 79 callers, 2.2% of the logic) takes its bucket by a multiply instead of a 32-bit `div`. Exact
  (checked offline over 108 million keys). `HASHFIND_OFF`.
- **CHANGELOG.md** (this file).
- Code review: every patch site listed and checked for overlaps; clang `-Wall -Wextra` clean for both builds.

## v81 (build 131)
- **Object::findModule per object** (`aotr_fmfind.inc`, logic, self-proving): an object's module looked up by key is
  remembered for that same object (same ObjectID and module array). ~2% of the logic. `FMFIND_OFF`.
- **Flag mask test in SSE** (`aotr_maskfast.inc`, logic, self-proving): the 19-word must-be-set / must-be-clear test
  in four SSE steps. ~2% of the logic. `MASKFAST_OFF`.

## v80 (build 130)
- **Per-PC trials**: the particle spread and the render task read-ahead time themselves in the first minutes of play
  (alternating 64-frame blocks, same results either way) and stay on only where they are faster.

## v79 (build 129)
- **Small PCs** (4 GB, two cores): ground-texture keeping sized by memory (off under 3.5 GB), the menu's learned-list
  read capped by free memory, the particle spread only with three or more cores.
- Diagnostics: the render task timing hook keeps a return-address stack (nested calls safe).

## v78 (build 128)
- **strncpy for large sizes** in SSE (the render stage scope copies a 256-byte name thousands of times a frame).
- **Working-set floor**: Windows is asked not to trim the game's memory on PCs with 6 GB+ (fewer page faults).
  `WSFLOOR_OFF`.

## v77 (build 127)
- **Command buttons by name** (`aotr_cbfind.inc`, logic, self-proving): the top path of the logic samples.
  `CBFIND_OFF`.
- **Render task list read ahead** (`aotr_taskwarm.inc`, prefetch only): the batching step's light-environment
  compares wait less for memory. `TASKWARM_OFF`.
- Diagnostics: who commits and frees memory (`vmprobe:`), this DLL's own functions named from the map file.

## v76 (build 126)
- Diagnostics: **glitch hunt** - the logic slicer and the particle spread switch off and on in turns; the right Ctrl
  marks a broken picture, the log says which state it fell in.
- The learned list is read newest first and trimmed instead of freezing at 512 KB.

## v75 (build 125)
- The particle spread covers ~95% of AOTR's particles (drift module and the system modules in use verified).

## v74 (build 124)
- **Particles on the spare cores** (`aotr_pxpar.inc`): Particle::update for the particles that cannot depend on
  anything before their system's turn runs on helper threads; results are used at the engine's own moment.
  Self-checking all session. `PXPAR_OFF`.
- Diagnostics: the stack sampler really runs in play (a statement after an `#endif` had never been compiled); the
  address space by kind; the particle census.

## v73 (build 123)
- Diagnostics: the particle update measured (time, model share, module classes).

## v72 (build 122)
- **Age of the Ring 10** recognised; later game.dat updates are accepted when every place this DLL uses still holds the
  verified bytes (the anchor table).
- The starter no longer reports a false power-settings failure.

## v71 (build 121)
- **One-click install**, tray ring (gold / yellow / red), autostart, desktop shortcut, single instance.
- **Status light** in the game (F7), refresh after every fifth match, EN and UZ guides.

## v70 (build 120)
- **Release build** for the AOTR team: English, no tests, nothing on screen but the status light.
