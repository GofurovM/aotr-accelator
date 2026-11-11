// aotr_psremove.inc - v55: a particle system leaves the FX manager's list without a search of the whole list.
//
// WHAT WAS SLOW (test-37, the peak of a big battle with spells: client step 35 ms, the FX particle update 7-8 ms of it):
// the single hottest function of the client step was 0x5F3FD7 (2.8% of its samples, 10 instructions): std::find over
// the FX manager's list of systems (TheFXParticleSystemManager+0x4C, node = { next, prev, handle { system*, ...} }),
// called from 0x5F7909 - "remove this system from the manager" - which every particle system's destructor calls
// (0x5FB5B6). With 1 000 - 13 000 systems alive and many effects ending every frame, every ending effect walked the
// whole list. Every faction's every effect goes through this one function.
//
// WHAT THIS DOES: the two functions that change that list are replaced by the same work plus a table system -> node:
//   0x5F806C (add: push_back of the handle, count +1) - and the new node (the one before the list's end) is remembered;
//   0x5F7909 (remove: find the first node holding the system, erase it with the engine's 0x5F5BC9, count -1) - the
//   node is taken from the table when it is still linked both ways and still holds this system, else found by the
//   same walk as the original. The table entry goes with the erase. The list is only ever changed by these two and by
//   the manager's destructor (0x5F962A: the table is per manager and checked, a dead manager's nodes are never used).
// SELF-PROOF: the first 20000 removals, then 1 in 64, also walk the list; a difference turns the table off for the
// session (the walk is then used). Particles are pictures only. Off: PSREMOVE_OFF next to the DLL.
#define PSRM_TAB 65536
struct PsrEnt { DWORD sys, node, mgr; };
static PsrEnt* g_psr = NULL; static LONG g_psrCount = 0;
static CRITICAL_SECTION g_psrCs;
static volatile LONG g_psrOn = 0, g_psrBad = 0, g_psrProof = 20000, g_psrTick = 0;
static volatile LONG g_psrRemoves = 0, g_psrHits = 0, g_psrWalks = 0, g_psrChecked = 0, g_psrAdds = 0; static volatile LONG64 g_psrWalkNodes = 0;
static __forceinline DWORD psrSlot(DWORD sys) { return ((sys >> 3) * 2654435761u) >> 16; }
static void psrPut(DWORD mgr, DWORD sys, DWORD node) {
    if (!g_psr || g_psrCount > PSRM_TAB / 2) return;
    for (DWORD i = psrSlot(sys), k = 0; k < PSRM_TAB; i = (i + 1) & (PSRM_TAB - 1), ++k) {
        if (g_psr[i].sys == sys) return;                          // a second node for the same system: the walk finds the first, keep it
        if (!g_psr[i].sys) { g_psrCount++; g_psr[i].sys = sys; g_psr[i].node = node; g_psr[i].mgr = mgr; return; }
    }
}
static PsrEnt* psrGet(DWORD sys) {
    if (!g_psr) return NULL;
    for (DWORD i = psrSlot(sys), k = 0; k < PSRM_TAB; i = (i + 1) & (PSRM_TAB - 1), ++k) {
        if (!g_psr[i].sys) return NULL;
        if (g_psr[i].sys == sys) return &g_psr[i];
    }
    return NULL;
}
static void psrDel(PsrEnt* e) {                                     // linear probing: shift the cluster back over the hole
    DWORD i = (DWORD)(e - g_psr);
    g_psr[i].sys = 0; g_psrCount--;
    for (DWORD j = (i + 1) & (PSRM_TAB - 1); g_psr[j].sys; j = (j + 1) & (PSRM_TAB - 1)) {
        DWORD h = psrSlot(g_psr[j].sys);
        bool move = (j > i) ? (h <= i || h > j) : (h <= i && h > j);
        if (move) { g_psr[i] = g_psr[j]; g_psr[j].sys = 0; i = j; }
    }
}
static DWORD psrWalk(DWORD mgr, DWORD sys) {                        // the original find (0x5F3FD7 over [mgr+0x4C])
    DWORD head = *(DWORD*)(ULONG_PTR)(mgr + 0x4C); LONG v = 0;
    DWORD n = *(DWORD*)(ULONG_PTR)head;
    for (; n != head; n = *(DWORD*)(ULONG_PTR)n) { ++v; if (*(DWORD*)(ULONG_PTR)(n + 8) == sys) break; }
    g_psrWalkNodes += v;
    return n;                                                    // head (end) when not found
}
typedef void (__fastcall* tPsrPushBack)(DWORD list, DWORD edx, DWORD handle);
typedef void (__fastcall* tPsrErase)(DWORD list, DWORD edx, DWORD* out, DWORD node);
static void __fastcall psrAdd(DWORD mgr, DWORD, DWORD handle) {      // = 0x5F806C
    EnterCriticalSection(&g_psrCs);
    ((tPsrPushBack)(ULONG_PTR)0x005F79BF)(mgr + 0x4C, 0, handle);
    *(LONG*)(ULONG_PTR)(mgr + 0x58) += 1;
    if (g_psrOn) {
        DWORD head = *(DWORD*)(ULONG_PTR)(mgr + 0x4C), node = *(DWORD*)(ULONG_PTR)(head + 4), sys = *(DWORD*)(ULONG_PTR)(node + 8);
        if (node != head && sys) { psrPut(mgr, sys, node); g_psrAdds++; }
    }
    LeaveCriticalSection(&g_psrCs);
}
// 0x5F7909's argument is a POINTER to a handle (its caller 0x5FB5B6 makes a temporary handle of the system with
// 0x44C49C and passes its address); std::find (0x5F3FD7) compares *arg - the system - with each node's handle.system.
// (v55 first build read the argument as the system itself: nothing was found, the count drifted from the list and the
// manager's reset loop 0x5F50D4 never ended - the game hung on the loading screen. Fixed in build 104.)
static void __fastcall psrRemove(DWORD mgr, DWORD, DWORD keyPtr) {   // = 0x5F7909
    DWORD sys = keyPtr ? *(DWORD*)(ULONG_PTR)keyPtr : 0;
    EnterCriticalSection(&g_psrCs);
    g_psrRemoves++;
    DWORD head = *(DWORD*)(ULONG_PTR)(mgr + 0x4C), node = 0;
    PsrEnt* e = g_psrOn ? psrGet(sys) : NULL;
    if (e) {
        DWORD n = e->node;
        __try {
            DWORD nx = *(DWORD*)(ULONG_PTR)n, pv = *(DWORD*)(ULONG_PTR)(n + 4);
            if (e->mgr == mgr && n != head && nx && pv && *(DWORD*)(ULONG_PTR)(nx + 4) == n && *(DWORD*)(ULONG_PTR)pv == n && *(DWORD*)(ULONG_PTR)(n + 8) == sys) node = n;
        } __except (EXCEPTION_EXECUTE_HANDLER) { node = 0; }
        psrDel(e);
    }
    if (node) {
        g_psrHits++;
        bool check = g_psrProof > 0 ? (g_psrProof--, true) : AOTR_STEADY((++g_psrTick & 63) == 0);
        if (check) {
            DWORD w = psrWalk(mgr, sys); g_psrChecked++;
            if (w != node) { g_psrBad++; g_psrOn = 0; logf("psremove: the remembered node %08X differed from the list walk (%08X) for system %08X - the table is OFF for the rest of this session.", node, w, sys); node = w; }
        }
    } else { node = psrWalk(mgr, sys); g_psrWalks++; }
    if (node != head) {                                          // the original: erase the found node, count -1 (nothing when not found)
        DWORD out = 0;
        ((tPsrErase)(ULONG_PTR)0x005F5BC9)(mgr + 0x4C, 0, &out, node);
        *(LONG*)(ULONG_PTR)(mgr + 0x58) -= 1;
    }
    LeaveCriticalSection(&g_psrCs);
}
static void installPsRemove() {
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "PSREMOVE_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("psremove: PSREMOVE_OFF is present - the engine's list search stays."); return; }
    static const BYTE kRem[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0xFF, 0x75, 0x08, 0x8B, 0xF9, 0x51, 0x8B, 0xC4, 0x8D, 0x77, 0x4C, 0x8B, 0x0E };
    static const BYTE kAdd[] = { 0x56, 0xFF, 0x74, 0x24, 0x08, 0x8B, 0xF1, 0x8D, 0x4E, 0x4C, 0xE8, 0x44, 0xF9, 0xFF, 0xFF, 0xFF, 0x46, 0x58, 0x5E, 0xC2, 0x04, 0x00 };
    static const BYTE kPb[]  = { 0x55, 0x8B, 0xEC, 0xFF, 0x75, 0x08, 0x8B, 0x11, 0x51, 0x8B, 0xC4, 0x89, 0x10 };
    static const BYTE kEr[]  = { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10, 0x8B, 0x37, 0x8B, 0x47, 0x04, 0x89, 0x30 };
    static const BYTE kFind[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x3B, 0x4D, 0x10, 0x74, 0x11, 0x8B, 0x45, 0x14, 0x8B, 0x00, 0x39, 0x41, 0x08 };
    __try {
        if (memcmp((void*)0x005F7909, kRem, sizeof(kRem)) || memcmp((void*)0x005F806C, kAdd, sizeof(kAdd)) || memcmp((void*)0x005F79BF, kPb, sizeof(kPb)) ||
            memcmp((void*)0x005F5BC9, kEr, sizeof(kEr)) || memcmp((void*)0x005F3FD7, kFind, sizeof(kFind)) ||
            memcmp((void*)0x005F7944, "\xE8\x80\xE2\xFF\xFF\xFF\x4F\x58", 8) || memcmp((void*)0x005F5BF2, "\xC2\x08\x00", 3)) {
            logf("psremove: the FX manager's add / remove differ from the analysed build - off."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_psr = (PsrEnt*)VirtualAlloc(NULL, sizeof(PsrEnt) * PSRM_TAB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_psr) { logf("psremove: no memory - off."); return; }
    InitializeCriticalSectionAndSpinCount(&g_psrCs, 4000);
    g_psrOn = 1;
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x005F7909, (void*)&psrRemove, 5);
    patchJmp((BYTE*)0x005F806C, (void*)&psrAdd, 5);
    resumeAll(fz, nf);
    logf("psremove: a particle system leaving the FX manager (0x5F7909) finds its list node from a table kept at 0x5F806C instead of walking the whole list (self-proving; PSREMOVE_OFF).");
}
static void psrReport() {                                          // 30 s report thread
    static LONG lR = 0, lH = 0, lW = 0; static LONG64 lN = 0;
    LONG r = g_psrRemoves, h = g_psrHits, w = g_psrWalks; LONG64 n = g_psrWalkNodes;
    if (!g_psr || r == lR) return;
    logf("psremove: %s | last 30 s: %d systems removed, %d from the table, %d walked (%d list nodes visited in all walks and checks) | table %d entries | proof left %d, compared %d, different %d",
         g_psrOn ? "on" : (g_psrBad ? "OFF (a difference was found)" : "off"), (int)(r - lR), (int)(h - lH), (int)(w - lW), (int)(n - lN), (int)g_psrCount, (int)g_psrProof, (int)g_psrChecked, (int)g_psrBad);
    lR = r; lH = h; lW = w; lN = n;
}
