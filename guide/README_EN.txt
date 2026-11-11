AOTR ACCELERATOR - quick guide
==============================

What it is
  A small add-on for Age of the Ring: fewer stutters and a steadier 30 fps in big battles.
  It works in memory only - game.dat and the AOTR folder are never changed.
  Multiplayer-safe: the game logic is untouched, and the other players do not need it.
  Any PC: on a small one (4 GB of memory, two cores) the parts that need spare memory or cores
  scale down or stay off by themselves, so the game never pays for them.

INSTALL (once, one click)
  1. Unzip the folder anywhere.
  2. Double-click AOTR_ACCELERATOR.exe and answer "Yes" to the administrator question.
     If Windows says "Windows protected your PC": More info -> Run anyway.
  3. A window says "installed". Answer Yes to start the game now.
  Done. The unzipped folder is no longer needed - you may delete it.
  (It is installed to C:\ProgramData\AOTR Accelerator.)

PLAYING
  - Open the AOTR launcher as usual, or the new desktop shortcut "AOTR Accelerator".
    The accelerator loads into the game by itself - nothing else to do.
  - It starts hidden with Windows and waits; it never runs twice.
  - In the game, a small light in the top right corner:
      green  = everything is running
      yellow = one part switched itself off after a self-check (the game goes on normally)
      red    = the main part is not running (see "If it is red")
    F7 hides or shows the light.

THE RING NEAR THE CLOCK (notification area)
  - gold = all is well, yellow = a part is off, red = not running.
  - Double-click: start Age of the Ring.
  - Right-click: Start Age of the Ring, Start with Windows (on/off), Open the folder (log),
    Uninstall, Exit.

IF IT IS RED
  - "DLL missing" or "not loaded": an antivirus probably blocked it. Add the folder
    C:\ProgramData\AOTR Accelerator to the antivirus exceptions, then install again.
  - "render thread off - Direct3D is not DXVK": the game uses Windows' own Direct3D instead of
    DXVK; check the graphics settings of the AOTR launcher (DXVK on).

GOOD TO KNOW
  - Every 5 matches, back in the menu, the accelerator clears its own temporary tables.
    What the game has loaded stays, so the next matches are still fast.
  - After 2.5 hours in one session a message suggests restarting the game - that keeps
    the graphics clean.
  - While the game runs, Windows uses the "High performance" power plan; it is put back when
    the game closes. To keep your own plan, create an empty file named POWER_OFF in
    C:\ProgramData\AOTR Accelerator.

UPDATE
  Close the game, then run AOTR_ACCELERATOR.exe from the new zip - it replaces the old one.

UNINSTALL
  Right-click the ring near the clock -> Uninstall.
