// ================================================================ v50: one session, four settings, measured side by side
// test-30 (v47) had 14 hitches of 100 ms+ in the battle, the longest 140 ms; test-31/32 (v48.1/v49) on the same saved
// game had 35-45, many of 250-460 ms - logic steps of 250-690 ms whose module updates all ran several times slower than
// usual. The two things v46-v48 changed that stay active through the whole battle are the ground textures kept out of
// view (terrain-keep, v48) and the game thread having a physical core to itself (v46). Instead of a test per question,
// the battle is cut into 45 s stretches that take turns: both on / both off / only terrain-keep off / only the core off,
// and each setting's frames are counted on their own. The report says which setting the hitches come with.
// AB_OFF beside the DLL = no test (both stay on).
static volatile LONG g_abOn = 0, g_abMode = 0, g_avSeg = 0;
static DWORD g_abSegStart = 0;
static const BYTE kAbOrder[4] = { 0, 3, 1, 2 };            // bit 0: terrain-keep off, bit 1: own core off
static const char* const kAbName[4] = { "both on", "terrain-keep OFF", "own core OFF", "both OFF" };
struct AbStat { LONG frames, f100, f250; LONG64 time, logic, client; DWORD ms; };
static AbStat g_av[4];
static void abApply(LONG mode) {                            // game thread
    LONG tkOff = mode & 1, coreOff = (mode >> 1) & 1;
    if (g_tkInstalled) {
        if (tkOff && g_tkOn) { InterlockedExchange(&g_tkOn, 0); tkDropAll(); }
        else if (!tkOff && !g_tkOn) InterlockedExchange(&g_tkOn, 1);
    }
    if (g_coreOn) {
        if (coreOff && !g_corePaused) { g_corePaused = 1; SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)g_coreProc); }
        else if (!coreOff && g_corePaused) { SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)g_coreGame); g_corePaused = 0; }
    }
    g_abMode = mode;
}
static void abFrame(DWORD period, DWORD logic, DWORD client) {   // game thread, every frame (frameprof)
    if (!g_abOn) return;
    DWORD now = GetTickCount(), lt = g_frLastLogicTick;
    bool play = lt && now - lt < 2000;
    if (!play) return;
    if (!g_abSegStart) { g_abSegStart = now; abApply(kAbOrder[0]); }
    if (now - g_abSegStart >= 45000) {
        g_av[g_abMode].ms += now - g_abSegStart;
        g_avSeg++; g_abSegStart = now;
        abApply(kAbOrder[g_avSeg & 3]);
        logf("abtest: stretch %d - %s for the next 45 s.", (int)g_avSeg + 1, kAbName[g_abMode]);
    }
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    AbStat& a = g_av[g_abMode];
    a.frames++; a.time += period; a.logic += logic; a.client += client;
    if ((LONG64)period * 10 > f) a.f100++;
    if ((LONG64)period * 4 > f) a.f250++;
}
static void abReport() {                                    // every 30 s from the report thread, and into aotr_report.txt
    if (!g_abOn || !g_avSeg) return;
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    char ln[900]; int p = 0;
    for (int m = 0; m < 4; ++m) {
        const AbStat& a = g_av[m]; if (!a.frames) continue;
        double sec = (double)a.time / f, fps = sec > 0 ? a.frames / sec : 0, per = sec / 60.0;
        p += wsprintfA(ln + p, " | %s: %d s, %d.%d fps, hitches 100ms+ %d (%d a minute), 250ms+ %d, logic %d.%d + drawing %d.%d ms a frame",
                       kAbName[m], (int)sec, (int)fps, (int)(fps * 10) % 10, (int)a.f100, per > 0 ? (int)(a.f100 / per + 0.5) : 0, (int)a.f250,
                       (int)(a.logic * 1000 / f / a.frames), (int)(a.logic * 10000 / f / a.frames) % 10, (int)(a.client * 1000 / f / a.frames), (int)(a.client * 10000 / f / a.frames) % 10);
    }
    logf("abtest: so far%s", ln);
}
static void installAbTest() {
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "AB_ON")) == INVALID_FILE_ATTRIBUTES) return;     // v51: only when asked for (AB_ON)
    g_abOn = 1;
    logf("abtest: on - the battle is cut into 45 s stretches that take turns: %s / %s / %s / %s; each setting's frames are counted on their own (the 'abtest:' lines).", kAbName[0], kAbName[3], kAbName[1], kAbName[2]);
}
