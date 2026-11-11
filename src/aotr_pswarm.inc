// aotr_pswarm.inc - v54: the particle system list read ahead before the engine walks it (read only).
//
// WHAT WAS SLOW (test-35, big battle): twice a frame the engine walks every particle system the FX manager has (STLport
// list at TheFXParticleSystemManager (0xDE3744) + 0x4C, node = { next, prev, handle { system*, ... } }; 1 400 to 13 000
// systems): ParticleSystemManager::update (0x5F5123 = 1.06 0x5F5923) and the W3D particle draw (0x44C84A). For each
// system it links a handle into the system's handle list (0x44C43F: system+0xA0 head, the old head's +8) - ten
// instructions that were 2.9% of the whole client step's samples on their own, with 0x44C4BE 1.8% and 0x44C84A 1.6%:
// the waits for memory of a list walk, one cold system after another.
//
// WHAT THIS DOES: at the entry of those two functions, on the game thread, the list is walked once and each system's
// lines (its first line, +0x98..+0xA4) and the handle at the head of its handle list are requested from memory
// (prefetch, nothing written, nothing called); then the engine's own function runs unchanged. The same thread, before
// the same walk: the list cannot change in between. The particles are pictures only (client side). Off: PSWARM_OFF.
static volatile LONG g_pswOn = 0, g_pswCalls = 0; static volatile LONG64 g_pswNodes = 0, g_pswT = 0;
static DWORD* g_pswSys = NULL;
#define PW_CAP 32768
static void __stdcall pswWarm(DWORD mgr) {                        // mgr: the manager whose list the function walks
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!g_pswOn || !mgr || !gt || __readfsdword(0x24) != gt) return;
    ULONG64 t0 = __rdtsc();
    DWORD head = *(DWORD*)(ULONG_PTR)(mgr + 0x4C); if (!head) return;
    int n = 0;
    for (DWORD node = *(DWORD*)(ULONG_PTR)head; node != head && node && n < PW_CAP; node = *(DWORD*)(ULONG_PTR)node) {
        DWORD s = *(DWORD*)(ULONG_PTR)(node + 8);
        g_pswSys[n++] = s;
        if (s) { _mm_prefetch((const char*)(ULONG_PTR)s, _MM_HINT_T0); _mm_prefetch((const char*)(ULONG_PTR)(s + 0x98), _MM_HINT_T0); _mm_prefetch((const char*)(ULONG_PTR)(s + 0xA4), _MM_HINT_T0); }
    }
    for (int i = 0; i < n; ++i) {
        DWORD s = g_pswSys[i]; if (!s) continue;
        DWORD h = *(DWORD*)(ULONG_PTR)(s + 0xA0);
        if (h) _mm_prefetch((const char*)(ULONG_PTR)(h + 4), _MM_HINT_T0);
        DWORD t = *(DWORD*)(ULONG_PTR)(s + 0x3C);                    // the system's template (0x5F449F reads its +8 for every system it draws)
        if (t) _mm_prefetch((const char*)(ULONG_PTR)(t + 8), _MM_HINT_T0);
    }
    g_pswCalls++; g_pswNodes += n; g_pswT += (LONG64)(__rdtsc() - t0);
}
// Entry stubs: both functions begin with `mov eax, imm32` (their SEH frame's handler) - moved here unchanged.
static __declspec(naked) void pswStubUpdate() {                   // ecx = the manager (its list at +0x4C, 0x5F51BC)
    __asm {
        pushad
        push ecx
        call pswWarm
        popad
        mov  eax, 0x00B7F689
        push 0x005F5128
        ret
    }
}
static __declspec(naked) void pswStubDraw() {                     // the list of TheFXParticleSystemManager (0x44C926)
    __asm {
        cmp  byte ptr [ecx+0xA8], 0                                    // the draw walks the list only when its flag is set (0x44C863)
        je   skip
        pushad
        push dword ptr ds:[0x00DE3744]
        call pswWarm
        popad
    skip:
        mov  eax, 0x00B715CC
        push 0x0044C84F
        ret
    }
}
static void installPsWarm() {
    char mk[MAX_PATH];
    // v55: OFF by default. test-37 measured it: the read-ahead itself was this DLL's largest cost in the client step
    // (~0.7 ms a frame: it pays the same misses the engine's walk paid) and the engine's walk did not get faster
    // (0x44C43F 2.9% -> 2.5% of the client samples). The real waste in that list was the whole-list search of every
    // removal - aotr_psremove.inc. PSWARM_ON next to the DLL turns this back on for a measurement.
    if (GetFileAttributesA(aotrPath(mk, "PSWARM_ON")) == INVALID_FILE_ATTRIBUTES) { logf("pswarm: off (v55 default - measured no gain in test-37; PSWARM_ON turns it on)."); return; }
    __try {
        if (memcmp((void*)0x005F5123, "\xB8\x89\xF6\xB7\x00\xE8", 6) || memcmp((void*)0x005F51BC, "\x8B\x47\x4C\x56\x8B\x30\x3B\xF0", 8) ||
            memcmp((void*)0x0044C84A, "\xB8\xCC\x15\xB7\x00\xE8", 6) || memcmp((void*)0x0044C926, "\xA1\x44\x37\xDE\x00\x83\xC0\x4C", 8) ||
            memcmp((void*)0x0044C43F, "\x8B\x01\x05\xA0\x00\x00\x00", 7)) { logf("pswarm: the particle list walks differ from the analysed build - off."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_pswSys = (DWORD*)VirtualAlloc(NULL, PW_CAP * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_pswSys) { logf("pswarm: no memory - off."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x005F5123, (void*)&pswStubUpdate, 5);
    patchJmp((BYTE*)0x0044C84A, (void*)&pswStubDraw, 5);
    resumeAll(fz, nf);
    g_pswOn = 1;
    logf("pswarm: the particle system list is read ahead (prefetch only) before ParticleSystemManager::update (0x5F5123) and the W3D particle draw (0x44C84A) walk it (PSWARM_OFF).");
}
static void pswReport() {                                          // 30 s report thread
    static LONG lC = 0; static LONG64 lN = 0, lT = 0;
    LONG c = g_pswCalls; LONG64 n = g_pswNodes, t = g_pswT, tm = g_msTicksMs;
    if (c == lC || !tm) { lC = c; return; }
    logf("pswarm: last 30 s - %d list read-aheads, %d systems a walk, %d us a read-ahead", (int)(c - lC), (int)((n - lN) / (c - lC)), (int)((t - lT) * 1000 / tm / (c - lC)));
    lC = c; lN = n; lT = t;
}
