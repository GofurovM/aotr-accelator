// aotr_sched.inc - v33 (ISH-7): how the game thread waits and where it runs. Read only for the game's results.
//
// 1. The frame limiter (game.dat 0x0063A1DC): when a frame is done early the engine loops
//        do { Sleep(0); t = timeGetTime(); } while (t - [0x00DE4318] < esi);      // esi = the frame's length in ms
//    Sleep(0) returns at once when nothing else wants the processor, so the game thread spins on one core for the
//    whole rest of the frame - the "game thread 99 %" of every log, also in calm moments with 1 ms of work a frame.
//    On a laptop that core's heat and power come out of the budget of the cores that do the work (turbo), and its
//    hyper-thread twin gets half a core. The call to Sleep(0) at 0x0063A1DD becomes a call to limWait: while 3 ms or
//    more of the frame are left it sleeps on a high-resolution timer until 2 ms before the end, then returns; the
//    engine's own loop is unchanged (its timeGetTime test still decides when the frame ends, with its Sleep(0) spin
//    in the last 2 ms), so every frame ends at the same millisecond as before. The limit itself (the frame length
//    the engine computes) is not touched. AOTR_LIMWAIT=0 turns it off.
// 2. Options for measurement only (all off unless set): AOTR_PRIO=1 high process priority, AOTR_MMCSS=1 the game
//    thread in the "Games" multimedia class, AOTR_AFFINITY=1 the game thread and the render worker each on its own
//    physical core (first logical processors of cores 2 and 3).

#define LW_SITE   0x0063A1DD
#define LW_LAST   0x00DE4318
#define LW_TGT    0x00BD0920                               // the game's timeGetTime import slot
static volatile LONG g_lwOn = 0, g_lwSleeps = 0, g_lwSpins = 0;
static volatile LONG64 g_lwSleptUs = 0;
static HANDLE g_lwTimer = NULL;
static LONG g_schPrio = 0, g_schMmcss = 0, g_schAff = 0, g_schDone = 0;
typedef HANDLE (WINAPI* tAvSet)(LPCSTR, LPDWORD);
static tAvSet p_avSet = NULL;

