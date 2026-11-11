// ---------------------------------------------------------------- logic step timing by module type and subsystem (measurement)
// GameLogic::update runs every due sleepy-update module through its UpdateModuleInterface (0x62EA97: lea ecx,[ebx+0x10];
// mov eax,[ecx]; call [eax]) and, in sub-frame 5, twelve subsystem updates (mov ecx,[global]; mov eax,[ecx];
// call [eax+0x28] at 0x62EB69..0x62EC05). Those sites call the same functions through timed wrappers that charge the
// time to the module's interface vtable (per logic sub-frame) or to the subsystem; arguments and results are unchanged.
#define SUB_N 12
#define SUB_N_FWD SUB_N
struct ModT { DWORD vt; LONG n[7]; LONG64 t[7]; LONG ln; LONG64 lt; DWORD w; };
// A fixed cost per module type (units of 0.1 microsecond per update, measured in the v23 battle). The logic spread cuts
// the list work by this count instead of by the clock, so the cuts are a pure function of the game state: the same on
// every machine of a multiplayer game and in every replay. The numbers only need to be roughly right - they balance the
// frames, they do not change what runs or in which order.
struct ModW { DWORD vt; DWORD w; };
static const ModW kModW[] = {                              // v25: the v23 and v24 battles together
    { 0x00C6A5A8, 108 }, { 0x00C5D3E0, 880 }, { 0x00C66D3C, 527 }, { 0x00C6D948, 61 }, { 0x00C64284, 1608 }, { 0x00C6D1A0, 79 }, { 0x00C57F80, 8379 }, { 0x00C6B408, 98 },
    { 0x00C67E2C, 77 }, { 0x00C6AC9C, 145 }, { 0x00C585F8, 551 }, { 0x00C629BC, 832 }, { 0x00C67598, 1502 }, { 0x00C11F18, 4 }, { 0x00C6450C, 76 }, { 0x00C12154, 1698 },
    { 0x00C63888, 51 }, { 0x00C5E378, 87 }, { 0x00C68844, 12 }, { 0x00C60670, 46 }, { 0x00C6E150, 2 }, { 0x00C57FC8, 65 }, { 0x00C769EC, 527 }, { 0x00C58DC8, 11 },
    { 0x00C61FA4, 51 }, { 0x00C5FBCC, 481 }, { 0x00C56C5C, 21 }, { 0x00C60C28, 168 }, { 0x00C66C04, 90 }, { 0x00C689A8, 160 }, { 0x00C306B8, 343 }, { 0x00C2E1B8, 2200 },
    { 0x00C4E9F8, 9 }, { 0x00C57828, 2 }, { 0x00C56D60, 1 },
};
#define MODW_DEFAULT 3                                     // a type that never reached the top of a timing report is cheap (v24 over-weighted the tail: the n5 slice ran short)
static ULONG64 g_lsCost = 0;                               // running sum of the costs of the module updates made (game thread)
static DWORD mtWeight(DWORD vt) { for (int i = 0; i < (int)(sizeof(kModW) / sizeof(kModW[0])); ++i) if (kModW[i].vt == vt) return kModW[i].w; return MODW_DEFAULT; }
#define MT_N 1024
static ModT* g_mt = NULL;
static volatile LONG g_mtDrop = 0, g_mtOn = 0;
// ---- v49: the logic spike catcher. test-31: every 15-60 s of a big battle one logic step took ~600 ms instead of
// ~70 (two render frames of ~300 ms in a row, the game thread busy, nothing read from disk, the samples spread over
// hundreds of functions). Which part of the step is it? For every step the module updates are summed by module type
// (with the longest single call and the template of its object), the twelve sub-frame-5 subsystems, the pathfinder
// queue and the six logical calls; a step over 150 ms and over three times the usual is written out. Read only.
#define MS_N 256
struct MsEnt { DWORD vt; LONG n; LONG64 t; LONG64 maxT; char nm[40]; };
static MsEnt g_ms[MS_N]; static WORD g_msUsed[MS_N]; static LONG g_msNUsed = 0, g_msModN = 0;
static LONG64 g_msMod = 0, g_msSubS[SUB_N_FWD], g_msPath = 0, g_msP2b = 0, g_msU[8];
static LONG64 g_msTicksMs = 0;                             // rdtsc ticks in a millisecond (0 until known)
static volatile LONG g_msSpikes = 0;
static LONG64 g_msCallsEma = 0, g_msModTicksEma = 0, g_msModTicksNorm = 0;   // Norm: the module time of a step that was not heavy (v52)
// v52: the module time a step has used by the time it reaches its k*64th module update - this step's (g_msProfCur) and
// the usual (g_msProf, a moving average). A step is "heavy" only against the SAME point of the usual step: the module
// lists are not uniform (the AI updates come early), so v50's "time per call so far x calls per step" overrated every
// ordinary step and gave one of its frames 40-50 ms (test-33).
#define MS_PB 256
static LONG64 g_msProf[MS_PB], g_msProfCur[MS_PB];                            // module updates in a step (moving average): the slicer's progress gauge (v50)
// v53: the partition manager's whole-map query (0xA39420 = 1.06 0x6256F0: every object on the map through a filter
// chain). AI::findClosestEnemy uses it instead of the range query when its range is 9000 or more, and the frame sampler
// put 12.8% of the time inside AIUpdate on one 8-instruction status test called from that chain (test-33/34). Its six
// call sites are timed (calls and time per site, per step and per 30 s) - the same call, the same result.
#define WMQ_N 6
static const DWORD kWmqSite[WMQ_N] = { 0x006AF9E0, 0x00700B00, 0x0079750C, 0x008AFE6A, 0x008CBD36, 0x008CBE6F };
static volatile LONG64 g_wmqT[WMQ_N]; static volatile LONG g_wmqN[WMQ_N];
static LONG64 g_wmqStepT = 0; static LONG g_wmqStepN = 0;
static DWORD __fastcall wmqCall(DWORD self, DWORD, DWORD out, DWORD filter) {
    ULONG64 t0 = __rdtsc();
    DWORD r = ((DWORD (__fastcall*)(DWORD, DWORD, DWORD, DWORD))(ULONG_PTR)0x00A39420)(self, 0, out, filter);
    LONG64 dt = (LONG64)(__rdtsc() - t0);
    DWORD site = (DWORD)(ULONG_PTR)_ReturnAddress() - 5;
    for (int i = 0; i < WMQ_N; ++i) if (kWmqSite[i] == site) { g_wmqT[i] += dt; g_wmqN[i]++; break; }
    g_wmqStepT += dt; g_wmqStepN++;
    return r;
}
static void installWholeMapTimer() {
    for (int i = 0; i < WMQ_N; ++i) {
        const BYTE* q = (const BYTE*)(ULONG_PTR)kWmqSite[i];
        if (q[0] != 0xE8 || kWmqSite[i] + 5 + *(const LONG*)(q + 1) != 0x00A39420) { logf("wmq: call site %08X differs - the whole-map query timer is off.", kWmqSite[i]); return; }
    }
    if (memcmp((const void*)0x00A39420, "\x51\x8B\x44\x24\x0C\x8B\x49\x10", 8)) { logf("wmq: the whole-map query differs - timer off."); return; }
    HANDLE fz[256]; int nf = 0; suspendOthers(fz, &nf, 256);
    for (int i = 0; i < WMQ_N; ++i) {
        BYTE* q = (BYTE*)(ULONG_PTR)kWmqSite[i]; DWORD old = 0;
        if (VirtualProtect(q, 5, PAGE_EXECUTE_READWRITE, &old)) { *(LONG*)(q + 1) = (LONG)((DWORD)(ULONG_PTR)&wmqCall - (kWmqSite[i] + 5)); VirtualProtect(q, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), q, 5); }
    }
    resumeAll(fz, nf);
    logf("wmq: the whole-map partition query (0xA39420) is timed at its 6 call sites (read only).");
}
static void wmqReport() {                                   // 30 s report thread
    static LONG64 lT[WMQ_N]; static LONG lN[WMQ_N];
    LONG64 tm = g_msTicksMs; if (!tm) return;
    char ln[600]; int p = 0; LONG64 all = 0; LONG alln = 0;
    for (int i = 0; i < WMQ_N; ++i) { LONG64 t = g_wmqT[i] - lT[i]; LONG n = g_wmqN[i] - lN[i]; lT[i] = g_wmqT[i]; lN[i] = g_wmqN[i]; all += t; alln += n;
        if (n) p += wsprintfA(ln + p, " | game.dat+%06X: %d calls, %d ms, %d us a call", kWmqSite[i] - 0x400000, (int)n, (int)(t / tm), (int)(t * 1000 / tm / n)); }
    if (alln) logf("wmq: last 30 s - %d whole-map queries, %d ms in all%s", (int)alln, (int)(all / tm), ln);
}
static __declspec(noinline) void msName(char* out, DWORD* iface) {
    out[0] = 0;
    __try {
        DWORD obj = *(DWORD*)((BYTE*)iface - 0x10 + 8), t = obj ? *(DWORD*)(ULONG_PTR)(obj + 4) : 0, sd = t ? *(DWORD*)(ULONG_PTR)(t + 0x64) : 0;
        if (sd) { const char* nm = (const char*)(ULONG_PTR)(sd + 8); int k = 0; while (k < 39 && nm[k] >= 32 && nm[k] < 127) { out[k] = nm[k]; ++k; } out[k] = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}
static __forceinline void msAdd(DWORD vt, LONG64 dt, DWORD* iface) {
    g_msMod += dt; g_msModN++;
    if (!(g_msModN & 63)) { LONG b = g_msModN >> 6; if (b < MS_PB) g_msProfCur[b] = g_msMod; }
    DWORD h = (vt * 2654435761u) >> 24;
    for (int i = 0; i < 8; ++i) {
        DWORD ix = (h + (DWORD)i) & (MS_N - 1); MsEnt& e = g_ms[ix];
        if (e.vt != vt) { if (e.vt) continue; e.vt = vt; e.n = 0; e.t = 0; e.maxT = 0; e.nm[0] = 0; if (g_msNUsed < MS_N) g_msUsed[g_msNUsed++] = (WORD)ix; }
        e.n++; e.t += dt;
        if (dt > e.maxT) { e.maxT = dt; if (g_msTicksMs && dt > g_msTicksMs) msName(e.nm, iface); }
        return;
    }
}
static volatile DWORD g_msCurVt = 0;                       // the module type being updated now (the frame sampler files samples under it)
static DWORD __fastcall mtUpdate(DWORD* iface) {
    DWORD vt = *iface;
    ULONG64 t0 = __rdtsc();
    g_msCurVt = vt;
    DWORD r = ((DWORD (__fastcall*)(DWORD*, void*))(ULONG_PTR)(*(DWORD*)(ULONG_PTR)vt))(iface, NULL);
    g_msCurVt = 0;
    LONG64 dt = (LONG64)(__rdtsc() - t0);
    LONG k = g_lgCurN; if (k < 0 || k > 6) k = 0;
    msAdd(vt, dt, iface);
    DWORD h = (vt * 2654435761u) >> 22;
    for (int i = 0; i < 16; ++i) {
        ModT& m = g_mt[(h + i) & (MT_N - 1)];
        if (m.vt == vt || !m.vt) { if (!m.vt) { m.vt = vt; m.w = mtWeight(vt); } m.n[k]++; m.t[k] += dt; g_lsCost += m.w; return r; }
    }
    g_mtDrop++; g_lsCost += mtWeight(vt);
    return r;
}
static __declspec(naked) void mtStub() {
    __asm {
        lea ecx, [ebx + 0x10]
        jmp mtUpdate
    }
}
static const DWORD kSubSite[SUB_N] = { 0x0062EB69, 0x0062EB85, 0x0062EB90, 0x0062EB9B, 0x0062EBA6, 0x0062EBB8, 0x0062EBC3, 0x0062EBCE, 0x0062EBD9, 0x0062EBEF, 0x0062EBFA, 0x0062EC05 };
static const char* const kSubName[SUB_N] = { "AI", "sub4358", "Taint", "BuildAssistant", "sub3BE8", "WeaponStore", "LocomotorStore", "VictoryConditions", "DelayedXP", "SkirmishAI", "MineshaftNet", "TeamFactory" };
static DWORD g_subGlob[SUB_N];
static DWORD g_subFn[SUB_N];                       // the update each subsystem singleton actually runs (named once seen)
static volatile LONG64 g_subT[SUB_N];
static volatile LONG g_subN[SUB_N];
static void __stdcall subTimed(int k) {
    DWORD obj = *(DWORD*)(ULONG_PTR)g_subGlob[k];
    DWORD fn = *(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)obj + 0x28);
    if (g_subFn[k] != fn) { g_subFn[k] = fn; logf("modtime: %s update is game.dat+%06X (object %08X)", kSubName[k], fn - 0x400000, obj); }
    ULONG64 t0 = __rdtsc();
    ((void (__fastcall*)(DWORD, void*))(ULONG_PTR)fn)(obj, NULL);
    LONG64 dt = (LONG64)(__rdtsc() - t0);
    g_subT[k] += dt; g_subN[k]++; g_msSubS[k] += dt;
}
#define SUB_STUB(k) static __declspec(naked) void subStub##k() { __asm push k __asm call subTimed __asm ret }
SUB_STUB(0) SUB_STUB(1) SUB_STUB(2) SUB_STUB(3) SUB_STUB(4) SUB_STUB(5) SUB_STUB(6) SUB_STUB(7) SUB_STUB(8) SUB_STUB(9) SUB_STUB(10) SUB_STUB(11)
static void* const kSubStub[SUB_N] = { (void*)&subStub0, (void*)&subStub1, (void*)&subStub2, (void*)&subStub3, (void*)&subStub4, (void*)&subStub5, (void*)&subStub6, (void*)&subStub7, (void*)&subStub8, (void*)&subStub9, (void*)&subStub10, (void*)&subStub11 };
static int __cdecl msCmp(const void* a, const void* b) { LONG64 x = g_ms[*(const WORD*)a].t, y = g_ms[*(const WORD*)b].t; return x < y ? 1 : (x > y ? -1 : 0); }
// game thread, when the slicer closes a step: write the step out if it was a spike, then start the next one
static void msStepEnd(ULONG64 stepTicks, ULONG64 emaTicks) {
    if (!g_msTicksMs && g_tscPerQpc > 0 && g_pqpf.QuadPart) g_msTicksMs = (LONG64)(g_tscPerQpc * (double)g_pqpf.QuadPart / 1000.0);
    LONG64 tm = g_msTicksMs;
    {   // v50: the baseline every 150 steps (~30 s), to set the spikes against: an ordinary step and its AI updates
        static LONG64 wStep = 0, wMod = 0, wAI = 0, wPath = 0, wSub = 0; static LONG wN = 0, wAIN = 0;
        LONG64 ai = 0; LONG aiN = 0;
        for (LONG i = 0; i < g_msNUsed; ++i) { MsEnt& e = g_ms[g_msUsed[i]]; if (e.vt == 0x00C6A5A8) { ai = e.t; aiN = e.n; } }
        LONG64 sub = 0; for (int k = 0; k < SUB_N; ++k) sub += g_msSubS[k];
        wStep += (LONG64)stepTicks; wMod += g_msMod; wAI += ai; wAIN += aiN; wPath += g_msPath; wSub += sub; wN++;
        if (wN >= 150 && tm) {
            logf("logic baseline: %d steps - a step %d ms on average | module updates %d ms (%d calls) | AIUpdateInterface::update %d.%d ms in %d calls, %d us a call | subsystems %d ms | pathfinder queue %d.%d ms | logic spikes so far %d",
                 (int)wN, (int)(wStep / wN / tm), (int)(wMod / wN / tm), (int)(g_msCallsEma), (int)(wAI / wN / tm), (int)((wAI * 10 / wN / tm) % 10), (int)(wAIN / wN), wAIN ? (int)(wAI * 1000 / tm / wAIN) : 0,
                 (int)(wSub / wN / tm), (int)(wPath / wN / tm), (int)((wPath * 10 / wN / tm) % 10), (int)g_msSpikes);
            wStep = wMod = wAI = wPath = wSub = 0; wN = wAIN = 0;
        }
    }
    if (tm && (LONG64)stepTicks > 150 * tm && (!emaTicks || stepTicks > emaTicks * 2) && g_msSpikes < 400) {
        g_msSpikes++;
        char a[1500]; int p = 0;
        LONG64 sub = 0; for (int k = 0; k < SUB_N; ++k) sub += g_msSubS[k];
        LONG64 rest = (LONG64)stepTicks - g_msMod - sub - g_msPath - g_msP2b;
        p += wsprintfA(a + p, "logic spike #%d: a step of %d ms (usual %d ms) | logical calls U1 %d U2 %d U3 %d U4 %d U5 %d U6 %d ms | module updates %d ms in %d calls, subsystems %d ms, pathfinder queue %d ms, whole-map queries %d ms in %d, the rest %d ms |",
                       (int)g_msSpikes, (int)(stepTicks / tm), (int)(emaTicks / tm), (int)(g_msU[1] / tm), (int)(g_msU[2] / tm), (int)(g_msU[3] / tm), (int)(g_msU[4] / tm), (int)(g_msU[5] / tm), (int)(g_msU[6] / tm),
                       (int)(g_msMod / tm), (int)g_msModN, (int)(sub / tm), (int)(g_msPath / tm), (int)(g_wmqStepT / tm), (int)g_wmqStepN, (int)(rest / tm));
        for (int k = 0; k < SUB_N; ++k) if (g_msSubS[k] >= tm * 2) p += wsprintfA(a + p, " %s %d ms", kSubName[k], (int)(g_msSubS[k] / tm));
        qsort(g_msUsed, (size_t)g_msNUsed, sizeof(WORD), msCmp);
        p += wsprintfA(a + p, " | heaviest module types:");
        for (int i = 0; i < g_msNUsed && i < 8 && p < 1300; ++i) {
            MsEnt& e = g_ms[g_msUsed[i]]; if (e.t < tm) break;
            p += wsprintfA(a + p, " %s %d ms in %d calls, %d us a call (longest %d.%d ms%s%s)", e.vt == 0x00C6A5A8 ? "AIUpdate" : "vt", (int)(e.t / tm), (int)e.n, e.n ? (int)(e.t * 1000 / tm / e.n) : 0, (int)(e.maxT / tm), (int)((e.maxT * 10 / tm) % 10), e.nm[0] ? " - " : "", e.nm);
            if (e.vt != 0x00C6A5A8) p += wsprintfA(a + p, "[%06X]", e.vt);
        }
        logf("%s", a);
    }
    {   // the usual profile at each point of the step
        LONG nb = g_msModN >> 6; if (nb > MS_PB - 1) nb = MS_PB - 1;
        // every step moves the usual profile, but by at most 1/8 of 1.5x its point: one heavy step barely moves it,
        // a battle that grows heavier is followed within a few dozen steps (v52: without that, a battle 1.5x heavier
        // than its start would count every step as heavy - the v50 failure again)
        if (g_msModN > 0) {
            for (LONG b = 1; b <= nb; ++b) { LONG64 c = g_msProfCur[b], o = g_msProf[b]; if (o && c > o + o / 2) c = o + o / 2; g_msProf[b] = o ? (o * 7 + c) / 8 : c; }
            LONG64 c = g_msMod, o = g_msModTicksNorm; if (o && c > o + o / 2) c = o + o / 2; g_msModTicksNorm = o ? (o * 7 + c) / 8 : c; }
        for (LONG b = 0; b < MS_PB; ++b) g_msProfCur[b] = 0;
    }
    if (g_msModN > 0) { g_msCallsEma = g_msCallsEma ? (g_msCallsEma * 3 + g_msModN) / 4 : g_msModN;
                        LONG64 t = g_msMod; if (g_msModTicksEma && t > g_msModTicksEma * 3) t = g_msModTicksEma * 3;   // one heavy step does not make the next ones look light
                        g_msModTicksEma = g_msModTicksEma ? (g_msModTicksEma * 7 + t) / 8 : t; }
    for (LONG i = 0; i < g_msNUsed; ++i) g_ms[g_msUsed[i]].vt = 0;
    g_msNUsed = 0; g_msMod = 0; g_msModN = 0; g_wmqStepT = 0; g_wmqStepN = 0; g_msPath = 0; g_msP2b = 0;
    for (int k = 0; k < SUB_N; ++k) g_msSubS[k] = 0;
    for (int k = 0; k < 8; ++k) g_msU[k] = 0;
}
static void mtWriteCall(DWORD site, void* fn, int len) {
    BYTE* q = (BYTE*)(ULONG_PTR)site; DWORD op = 0;
    VirtualProtect(q, len, PAGE_EXECUTE_READWRITE, &op);
    q[0] = 0xE8; *(DWORD*)(q + 1) = (DWORD)(ULONG_PTR)fn - (site + 5);
    for (int i = 5; i < len; ++i) q[i] = 0x90;
    VirtualProtect(q, len, op, &op);
    FlushInstructionCache(GetCurrentProcess(), q, len);
}
static void installModuleTimers() {
    static const BYTE kModSite[] = {0x8D,0x4B,0x10,0x8B,0x01,0xFF,0x10,0x83,0xF8,0x01};
    __try {
        if (memcmp((void*)0x0062EA97, kModSite, sizeof(kModSite))) { logf("modtime: module update site differs - logic timers OFF."); return; }
        for (int k = 0; k < SUB_N; ++k) {
            const BYTE* b = (const BYTE*)(ULONG_PTR)kSubSite[k];
            if (b[0] != 0x8B || b[1] != 0x0D || b[6] != 0x8B || b[7] != 0x01 || b[8] != 0xFF || b[9] != 0x50 || b[10] != 0x28) { logf("modtime: subsystem site %08X differs - logic timers OFF.", kSubSite[k]); return; }
            g_subGlob[k] = *(DWORD*)(b + 2);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_mt = (ModT*)VirtualAlloc(NULL, MT_N * sizeof(ModT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_mt) return;
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    mtWriteCall(0x0062EA97, (void*)&mtStub, 7);
    for (int k = 0; k < SUB_N; ++k) mtWriteCall(kSubSite[k], kSubStub[k], 11);
    resumeAll(fz, nf);
    g_mtOn = 1;
    logf("modtime: logic module updates (0x62EA97) and the 12 sub-frame-5 subsystem updates are timed.");
}
static int __cdecl mtCmp(const void* a, const void* b) {
    const ModT* x = *(const ModT* const*)a, *y = *(const ModT* const*)b;
    return x->lt < y->lt ? 1 : (x->lt > y->lt ? -1 : 0);
}
static void mtReport(LONG frames) {                      // every 30 s from the RT report thread
    if (!g_mtOn || frames <= 0 || g_tscPerQpc <= 0) return;
    static ModT* order[MT_N]; int m = 0;
    static LONG lastN[MT_N][7]; static LONG64 lastT[MT_N][7];
    static LONG64 dT[MT_N][7]; static LONG dN[MT_N][7];
    for (int i = 0; i < MT_N; ++i) {
        ModT& e = g_mt[i]; if (!e.vt) continue;
        LONG64 tot = 0; LONG cnt = 0;
        for (int k = 0; k < 7; ++k) { LONG n = e.n[k]; LONG64 t = e.t[k]; dN[i][k] = n - lastN[i][k]; dT[i][k] = t - lastT[i][k]; lastN[i][k] = n; lastT[i][k] = t; tot += dT[i][k]; cnt += dN[i][k]; }
        e.lt = tot; e.ln = cnt;
        if (tot > 0) order[m++] = &e;
    }
    qsort(order, m, sizeof(order[0]), mtCmp);
    LONG64 all = 0; for (int i = 0; i < m; ++i) all += order[i]->lt;
    char a1[16]; logf("modtime: %d module types ran; all module updates %s ms/f (drops %d)", m, msStr((LONG64)((double)all / g_tscPerQpc), frames, a1), (int)g_mtDrop);
    for (int i = 0; i < m && i < 30; ++i) {
        ModT& e = *order[i]; int idx = (int)(&e - g_mt);
        char c1[16], t1[16], sub[200]; int sp = 0; sub[0] = 0;
        for (int k = 1; k < 7; ++k) if (dT[idx][k] > 0) { char x[16]; sp += wsprintfA(sub + sp, " n%d %s", k, msStr((LONG64)((double)dT[idx][k] / g_tscPerQpc), frames, x)); }
        logf("modtime:   vt %08X calls/f %s ms/f %s |%s", e.vt, perStr(e.ln, frames, c1), msStr((LONG64)((double)e.lt / g_tscPerQpc), frames, t1), sub);
    }
    static LONG64 lSubT[SUB_N]; static LONG lSubN[SUB_N];
    char line[600]; int lp = 0;
    for (int k = 0; k < SUB_N; ++k) {
        LONG64 t = rd64(&g_subT[k]); LONG n = g_subN[k]; LONG64 d = t - lSubT[k]; LONG dn = n - lSubN[k]; lSubT[k] = t; lSubN[k] = n;
        char x[16]; lp += wsprintfA(line + lp, " %s %s", kSubName[k], msStr((LONG64)((double)d / g_tscPerQpc), dn > 0 ? dn : 1, x));
    }
    logf("modtime: sub-frame-5 subsystems ms per logic step:%s", line);
}
