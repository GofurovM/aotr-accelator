// aotr_guard.inc - v55.1: a load that never ended turns this build's new parts off at the next start.
//
// test-38 (v55 build 103): the game hung on the loading screen - a bug of this DLL (aotr_psremove.inc read 0x5F7909's
// argument wrongly, the FX manager's reset loop never ended). The user's rule: what we add must never freeze the game,
// and if it does, it must be plain that it was us. So:
//   * out\aotr_guard.txt holds this build's number and where the last run got to: "started" at start, "loading" when
//     a match is being loaded (startNewGame's tail), "playing" after 100 logic steps of a match;
//   * a run of the SAME build that stopped at "loading" (the window closed or killed while loading) puts the next start
//     in SAFE mode: the parts new in this build are not installed (their install functions ask gdNewOff()), the log and
//     the report say so in their first lines; SAFE stays for this build until a run reaches "playing" in it twice;
//   * a watchdog thread: no picture for 20 s with the game's thread busy -> one log line naming it as a hang, where
//     the guard state was, and that the next start will be safe if this ends here.
// A new build starts clean. Delete out\aotr_guard.txt to clear it by hand.
#define AOTR_BUILDNO_GUARD 134                                     // = AOTR_BUILDNO (checked where that is defined)
static char g_gdPath[MAX_PATH]; static volatile LONG g_gdSafe = 0, g_gdFrames = 0, g_gdSteps = 0, g_gdState = 0, g_gdPlayed = 0;
static const char* const kGdState[] = { "started", "loading", "playing" };
static void gdWrite() {
    char b[160]; int n = wsprintfA(b, "build=%d state=%s safe=%d played=%d\r\n", AOTR_BUILDNO_GUARD, kGdState[g_gdState], (int)g_gdSafe, (int)g_gdPlayed);
    HANDLE f = CreateFileA(g_gdPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(f, b, (DWORD)n, &w, NULL); FlushFileBuffers(f); CloseHandle(f);
}
static bool gdNewOff() { return g_gdSafe != 0; }             // the install functions of this build's new parts ask this
static void gdInit() {
    aotrPath(g_gdPath, "aotr_guard.txt");
    char b[200]; b[0] = 0; DWORD r = 0;
    HANDLE f = CreateFileA(g_gdPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { ReadFile(f, b, sizeof(b) - 1, &r, NULL); b[r] = 0; CloseHandle(f); }
    int build = 0, safe = 0, played = 0; char st[32]; st[0] = 0;
    const char* p = strstr(b, "build="); if (p) build = atoi(p + 6);
    p = strstr(b, "state="); if (p) { int k = 0; p += 6; while (k < 31 && p[k] > ' ') { st[k] = p[k]; ++k; } st[k] = 0; }
    p = strstr(b, "safe="); if (p) safe = atoi(p + 5);
    p = strstr(b, "played="); if (p) played = atoi(p + 7);
    if (build == AOTR_BUILDNO_GUARD) {
        g_gdPlayed = played;
        if (!lstrcmpA(st, "loading")) {
            g_gdSafe = 1; g_gdPlayed = 0;
            logf("GUARD: SAFE MODE - the last run of this build stopped while a match was LOADING (it never reached play: a hang or the window closed there). This build's new parts are NOT installed this time (psremove, amfind, the faction-wide and by-name preload; v57: the loading-screen preload is held to 100 templates; v59: no preload during play, no early file read). If the game now loads, the hang was ours. Delete out\\aotr_guard.txt to try them again.");
        } else if (safe && played < 2) {
            g_gdSafe = 1;
            logf("GUARD: safe mode continues for this build (a run hung while loading before; %d of 2 runs in safe mode have reached play). The new parts stay off.", played);
        }
    }
    g_gdState = 0; gdWrite();
    if (!g_gdSafe) logf("GUARD: this build's new parts are on; out\\aotr_guard.txt follows the run (started / loading / playing) - a run that stops while loading turns them off at the next start.");
}
static void gdOnLoading() { if (g_gdState != 1) { g_gdState = 1; g_gdSteps = 0; gdWrite(); } }
static void gdOnLogicStep() {
    if (g_gdState == 1 && ++g_gdSteps == 100) { g_gdState = 2; if (g_gdSafe) g_gdPlayed++; gdWrite(); }
}
static __forceinline void gdOnFrame() { g_gdFrames++; }
// v55.2: is the MACHINE slow? (test-39: every part - the game thread, the render worker on its own core, the ground
// patch builders on six more - ran 3.5-4x slower than the same save an hour before: the processor itself was slowed,
// battery / power saving / heat.) A fixed chain of dependent multiplies is timed every 2 s (the fastest of three runs,
// ~0.3 ms) against the fastest seen this session; Windows' power status says battery / mains / saver.
static volatile LONG g_gdSpeed = 100, g_gdSpeedMin = 100, g_gdSlowSecs = 0, g_gdAc = -1, g_gdSaver = 0, g_gdBatt = -1;
static LONG64 g_gdBestProbe = 0; static volatile DWORD g_gdSink = 0;
// v63: the processor's clock, set step by step instead of switching Turbo off (test-45: Turbo off stopped the 100%/20%
// jumps but the steady clock was ~56% - fewer fps on average). Level 0 = as Windows has it; 1..3 = the highest clock
// capped at 150 / 130 / 115 % of the processor's base clock; 4 = Turbo off. Every 60 s of play the level's frames per
// second and its deep falls (under 35%) are measured: 3 deep falls in the minute -> one level lower; a level whose fps is
// more than 3% under the best level so far -> back to the best one, and there it stays. The launcher sets the levels
// (out\aotr_throttle.txt "level=N" asks, out\aotr_throttle_ack.txt "level=N" answers) and puts everything back when
// the game is closed. TURBO_KEEP next to the launcher: nothing is changed.
static volatile LONG g_gdLvl = 0, g_gdLvlWant = 0, g_gdLvlBest = 0, g_gdLvlFinal = 0, g_gdLvlChanges = 0;
static volatile LONG g_gdLvlSecs[5], g_gdLvlFrames[5], g_gdLvlDeep[5];
static LONG g_gdWinSecs = 0, g_gdWinDeep = 0, g_gdWinFrames = 0, g_gdFramesLast = -1; static LONG g_gdFpsBest10 = 0;   // fps x10
static volatile LONG g_gdDeepBefore = 0, g_gdDeepAfter = 0, g_gdSecsBefore = 0, g_gdSecsAfter = 0, g_gdTurboAsked = 0, g_gdTurboOff = 0;   // (v60 names, kept for the report)
static const char* const kGdLvlName[5] = { "as Windows has it (Turbo)", "capped at 150% of the base clock", "capped at 130%", "capped at 115%", "Turbo off" };
static void gdLvlRequest(LONG lvl, const char* why) {
    char p[MAX_PATH]; aotrPath(p, "aotr_throttle.txt");
    char b[32]; int n = wsprintfA(b, "level=%d\r\n", (int)lvl);
    HANDLE f = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(f, b, (DWORD)n, &w, NULL); CloseHandle(f); }
    g_gdLvlWant = lvl; g_gdTurboAsked = 1;
    logf("machine: the launcher is asked for processor level %d (%s) - %s.", (int)lvl, kGdLvlName[lvl], why);
}
static void gdLvlTick(LONG pct) {                                  // every 2 s, in play
    LONG fr = g_gdFrames; LONG df = g_gdFramesLast < 0 ? 0 : fr - g_gdFramesLast; g_gdFramesLast = fr;
    if (g_gdLvlWant != g_gdLvl) {                                    // waiting for the launcher
        char p[MAX_PATH]; aotrPath(p, "aotr_throttle_ack.txt");
        char b[32] = ""; DWORD r = 0;
        HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) { ReadFile(f, b, sizeof(b) - 1, &r, NULL); b[r] = 0; CloseHandle(f); }
        const char* q = strstr(b, "level=");
        if (q && atoi(q + 6) == g_gdLvlWant) {
            g_gdLvl = g_gdLvlWant; g_gdLvlChanges++; g_gdTurboOff = g_gdLvl > 0;
            g_gdWinSecs = g_gdWinDeep = g_gdWinFrames = 0;
            logf("machine: processor level %d (%s) is set.", (int)g_gdLvl, kGdLvlName[g_gdLvl]);
        }
        return;
    }
    LONG L = g_gdLvl;
    g_gdLvlSecs[L] += 2; g_gdLvlFrames[L] += df; if (pct < 35) g_gdLvlDeep[L]++;
    if (L == 0) { g_gdSecsBefore += 2; if (pct < 35) g_gdDeepBefore++; } else { g_gdSecsAfter += 2; if (pct < 35) g_gdDeepAfter++; }
    g_gdWinSecs += 2; g_gdWinFrames += df; if (pct < 35) g_gdWinDeep++;
    if (g_gdWinSecs < 60) return;
    LONG fps10 = g_gdWinFrames * 10 / g_gdWinSecs;
    logf("machine: processor level %d (%s) - the last %d s: %d.%d fps, %d deep falls.", (int)L, kGdLvlName[L], (int)g_gdWinSecs, (int)(fps10 / 10), (int)(fps10 % 10), (int)g_gdWinDeep);
    if (!g_gdFpsBest10 || (L == g_gdLvlBest && fps10 > g_gdFpsBest10) || fps10 * 100 > g_gdFpsBest10 * 103) { g_gdLvlBest = L; g_gdFpsBest10 = fps10; }
    if (!g_gdLvlFinal) {
        char mk[MAX_PATH];
        if (L != g_gdLvlBest && fps10 * 100 < g_gdFpsBest10 * 97) { g_gdLvlFinal = 1; gdLvlRequest(g_gdLvlBest, "this level gave fewer frames than the best one so far: back to it, and there it stays"); }
        else if (g_gdWinDeep >= 3 && L < 4 && GetFileAttributesA(aotrPath(mk, "TURBO_KEEP")) == INVALID_FILE_ATTRIBUTES) gdLvlRequest(L + 1, "3 or more deep falls in the last minute");
    }
    g_gdWinSecs = g_gdWinDeep = g_gdWinFrames = 0;
}
static volatile DWORD g_gdLastSlowTick = 0; static volatile LONG g_gdLastSlowPct = 100, g_gdMarks = 0, g_gdMarksSlow = 0;   // v57: markers vs the machine's slow moments
static LONG64 gdProbe() {
    LONG64 best = 0;
    for (int r = 0; r < 3; ++r) {
        LONG64 a = qpcNow(); DWORD x = 0x12345678u ^ (DWORD)r;
        for (int i = 0; i < 300000; ++i) x = x * 1664525u + 1013904223u;
        LONG64 d = qpcNow() - a; g_gdSink += x;
        if (!best || d < best) best = d;
    }
    return best;
}
static void gdSpeedTick() {
    LONG64 d = gdProbe(); if (d <= 0) return;
    if (!g_gdBestProbe || d < g_gdBestProbe) g_gdBestProbe = d;
    LONG pct = (LONG)(g_gdBestProbe * 100 / d); if (pct > 100) pct = 100;
    LONG prev = g_gdSpeed; g_gdSpeed = pct;
    if (g_gdFrames > 0) { if (pct < g_gdSpeedMin) g_gdSpeedMin = pct; if (pct < 70) { g_gdSlowSecs += 2; g_gdLastSlowTick = GetTickCount(); g_gdLastSlowPct = pct; } }
    if (g_gdState == 2) gdLvlTick(pct);                                         // v63: the processor level controller
    SYSTEM_POWER_STATUS ps; LONG ac = -1, saver = 0, batt = -1;
    if (GetSystemPowerStatus(&ps)) { ac = ps.ACLineStatus == 1 ? 1 : (ps.ACLineStatus == 0 ? 0 : -1); saver = (ps.SystemStatusFlag & 1) ? 1 : 0; batt = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : -1; }
    bool powerChanged = ac != g_gdAc || saver != g_gdSaver;
    g_gdAc = ac; g_gdSaver = saver; g_gdBatt = batt;
    if (powerChanged || (pct < 80 && prev >= 80) || (pct >= 80 && prev < 80) || (pct < 80 && (prev - pct > 10 || pct - prev > 10)))
        logf("machine: processor speed now %d%% of the fastest seen this session | power: %s%s%s", (int)pct,
             ac == 1 ? "mains" : (ac == 0 ? "BATTERY" : "unknown"), saver ? ", BATTERY SAVER ON" : "",
             pct < 70 ? " - the PC itself is slowed (power saving, battery or heat): every part of the game runs slower, not because of the game or this DLL" : "");
}
// v57: the address space of this 32-bit game. aotr_preload.inc now hands art over on the loading screen up to a
// MEASURED 2.25 GB (before: an estimate that stopped near 2.0 GB); what play adds on top is followed here - the peak,
// a line at every new 100 MB, a warning above 3.3 GB (a 32-bit process ends at 4 GB).
static volatile LONG g_gdVaMB = 0, g_gdVaPeak = 0, g_gdVaTotal = 0, g_gdVaPlayStart = 0;
static void gdVaTick() {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;
    LONG used = (LONG)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20); g_gdVaTotal = (LONG)(ms.ullTotalVirtual >> 20); g_gdVaMB = used;
    if (g_gdState == 2 && !g_gdVaPlayStart) g_gdVaPlayStart = used;
    if (used / 100 > g_gdVaPeak / 100 && used >= 1500)
        logf("memory: the game's address space reached %d MB of %d MB%s%s", (int)used, (int)g_gdVaTotal, g_gdState == 2 ? " (in play)" : (g_gdState == 1 ? " (loading)" : ""),
             used >= 3300 ? " - CLOSE TO THE END of a 32-bit process: the game could fail to load more art" : "");
    if (used > g_gdVaPeak) g_gdVaPeak = used;
}
// v57: test-41 - all three of the player's markers came within 2 s of a reading of the processor at 19% (the machine
// slowed itself for a moment; every part of the game ran ~5x slower). Each marker now says whether that was so.
static const char* gdMarkNote() {
    static char b[160]; g_gdMarks++;
    DWORD t = g_gdLastSlowTick, now = GetTickCount();
    if (t && now - t < 5000) { g_gdMarksSlow++; wsprintfA(b, " - the processor was slowed to %d%% %u s before (the machine itself: heat / power limit)", (int)g_gdLastSlowPct, (now - t) / 1000); return b; }
    return "";
}
static void gdTurboReport() {                                     // 30 s report thread
    static LONG lC = 0, lS = 0;
    if (!g_gdLvlChanges || (g_gdLvlChanges == lC && g_gdSecsAfter - lS < 120)) return;
    lC = g_gdLvlChanges; lS = g_gdSecsAfter;
    char b[600]; int n = 0;
    for (int i = 0; i < 5; ++i) if (g_gdLvlSecs[i]) n += wsprintfA(b + n, " | level %d: %d s, %d.%d fps, %d deep falls", i, (int)g_gdLvlSecs[i],
        (int)(g_gdLvlFrames[i] * 10 / g_gdLvlSecs[i] / 10), (int)(g_gdLvlFrames[i] * 10 / g_gdLvlSecs[i] % 10), (int)g_gdLvlDeep[i]);
    logf("machine: processor levels so far%s | now %d, best %d%s", b, (int)g_gdLvl, (int)g_gdLvlBest, g_gdLvlFinal ? " (settled)" : "");
}
static DWORD WINAPI gdThread(LPVOID) {
    LONG last = g_gdFrames; DWORD since = GetTickCount(); bool told = false;
    for (;;) {
        Sleep(2000);
        gdSpeedTick(); gdVaTick();
        LONG f = g_gdFrames;
        if (f != last) { last = f; since = GetTickCount(); if (told) { logf("GUARD: pictures again after the long stop."); told = false; } continue; }
        if (!told && f > 0 && GetTickCount() - since > 20000) {
            told = true;
            logf("GUARD: HANG? no picture for 20 s (guard state: %s, safe mode %s). The long-frame profile below names where the game's thread is. If the game is closed now, the next start of this build runs without this build's new parts.",
                 kGdState[g_gdState], g_gdSafe ? "ON" : "off");
        }
    }
}
// v59: Windows' power throttling (EcoQoS) off for this process - Windows 10 1709+ / 11 lowers the clock of processes
// it thinks are in the background and may ignore their timer resolution; the game asked for 1 ms timers
// (timeBeginPeriod) and must never be "background". Process-wide, nothing else changes. (The power plan and slider
// are set by aotr_start.exe while the game runs.)
static void gdPowerThrottlingOff() {
    struct PPTS { ULONG Version, ControlMask, StateMask; } st = { 1, 1 | 4, 0 };   // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION: both off
    typedef BOOL (WINAPI* tSPI)(HANDLE, int, void*, DWORD);
    tSPI f = (tSPI)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    BOOL ok = f ? f(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof(st)) : FALSE;
    SYSTEM_POWER_STATUS ps; BOOL hp = GetSystemPowerStatus(&ps);
    logf("power: Windows power throttling (EcoQoS) for the game %s | power: %s%s (the plan and slider are set by aotr_start.exe: its window says which)",
         ok ? "OFF (the game is never treated as a background process)" : (f ? "could not be changed on this Windows" : "is not known to this Windows (older than 8)"),
         hp ? (ps.ACLineStatus == 1 ? "mains" : (ps.ACLineStatus == 0 ? "BATTERY" : "unknown")) : "unknown", hp && (ps.SystemStatusFlag & 1) ? ", battery saver ON" : "");
}
// v78: a working-set floor. The v74 test counted 14 000-19 000 page faults a second in calm play (none from the disk)
// and tens of thousands in the slow frames. One source of soft faults is Windows taking pages out of the game's
// working set (trimming) and the game touching them again a moment later. A soft minimum (QUOTA_LIMITS_HARDWS_MIN_
// DISABLE: Windows may still go below it when the machine itself runs short) asks it to leave the game ~1-1.5 GB -
// the game uses 1.6-1.8 GB in a battle. Only on a PC with 6 GB of memory or more, and only when 1.5 GB of it is free.
// Nothing the game computes or draws changes. The diagnostics build counts the trims that remain (vmprobe). Off:
// WSFLOOR_OFF next to the DLL.
static void gdWorkingSetFloor() {
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "WSFLOOR_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("memory: WSFLOOR_OFF is present - Windows' own working-set rules."); return; }
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;
    ULONG64 total = ms.ullTotalPhys >> 20, avail = ms.ullAvailPhys >> 20;
    if (total < 6 * 1024 || avail < 1536) { logf("memory: working-set floor not set (%u MB of memory, %u MB free).", (unsigned)total, (unsigned)avail); return; }
    SIZE_T mn = (SIZE_T)(total >= 12 * 1024 ? 1536 : 1024) << 20, mx = (SIZE_T)3584 << 20;
    HANDLE tok = NULL; TOKEN_PRIVILEGES tp; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        if (LookupPrivilegeValueA(NULL, "SeIncreaseWorkingSetPrivilege", &tp.Privileges[0].Luid)) AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    BOOL ok = SetProcessWorkingSetSizeEx(GetCurrentProcess(), mn, mx, QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);
    DWORD err = ok ? 0 : GetLastError();
    logf("memory: working-set floor %s (%u MB soft minimum; %u MB of memory, %u MB free)%s", ok ? "set" : "NOT set", (unsigned)(mn >> 20), (unsigned)total, (unsigned)avail,
         ok ? "" : (err == ERROR_PRIVILEGE_NOT_HELD ? " - Windows refused the privilege" : " - Windows refused it"));
}
static void installGuard() { { char p[MAX_PATH]; DeleteFileA(aotrPath(p, "aotr_throttle.txt")); DeleteFileA(aotrPath(p, "aotr_throttle_ack.txt")); } gdInit(); gdPowerThrottlingOff(); gdWorkingSetFloor(); CreateThread(NULL, 0, gdThread, NULL, 0, NULL); }
