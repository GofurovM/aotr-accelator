// aotr_cbfind.inc - v77: ControlBar::findNonConstCommandButton by name without walking every command button.
//
// WHAT WAS SLOW (v74 test, 50 samples a second): the top path of the logic step's samples, and in the client's too, was
// 0x406307 < 0x4065AA < 0x71D6B5 < 0x71D6EA: ControlBar::findNonConstCommandButton (0x71D6B5, Open-BFME-2's name) walks
// the control bar's list of command buttons (head at bar+0x2C, next at +0x18, name - an AsciiString - at +0x10) and
// compares each name with the one asked (0x4065AA > 0x4052F9: memcmp over the shorter length, then the lengths: equal
// exactly when the bytes and the lengths are). AOTR defines thousands of command buttons; 83 places in game.dat ask
// (0x71D6EA, the logic's modules and AI among them), so every question walks a long list of cold nodes.
//
// WHAT THIS DOES: the answer for a name (the node, or none) is remembered with the list head it was found under. The
// next question for the same bytes takes it when the head is still the same and, for a node, the node still carries
// that name. Command buttons come from the INI files and the map's INI while a match loads (new ones go in at the head,
// 0x71D79F; an override goes in after the button it replaces); nothing is added or removed during play, and a new head
// clears everything, and so does every new match (the INI of the next map). SELF-PROOF: the first 20000 remembered answers of every match, then (diagnostics build) 1 in 64,
// are also found by the walk; a difference turns this off for the rest of the session. Game thread only; other threads
// get the walk. The result is the engine's own result, so the logic and multiplayer are untouched. Off: CBFIND_OFF.
#define CBF_FN   0x0071D6B5
#define CBF_N    4096
#define CBF_MAXL 47
struct CbfEnt { DWORD gen, node, hash; WORD len; char name[CBF_MAXL + 1]; };
static CbfEnt* g_cbf = NULL;
static volatile LONG g_cbfOn = 0, g_cbfBad = 0, g_cbfProof = 20000, g_cbfTick = 0, g_cbfCalls = 0, g_cbfHits = 0, g_cbfChecked = 0, g_cbfClears = 0;
static DWORD g_cbfHead = 0, g_cbfBar = 0, g_cbfGen = 1; static LONG g_cbfMatch = -1;   // a clear starts a new generation (no memset: a map's INI adds thousands of buttons)
static __forceinline void cbfStr(DWORD s, const char** p, DWORD* n) {     // an AsciiString: data* -> { refs, WORD len (+4), chars (+8) }
    DWORD d = *(DWORD*)(ULONG_PTR)s;
    if (d) { *n = *(WORD*)(ULONG_PTR)(d + 4); *p = (const char*)(ULONG_PTR)(d + 8); } else { *n = 0; *p = ""; }
}
static DWORD cbfWalk(DWORD bar, DWORD want) {                              // the original (0x71D6B5)
    const char* wp; DWORD wn; cbfStr(want, &wp, &wn);
    for (DWORD node = *(DWORD*)(ULONG_PTR)(bar + 0x2C); node; node = *(DWORD*)(ULONG_PTR)(node + 0x18)) {
        const char* np; DWORD nn; cbfStr(node + 0x10, &np, &nn);
        if (nn == wn && !memcmp(np, wp, wn)) return node;
    }
    return 0;
}
static DWORD __fastcall cbfFind(DWORD bar, DWORD, DWORD want) {            // = 0x71D6B5 (thiscall, one argument)
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!g_cbfOn || !gt || __readfsdword(0x24) != gt) return cbfWalk(bar, want);
    g_cbfCalls++;
    const char* wp; DWORD wn; cbfStr(want, &wp, &wn);
    if (wn > CBF_MAXL) return cbfWalk(bar, want);
    DWORD head = *(DWORD*)(ULONG_PTR)(bar + 0x2C);
    if (head != g_cbfHead || bar != g_cbfBar || g_bdgMatches != g_cbfMatch) {   // the list changed at its head, another bar, or a new match
        g_cbfGen++; g_cbfHead = head; g_cbfBar = bar; g_cbfMatch = g_bdgMatches; g_cbfClears++; InterlockedExchange(&g_cbfProof, 20000);
    }
    DWORD h = 0x811C9DC5u; for (DWORD i = 0; i < wn; ++i) h = (h ^ (BYTE)wp[i]) * 0x01000193u;
    h ^= wn;
    CbfEnt& e = g_cbf[(h * 2654435761u) >> 20];
    if (e.gen == g_cbfGen && e.hash == h && e.len == wn && !memcmp(e.name, wp, wn)) {
        DWORD node = e.node;
        bool still = true;
        if (node) { const char* np; DWORD nn; cbfStr(node + 0x10, &np, &nn); still = nn == wn && !memcmp(np, wp, wn); }
        if (still) {
            g_cbfHits++;
            bool check = g_cbfProof > 0 ? (InterlockedDecrement(&g_cbfProof), true) : AOTR_STEADY((++g_cbfTick & 63) == 0);
            if (check) {
                DWORD w = cbfWalk(bar, want); g_cbfChecked++;
                if (w != node) { g_cbfBad++; InterlockedExchange(&g_cbfOn, 0);
                    logf("cbfind: SELF-CHECK - the remembered command button %08X differed from the walk (%08X) - OFF for the rest of this session.", node, w); return w; }
            }
            return node;
        }
    }
    DWORD w = cbfWalk(bar, want);
    e.gen = g_cbfGen; e.node = w; e.hash = h; e.len = (WORD)wn; memcpy(e.name, wp, wn); e.name[wn] = 0;
    return w;
}
static void installCbFind() {
    char mk[MAX_PATH];
    if (!g_engineHooks) return;
    if (GetFileAttributesA(aotrPath(mk, "CBFIND_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("cbfind: CBFIND_OFF is present - the engine's own walk stays."); return; }
    static const BYTE kFn[] = { 0x56, 0x8B, 0x71, 0x2C, 0xEB, 0x13, 0xFF, 0x74, 0x24, 0x08, 0x8D, 0x4E, 0x10, 0xE8, 0xE3, 0x8E, 0xCE, 0xFF,
                                0x85, 0xC0, 0x74, 0x0D, 0x8B, 0x76, 0x18, 0x85, 0xF6, 0x75, 0xE9, 0x33, 0xC0, 0x5E, 0xC2, 0x04, 0x00 };   // 0x71D6B5..0x71D6D8
    static const BYTE kCmp[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x00, 0x85, 0xC0, 0x74, 0x06, 0x0F, 0xB7, 0x50, 0x04 };   // 0x4065AA: the name's length at data+4
    __try {
        if (memcmp((void*)(ULONG_PTR)CBF_FN, kFn, sizeof(kFn)) || memcmp((void*)(ULONG_PTR)0x004065AA, kCmp, sizeof(kCmp)) ||
            memcmp((void*)(ULONG_PTR)0x004052F9, "\x55\x8B\xEC\x56\x8B\x75\x0C\x3B\x75\x14", 10) || *(DWORD*)(ULONG_PTR)0x00405311 != 0x00637D05u) {
            logf("cbfind: findNonConstCommandButton differs from the analysed build - not installed."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_cbf = (CbfEnt*)VirtualAlloc(NULL, sizeof(CbfEnt) * CBF_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_cbf) return;
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)CBF_FN, (void*)&cbfFind, 6);
    resumeAll(fz, nf);
    InterlockedExchange(&g_cbfOn, 1);
    logf("cbfind: ControlBar::findNonConstCommandButton (0x71D6B5) remembers its answers by name (self-proving; CBFIND_OFF).");
}
static void cbfReport() {                                                    // 30 s report thread
    static LONG lC = 0, lH = 0;
    LONG c = g_cbfCalls, h = g_cbfHits;
    if (c != lC) logf("cbfind: %s | last 30 s: %d questions, %d from the remembered answers | proof left %d, compared %d, different %d, cleared %d",
                      g_cbfOn ? "on" : "OFF", (int)(c - lC), (int)(h - lH), (int)g_cbfProof, (int)g_cbfChecked, (int)g_cbfBad, (int)g_cbfClears);
    lC = c; lH = h;
}
