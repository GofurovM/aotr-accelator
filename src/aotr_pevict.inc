// aotr_pevict.inc - v42: the particle limit's "evict the oldest" loop stops repeating a scan that cannot change anything.
//
// WHAT WAS SLOW (test-23, big battle): the hottest single spot of the client step (6% of its samples) was game.dat
// 0x5F3F18 - the particle manager's eviction (Open-BFME-2: Rva001F48DFOldestParticle, called from createParticle 0x5FC44B
// when there are more particles than the limit allows: count = particles - limit). For count times it scans the oldest
// particle of each priority 1..cap-1 and destroys the first whose system is not "locked" (0x44C358: system kind 7 or 8).
// When every candidate is locked, a scan destroys nothing - and the loop scans again, count times (hundreds or thousands),
// for every particle the game tries to create.
//
// WHY THE SHORTCUT IS EXACT: a scan that destroyed nothing changed nothing - 0x44C358 only reads the kind, 0x5FC2FE only
// returns a fixed default system (built once), the heads and the particle count are untouched. So every remaining pass
// finds the same and does the same nothing, until count reaches 0, and the function then returns original + 1 (its own
// arithmetic: count is decremented once more after the loop). This jumps there directly: count = 0, then the function's
// own tail at 0x5F3F83. A scan that destroyed a particle continues exactly as before. Particles are pictures only.
// Off: PEVICT_OFF next to the DLL.
static volatile LONG g_peShort = 0;
static __declspec(naked) void peScanEnd() {                   // replaces 0x5F3F60: cmp edi,[ebp+0Ch] / jl 0x5F3F3F / (jmp 0x5F3F7D)
    __asm {
        cmp  edi, dword ptr [ebp + 0Ch]
        jl   pe_more
        inc  dword ptr [g_peShort]
        and  dword ptr [ebp + 8], 0                            // nothing left to do: the loop's end, as the remaining passes would reach it
        push 0x005F3F83
        ret
    pe_more:
        push 0x005F3F3F                                        // the next priority, as before
        ret
    }
}
static void installPartEvict() {
    static const BYTE kFn[] = {
        0x55,0x8b,0xec,0x51,0x8b,0x45,0x08,0x85,0xc0,0x53,0x56,0x57,0x8b,0xd9,0x89,0x45,0xfc,0x74,0x58,0xff,0x4d,0x08,0x83,0x7b,0x50,0x00,
        0x74,0x52,0x33,0xff,0x47,0x39,0x7d,0x0c,0x7e,0x41,0x8d,0x73,0x14,0x8b,0x06,0x85,0xc0,0x74,0x17,0x8b,0x40,0x3c,0x85,0xc0,0x75,0x05,
        0xe8,0xad,0x83,0x00,0x00,0x8b,0xc8,0xe8,0x00,0x84,0xe5,0xff,0x84,0xc0,0x74,0x0b,0x47,0x83,0xc6,0x04,0x3b,0x7d,0x0c,0x7c,0xda,0xeb,
        0x16,0x8b,0x4c,0xbb,0x10,0x33,0xc0,0x3b,0xc8,0x74,0x05,0x8b,0x11,0x50,0xff,0x12,0x50,0xe8,0x24,0xb7,0xe3,0xff,0x59,0x83,0x7d,0x08,
        0x00,0x75,0xa8,0xff,0x4d,0x08,0x8b,0x45,0xfc,0x2b,0x45,0x08,0x5f,0x5e,0x5b,0xc9,0xc2 };      // 0x5F3F18..0x5F3F90 as analysed
    static const BYTE kSel[] = { 0x8b,0x41,0x0c,0x83,0xf8,0x07,0x74,0x08,0x83,0xf8,0x08,0x74,0x03,0x33,0xc0,0xc3 };   // 0x44C358
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "PEVICT_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("pevict: PEVICT_OFF is present - the engine's own loop stays."); return; }
    __try {
        if (memcmp((void*)0x005F3F18, kFn, sizeof(kFn)) || memcmp((void*)0x0044C358, kSel, sizeof(kSel))) {
            logf("pevict: the particle eviction code differs from the analysed build - off."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x005F3F60, (void*)&peScanEnd, 5);
    resumeAll(fz, nf);
    logf("pevict: the particle limit's eviction loop (0x5F3F18) ends after the first scan that can destroy nothing (the rest would repeat it unchanged).");
}
static void peReport() {
    static LONG l = 0; LONG s = g_peShort;
    if (s != l) logf("pevict: last 30 s - %d eviction loops ended at their first empty scan instead of repeating it", (int)(s - l));
    l = s;
}