static void __stdcall limWait(DWORD target) {
    DWORD now = ((DWORD(WINAPI*)())*(void**)(ULONG_PTR)LW_TGT)();
    DWORD el = now - *(volatile DWORD*)(ULONG_PTR)LW_LAST;
    if (g_lwTimer && el + 3 <= target && target < 1000) {
        DWORD ms = target - el - 2;
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)ms * 10000;
        LONG64 t0 = qpcNow();
        if (SetWaitableTimer(g_lwTimer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(g_lwTimer, ms + 50);
        else Sleep(0);
        g_lwSleptUs += (qpcNow() - t0) * 1000000 / qpcHz(); g_lwSleeps++;
        return;
    }
    g_lwSpins++;
    Sleep(0);
}
static __declspec(naked) void limStub() {
    __asm {
        push esi                      // the frame's length in ms (the loop's bound)
        call limWait
        ret 4                         // the engine pushed Sleep's argument
    }
}

// 3. v45: the game thread gets a physical core of its own (test-26: the game thread was 96-97 % busy, but no logical
//    processor was more than ~50 % busy - Windows moved it between cores all the time, and the render worker (45 %),
//    DXVK's threads and our helpers could land on its hyper-thread twin, where two busy threads each run far slower).
//    Ten to thirty seconds into the frames, the physical core Windows itself ran the game thread on most (a
//    performance core on a hybrid CPU, not the first core, which takes most interrupts) is given to the game thread
//    alone: the game thread may run on that core's logical processors only, and every other thread of the game -
//    render worker, DXVK, sound, our helpers, threads started later - on all the others. Threads that set their own
//    affinity are left as they are. Only with four or more physical cores. Scheduling only: what any thread computes
//    is not touched. CORES_OFF next to the DLL turns it off.
#define CORE_SAMPLE_FROM 300
#define CORE_SAMPLE_TO   900
static volatile LONG g_corePaused = 0;                               // v50: the A/B test lets Windows place the game thread for a while (aotr_abtest.inc)
static volatile LONG g_coreOn = 0, g_coreMoved = 0, g_coreCustom = 0, g_coreFailed = 0, g_corePasses = 0;
static KAFFINITY g_coreGame = 0, g_coreRest = 0, g_coreProc = 0;
static LONG g_coreHist[64]; static LONG g_coreSamples = 0; static LONG g_coreIdx = -1;
static DWORD g_coreGameTid = 0;
static DWORD g_coreSkip[256]; static LONG g_coreNSkip = 0;          // threads with an affinity of their own
static void coreApplyAll() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te); DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_coreGameTid) continue;
        bool skip = false; for (LONG i = 0; i < g_coreNSkip; ++i) if (g_coreSkip[i] == te.th32ThreadID) { skip = true; break; }
        if (skip) continue;
        HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) { g_coreFailed++; continue; }
        DWORD_PTR old = SetThreadAffinityMask(h, (DWORD_PTR)g_coreRest);
        if (!old) g_coreFailed++;
        else if (old == (DWORD_PTR)g_coreRest) {}                                     // already ours
        else if (old == (DWORD_PTR)g_coreProc) g_coreMoved++;                         // was the default: now off the game's core
        else { SetThreadAffinityMask(h, old); g_coreCustom++; if (g_coreNSkip < 256) g_coreSkip[g_coreNSkip++] = te.th32ThreadID; }   // its own choice: put back
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
static DWORD WINAPI coreKeeper(LPVOID) {                             // new threads are moved off the game's core as they appear
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) { if (g_coreOn && !g_corePaused) { coreApplyAll(); g_corePasses++; } Sleep(2000); }
}
static void corePick() {                                             // game thread, once
    if (g_vanilla) return;
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "CORES_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("cores: CORES_OFF is present - Windows places the game thread as it likes."); return; }
    if (g_schAff) return;                                            // the measurement option already placed it
    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || !procMask) return;
    DWORD len = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (!len || len > (1u << 20)) return;
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    KAFFINITY core[64]; BYTE eff[64]; int nc = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len && nc < 64; ) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupMask[0].Group == 0) {
                KAFFINITY m = e->Processor.GroupMask[0].Mask & (KAFFINITY)procMask;
                if (m) { core[nc] = m; eff[nc] = ((BYTE*)&e->Processor)[1]; ++nc; }   // EfficiencyClass (0 on CPUs that are not hybrid)
            }
            if (!e->Size) break;
            off += e->Size;
        }
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    if (nc < 4) { logf("cores: %d physical cores for this game - the game thread is not given one of its own.", nc); return; }
    BYTE top = 0; for (int c = 0; c < nc; ++c) if (eff[c] > top) top = eff[c];
    int best = -1; LONG bestN = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass)
        for (int c = 0; c < nc; ++c) {
            if (eff[c] != top) continue;                                 // performance cores only
            if (pass == 0 && (core[c] & 1)) continue;                    // not the first core, unless nothing else is left
            LONG n = 0; for (int i = 0; i < (int)(sizeof(KAFFINITY) * 8); ++i) if (core[c] & ((KAFFINITY)1 << i)) n += g_coreHist[i];
            if (n > bestN) { bestN = n; best = c; }
        }
    if (best < 0) return;
    KAFFINITY all = 0; for (int c = 0; c < nc; ++c) all |= core[c];
    g_coreGame = core[best]; g_coreRest = all & ~g_coreGame; g_coreProc = (KAFFINITY)procMask; g_coreIdx = best;
    if (!g_coreRest) return;
    g_coreGameTid = GetCurrentThreadId();
    if (!SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)g_coreGame)) { logf("cores: could not place the game thread (error %u) - left to Windows.", GetLastError()); return; }
    g_coreOn = 1;
    coreApplyAll();
    HANDLE th = CreateThread(NULL, 0, coreKeeper, NULL, 0, NULL); if (th) CloseHandle(th);
    char lps[64]; int q = 0; lps[0] = 0;
    for (int i = 0; i < (int)(sizeof(KAFFINITY) * 8) && q < 50; ++i) if (g_coreGame & ((KAFFINITY)1 << i)) q += wsprintfA(lps + q, "%s%d", q ? "+" : "", i);
    logf("cores: the game thread now has physical core %d of %d to itself (logical processors %s; Windows ran it there in %d of %d sampled frames%s); %d other threads of the game moved to the other cores, %d with an affinity of their own left as they were. CORES_OFF = Windows' own placement.",
         best + 1, nc, lps, (int)bestN, (int)g_coreSamples, top ? ", a performance core" : "", (int)g_coreMoved, (int)g_coreCustom);
}
static void coreFrame(LONG w) {                                      // frameprof, every frame on the game thread
    if (w >= CORE_SAMPLE_FROM && w < CORE_SAMPLE_TO) { DWORD c = GetCurrentProcessorNumber(); if (c < 64) g_coreHist[c]++; g_coreSamples++; }
    else if (w == CORE_SAMPLE_TO) corePick();
}
static void coreReport() {                                           // 30 s product report
    static LONG lm = 0, lc = 0;
    if (!g_coreOn) return;
    LONG m = g_coreMoved, c = g_coreCustom;
    if (m != lm || c != lc) logf("cores: game thread on physical core %d alone; threads moved off it so far %d (%d new in the last 30 s), own-affinity threads left %d, could not open %d", (int)g_coreIdx + 1, (int)m, (int)(m - lm), (int)c, (int)g_coreFailed);
    lm = m; lc = c;
}

