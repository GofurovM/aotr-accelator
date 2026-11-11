// aotr_frameprof.inc - v33 (ISH-7): what every frame is made of, measured all the time, and which cores do the work.
//
// test-13: "fps falls to ~20 when the armies grow" - and the only instrument looked at frames of 80 ms and more, which a
// 45 ms battle frame never is. This one sees every frame:
//
//  1. Exact times, every engine frame (game thread). The engine's per-frame scheduler 0x006325A0 is entered once per
//     frame from 0x00441822 and makes three calls that are the whole frame between them:
//        0x006325CF  call [eax+9Ch]  -> 0x00632409  input, the client update, the drawing of the picture and its Present
//        0x006326C6 / 0x006326F0  call [eax/edx+98h] -> 0x006329B0  GameLogic::update(n), n = the logic sub-frame 1..6
//        0x006325ED / 0x006326FE  call [eax+94h] on [0x00DC62C0] (the engine's Debug singleton) - its per-frame call
//     Each of these six-byte indirect calls becomes a five-byte call to a wrapper here that reads the clock, makes the
//     very same call through the very same method table, and reads the clock again (+1 nop). The scheduler call at
//     0x00441822 is wrapped the same way. Nothing is skipped, reordered or delayed: the logic runs exactly as before,
//     ~0.1 us later. The wrappers only store integers (QueryPerformanceCounter ticks) into a ring that the report
//     thread reads: no lock, no allocation, no floating point, no file on the game thread.
//  2. A sampler (own thread, 10 a second while frames come): it stops the game thread for a few microseconds, reads
//     which of the phases above it is in (an exact marker the wrappers set), whether it waits for the render queue or
//     is inside a D3DX call, the module of the instruction, and the game.dat functions on its stack - which client
//     subsystem, texture loading, shadows. The time it stops the game thread is measured and reported per frame.
//  3. Every 5 s: `frame:` (fps, frame time, ms per frame in each part, the slowest tenth of the frames by part, the
//     sampled composition, the instrument's own cost) and `cores:` (every physical core's busy share for the whole
//     machine, and the threads of this game by CPU: the game thread, the render worker, our helpers, DXVK's threads).
//
// Read only: it changes no result of any game function. [Engine] FrameProf = 0 turns it off; FrameSampleHz (default 50)
// sets the sample rate (default 10: about 0.03 ms a frame) - 400+ also writes the functions the time is spent in every 30 s (diagnosis).

#define FR_SCHED_CALL   0x00441822
#define FR_SCHED_FN     0x006325A0
#define FR_CLIENT_CALL  0x006325CF
#define FR_LOGIC_CALL1  0x006326C6
#define FR_LOGIC_CALL2  0x006326F0
#define FR_DBG_CALL1    0x006325ED
#define FR_DBG_CALL2    0x006326FE
#define FR_CLIENT_FN    0x00632409
#define FR_LOGIC_FN     0x006329B0
#define FR_TEXLOAD_FN   0x0053193E

struct FrRec { DWORD period, sched, client, logic, dbg, stage[4], pre, wait; BYTE n, nlog, pad[2]; };   // pre: engine step end -> the limiter's test; wait: the limiter     // QPC ticks; stage: UpdateShadowMap, UpdateWaterReflection, RenderViews, RenderUI
#define FR_RING 4096
static FrRec g_frRing[FR_RING];
// g_frW (frames written, game thread) is declared in aotr_accel.cpp before aotr_loadprof.inc, which also reads it
static FrRec g_frCur;
static LONG64 g_frLastStart = 0, g_frTs = 0, g_frTc = 0, g_frTl = 0, g_frTd = 0;
static volatile LONG g_frPhase = 0;             // 0 outside the engine step, 1 scheduler, 2 client (drawing), 3 logic, 4 debug
static volatile LONG g_frStage = 0;             // inside the client: 0 none, 1 UpdateShadowMap, 2 UpdateWaterReflection, 3 RenderViews, 4 RenderUI
static LONG64 g_frTst = 0;
static volatile DWORD g_frTid = 0;
static volatile LONG g_frOn = 0;
static LONG g_frHz = 10, g_frDiag = 0, g_frHzNow = 0; static volatile LONG g_frBackoffs = 0;
static LONG64 g_frQpcCost = 0;                  // one QueryPerformanceCounter call, in its own ticks
static LONG g_frStagesOn = 0;                   // the four drawing stages are timed too
static LONG g_frLimOn = 0;                      // the main loop's frame limiter is timed too

static void __cdecl frSchedBegin() {
    LONG64 t = qpcNow();
    if (!g_frTid) { g_frTid = GetCurrentThreadId(); g_frTidEarly = g_frTid; }
    if (g_frLastStart) {
        LONG64 d = t - g_frLastStart;
        g_frCur.period = d > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)d;
        g_frRing[g_frW & (FR_RING - 1)] = g_frCur;
        abFrame(g_frCur.period, g_frCur.logic, g_frCur.client);   // v50 (aotr_abtest.inc)
        _WriteBarrier();
        g_frW = g_frW + 1; gdOnFrame();
        plTrickleOnFrame((LONG64)g_frCur.period);      // v59 (aotr_preload.inc): the rest of the preload plan during play, in small pieces
        bdgOnFrame();                                   // v71 (aotr_badge.inc): menu or match, the refresh after every fifth match
        if (g_frW == 300) schedOnGameThread();         // (aotr_sched.inc: once, ten seconds into the frames - the render worker exists by then)
        coreFrame(g_frW);                               // v45 (aotr_sched.inc): which core Windows runs the game thread on, then that core for it alone
    }
    g_frLastStart = t; g_frTs = t;
    g_frCur.sched = g_frCur.client = g_frCur.logic = g_frCur.dbg = 0; g_frCur.n = 0; g_frCur.nlog = 0;
    g_frCur.stage[0] = g_frCur.stage[1] = g_frCur.stage[2] = g_frCur.stage[3] = 0; g_frCur.pre = g_frCur.wait = 0;
    g_frPhase = 1;
}
static LONG64 g_frTse = 0, g_frTlim = 0;
static void __cdecl frSchedEnd() { g_frTse = qpcNow(); g_frCur.sched = (DWORD)(g_frTse - g_frTs); g_frPhase = 0; }
// the main loop after the engine step (game.dat 0x639EC6..0x63A232): its frame limiter starts with timeGetTime at
// 0x63A196 (limit on) or 0x63A200 (off) and ends writing the frame's end time to [0x00DE4318] (0x63A1F8 / 0x63A21A).
static void __cdecl frLimBegin() { g_frTlim = qpcNow(); if (g_frTse) g_frCur.pre += (DWORD)(g_frTlim - g_frTse); g_frTse = 0; }
static void __cdecl frLimEnd() { if (g_frTlim) g_frCur.wait += (DWORD)(qpcNow() - g_frTlim); g_frTlim = 0; }
static __declspec(naked) void frLimA() { __asm { call frLimBegin } __asm { mov eax, 0x00BD0920 } __asm { jmp dword ptr [eax] } }
static __declspec(naked) void frLimB() { __asm { mov dword ptr ds:[0x00DE4318], edi } __asm { push eax } __asm { push ecx } __asm { push edx } __asm { call frLimEnd } __asm { pop edx } __asm { pop ecx } __asm { pop eax } __asm { ret } }
static __declspec(naked) void frLimC() { __asm { mov dword ptr ds:[0x00DE4318], eax } __asm { push eax } __asm { push ecx } __asm { push edx } __asm { call frLimEnd } __asm { pop edx } __asm { pop ecx } __asm { pop eax } __asm { ret } }
// v49: a client step over 120 ms is written out by its parts (the engine's drawing stages and the client subsystems
// - DE4418-vt30 is TheDisplay's draw, 0x44B788), to set against the logic spike lines. Read only.
static LONG64 g_frSsAt[SS_N]; static DWORD g_frStAt[4]; static volatile LONG g_frCSpikes = 0;
static void __cdecl frClientBegin() { g_frTc = qpcNow(); g_frPhase = 2; for (int i = 0; i < SS_N; ++i) g_frSsAt[i] = g_ssT[i]; for (int i = 0; i < 4; ++i) g_frStAt[i] = g_frCur.stage[i]; }
static void frClientSpike(LONG64 dt) {
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1; double tk = g_tscPerQpc > 0 ? g_tscPerQpc * (double)f / 1000.0 : 0;
    char a[900]; int p = 0;
    p += wsprintfA(a + p, "client spike #%d: a client step of %d ms | stages: shadow map %d, water %d, views %d, UI %d ms | subsystems:", (int)g_frCSpikes, (int)(dt * 1000 / f),
                   (int)((g_frCur.stage[0] - g_frStAt[0]) * 1000 / f), (int)((g_frCur.stage[1] - g_frStAt[1]) * 1000 / f), (int)((g_frCur.stage[2] - g_frStAt[2]) * 1000 / f), (int)((g_frCur.stage[3] - g_frStAt[3]) * 1000 / f));
    for (int i = 0; i < SS_N && tk > 0 && p < 800; ++i) { double ms = (double)(g_ssT[i] - g_frSsAt[i]) / tk; if (ms >= 2.0) p += wsprintfA(a + p, " %s %d ms", kSsName[i], (int)ms); }
    logf("%s", a);
}
static void __cdecl frClientEnd() {
    LONG64 dt = qpcNow() - g_frTc;
    g_frCur.client += (DWORD)dt; g_frPhase = 1;
    if (dt * 1000 > g_pqpf.QuadPart * 120 && g_frCSpikes < 300 && g_lsMatches > 0) { g_frCSpikes++; frClientSpike(dt); }
}
static ULONG64 g_frLsTsc = 0;                         // v35: the slicer measures the engine's own steps through this wrapper
static void __cdecl frLogicBegin(LONG n) { lsFrameBegin(); plSeedOnGameThread(); g_frLsTsc = __rdtsc(); g_frTl = qpcNow(); g_frCur.n = (BYTE)n; g_frCur.nlog++; g_frPhase = 3; g_frLastLogicTick = GetTickCount(); }
static void __cdecl frLogicEnd() { g_frCur.logic += (DWORD)(qpcNow() - g_frTl); g_frPhase = 1; lsFrameDone(g_frCur.n, __rdtsc() - g_frLsTsc); }
static void __cdecl frDbgBegin() { g_frTd = qpcNow(); g_frPhase = 4; }
static LONG g_frPrevPhase = PH_OTHER;
static void __cdecl frStageBegin(LONG i) {
    g_frTst = qpcNow(); g_frStage = i;
    g_frPrevPhase = g_phase;                         // the other modules' per-pass tables (pose warm, render lists) read g_phase
    if (i == 1) g_phase = PH_SHADOW; else if (i == 3) g_phase = PH_VIEWS; else if (i == 4) g_phase = PH_UI;
}
static void __cdecl frStageEnd(LONG i) { if (g_frStage == i) g_frCur.stage[i - 1] += (DWORD)(qpcNow() - g_frTst); g_frStage = 0; g_phase = g_frPrevPhase; }
static void __cdecl frDbgEnd() { g_frCur.dbg += (DWORD)(qpcNow() - g_frTd); g_frPhase = 1; }

