// ================================================================ the status badge, the status file and the refresh (v71)
// The release build shows one thing on the screen: a small round light in the top right corner of the picture.
//   green  - everything is running;
//   yellow - a part switched itself off (a self-check found a difference, the render thread stepped aside for a
//            moment, or the load guard's safe mode) - the game goes on with the engine's own code for that part;
//   red    - the main part is not running: the render thread could not start (Direct3D is Windows' own, not DXVK),
//            stepped aside for the rest of the session, or this game.dat is not a build whose addresses we know.
// F7 hides and shows it; the choice is kept in bfme2_accel.ini ([Engine] OverlayShow). It is painted by the overlay's
// in-frame code (aotr_overlay.inc, badge mode): a few IDirect3DDevice9::Clear rectangles, nothing else is set.
//
// aotr_status.txt (beside the DLL) tells aotr_start.exe the same, so its tray icon can say it too - and so that it
// can tell when the DLL never came up at all (an antivirus that blocked it), which nothing inside the game can show.
//
// The refresh: after every fifth match, back in the menu, the render thread's short-cut tables (new buffers and
// textures nothing has named yet, CPU mirrors of buffers and radar textures) and terrain-keep's parked ground
// textures are let go - a session of many matches starts the next five from a clean slate. Nothing the game has
// loaded, nothing in the Windows file cache and nothing of the learned list is touched: those are what make the
// second and third match lighter, and they stay. It runs on the game thread, in the menu, in a few milliseconds.

static volatile LONG g_bdgMenu = 0, g_bdgMatches = 0, g_bdgRefreshes = 0;
static LONG g_bdgMode = -1, g_bdgRefreshAt = 0;

// 0 green, 1 yellow, 2 red; why: a short reason for the tray (English)
static int bdgState(const char** why) {
    static const char* const kNone = "";
    if (why) *why = kNone;
    if (!g_engineHooks) { if (why) *why = "unknown game.dat build - most parts stay off"; return 2; }
    if (g_rtTried && !g_rtInstalled) { if (why) *why = "render thread off - Direct3D is not DXVK"; return 2; }
    if (g_rtBypassN > RT_BYP_RETRIES) { if (why) *why = "render thread stepped aside for the session"; return 2; }
    if (g_bdgProblems > 0 || g_gdSafe) { if (why) *why = g_gdSafe ? "safe mode after a hang while loading" : (g_bdgWhy[0] ? g_bdgWhy : "a part switched itself off"); return 1; }
    return 0;
}
static DWORD bdgColour() {
    switch (bdgState(NULL)) { case 2: return 0xFFFF3030u; case 1: return 0xFFFFC020u; default: return 0xFF30E050u; }
}

// game thread, every frame (aotr_frameprof.inc): which screen the game is on, matches counted, the refresh
static void bdgRefreshNow() {
    if (g_rtInstalled && GetCurrentThreadId() == g_rtMainTid) {
        if (g_rtTMAny) { rtTMKillAll(); rtTMReap(); }
        if (g_rtBMAny) { rtBMKillAll(); rtBMReap(); }
        rtDirectLocksForget(); rtFreshForget(); rtFBForget();
        if (g_rtActive) rtDrain();
    }
    if (g_tkN) tkDropAll();
    g_bdgRefreshes++;
    logf("refresh: %d matches played - in the menu the render thread's short-cut tables and the parked ground textures were let go (the game's loaded art, the file cache and the learned list are kept).",
         (int)g_bdgMatches);
}
static void bdgOnFrame() {
    if (!g_engineHooks) return;
    DWORD gl = *(DWORD*)(ULONG_PTR)0x00DE412C;                   // TheGameLogic
    if (!gl) return;
    LONG mode = *(LONG*)(ULONG_PTR)(gl + 0x110);                 // 9 = the menu's shell map (0 before it starts)
    bool menu = mode == 9 || mode == 0;
    if (mode != g_bdgMode) {
        if (!menu && (g_bdgMode == 9 || g_bdgMode == 0 || g_bdgMode == -1)) InterlockedIncrement(&g_bdgMatches);
        g_bdgMode = mode;
    }
    g_bdgMenu = menu ? 1 : 0;
    if (menu && g_bdgMatches - g_bdgRefreshAt >= 5) { g_bdgRefreshAt = g_bdgMatches; bdgRefreshNow(); }
}

// report thread, every 5 s: aotr_status.txt when something changed, and the F7 choice kept in the ini
static void bdgWriteStatus() {
    static char last[256] = "";
    const char* why = ""; int st = bdgState(&why);
    static const char* const kSt[3] = { "green", "yellow", "red" };
    char b[256];
    wsprintfA(b, "state=%s\r\nwhy=%s\r\nmenu=%d\r\nmatches=%d\r\npid=%u\r\nbuild=%d\r\n", kSt[st], why, (int)g_bdgMenu, (int)g_bdgMatches,
              GetCurrentProcessId(), AOTR_BUILDNO_GUARD);
    if (!lstrcmpA(b, last)) return;
    char p[MAX_PATH], t[MAX_PATH];
    aotrPath(p, "aotr_status.txt"); aotrPath(t, "aotr_status.tmp");
    HANDLE h = CreateFileA(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; BOOL ok = WriteFile(h, b, (DWORD)lstrlenA(b), &w, NULL); CloseHandle(h);
    if (ok && MoveFileExA(t, p, MOVEFILE_REPLACE_EXISTING)) lstrcpynA(last, b, sizeof(last));
}
static volatile LONG g_bdgShowSaved = -1;
static void bdgSaveShow(LONG show) {
    if (show == g_bdgShowSaved) return;
    if (g_bdgShowSaved >= 0) { char p[MAX_PATH]; WritePrivateProfileStringA("Engine", "OverlayShow", show ? "1" : "0", aotrPath(p, "bfme2_accel.ini")); }
    g_bdgShowSaved = show;
}