// on the game thread, at its first frame (frameprof's scheduler wrapper calls this)
static void schedOnGameThread() {
    if (g_schDone) return; g_schDone = 1;
    if (g_lwOn && !g_lwTimer) g_lwTimer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (g_lwOn && !g_lwTimer) g_lwTimer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (g_schMmcss && p_avSet) { DWORD idx = 0; HANDLE h = p_avSet("Games", &idx); g_schMmcss = h ? 2 : 1; }
    if (g_schAff) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION li[64]; DWORD len = sizeof(li); ULONG_PTR core[64]; int nc = 0;
        if (GetLogicalProcessorInformation(li, &len))
            for (DWORD i = 0; i < len / sizeof(li[0]) && nc < 64; ++i) if (li[i].Relationship == RelationProcessorCore) core[nc++] = li[i].ProcessorMask;
        if (nc >= 4) {
            ULONG_PTR m = core[1] & (~core[1] + 1);                               // first logical processor of core 2
            SetThreadAffinityMask(GetCurrentThread(), m);
            if (g_rtWorkerTid) { HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, g_rtWorkerTid);
                if (h) { SetThreadAffinityMask(h, core[2] & (~core[2] + 1)); CloseHandle(h); } }
            g_schAff = 2;
        }
    }
    logf("sched: on the game thread %u - limiter wait %s, process priority %s, MMCSS %s, affinity %s.", GetCurrentThreadId(),
         g_lwOn ? (g_lwTimer ? "ON (sleeps on a high-resolution timer instead of spinning)" : "on (plain timer)") : "off",
         g_schPrio ? "HIGH" : "normal", g_schMmcss == 2 ? "Games" : (g_schMmcss ? "FAILED" : "off"), g_schAff == 2 ? "game thread core 2, render worker core 3" : (g_schAff ? "not enough cores" : "off"));
}
static void installSched() {
    char env[8];
    env[0] = 0; bool lw = !(GetEnvironmentVariableA("AOTR_LIMWAIT", env, sizeof(env)) && env[0] == '0');
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_PRIO", env, sizeof(env)) && env[0] == '1') g_schPrio = 1;
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_MMCSS", env, sizeof(env)) && env[0] == '1') g_schMmcss = 1;
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_AFFINITY", env, sizeof(env)) && env[0] == '1') g_schAff = 1;
    if (g_schPrio) SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    if (g_schMmcss) { HMODULE h = LoadLibraryA("avrt.dll"); if (h) p_avSet = (tAvSet)GetProcAddress(h, "AvSetMmThreadCharacteristicsA"); }
    if (lw) {
        static const BYTE k[] = { 0x53, 0xFF, 0x15, 0xB4, 0x03, 0xBD, 0x00, 0xFF, 0x15, 0x20, 0x09, 0xBD, 0x00, 0x8B, 0xF8 };
        if (memcmp((const void*)(ULONG_PTR)(LW_SITE - 1), k, sizeof(k))) { logf("sched: the frame limiter is not the code this build has - its wait is left as it is."); }
        else {
            HANDLE fz[256]; int nfz = 0; suspendOthers(fz, &nfz, 256);
            BYTE* p = (BYTE*)(ULONG_PTR)LW_SITE; DWORD old = 0;
            VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old);
            p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)(ULONG_PTR)&limStub - (LW_SITE + 5); p[5] = 0x90;
            VirtualProtect(p, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), p, 6);
            resumeAll(fz, nfz);
            g_lwOn = 1;
            logf("sched: frame limiter (game.dat 0x63A1DC) - its Sleep(0) spin now sleeps until 2 ms before the frame's end; the engine's own timeGetTime test still ends the frame (same limit, same millisecond). AOTR_LIMWAIT=0 = the stock spin.");
        }
    } else logf("sched: frame limiter left spinning (AOTR_LIMWAIT=0).");
}