// The wrappers: the same call through the same method table, the clock read before and after. Callee-saved registers
// are kept by the cdecl helpers; eax (the result) is kept across the second helper.
static __declspec(naked) void frWrapSched() {
    __asm {
        push ecx
        call frSchedBegin
        pop ecx
        mov eax, FR_SCHED_FN
        call eax
        push eax
        call frSchedEnd
        pop eax
        ret
    }
}
static __declspec(naked) void frWrapClient() {
    __asm {
        push ecx
        call frClientBegin
        pop ecx
        mov eax, [ecx]
        call dword ptr [eax + 9Ch]
        push eax
        call frClientEnd
        pop eax
        ret
    }
}
static __declspec(naked) void frWrapLogic() {          // thiscall, one argument (the sub-frame), the callee pops it
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call frLogicBegin
        add esp, 4
        pop ecx
        push dword ptr [esp + 4]
        mov eax, [ecx]
        call dword ptr [eax + 98h]
        push eax
        call frLogicEnd
        pop eax
        ret 4
    }
}
static __declspec(naked) void frWrapDbg() {
    __asm {
        push ecx
        call frDbgBegin
        pop ecx
        mov eax, [ecx]
        call dword ptr [eax + 94h]
        push eax
        call frDbgEnd
        pop eax
        ret
    }
}

// The engine's own profile stages inside the frame's drawing (0x00449CF8): each is opened by a call to 0x00517690 and
// closed by a call to 0x00517740 at fixed places. Those eight calls go through a stub that notes the time and jumps on
// to the very same function with the same stack (the return address is the engine's own).
#define FR_STAGE_CTOR 0x00517690
#define FR_STAGE_DTOR 0x00517740
#define FR_STUB(name, fn, helper, idx) static __declspec(naked) void name() { __asm { push ecx } __asm { push idx } __asm { call helper } __asm { add esp, 4 } __asm { pop ecx } __asm { mov eax, fn } __asm { jmp eax } }
FR_STUB(frSt1b, FR_STAGE_CTOR, frStageBegin, 1)  FR_STUB(frSt1e, FR_STAGE_DTOR, frStageEnd, 1)
FR_STUB(frSt2b, FR_STAGE_CTOR, frStageBegin, 2)  FR_STUB(frSt2e, FR_STAGE_DTOR, frStageEnd, 2)
FR_STUB(frSt3b, FR_STAGE_CTOR, frStageBegin, 3)  FR_STUB(frSt3e, FR_STAGE_DTOR, frStageEnd, 3)
FR_STUB(frSt4b, FR_STAGE_CTOR, frStageBegin, 4)  FR_STUB(frSt4e, FR_STAGE_DTOR, frStageEnd, 4)
static const DWORD kFrStageSite[8] = { 0x00449DE0, 0x00449E03, 0x00449E42, 0x00449E6B, 0x00449FBA, 0x0044A008, 0x0044A049, 0x0044A0A8 };
static void* const kFrStageStub[8] = { (void*)&frSt1b, (void*)&frSt1e, (void*)&frSt2b, (void*)&frSt2e, (void*)&frSt3b, (void*)&frSt3e, (void*)&frSt4b, (void*)&frSt4e };
static const char* const kFrStageName[4] = { "UpdateShadowMap", "UpdateWaterReflection", "RenderViews", "RenderUI" };
static void frPatchCall(DWORD at, int len, void* dest) {
    BYTE* p = (BYTE*)(ULONG_PTR)at; DWORD old = 0;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)(ULONG_PTR)dest - (at + 5);
    for (int i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
}

