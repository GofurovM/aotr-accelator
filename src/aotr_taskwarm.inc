// aotr_taskwarm.inc - v77: the FX shader renderer's task list read ahead before the engine walks it (prefetch only).
//
// WHAT WAS SLOW (v74 test, full graphics, 50 samples a second): FXShaderRenderer::PerformRendering (0x573BEB, called by
// the three range flushers 0x574135 / 0x574284 / 0x5742DB, for the shadow map pass and the main pass alike) walks the
// sorted 32-byte rendering tasks and groups neighbours that can share a draw: for each pair it compares the meshes'
// light environments (0x549430 > 0x53F3B0 / 0x53F1A0). About half of the samples under it were in those two compares,
// on data spread over memory - each task's mesh (+0x00 vtable, +0xC4 / +0xC8, +0x314..+0x31C), the mesh's material
// view ([+0xC4]+0x94 > +0xB8 / +0x108) and its light environment (0x170 bytes: count +4, the lights' flags at
// +0x39 + 0x54 * i, ambient +0x164). One cold mesh after another: the walk waits for memory, task by task.
//
// WHAT THIS DOES: at the function's entry, on the game thread, the task array [begin, end) is walked once ahead of the
// engine, in three stages so the fetches overlap instead of waiting one after another: 12 tasks ahead the mesh's lines
// are requested; 6 tasks ahead its light environment and material view (now in cache) are requested; at the task
// itself the material view's target lines. Only `prefetch` instructions and reads of fields the engine reads right
// after; nothing is written, nothing is called. Then the engine's own function runs unchanged. The picture cannot
// change. Off: TASKWARM_OFF.
//
// The diagnostics build measures it: frames alternate in blocks of 64 between read-ahead and none, and PerformRendering's
// own time is added up per block kind - the 30 s line gives both, so a test shows the gain (or that there is none).
#define TKW_FN      0x00573BEB
#define TKW_FN_NEXT 0x00573BF0
static volatile LONG g_tkwOn = 0, g_tkwFault = 0;
// v80: the trial, in every build - frames alternate in blocks of 64 with and without the read-ahead and
// PerformRendering's own time is added up per kind; after 900 frames of each the release build decides once (the
// read-ahead stays when its frames cost at most 97% of the others) and stops timing. The diagnostics build keeps
// alternating all session, for its 30 s line.
static volatile LONG g_tkwTrial = 1;
static __forceinline void tkwPf(DWORD a) { _mm_prefetch((const char*)(ULONG_PTR)a, _MM_HINT_T0); }
static void __stdcall tkwWarm(DWORD begin, DWORD end) {
    if (g_tkwTrial && ((g_frW >> 6) & 1)) return;   // the measurement's other half: no read-ahead
    if (!g_tkwOn || end <= begin || ((end - begin) & 31)) return;
    DWORD n = (end - begin) >> 5;
    if (n < 8 || n > 262144) return;
    const DWORD* t = (const DWORD*)(ULONG_PTR)begin;
    __try {
        for (DWORD i = 0; i < n + 12; ++i) {
            if (i < n) { DWORD m = t[i * 8]; if (m) { tkwPf(m); tkwPf(m + 0xC0); tkwPf(m + 0x300); } }
            if (i >= 6 && i - 6 < n) {
                DWORD m = t[(i - 6) * 8];
                if (m) {
                    DWORD v = *(DWORD*)(ULONG_PTR)(m + 0xC4), le = *(DWORD*)(ULONG_PTR)(m + 0xC8);
                    if (v) tkwPf(v + 0x94);
                    if (le) { tkwPf(le); tkwPf(le + 0x40); tkwPf(le + 0x80); tkwPf(le + 0xC0); tkwPf(le + 0x100); tkwPf(le + 0x140); }
                }
            }
            if (i >= 12 && i - 12 < n) {
                DWORD m = t[(i - 12) * 8];
                DWORD v = m ? *(DWORD*)(ULONG_PTR)(m + 0xC4) : 0, s = v ? *(DWORD*)(ULONG_PTR)(v + 0x94) : 0;
                if (s) { tkwPf(s + 0xB8); tkwPf(s + 0x108); }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_tkwOn, 0); InterlockedIncrement(&g_tkwFault); }
}
// the measurement: the function's own time, by block kind (read-ahead / none)
static volatile LONG64 g_tkwTicks[2] = { 0, 0 }; static volatile LONG g_tkwCalls[2] = { 0, 0 }, g_tkwFrames[2] = { 0, 0 };
// v79: a stack of return addresses (game thread only, at most 8 deep): a nested call can never return to the wrong place
static DWORD g_tkwRet[8]; static ULONG64 g_tkwT0[8]; static LONG g_tkwK[8]; static LONG g_tkwDepth = 0, g_tkwLastFrame = -1;
static DWORD __stdcall tkwEnd() {                     // returns where the function's caller is
    LONG d = --g_tkwDepth;
    g_tkwTicks[g_tkwK[d]] += (LONG64)(__rdtsc() - g_tkwT0[d]); g_tkwCalls[g_tkwK[d]]++;
    return g_tkwRet[d];
}
static __declspec(naked) void tkwExit() {             // the function's `ret 8` lands here; eax (its result) is kept
    __asm {
        push 0                                         // becomes the real return address
        pushad
        call tkwEnd
        mov  [esp + 32], eax
        popad
        ret
    }
}
static void __stdcall tkwBegin(DWORD* retSlot) {
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!gt || __readfsdword(0x24) != gt || g_tkwDepth >= 8) return;   // not measured: the call returns as usual
    LONG f = g_frW, d = g_tkwDepth;
    g_tkwK[d] = (f >> 6) & 1;
    if (f != g_tkwLastFrame) { g_tkwLastFrame = f; g_tkwFrames[g_tkwK[d]]++; }
    g_tkwRet[d] = *retSlot; *retSlot = (DWORD)(ULONG_PTR)&tkwExit;
    g_tkwDepth = d + 1;
    g_tkwT0[d] = __rdtsc();
}
static void tkwDecide() {                             // the 30 s report thread: once, in the release build
#ifndef AOTR_DIAG
    if (!g_tkwTrial || g_tkwFrames[0] < 900 || g_tkwFrames[1] < 900) return;
    double a = (double)g_tkwTicks[0] / g_tkwFrames[0], b = (double)g_tkwTicks[1] / g_tkwFrames[1];
    int pct = b > 0 ? (int)(a * 100.0 / b) : 100;
    InterlockedExchange(&g_tkwTrial, 0);
    if (pct > 97) { InterlockedExchange(&g_tkwOn, 0); logf("taskwarm: trial - with the read-ahead PerformRendering took %d%% of its time without; off on this PC.", pct); }
    else logf("taskwarm: trial - with the read-ahead PerformRendering takes %d%% of its time without; it stays on.", pct);
#endif
}
static __declspec(naked) void tkwStub() {             // replaces 0x573BEB: mov eax, 0B7BF8Ch
    __asm {
        pushad
        push dword ptr [esp + 32 + 8]                  // end   (the caller's second argument)
        push dword ptr [esp + 32 + 4 + 4]              // begin (the first)
        call tkwWarm
        cmp  g_tkwTrial, 0
        je   no_time
        lea  eax, [esp + 32]                           // the return address
        push eax
        call tkwBegin
    no_time:
        popad
        mov  eax, 0x00B7BF8C
        push TKW_FN_NEXT
        ret
    }
}
static void installTaskWarm() {
    char mk[MAX_PATH];
    if (!g_engineHooks) return;
    // v82.1: OFF by default. The v82 test's 55 alternating 30 s windows: PerformRendering 3698 us a frame with the
    // read-ahead, 3617 us without (102%) - the compares were not waiting for memory after all. TASKWARM_ON turns it on.
    if (GetFileAttributesA(aotrPath(mk, "TASKWARM_ON")) == INVALID_FILE_ATTRIBUTES) { logf("taskwarm: off (v82.1 default - no gain measured in the v82 test; TASKWARM_ON turns it on)."); return; }
    static const BYTE kHead[16] = { 0xB8, 0x8C, 0xBF, 0xB7, 0x00, 0xE8, 0xFB, 0x92, 0x4C, 0x00, 0x81, 0xEC, 0xE8, 0x05, 0x00, 0x00 };
    __try {
        if (memcmp((void*)(ULONG_PTR)TKW_FN, kHead, sizeof(kHead)) || memcmp((void*)(ULONG_PTR)0x00573F1B, "\xC2\x08\x00", 3) ||
            memcmp((void*)(ULONG_PTR)0x00573C26, "\x8B\x06\x8B\x0B\x50\xE8", 6) || memcmp((void*)(ULONG_PTR)0x00548C50, "\x8B\x91\x14\x03\x00\x00", 6)) {
            logf("taskwarm: PerformRendering differs from the analysed build - not installed."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)TKW_FN, (void*)&tkwStub, 5);
    resumeAll(fz, nf);
    InterlockedExchange(&g_tkwOn, 1);
    logf("taskwarm: the FX renderer's task list is read ahead (prefetch only) before PerformRendering (0x573BEB) compares its neighbours (TASKWARM_OFF).");
}
static void tkwReport() {                             // 30 s
    tkwDecide();
#ifdef AOTR_DIAG
    static LONG64 lT[2]; static LONG lC[2], lF[2];
    double tpm = g_tscPerQpc > 0 ? g_tscPerQpc * (double)g_pqpf.QuadPart / 1000.0 : 0;
    if (tpm <= 0) return;
    int us[2], calls[2];
    for (int k = 0; k < 2; ++k) {
        LONG64 d = g_tkwTicks[k] - lT[k]; LONG c = g_tkwCalls[k] - lC[k], f = g_tkwFrames[k] - lF[k];
        lT[k] = g_tkwTicks[k]; lC[k] = g_tkwCalls[k]; lF[k] = g_tkwFrames[k];
        us[k] = f > 0 ? (int)(d / tpm * 1000.0 / f) : -1; calls[k] = f > 0 ? (int)(c / f) : 0;
    }
    logf("taskwarm: last 30 s - PerformRendering %d us a frame with the read-ahead (%d calls), %d us without (%d calls)%s",
         us[0], calls[0], us[1], calls[1], g_tkwOn ? "" : (g_tkwFault ? " - off after an exception" : " - off"));
#endif
}
