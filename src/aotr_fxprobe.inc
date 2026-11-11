// aotr_fxprobe.inc - v73 (diagnostics build only): what the particle update is made of, measured in a real battle.
//
// Before the particle update can be spread over several cores (NOTES.md, "Work in progress"), two things have to be
// known from the game itself, not guessed from the code:
//   1. which per-particle module classes AOTR's effects really use - their update functions are the ones that have to
//      be proved free of shared writes (25 module types exist; a battle may use only a few);
//   2. how much of the frame the particle update takes, and how much of it is model particles (template type 2: they
//      carry a W3D render object, so they stay on the game thread whatever happens).
// It reads and times only. The particle containers' update (vtable slot 0x28: 0x7B09A1 for the default container,
// 0x7B12F4 for types 7 and 8) is wrapped: the original runs unchanged, its time is added up, and every 150th frame
// each container's particles are walked once (first particle at container+0x14, next at +0x64) to count, per
// module slot (+0x94 + 4 * slot, 9 slots on the 0xC0-byte particle class 0xBF7B70), the vtables found there.
// Also every 60 s: the largest free block of the game's address space (the long-session broken pictures were
// suspected of a fragmented 32-bit address space - the total in use never looked high).

#define FXP_VT_DEF   0x00C33AE8                  // the default particle container
#define FXP_VT_T7    0x00C33B48                  // template type 7
#define FXP_VT_T8    0x00C33BA8                  // template type 8
#define FXP_PART_VT  0x00BF7B70                  // the 0xC0-byte particle (its module slots at +0x94)
typedef void (__fastcall* tFxpCont)(void*, void*);
static tFxpCont o_fxpDef = NULL, o_fxpT78 = NULL;
static volatile LONG g_fxpOn = 0;
static volatile LONG64 g_fxpTicksDef = 0, g_fxpTicksT78 = 0;
static volatile LONG g_fxpCallsDef = 0, g_fxpCallsT78 = 0;
static LONG g_fxpCensusFrame = -1;
// one census: particles by template type and by class, module vtables by slot
static LONG g_fxpParts = 0, g_fxpPartsByType[16], g_fxpOtherClass = 0, g_fxpCensuses = 0;
struct FxpMod { DWORD vt; BYTE slot; LONG n; };
#define FXP_MODS 256
static FxpMod g_fxpMod[FXP_MODS]; static LONG g_fxpNMod = 0;
static void fxpNoteMod(int slot, DWORD vt) {
    for (LONG i = 0; i < g_fxpNMod; ++i) if (g_fxpMod[i].vt == vt && g_fxpMod[i].slot == slot) { g_fxpMod[i].n++; return; }
    if (g_fxpNMod < FXP_MODS) { g_fxpMod[g_fxpNMod].vt = vt; g_fxpMod[g_fxpNMod].slot = (BYTE)slot; g_fxpMod[g_fxpNMod].n = 1; g_fxpNMod++; }
}
// v74: the census as the code reads the particle (0x5FA0E6 > 0x5F9F0A chain): modules at +0x94, +0x98, +0x9C, +0xA0,
// then +0xAC (the drawable-attached one), then a vector [+0xB0, +0xB4) of extra modules; and per system (the particle's
// +0x3C) the system's own modules at +0x1AC + 4 * k (k = 0..7; 0x5F9F6D's chain), then its module vector at +0x1CC.
// Slot codes in the table: 0-3 the chain, 4 = +0xAC, 5 = vector entries, 16 + k = the system's module k (24 = its vector).
// Each particle is also sorted by aotr_pxpar.inc's own test: why it can or cannot be updated early on a helper thread.
static LONG g_fxpWhy[PXW_N];
static DWORD g_fxpSysSeen[1024]; static LONG g_fxpNSysSeen = 0;
static bool fxpSysNew(DWORD sys) {
    DWORD h = (sys >> 4) & 1023;
    for (int k = 0; k < 1024; ++k, h = (h + 1) & 1023) { if (g_fxpSysSeen[h] == sys) return false; if (!g_fxpSysSeen[h]) { g_fxpSysSeen[h] = sys; g_fxpNSysSeen++; return true; } }
    return false;
}
static void fxpCensus(void* cont) {                      // game thread, inside the engine's own call
    __try {
        if (g_fxpNSysSeen > 700) { memset(g_fxpSysSeen, 0, sizeof(g_fxpSysSeen)); g_fxpNSysSeen = 0; }
        DWORD p = *(DWORD*)((BYTE*)cont + 0x14);
        for (int k = 0; p && k < 20000; ++k, p = *(DWORD*)(ULONG_PTR)(p + 0x64)) {
            g_fxpParts++;
            DWORD t = *(DWORD*)(ULONG_PTR)(p + 0x3C); DWORD type = t ? *(DWORD*)(ULONG_PTR)(t + 0xC) : 15;   // its system: the template's type copied at +0xC (0x5FC039 / 0x5FC469)
            g_fxpPartsByType[type < 16 ? type : 15]++;
            int why = t ? pxSysWhy(t) : PXW_HANDLE; if (why == PXW_OK) why = pxPartWhy(p, t);
            g_fxpWhy[why]++;
            if (*(DWORD*)(ULONG_PTR)p != FXP_PART_VT) { g_fxpOtherClass++; continue; }
            for (int s = 0; s < 4; ++s) { DWORD m = *(DWORD*)(ULONG_PTR)(p + 0x94 + 4 * s); if (m) fxpNoteMod(s, *(DWORD*)(ULONG_PTR)m); }
            DWORD at = *(DWORD*)(ULONG_PTR)(p + 0xAC); if (at) fxpNoteMod(4, *(DWORD*)(ULONG_PTR)at);
            DWORD vb = *(DWORD*)(ULONG_PTR)(p + 0xB0), ve = *(DWORD*)(ULONG_PTR)(p + 0xB4);
            if (vb && ve > vb && ve - vb < 256) for (DWORD q = vb; q < ve; q += 4) { DWORD m = *(DWORD*)(ULONG_PTR)q; if (m) fxpNoteMod(5, *(DWORD*)(ULONG_PTR)m); }
            if (t && fxpSysNew(t)) {
                for (int k2 = 0; k2 < 8; ++k2) { DWORD m = *(DWORD*)(ULONG_PTR)(t + 0x1AC + 4 * k2); if (m) fxpNoteMod(16 + k2, *(DWORD*)(ULONG_PTR)m); }
                DWORD sb = *(DWORD*)(ULONG_PTR)(t + 0x1CC), se = *(DWORD*)(ULONG_PTR)(t + 0x1D0);
                if (sb && se > sb && se - sb < 256) for (DWORD q = sb; q < se; q += 4) { DWORD m = *(DWORD*)(ULONG_PTR)q; if (m) fxpNoteMod(24, *(DWORD*)(ULONG_PTR)m); }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static __forceinline void fxpMaybeCensus(void* cont) {
    LONG f = g_frW;
    if ((f % 150) == 0) { if (g_fxpCensusFrame != f) { g_fxpCensusFrame = f; g_fxpCensuses++; } fxpCensus(cont); }
}
static void __fastcall fxpDef(void* ecx, void* edx) {
    fxpMaybeCensus(ecx);
    ULONG64 t0 = __rdtsc(); o_fxpDef(ecx, edx); g_fxpTicksDef += (LONG64)(__rdtsc() - t0); g_fxpCallsDef++;
}
static void __fastcall fxpT78(void* ecx, void* edx) {
    fxpMaybeCensus(ecx);
    ULONG64 t0 = __rdtsc(); o_fxpT78(ecx, edx); g_fxpTicksT78 += (LONG64)(__rdtsc() - t0); g_fxpCallsT78++;
}
static bool fxpPatchSlot(DWORD vt, void* hook, void* expect) {
    void** slot = (void**)(ULONG_PTR)(vt + 0x28); DWORD op = 0;
    if (*slot != expect || !VirtualProtect(slot, 4, PAGE_READWRITE, &op)) return false;
    *slot = hook; VirtualProtect(slot, 4, op, &op);
    return true;
}
static void installFxProbe() {
    if (!g_engineHooks) return;
    __try {
        DWORD defNow = *(DWORD*)(FXP_VT_DEF + 0x28);      // 0x7B09A1, or aotr_pxpar.inc's replacement (installed first)
        if ((defNow != 0x007B09A1 && defNow != (DWORD)(ULONG_PTR)&pxCont) || *(DWORD*)(FXP_VT_T7 + 0x28) != 0x007B12F4 || *(DWORD*)(FXP_VT_T8 + 0x28) != 0x007B12F4 ||
            *(DWORD*)(FXP_VT_DEF + 0x20) != 0x009F9F37) { logf("fxprobe: the particle containers differ from the analysed build - not installed."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    o_fxpDef = (tFxpCont)(ULONG_PTR)*(DWORD*)(FXP_VT_DEF + 0x28); o_fxpT78 = (tFxpCont)(ULONG_PTR)0x007B12F4;
    bool a = fxpPatchSlot(FXP_VT_DEF, (void*)&fxpDef, (void*)o_fxpDef);
    bool b = fxpPatchSlot(FXP_VT_T7, (void*)&fxpT78, (void*)o_fxpT78) && fxpPatchSlot(FXP_VT_T8, (void*)&fxpT78, (void*)o_fxpT78);
    InterlockedExchange(&g_fxpOn, a || b ? 1 : 0);
    logf("fxprobe: installed (%s%s) - the particle update is timed, and every 150th frame the particles' module classes are counted (read only).",
         a ? "default container" : "", b ? ", types 7 and 8" : "");
}
// the report thread, every 60 s
static void fxpReport(LONG frames) {
    if (!g_fxpOn || frames <= 0) return;
    static LONG64 lDef = 0, lT78 = 0; static LONG lCd = 0, lCt = 0;
    double tpm = g_tscPerQpc > 0 ? g_tscPerQpc * (double)g_pqpf.QuadPart / 1000.0 : 0;   // rdtsc ticks a millisecond
    LONG64 dDef = g_fxpTicksDef - lDef, dT = g_fxpTicksT78 - lT78; LONG cd = g_fxpCallsDef - lCd, ct = g_fxpCallsT78 - lCt;
    lDef = g_fxpTicksDef; lT78 = g_fxpTicksT78; lCd = g_fxpCallsDef; lCt = g_fxpCallsT78;
    char a[32], b[32];
    if (tpm > 0) { int x = (int)(dDef / tpm * 1000.0 / frames), y = (int)(dT / tpm * 1000.0 / frames);
                   wsprintfA(a, "%d.%03d", x / 1000, x % 1000); wsprintfA(b, "%d.%03d", y / 1000, y % 1000); }
    else { lstrcpyA(a, "?"); lstrcpyA(b, "?"); }
    char ty[200]; int n = 0; ty[0] = 0;
    for (int i = 0; i < 16; ++i) if (g_fxpPartsByType[i]) n += wsprintfA(ty + n, " t%d:%d", i, (int)g_fxpPartsByType[i]);
    logf("fxprobe: last 30 s, a frame - particle update %s ms in the default containers (%d calls a frame), %s ms in types 7/8 (%d) | censuses %d: %d particles counted (by template type%s; type 2 = model particles), %d of another class",
         a, frames ? (int)(cd / frames) : 0, b, frames ? (int)(ct / frames) : 0, (int)g_fxpCensuses, (int)g_fxpParts, ty[0] ? ty : " -", (int)g_fxpOtherClass);
    { char w[700]; int q = 0; w[0] = 0;
      for (int i = 0; i < PXW_N && q < 600; ++i) if (g_fxpWhy[i]) q += wsprintfA(w + q, " %s %d;", kPxWhy[i], (int)g_fxpWhy[i]);
      logf("fxprobe: the particles counted, by aotr_pxpar.inc's test (can a helper thread update it early?) -%s", w[0] ? w : " none"); }
    static const int kSlots[] = { 0, 1, 2, 3, 4, 5, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
    for (int si = 0; si < (int)(sizeof(kSlots) / sizeof(kSlots[0])); ++si) {
        int s = kSlots[si];
        char ln[900]; int q = 0; ln[0] = 0; int k = 0;
        for (LONG i = 0; i < g_fxpNMod && q < 800; ++i) if (g_fxpMod[i].slot == s) {
            DWORD u1 = 0, u4 = 0, u5 = 0;
            __try { u1 = *(DWORD*)(ULONG_PTR)(g_fxpMod[i].vt + 4); u4 = *(DWORD*)(ULONG_PTR)(g_fxpMod[i].vt + 0x10); u5 = *(DWORD*)(ULONG_PTR)(g_fxpMod[i].vt + 0x14); } __except (EXCEPTION_EXECUTE_HANDLER) { }
            q += wsprintfA(ln + q, s >= 16 ? " vt %08X (slots 1/4/5: %08X %08X %08X) x%d;" : " vt %08X (update %08X) x%d;", g_fxpMod[i].vt, u1, s >= 16 ? u4 : (DWORD)g_fxpMod[i].n, u5, (int)g_fxpMod[i].n); ++k;
        }
        if (k) logf("fxprobe: %s %d: %d classes -%s", s >= 16 ? "system module" : (s == 4 ? "attached module (+0xAC)" : (s == 5 ? "extra modules (vector)" : "particle module slot")), s >= 16 ? s - 16 : s, k, ln);
    }
    // what grows in the address space: by kind, and what this DLL itself holds
    {
        ULONG_PTR at2 = 0x10000; MEMORY_BASIC_INFORMATION mi; ULONG64 privC = 0, privR = 0, mapC = 0, mapR = 0, img = 0;
        while (at2 < 0xFFFE0000u && VirtualQuery((void*)at2, &mi, sizeof(mi)) == sizeof(mi)) {
            if (mi.State != MEM_FREE) {
                ULONG64 sz = mi.RegionSize; bool c = mi.State == MEM_COMMIT;
                if (mi.Type == MEM_IMAGE) img += sz; else if (mi.Type == MEM_MAPPED) { if (c) mapC += sz; else mapR += sz; } else { if (c) privC += sz; else privR += sz; }
            }
            ULONG_PTR nx = (ULONG_PTR)mi.BaseAddress + mi.RegionSize; if (nx <= at2) break; at2 = nx;
        }
        logf("fxprobe: address space by kind - private %u MB committed + %u MB reserved, mapped %u MB committed + %u MB reserved (DXVK's GPU memory mappings among them), images %u MB | this DLL: parked ground textures %u MB, texture decoding in flight %u MB, render-thread buffer copies %u MB",
             (unsigned)(privC >> 20), (unsigned)(privR >> 20), (unsigned)(mapC >> 20), (unsigned)(mapR >> 20), (unsigned)(img >> 20), (unsigned)(g_tkBytes >> 20), (unsigned)(g_txjInflight >> 20), (unsigned)(g_rtBMBytes >> 20));
    }
    // the largest free block of the address space (fragmentation, for the long-session pictures)
    ULONG_PTR at = 0x10000, best = 0, bestAt = 0; MEMORY_BASIC_INFORMATION mbi; int regions = 0;
    while (at < 0xFFFE0000u && VirtualQuery((void*)at, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > best) { best = mbi.RegionSize; bestAt = (ULONG_PTR)mbi.BaseAddress; }
        ULONG_PTR next = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize; if (next <= at) break; at = next; ++regions;
    }
    logf("fxprobe: address space - the largest free block is %u MB (at %08X), %d regions walked", (unsigned)(best >> 20), (unsigned)bestAt, regions);
}