// ---------------------------------------------------------------- the sampler
// Client subsystems: the update each one gets from 0x00632409 (resolved from the live objects once frames come).
#define FR_NSUB 12
static DWORD g_frSubFn[FR_NSUB]; static const char* g_frSubName[FR_NSUB]; static int g_frNSub = 0;
// What a sample is filed under (one of each group).
enum { FRP_OUT, FRP_SCHED, FRP_CLIENT, FRP_LOGIC, FRP_DBG, FRP_N };
enum { FRM_GAME, FRM_D3DX, FRM_D3D9, FRM_DRIVER, FRM_SYS, FRM_OURS, FRM_CRT, FRM_OTHER, FRM_N };
static const char* const kFrPhase[FRP_N] = { "outside the engine step", "scheduler", "client+drawing", "logic", "debug" };
static const char* const kFrMod[FRM_N] = { "game.dat", "D3DX", "d3d9/DXVK", "driver", "system (waits, heap, files)", "this DLL", "C runtime", "other" };
struct FrWin {
    LONG n, phase[FRP_N], mod[FRP_N][FRM_N], waitQ, d3dxScope, texload, sub[FR_NSUB + 1];
    LONG64 suspTicks; LONG suspN;
};
static FrWin g_frWin;                               // the current 5 s window (sampler thread writes, report thread swaps)
static CRITICAL_SECTION g_frWinCs;                  // sampler <-> report thread only; the game thread never takes it
static LeafEnt* g_frIncl = NULL, *g_frLeaf = NULL, *g_frTmp = NULL;   // diagnosis: inclusive / innermost game.dat functions (client phase)
static LeafEnt* g_frInclL = NULL, *g_frLeafL = NULL;                  // the same in the logic phase
static LeafEnt* g_frOurs = NULL;                                      // this DLL's time by the game.dat function that called into it
static LONG g_frDiagN = 0, g_frDiagNL = 0, g_frDiagOursN = 0;
// v45: call paths - the innermost function and the three game.dat functions that called it, counted per sample. The
// "under" lists say a function was somewhere on the stack (stale return addresses included), the "in" list only where
// the processor was; a path says which caller the time came through (who calls the heap, which loop a hot leaf is in).
struct FrPath { DWORD k[4]; DWORD count; };
#define FR_PATHN 16384
static FrPath* g_frPathC = NULL, *g_frPathL = NULL, *g_frPathA = NULL;   // A (v50): logic samples inside AIUpdateInterface::update (module vtable C6A5A8)
static LONG g_frPathNA = 0;
static LONG g_frPathNC = 0, g_frPathNL = 0, g_frPathDrop = 0;
static void frPathAdd(FrPath* t, const DWORD* k) {
    DWORD h = (k[0] * 2654435761u) ^ (k[1] * 40503u) ^ (k[2] * 2246822519u) ^ (k[3] * 3266489917u);
    h ^= h >> 15;
    for (int i = 0; i < 64; ++i) {
        FrPath& e = t[(h + (DWORD)i) & (FR_PATHN - 1)];
        if (!e.count) { e.k[0] = k[0]; e.k[1] = k[1]; e.k[2] = k[2]; e.k[3] = k[3]; e.count = 1; return; }
        if (e.k[0] == k[0] && e.k[1] == k[1] && e.k[2] == k[2] && e.k[3] == k[3]) { e.count++; return; }
    }
    g_frPathDrop++;
}
static int __cdecl frPathCmp(const void* a, const void* b) { DWORD x = ((const FrPath*)a)->count, y = ((const FrPath*)b)->count; return x < y ? 1 : (x > y ? -1 : 0); }
static HANDLE g_frDump = INVALID_HANDLE_VALUE;                       // diagnosis: every sample's stack (framesamples.bin), read by work\ish7\tree.py
static BYTE g_frDumpBuf[1 << 16]; static DWORD g_frDumpN = 0;
static DWORD g_frOursLo = 0, g_frOursHi = 0;
static int frModClass(DWORD eip) {
    if (eip >= g_gameBase && eip < g_gameEnd) return FRM_GAME;
    if (eip >= g_frOursLo && eip < g_frOursHi) return FRM_OURS;
    int mi = pmOf(eip);
    if (mi < 0) { profRefreshModules(); mi = pmOf(eip); if (mi < 0) return FRM_OTHER; }
    const char* n = g_pm[mi].name;
    if (!_strnicmp(n, "d3dx9", 5)) return FRM_D3DX;
    if (!_stricmp(n, "d3d9.dll") || !_strnicmp(n, "dxgi", 4)) return FRM_D3D9;
    if (!_strnicmp(n, "nv", 2) || !_strnicmp(n, "vulkan", 6) || !_strnicmp(n, "ig", 2) || !_strnicmp(n, "ati", 3) || !_strnicmp(n, "amd", 3)) return FRM_DRIVER;
    if (!_stricmp(n, "ntdll.dll") || !_stricmp(n, "kernelbase.dll") || !_stricmp(n, "kernel32.dll") || !_stricmp(n, "win32u.dll") || !_stricmp(n, "user32.dll")) return FRM_SYS;
    if (!_strnicmp(n, "msvcr", 5) || !_stricmp(n, "ucrtbase.dll") || !_strnicmp(n, "msvcp", 5)) return FRM_CRT;
    return FRM_OTHER;
}
static void frResolveSubs() {
    if (g_frNSub) return;
    struct { DWORD global; int add; int slot; const char* name; } s[] = {
        { 0x00DE3F0C, 0, 0x28, "subsystem DE3F0C" }, { 0x00DE4AB8, 4, 0x28, "subsystem DE4AB8+4" }, { 0x00DE4388, 0, 0x28, "subsystem DE4388" },
        { 0x00DE4334, 0, 0x28, "subsystem DE4334" }, { 0x00DE42FC, 0, 0x28, "subsystem DE42FC" }, { 0x00DE4468, 0, 0x3C, "network DE4468" } };
    int n = 0;
    __try {
        for (int i = 0; i < (int)(sizeof(s) / sizeof(s[0])); ++i) {
            DWORD o = *(DWORD*)(ULONG_PTR)s[i].global; if (!o) continue;
            o += s[i].add;
            DWORD vt = *(DWORD*)(ULONG_PTR)o; if (vt < g_gameBase || vt >= g_gameEnd) continue;
            DWORD f = *(DWORD*)(ULONG_PTR)(vt + s[i].slot); if (f < g_textLo || f >= g_textHi) continue;
            DWORD fn = gsFunc(f); if (!fn) continue;
            g_frSubFn[n] = fn; g_frSubName[n] = s[i].name; ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    static const DWORD direct[] = { 0x007128C3, 0x0080000F, 0x0062B385 };
    static const char* const directName[] = { "client 7128C3", "client 80000F", "client 62B385" };
    for (int i = 0; i < 3 && n < FR_NSUB; ++i) { DWORD fn = gsFunc(direct[i]); if (fn) { g_frSubFn[n] = fn; g_frSubName[n] = directName[i]; ++n; } }
    g_frNSub = n;
    char line[900]; int p = 0; line[0] = 0;
    for (int i = 0; i < n; ++i) p += wsprintfA(line + p, " %s=game.dat+%06X", g_frSubName[i], g_frSubFn[i] - g_gameBase);
    logf("frame: client parts timed by the sampler:%s", line);
}
static void frSample(HANDLE h, NT_TIB* tib, BYTE* stk, DWORD cap) {
    LONG64 t0 = qpcNow();
    if (SuspendThread(h) == (DWORD)-1) return;
    CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
    DWORD eip = 0, n = 0;
    LONG phase = g_frPhase, waitQ = g_rtDrainWaiting, scope = g_rtDirectScope; DWORD curVt = g_msCurVt;
    if (GetThreadContext(h, &ctx)) {
        eip = ctx.Eip;
        DWORD esp = ctx.Esp, sb = (DWORD)(ULONG_PTR)tib->StackBase, sl = (DWORD)(ULONG_PTR)tib->StackLimit;
        if (esp >= sl && esp < sb) { n = (sb - esp) & ~3u; if (n > cap) n = cap; if (!gsCopyStack(stk, esp, n)) n = 0; }
    }
    ResumeThread(h);
    LONG64 t1 = qpcNow();
    if (!eip) return;
    if (phase < 0 || phase >= FRP_N) phase = FRP_OUT;
    int mc = frModClass(eip);
    // the game.dat functions on the stack (innermost first), with the same stale-return filter as the long-frame profiler
    DWORD seen[160]; int ns = 0; DWORD f0 = (eip >= g_gameBase && eip < g_gameEnd) ? gsFunc(eip) : 0, ret0 = 0;
    if (f0) seen[ns++] = f0;
    DWORD cur = f0; int miss = 0;
    for (DWORD o = 0; o + 4 <= n && ns < 160; o += 4) {
        DWORD v = *(DWORD*)(stk + o);
        if (v < g_textLo + 8 || v >= g_textHi || !gsIsRet(v)) continue;
        const BYTE* rp = (const BYTE*)(ULONG_PTR)v;
        if (cur && rp[-5] == 0xE8) {
            DWORD t = v + *(const LONG*)(rp - 4);
            bool ok = t == cur;
            if (!ok && t >= g_textLo && t + 5 <= g_textHi && ((const BYTE*)(ULONG_PTR)t)[0] == 0xE9) ok = t + 5 + *(const LONG*)((const BYTE*)(ULONG_PTR)t + 1) == cur;
            if (!ok && ++miss <= 3) continue;
        }
        miss = 0;
        if (!ret0) ret0 = v;
        DWORD f = gsFunc(v); cur = f; if (!f) continue;
        bool dup = false; for (int k = 0; k < ns; ++k) if (seen[k] == f) { dup = true; break; }
        if (!dup) seen[ns++] = f;
    }
    int sub = FR_NSUB; bool tex = false;
    for (int k = 0; k < ns; ++k) {
        if (seen[k] == FR_TEXLOAD_FN) tex = true;
        if (sub == FR_NSUB) for (int j = 0; j < g_frNSub; ++j) if (seen[k] == g_frSubFn[j]) { sub = j; break; }
    }
    EnterCriticalSection(&g_frWinCs);
    FrWin& w = g_frWin;
    w.n++; w.phase[phase]++; w.mod[phase][mc]++;
    if (waitQ) w.waitQ++;
    if (scope > 0) w.d3dxScope++;
    if (tex) w.texload++;
    if (phase == FRP_CLIENT) w.sub[sub]++;
    w.suspTicks += t1 - t0; w.suspN++;
    if (g_frDiag && g_frIncl) {
        LeafEnt* inc = phase == FRP_LOGIC ? g_frInclL : g_frIncl; LeafEnt* lf = phase == FRP_LOGIC ? g_frLeafL : g_frLeaf;
        if (phase == FRP_CLIENT || phase == FRP_LOGIC) {
            LONG drop = 0;
            for (int k = 0; k < ns; ++k) tabAdd(inc, 65536, seen[k], 1, &drop);
            tabAdd(lf, 65536, mc == FRM_GAME ? (f0 ? f0 : eip) : (eip & ~0xFFu), 1, &drop);
            if (phase == FRP_LOGIC) g_frDiagNL++; else g_frDiagN++;
        }
    }
    if (g_frPathC && (phase == FRP_CLIENT || phase == FRP_LOGIC)) {
        DWORD k[4] = { 0, 0, 0, 0 }; int s0 = 0;
        if (mc == FRM_GAME && f0) { k[0] = f0; s0 = 1; } else k[0] = eip & ~0xFFu;
        for (int j = 1; j < 4 && s0 < ns; ++j) k[j] = seen[s0++];
        if (phase == FRP_LOGIC) { frPathAdd(g_frPathL, k); g_frPathNL++; if (curVt == 0x00C6A5A8 && g_frPathA) { frPathAdd(g_frPathA, k); g_frPathNA++; } } else { frPathAdd(g_frPathC, k); g_frPathNC++; }
    }
    if (g_frDump != INVALID_HANDLE_VALUE && ns > 0) {
        DWORD rec[4 + 160]; rec[0] = 0xF5A3D000u | (DWORD)phase | ((DWORD)mc << 4) | (waitQ ? 0x100 : 0) | (scope > 0 ? 0x200 : 0); rec[1] = eip; rec[2] = (DWORD)ns; rec[3] = GetTickCount();
        for (int k = 0; k < ns; ++k) rec[4 + k] = seen[k];
        DWORD bytes = (4 + ns) * 4;
        if (g_frDumpN + bytes > sizeof(g_frDumpBuf)) { DWORD wr = 0; WriteFile(g_frDump, g_frDumpBuf, g_frDumpN, &wr, NULL); g_frDumpN = 0; }
        memcpy(g_frDumpBuf + g_frDumpN, rec, bytes); g_frDumpN += bytes;
    }
    if (mc == FRM_OURS && g_frOurs) { LONG drop = 0; tabAdd(g_frOurs, 4096, ret0 ? gsFunc(ret0) : 1, 1, &drop); g_frDiagOursN++; }
    LeaveCriticalSection(&g_frWinCs);
}

// ---------------------------------------------------------------- cores: the machine's and this game's
typedef LONG (NTAPI* tNtQSI)(ULONG, PVOID, ULONG, PULONG);
struct FrSppi { LARGE_INTEGER idle, kernel, user, dpc, intr; ULONG cnt; };
#define FR_MAXCPU 64
static tNtQSI p_frNtQSI = NULL;
static int g_frNCpu = 0, g_frNCore = 0; static ULONG64 g_frCoreMask[FR_MAXCPU]; static BYTE g_frCoreHT[FR_MAXCPU];
static FrSppi g_frCpuPrev[FR_MAXCPU];
struct FrThr { DWORD tid; ULONG64 t; DWORD start; char name[40]; };
#define FR_MAXTHR 256
static FrThr g_frThr[FR_MAXTHR]; static int g_frNThr = 0;
typedef HRESULT (WINAPI* tGetThreadDescription)(HANDLE, PWSTR*);
static tGetThreadDescription p_frGTD = NULL;
static void frCoresInit() {
    p_frNtQSI = (tNtQSI)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    p_frGTD = (tGetThreadDescription)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    SYSTEM_INFO si; GetSystemInfo(&si); g_frNCpu = (int)si.dwNumberOfProcessors; if (g_frNCpu > FR_MAXCPU) g_frNCpu = FR_MAXCPU;
    DWORD len = 0; GetLogicalProcessorInformation(NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* li = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)VirtualAlloc(NULL, len + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (li && GetLogicalProcessorInformation(li, &len)) {
        for (DWORD i = 0; i < len / sizeof(*li) && g_frNCore < FR_MAXCPU; ++i)
            if (li[i].Relationship == RelationProcessorCore) { g_frCoreMask[g_frNCore] = li[i].ProcessorMask; g_frCoreHT[g_frNCore] = li[i].ProcessorCore.Flags; ++g_frNCore; }
    }
    if (li) VirtualFree(li, 0, MEM_RELEASE);
    if (!g_frNCore) for (int i = 0; i < g_frNCpu; ++i) { g_frCoreMask[i] = 1ull << i; ++g_frNCore; }
    if (p_frNtQSI) p_frNtQSI(8, g_frCpuPrev, sizeof(FrSppi) * g_frNCpu, NULL);
}
static void frThreadName(DWORD tid, HANDLE h, char* out) {
    if (tid == g_frTid) { lstrcpyA(out, "GAME thread (logic+drawing)"); return; }
    if (tid == g_rtWorkerTid && tid) { lstrcpyA(out, "render worker (D3D9 calls)"); return; }
    if (p_frGTD) { PWSTR d = NULL; if (SUCCEEDED(p_frGTD(h, &d)) && d) { if (d[0]) { WideCharToMultiByte(CP_ACP, 0, d, -1, out, 40, NULL, NULL); out[39] = 0; LocalFree(d); return; } LocalFree(d); } }
    DWORD start = 0;
    if (p_NtQIT) p_NtQIT(h, 9, &start, sizeof(start), NULL);
    struct { void* fn; const char* name; } ours[] = { { (void*)&tpHelper, "ground-patch helper" }, { (void*)&snbHelper, "shadow helper" }, { (void*)&fwWorker, "file read-ahead" },
        { (void*)&fwDirector, "file read-ahead" }, { (void*)&rtWorker, "render worker" }, { (void*)&ovThread, "panel" }, { (void*)&lpThread, "long-frame profiler" },
        { (void*)&rtProdReport, "report" }, { (void*)&tpReport, "report" }, { (void*)&snbReport, "report" }, { (void*)&swdReport, "report" } };
    for (int i = 0; i < (int)(sizeof(ours) / sizeof(ours[0])); ++i) if ((DWORD)(ULONG_PTR)ours[i].fn == start) { lstrcpyA(out, ours[i].name); return; }
    if (start) { char s[160]; symName(start, s); lstrcpynA(out, s, 40); return; }
    lstrcpyA(out, "?");
}
static void frCoresLine(LONG64 spanTicks) {
    // the machine: each physical core's busy share (all programs), from the kernel's per-processor counters
    FrSppi now[FR_MAXCPU]; char line[1400]; int p = 0;
    if (p_frNtQSI && p_frNtQSI(8, now, sizeof(FrSppi) * g_frNCpu, NULL) == 0) {
        LONG busyCores10 = 0, hot = 0;
        p += wsprintfA(line + p, "cores: last 5 s - the whole machine, per physical core (logical processors in brackets):");
        for (int c = 0; c < g_frNCore; ++c) {
            LONG64 tot = 0, idle = 0; char lp[96]; int q = 0; lp[0] = 0;
            for (int i = 0; i < g_frNCpu; ++i) if (g_frCoreMask[c] & (1ull << i)) {
                LONG64 k = now[i].kernel.QuadPart - g_frCpuPrev[i].kernel.QuadPart, u = now[i].user.QuadPart - g_frCpuPrev[i].user.QuadPart, id = now[i].idle.QuadPart - g_frCpuPrev[i].idle.QuadPart;
                LONG64 t = k + u; LONG pc = t > 0 ? (LONG)((t - id) * 100 / t) : 0; if (pc < 0) pc = 0;
                tot += t; idle += id;
                q += wsprintfA(lp + q, "%s%d", q ? "/" : "", (int)pc);
            }
            LONG pc = tot > 0 ? (LONG)((tot - idle) * 1000 / tot) : 0; if (pc < 0) pc = 0;
            busyCores10 += pc / 10; if (pc >= 500) ++hot;
            p += wsprintfA(line + p, " core%d %d%% (%s)", c + 1, (int)(pc / 10), lp);
        }
        p += wsprintfA(line + p, " | %d of %d cores half busy or more", (int)hot, g_frNCore);
        memcpy(g_frCpuPrev, now, sizeof(FrSppi) * g_frNCpu);
        logf("%s", line);
    }
    // this game: its threads by CPU time in the window (100 % = one core)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    struct Row { DWORD tid; LONG pc10; char name[40]; } rows[FR_MAXTHR]; int nr = 0;
    FrThr next[FR_MAXTHR]; int nn = 0; LONG total10 = 0;
    THREADENTRY32 te; te.dwSize = sizeof(te); DWORD pid = GetCurrentProcessId();
    LONG64 span100ns = spanTicks * 10000000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1); if (span100ns <= 0) span100ns = 1;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || nn >= FR_MAXTHR) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
        FILETIME c0, e0, k0, u0; ULONG64 t = 0;
        if (GetThreadTimes(h, &c0, &e0, &k0, &u0)) t = ((ULONG64)k0.dwHighDateTime << 32 | k0.dwLowDateTime) + ((ULONG64)u0.dwHighDateTime << 32 | u0.dwLowDateTime);
        FrThr& x = next[nn++]; x.tid = te.th32ThreadID; x.t = t; x.name[0] = 0; x.start = 0;
        int old = -1; for (int i = 0; i < g_frNThr; ++i) if (g_frThr[i].tid == x.tid) { old = i; break; }
        if (old >= 0) { lstrcpyA(x.name, g_frThr[old].name);
            LONG pc10 = (LONG)((LONG64)(t - g_frThr[old].t) * 1000 / span100ns);
            if (pc10 > 0) { total10 += pc10; Row& r = rows[nr++]; r.tid = x.tid; r.pc10 = pc10; lstrcpyA(r.name, x.name); } }
        else frThreadName(x.tid, h, x.name);
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    memcpy(g_frThr, next, sizeof(FrThr) * nn); g_frNThr = nn;
    for (int i = 0; i < nr; ++i) for (int j = i + 1; j < nr; ++j) if (rows[j].pc10 > rows[i].pc10) { Row t = rows[i]; rows[i] = rows[j]; rows[j] = t; }
    p = wsprintfA(line, "cores: this game used %d.%d cores in all (%d threads); by thread (100%% = one core):", (int)(total10 / 1000), (int)((total10 % 1000) / 100), nn);
    LONG rest = 0; int shown = 0;
    for (int i = 0; i < nr; ++i) {
        if (shown < 10 && rows[i].pc10 >= 5) { p += wsprintfA(line + p, " %s %d.%d%%,", rows[i].name, (int)(rows[i].pc10 / 10), (int)(rows[i].pc10 % 10)); ++shown; }
        else rest += rows[i].pc10;
    }
    p += wsprintfA(line + p, " all others %d.%d%%", (int)(rest / 10), (int)(rest % 10));
    logf("%s", line);
}

// ---------------------------------------------------------------- the 5 s report
static int __cdecl frCmpDw(const void* a, const void* b) { DWORD x = *(const DWORD*)a, y = *(const DWORD*)b; return x < y ? -1 : (x > y ? 1 : 0); }
static void frMs(LONG64 ticks, LONG64 div, char* out) {          // ticks / div as ms with 1 decimal
    if (div <= 0) div = 1; LONG64 us = ticks * 1000000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1) / div;
    wsprintfA(out, "%d.%d", (int)(us / 1000), (int)((us % 1000) / 100));
}
static LONG g_frReadIdx = 0;
static void frReport(LONG64 spanTicks) {
    LONG w = g_frW, from = g_frReadIdx; g_frReadIdx = w;
    if (w - from > FR_RING) from = w - FR_RING;
    LONG nf = w - from;
    if (nf < 10) return;                                         // a menu without an engine step, the game minimised
    // the sampled composition is written every 30 s in normal play (10 samples a second), every 5 s in diagnosis
    static int s_k = 0; static LONG64 s_sP = 0; static LONG s_nf = 0;
    bool sampledNow = g_frDiag || (++s_k % 6) == 0;
    FrWin win; memset(&win, 0, sizeof(win));
    if (sampledNow) { EnterCriticalSection(&g_frWinCs); win = g_frWin; memset(&g_frWin, 0, sizeof(g_frWin)); LeaveCriticalSection(&g_frWinCs); }
    static DWORD per[FR_RING]; LONG64 sP = 0, sS = 0, sC = 0, sL = 0, sD = 0; DWORD mx = 0; LONG slow = 0;
    LONG64 subL[8] = { 0 }; LONG subN[8] = { 0 };
    LONG64 tick33 = g_pqpf.QuadPart * 334 / 10000;              // 33.4 ms: a frame below 30 fps
    for (LONG i = 0; i < nf; ++i) {
        const FrRec& r = g_frRing[(from + i) & (FR_RING - 1)];
        per[i] = r.period; sP += r.period; sS += r.sched; sC += r.client; sL += r.logic; sD += r.dbg;
        if (r.period > mx) mx = r.period;
        if ((LONG64)r.period > tick33) ++slow;
        int k = r.n < 8 ? r.n : 0; subL[k] += r.logic; subN[k]++;
    }
    qsort(per, nf, sizeof(DWORD), frCmpDw);
    DWORD p90 = per[(nf * 9) / 10], p50 = per[nf / 2];
    // the slowest tenth of the frames: what they are made of
    LONG64 tS = 0, tC = 0, tL = 0, tD = 0, tP = 0; LONG tn = 0;
    for (LONG i = 0; i < nf; ++i) { const FrRec& r = g_frRing[(from + i) & (FR_RING - 1)]; if (r.period >= p90) { tP += r.period; tS += r.sched; tC += r.client; tL += r.logic; tD += r.dbg; ++tn; } }
    char a[16], b[16], c[16], d[16], e[16], f[16], g[16], h[16];
    LONG64 fps10 = sP ? (LONG64)nf * 10 * g_pqpf.QuadPart / sP : 0;
    frMs(sP, nf, a); frMs(p50, 1, b); frMs(p90, 1, c); frMs(mx, 1, d);
    frMs(sL, nf, e); frMs(sC, nf, f); frMs(sD, nf, g); frMs(sP - sS, nf, h);
    char t1[16], t2[16], t3[16], t4[16];
    frMs(tP, tn, t1); frMs(tL, tn, t2); frMs(tC, tn, t3); frMs(tP - tS, tn, t4);
    { LONG64 sPre = 0, sW = 0, tPre = 0, tW = 0, tPer = 0, tSch = 0; LONG n2 = 0;
      for (LONG i = 0; i < nf; ++i) { const FrRec& r = g_frRing[(from + i) & (FR_RING - 1)]; sPre += r.pre; sW += r.wait; if (r.period >= p90) { tPre += r.pre; tW += r.wait; tPer += r.period; tSch += r.sched; ++n2; } }
      { LONG64 t40 = g_pqpf.QuadPart * 40 / 1000, aP = 0, aL = 0, aC = 0, aW = 0, aO = 0; LONG n40 = 0, nL = 0, nC = 0;
        for (LONG i = 0; i < nf; ++i) { const FrRec& r = g_frRing[(from + i) & (FR_RING - 1)]; if ((LONG64)r.period <= t40) continue;
            ++n40; aP += r.period; aL += r.logic; aC += r.client; aW += r.wait; aO += r.period - r.logic - r.client - r.wait;
            if (r.logic > r.client) ++nL; else ++nC; }
        if (n40) { char q1[16], q2[16], q3[16], q4[16], q5[16]; frMs(aP, n40, q1); frMs(aL, n40, q2); frMs(aC, n40, q3); frMs(aW, n40, q4); frMs(aO, n40, q5);
            logf("frame: %d frames over 40 ms (%d.%d%% of the frames): on average %s ms = LOGIC %s + CLIENT+DRAWING %s + the limiter's wait %s + the rest %s | logic the bigger part in %d of them, client+drawing in %d",
                 (int)n40, (int)(n40 * 100 / nf), (int)((n40 * 1000 / nf) % 10), q1, q2, q3, q4, q5, (int)nL, (int)nC); } }
      if (g_frLimOn) { char x1[16], x2[16], x3[16], y1[16], y2[16], y3[16];
        frMs(sPre, nf, x1); frMs(sW, nf, x2); frMs(sP - sS - sPre - sW, nf, x3); frMs(tPre, n2, y1); frMs(tW, n2, y2); frMs(tPer - tSch - tPre - tW, n2, y3);
        logf("frame: outside the engine step per frame = main loop before the frame limiter %s + the limiter's wait %s + the rest of the loop (messages, back to the next step) %s ms | in the slowest tenth: %s + %s + %s", x1, x2, x3, y1, y2, y3); } }
    LONG64 sSt[4] = { 0, 0, 0, 0 }, tSt[4] = { 0, 0, 0, 0 };
    for (LONG i = 0; i < nf; ++i) { const FrRec& r = g_frRing[(from + i) & (FR_RING - 1)]; for (int k = 0; k < 4; ++k) { sSt[k] += r.stage[k]; if (r.period >= p90) tSt[k] += r.stage[k]; } }
    char subs[200]; int sp = 0; subs[0] = 0;
    for (int k = 1; k < 8; ++k) if (subN[k]) { char x[16]; frMs(subL[k], subN[k], x); sp += wsprintfA(subs + sp, "%s%d:%s", sp ? " " : "", k, x); }
    logf("frame: last 5 s - %d frames, %d.%d fps; frame time average %s ms, median %s, slowest tenth from %s, longest %s; %d frames slower than 30 fps | the game thread per frame: LOGIC %s ms (by sub-frame %s), CLIENT+DRAWING %s ms, debug %s ms, outside the engine step %s ms (the frame limiter's wait, the message pump) | the slowest tenth: %s ms = logic %s + client+drawing %s + outside %s",
         (int)nf, (int)(fps10 / 10), (int)(fps10 % 10), a, b, c, d, (int)slow, e, subs, f, g, h, t1, t2, t3, t4);
    if (g_frStagesOn) {
        char st[4][16], tt[4][16], rest[16], trest[16];
        for (int k = 0; k < 4; ++k) { frMs(sSt[k], nf, st[k]); frMs(tSt[k], tn, tt[k]); }
        frMs(sC - sSt[0] - sSt[1] - sSt[2] - sSt[3], nf, rest); frMs(tC - tSt[0] - tSt[1] - tSt[2] - tSt[3], tn, trest);
        logf("frame: client+drawing per frame = %s %s + %s %s + %s %s + %s %s + the rest of the client step %s ms | in the slowest tenth: %s %s + %s %s + %s %s + %s %s + rest %s",
             kFrStageName[0], st[0], kFrStageName[1], st[1], kFrStageName[2], st[2], kFrStageName[3], st[3], rest,
             kFrStageName[0], tt[0], kFrStageName[1], tt[1], kFrStageName[2], tt[2], kFrStageName[3], tt[3], trest);
    }
    if (g_lwOn) {                                    // the limiter's wait (aotr_sched.inc)
        static LONG lS = 0, lP = 0; static LONG64 lU = 0;
        LONG sl = g_lwSleeps, sp = g_lwSpins; LONG64 us = g_lwSleptUs; char a1[16]; frMs((us - lU) * qpcHz() / 1000000, nf, a1);
        logf("frame: the frame limiter slept %s ms a frame instead of spinning (%d sleeps, %d short spins in these frames)", a1, (int)(sl - lS), (int)(sp - lP));
        lS = sl; lP = sp; lU = us;
    }
    if (o_custRender) {                              // pose warm (aotr_posewarm2.inc): what it did in these frames
        static LONG lW = 0, lS = 0, lSt = 0, lC = 0, lFJ = 0, lFS = 0, lFSt = 0, lFC = 0, lP = 0; static LONG64 lT = 0, lWT = 0;
        LONG w = g_pwWarmed, sk = g_pwSkips, st = g_pwStale, ch = g_pwChecked, fj = g_pwFullJobs, fs = g_pwFullSkips, fst = g_pwFullStale, fc = g_pwFullChecked, pp = g_pwPasses;
        LONG64 tt = g_pwTicks, wt = g_pwWaitRepT;
        char a1[16], a2[16]; frMs(tt - lT, nf, a1); frMs(wt - lWT, nf, a2);
        logf("pose-warm: last 5 s - %d skeleton updates moved to %d worker threads in %d passes (%d whole-model), the stock update then skipped %d + %d times, inputs changed %d + %d, self-checked %d + %d, MISMATCH %d + %d%s | on the game thread per frame: %s ms (its share of the jobs and the scan), of it waiting for the workers %s ms",
             (int)(w - lW), (int)g_pwNW, (int)(pp - lP), (int)(fj - lFJ), (int)(sk - lS), (int)(fs - lFS), (int)(st - lSt), (int)(fst - lFSt), (int)(ch - lC), (int)(fc - lFC),
             (int)g_pwMismatch, (int)g_pwFullMismatch, g_pwKill ? " - OFF (a mismatch or a fault)" : (g_pwStop ? " - OFF (it cost more than it saved)" : ""), a1, a2);
        lW = w; lS = sk; lSt = st; lC = ch; lFJ = fj; lFS = fs; lFSt = fst; lFC = fc; lP = pp; lT = tt; lWT = wt;
    }
    // the sampled composition, in ms per frame (share of samples x the average frame)
    s_sP += sP; s_nf += nf;
    if (sampledNow && win.n >= 20) {
        LONG64 perFrame = s_sP / (s_nf ? s_nf : 1);                     // ticks, over the frames the samples were taken in
        LONG nfS = s_nf;
        char line[1600]; int p = 0;
        p += wsprintfA(line + p, "frame: sampled (%d samples) - per frame:", (int)win.n);
        for (int ph = 0; ph < FRP_N; ++ph) {
            if (!win.phase[ph]) continue;
            char x[16]; frMs(perFrame * win.phase[ph], win.n, x);
            p += wsprintfA(line + p, " %s %s ms [", kFrPhase[ph], x);
            int first = 1;
            for (int m = 0; m < FRM_N; ++m) if (win.mod[ph][m]) { char y[16]; frMs(perFrame * win.mod[ph][m], win.n, y); p += wsprintfA(line + p, "%s%s %s", first ? "" : ", ", kFrMod[m], y); first = 0; }
            p += wsprintfA(line + p, "]");
            if (p > 1300) break;
        }
        { char x[16], y[16], z[16]; frMs(perFrame * win.waitQ, win.n, x); frMs(perFrame * win.d3dxScope, win.n, y); frMs(perFrame * win.texload, win.n, z);
          p += wsprintfA(line + p, " | waiting for the render worker %s ms, inside a D3DX call %s ms, loading textures %s ms", x, y, z); }
        logf("%s", line);
        if (win.phase[FRP_CLIENT] >= 20) {
            p = wsprintfA(line, "frame: client+drawing by part (sampled):");
            for (int k = 0; k <= g_frNSub; ++k) if (win.sub[k]) { char x[16]; frMs(perFrame * win.sub[k], win.n, x); p += wsprintfA(line + p, " %s %s ms,", k < g_frNSub ? g_frSubName[k] : "the rest of the client step", x); }
            logf("%s", line);
        }
        // the instrument's own cost: the game thread stopped by the sampler, and the wrappers' clock reads
        LONG64 cost = win.suspTicks + (LONG64)nfS * 18 * g_frQpcCost;
        char x[16]; LONG64 us1000 = cost * 1000000000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1) / nfS;   // nanoseconds per frame
        wsprintfA(x, "%d.%03d", (int)(us1000 / 1000000), (int)((us1000 / 1000) % 1000));
        logf("frame: the instrument cost %s ms per frame on the game thread (%d samples, each stopping it %d us on average, and %d clock reads per frame)",
             x, (int)win.suspN, win.suspN ? (int)(win.suspTicks * 1000000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1) / win.suspN) : 0, 18);
    }
    if (sampledNow) { s_sP = 0; s_nf = 0; }
}
static void frDiagReport() {
    if (!g_frDiag || !g_frIncl) return;
    EnterCriticalSection(&g_frWinCs);
    struct { LeafEnt* t; LONG n; const char* what; } tabs[] = { { g_frIncl, g_frDiagN, "client+drawing, under" }, { g_frLeaf, g_frDiagN, "client+drawing, in" },
                                                                 { g_frInclL, g_frDiagNL, "logic, under" }, { g_frLeafL, g_frDiagNL, "logic, in" } };
    for (int ti = 0; ti < 4; ++ti) {
        int m = 0; for (int i = 0; i < 65536; ++i) if (tabs[ti].t[i].key) g_frTmp[m++] = tabs[ti].t[i];
        qsort(g_frTmp, m, sizeof(LeafEnt), lpCmp);
        char line[1100]; int p = 0;
        for (int i = 0; i < m && i < 60; ++i) {
            if (g_frTmp[i].count * 200 < (DWORD)tabs[ti].n) break;
            char sym[160]; lpName(g_frTmp[i].key, sym); char pc[16]; pctStr(g_frTmp[i].count, tabs[ti].n, pc);
            p += wsprintfA(line + p, " %s=%s", sym, pc);
            if (p > 900) { logf("framediag: %s (%d samples):%s", tabs[ti].what, (int)tabs[ti].n, line); p = 0; }
        }
        if (p) logf("framediag: %s (%d samples):%s", tabs[ti].what, (int)tabs[ti].n, line);
        memset(tabs[ti].t, 0, 65536 * sizeof(LeafEnt));
    }
    { int m = 0; for (int i = 0; i < 4096; ++i) if (g_frOurs[i].key) g_frTmp[m++] = g_frOurs[i];
      qsort(g_frTmp, m, sizeof(LeafEnt), lpCmp);
      char line[1100]; int p = 0;
      for (int i = 0; i < m && i < 20; ++i) { char sym[160]; if (g_frTmp[i].key > 1) lpName(g_frTmp[i].key, sym); else lstrcpyA(sym, "(no game.dat caller)"); char pc[16]; pctStr(g_frTmp[i].count, g_frDiagOursN, pc); p += wsprintfA(line + p, " %s=%s", sym, pc); }
      if (p) logf("framediag: this DLL's samples by the game.dat function that called into it (%d):%s", (int)g_frDiagOursN, line);
      memset(g_frOurs, 0, 4096 * sizeof(LeafEnt)); }
    if (g_frPathC) {
        struct { FrPath* t; LONG n; const char* what; int top; } pt[] = { { g_frPathC, g_frPathNC, "client+drawing", 40 }, { g_frPathL, g_frPathNL, "logic", 24 }, { g_frPathA, g_frPathNA, "logic inside AIUpdateInterface::update", 24 } };
        static FrPath tmp[FR_PATHN];
        for (int ti = 0; ti < 3; ++ti) {
            if (!pt[ti].t) continue;
            int m = 0; for (int i = 0; i < FR_PATHN; ++i) if (pt[ti].t[i].count) tmp[m++] = pt[ti].t[i];
            qsort(tmp, m, sizeof(FrPath), frPathCmp);
            char line[1100]; int p = 0; line[0] = 0;
            for (int i = 0; i < m && i < pt[ti].top; ++i) {
                if (tmp[i].count * 400 < (DWORD)pt[ti].n || tmp[i].count < 3) break;
                char one[800]; int q = 0; char pc[16]; pctStr(tmp[i].count, pt[ti].n, pc);
                q += wsprintfA(one + q, " | %s", pc);
                for (int j = 0; j < 4 && tmp[i].k[j]; ++j) { char sym[160]; lpName(tmp[i].k[j], sym); sym[100] = 0; q += wsprintfA(one + q, "%s%s", j ? " < " : " ", sym); }
                if (p && p + q > 900) { logf("framediag: %s, paths (%d samples, innermost < its game.dat callers):%s", pt[ti].what, (int)pt[ti].n, line); p = 0; line[0] = 0; }
                if (q < 1000) { memcpy(line + p, one, (size_t)q + 1); p += q; }
            }
            if (p) logf("framediag: %s, paths (%d samples, innermost < its game.dat callers):%s", pt[ti].what, (int)pt[ti].n, line);
            memset(pt[ti].t, 0, FR_PATHN * sizeof(FrPath));
        }
        g_frPathNC = g_frPathNL = g_frPathNA = 0;
    }
    g_frDiagN = g_frDiagNL = g_frDiagOursN = 0;
    if (g_frDump != INVALID_HANDLE_VALUE && g_frDumpN) { DWORD wr = 0; WriteFile(g_frDump, g_frDumpBuf, g_frDumpN, &wr, NULL); g_frDumpN = 0; FlushFileBuffers(g_frDump); }
    LeaveCriticalSection(&g_frWinCs);
}
static DWORD WINAPI frThread(LPVOID) {
    while (!g_frTid) Sleep(200);
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_frTid);
    PROF_TBI tbi; NT_TIB* tib = NULL;
    if (h && p_NtQIT && p_NtQIT(h, 0, &tbi, sizeof(tbi), NULL) == 0 && tbi.TebBaseAddress) tib = (NT_TIB*)tbi.TebBaseAddress;
    if (!tib) logf("frame: cannot read the game thread - only the exact per-part times are reported, not the sampled parts.");
    static BYTE stk[64 * 1024];
    DWORD stkCap = g_frDiag ? 32 * 1024 : 24 * 1024;            // a battle frame's drawing is ~10-20 KB deep; less to copy = a shorter stop (v54: 64 -> 32 KB)
    HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    frCoresInit();
    logf("frame: armed on the game thread %u - exact times of every frame's parts, %d samples a second%s, the 'frame:' and 'cores:' lines every 5 s.", g_frTid, (int)g_frHz, g_frDiag ? " (DIAGNOSIS: the functions every 30 s)" : "");
    LONG64 last = qpcNow(), lastDiag = last; LONG lastW = g_frW; DWORD lastMove = GetTickCount();
    LONG64 step = g_pqpf.QuadPart / (g_frHz ? g_frHz : 1), nextS = last + step;
    for (;;) {
        LONG64 now = qpcNow();
        if (g_frW != lastW) { lastW = g_frW; lastMove = GetTickCount(); if (!g_frNSub) frResolveSubs(); }
        bool live = GetTickCount() - lastMove < 500;
        // v68: in a network game the sampler runs at 50 a second (the multiplayer measurement: which functions the logic
        // and the drawing spend their time in); in a game of one human it stays as set (0 = off)
        { static LONG lastNet = -1; LONG net = plNetGame() ? 1 : 0;
          if (net != lastNet) { lastNet = net; LONG hz = g_frHz ? g_frHz : (net ? 50 : 0);
#ifndef AOTR_DIAG
                                hz = g_frHz;                                  // v70 release: no stack sampling in network games
#endif
                                g_frHzNow = hz; g_frDiag = hz >= 50; step = g_pqpf.QuadPart / (hz ? hz : 1); nextS = now + step;
                                if (net && hz) logf("frame: a network game - the multiplayer measurement is on (%d samples a second, the functions every 30 s).", (int)hz); } }
        if (g_frHzNow && tib && live && now >= nextS) {
            LONG64 a = qpcNow(); frSample(h, tib, stk, stkCap); LONG64 d = qpcNow() - a;
            nextS += g_gdSpeed < 70 ? step * 4 : step; if (nextS < now) nextS = now + step;   // v60: a quarter of the samples while the machine itself is slowed (each costs it ~5x then)
            // v55.2: a sample that stopped the game thread over 1 ms (a loaded or slowed machine) - wait 10 periods before
            // the next one, so the instrument never adds to the slowness it measures
            if (d * 1000 > g_pqpf.QuadPart) { nextS = now + step * 10; g_frBackoffs++; }
        }
        if (now - last >= g_pqpf.QuadPart * 5) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            frReport(now - last); frCoresLine(now - last);
            if (g_frDiag && now - lastDiag >= g_pqpf.QuadPart * 30) { frDiagReport(); lastDiag = now; }
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            last = now;
        }
        LONG64 rem = nextS - qpcNow(); LONG64 w100 = rem * 10000000 / g_pqpf.QuadPart;    // until the next sample, 0.5..5 ms
        if (w100 < 5000) w100 = 5000; if (w100 > 50000) w100 = 50000;
        if (hTimer) { LARGE_INTEGER dd; dd.QuadPart = -w100; if (SetWaitableTimer(hTimer, &dd, 0, NULL, NULL, FALSE)) WaitForSingleObject(hTimer, 100); else Sleep(1); }
        else Sleep(1);
    }
}
static void installFrameProf() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    bool have = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    int want = have ? (int)GetPrivateProfileIntA("Engine", "FrameProf", 1, ini) : 1;
    // v36 (measurement build): 400 a second by default, so every 30 s the log names the game.dat functions the client
    // step and the logic spend their time in (test-17: 21-24 ms a frame of the engine's own client code in big battles,
    // unnamed). Costs about 1 ms a frame on the game thread. [Engine] FrameSampleHz = 10 puts the light sampler back.
    // v49: 100 a second (test-30: 400 a second cost a median 0.88 ms of every frame on the game thread, each sample
    // stopping it 54-75 us); 100 still gives ~3000 samples in each 30 s function report.
    // v54: 50 a second and at most 32 KB of stack per sample (test-35: 100 a second cost the game thread 0.3-1.2 ms of
    // every battle frame, each sample stopping it 65-280 us); ~1500 samples still go into each 30 s function report.
#ifdef AOTR_DIAG
    // v74: the diagnostics build samples 50 a second by default, so one test names the functions the drawing (shadows
    // included) spends its time in - the next things to move to other cores. About 0.3-1 ms a frame (v54 measurement).
    int hz = have ? (int)GetPrivateProfileIntA("Engine", "FrameSampleHz", 50, ini) : 50;
#else
    int hz = have ? (int)GetPrivateProfileIntA("Engine", "FrameSampleHz", 0, ini) : 0;   // v67: 0 - the stack sampler is off (the game thread is never stopped; FrameSampleHz = 50 turns the diagnosis back on)
#endif
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_FRAMEHZ", env, sizeof(env)) && env[0]) hz = atoi(env);
    if (want <= 0) { logf("frame: the per-frame instrument is off ([Engine] FrameProf = 0)."); return; }
    if (hz < 0) hz = 0; if (hz > 0 && hz < 10) hz = 10; if (hz > 1000) hz = 1000;
    g_frHz = hz; g_frDiag = hz >= 50;
    static const BYTE kSched[5] = { 0xE8, 0x79, 0x0D, 0x1F, 0x00 };
    static const BYTE kClient[6] = { 0xFF, 0x90, 0x9C, 0x00, 0x00, 0x00 };
    static const BYTE kLogic1[6] = { 0xFF, 0x90, 0x98, 0x00, 0x00, 0x00 }, kLogic2[6] = { 0xFF, 0x92, 0x98, 0x00, 0x00, 0x00 };
    static const BYTE kDbg[6] = { 0xFF, 0x90, 0x94, 0x00, 0x00, 0x00 };
    if (memcmp((void*)FR_SCHED_CALL, kSched, 5) || memcmp((void*)FR_CLIENT_CALL, kClient, 6) || memcmp((void*)FR_LOGIC_CALL1, kLogic1, 6) ||
        memcmp((void*)FR_LOGIC_CALL2, kLogic2, 6) || memcmp((void*)FR_DBG_CALL1, kDbg, 6) || memcmp((void*)FR_DBG_CALL2, kDbg, 6)) {
        logf("frame: the engine's frame scheduler is not the code this build has - the per-frame instrument is off."); return; }
    bool stagesOk = true;
    for (int i = 0; i < 8; ++i) {
        const BYTE* q = (const BYTE*)(ULONG_PTR)kFrStageSite[i];
        DWORD want = (i & 1) ? FR_STAGE_DTOR : FR_STAGE_CTOR;
        if (q[0] != 0xE8 || kFrStageSite[i] + 5 + *(const LONG*)(q + 1) != want) stagesOk = false;
    }
    InitializeCriticalSection(&g_frWinCs);
    { HMODULE hs = NULL; if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&frSample, &hs) && hs) {
        IMAGE_NT_HEADERS* nh = (IMAGE_NT_HEADERS*)((BYTE*)hs + ((IMAGE_DOS_HEADER*)hs)->e_lfanew); g_frOursLo = (DWORD)(ULONG_PTR)hs; g_frOursHi = g_frOursLo + nh->OptionalHeader.SizeOfImage; } }
    g_frOurs = (LeafEnt*)VirtualAlloc(NULL, 4096 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_frDiag) {
        g_frIncl = (LeafEnt*)VirtualAlloc(NULL, 65536 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_frLeaf = (LeafEnt*)VirtualAlloc(NULL, 65536 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_frInclL = (LeafEnt*)VirtualAlloc(NULL, 65536 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_frLeafL = (LeafEnt*)VirtualAlloc(NULL, 65536 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_frTmp = (LeafEnt*)VirtualAlloc(NULL, 65536 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_frIncl || !g_frLeaf || !g_frInclL || !g_frLeafL || !g_frTmp) { g_frIncl = NULL; g_frDiag = 0; }
        else { char dp[MAX_PATH]; aotrPath(dp, "framesamples.bin"); g_frDump = CreateFileA(dp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); }
        if (g_frDiag) {
            g_frPathC = (FrPath*)VirtualAlloc(NULL, FR_PATHN * sizeof(FrPath), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            g_frPathL = (FrPath*)VirtualAlloc(NULL, FR_PATHN * sizeof(FrPath), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            g_frPathA = (FrPath*)VirtualAlloc(NULL, FR_PATHN * sizeof(FrPath), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!g_frPathC || !g_frPathL) g_frPathC = g_frPathL = NULL;
        }
    } else g_frTmp = (LeafEnt*)VirtualAlloc(NULL, 4096 * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    // what one clock read costs on this machine (the wrappers make ten a frame)
    { LONG64 a = qpcNow(); for (int i = 0; i < 1000; ++i) qpcNow(); g_frQpcCost = (qpcNow() - a) / 1000; }
    HANDLE fz[256]; int nfz = 0;
    suspendOthers(fz, &nfz, 256);
    frPatchCall(FR_SCHED_CALL, 5, (void*)&frWrapSched);
    frPatchCall(FR_CLIENT_CALL, 6, (void*)&frWrapClient);
    frPatchCall(FR_LOGIC_CALL1, 6, (void*)&frWrapLogic);
    frPatchCall(FR_LOGIC_CALL2, 6, (void*)&frWrapLogic);
    frPatchCall(FR_DBG_CALL1, 6, (void*)&frWrapDbg);
    frPatchCall(FR_DBG_CALL2, 6, (void*)&frWrapDbg);
    if (stagesOk) for (int i = 0; i < 8; ++i) frPatchCall(kFrStageSite[i], 5, kFrStageStub[i]);
    { static const BYTE kA[6] = { 0xFF, 0x15, 0x20, 0x09, 0xBD, 0x00 }, kB[6] = { 0x89, 0x3D, 0x18, 0x43, 0xDE, 0x00 }, kC[5] = { 0xA3, 0x18, 0x43, 0xDE, 0x00 };
      if (!memcmp((void*)0x0063A196, kA, 6) && !memcmp((void*)0x0063A200, kA, 6) && !memcmp((void*)0x0063A1F8, kB, 6) && !memcmp((void*)0x0063A21A, kC, 5)) {
          frPatchCall(0x0063A196, 6, (void*)&frLimA); frPatchCall(0x0063A200, 6, (void*)&frLimA);
          frPatchCall(0x0063A1F8, 6, (void*)&frLimB); frPatchCall(0x0063A21A, 5, (void*)&frLimC);
          g_frLimOn = 1;
      } }
    resumeAll(fz, nfz);
    g_frStagesOn = stagesOk ? 1 : 0;
    InterlockedExchange(&g_frOn, 1);
    CreateThread(NULL, 0, frThread, NULL, 0, NULL);
    logf("frame: installed - every engine frame is timed in its parts (logic, client+drawing, debug, outside) at the scheduler's own calls (game.dat 0x6325CF / 0x6326C6 / 0x6326F0 / 0x6325ED / 0x6326FE, 0x441822); the same calls, the clock read around them. One clock read costs %d ticks of %d.", (int)g_frQpcCost, (int)g_pqpf.QuadPart);
}
