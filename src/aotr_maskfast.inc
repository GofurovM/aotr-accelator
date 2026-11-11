// aotr_maskfast.inc - v81: the 19-word flag mask test (0x5DF56A) in four SSE steps instead of a 19-step loop.
//
// WHAT WAS SLOW (v74 test): 0x5DF56A was 2% of the logic step's own samples. It is a pure test over three arrays of
// 19 DWORDs (608 flag bits; Open-BFME-2's Rva001DFE56TestMasks): true when, word by word, none of the "must be clear"
// bits is set in the flags (this) and every "must be set" bit is: `test [clear+i], flags[i]` then
// `(set[i] & flags[i]) == set[i]`, leaving at the first word that fails. Its callers (6 direct, and 0x7EEEDF's 4: the
// large-group audio map's per-object test among them) run it for many objects every logic step.
// WHAT THIS DOES: the same test on 16 words with SSE2 and the last 3 one by one. It reads the same 228 bytes the
// original can read (all three arrays are always 19 words; the original only stops early) and returns the same answer
// in al (every caller tests al only). SELF-PROOF: the first 20000 answers of the session are also computed the original
// way; a difference turns this off. Off: MASKFAST_OFF.
#define MSK_FN 0x005DF56A
static volatile LONG g_mskOn = 0, g_mskProof = 20000, g_mskBad = 0, g_mskChecked = 0;
static bool mskSlow(const DWORD* f, const DWORD* set, const DWORD* clr) {
    for (int i = 0; i < 19; ++i) { if (clr[i] & f[i]) return false; if ((set[i] & f[i]) != set[i]) return false; }
    return true;
}
static __forceinline bool mskFast(const DWORD* f, const DWORD* set, const DWORD* clr) {
    __m128i bad = _mm_setzero_si128();
    for (int i = 0; i < 16; i += 4) {
        __m128i a = _mm_loadu_si128((const __m128i*)(f + i)), s = _mm_loadu_si128((const __m128i*)(set + i)), c = _mm_loadu_si128((const __m128i*)(clr + i));
        bad = _mm_or_si128(bad, _mm_or_si128(_mm_and_si128(c, a), _mm_andnot_si128(a, s)));   // a clear bit set, or a set bit missing
    }
    DWORD t = (clr[16] & f[16]) | (set[16] & ~f[16]) | (clr[17] & f[17]) | (set[17] & ~f[17]) | (clr[18] & f[18]) | (set[18] & ~f[18]);
    return t == 0 && _mm_movemask_epi8(_mm_cmpeq_epi8(bad, _mm_setzero_si128())) == 0xFFFF;
}
static bool __fastcall mskTest(const DWORD* flags, DWORD, const DWORD* set, const DWORD* clr) {   // = 0x5DF56A (thiscall, two arguments)
    bool r = mskFast(flags, set, clr);
    if (g_mskProof > 0) {
        InterlockedDecrement(&g_mskProof); InterlockedIncrement(&g_mskChecked);
        bool w = mskSlow(flags, set, clr);
        if (w != r) { InterlockedIncrement(&g_mskBad); InterlockedExchange(&g_mskProof, 0);
            logf("maskfast: SELF-CHECK - the SSE mask test differed from the original - off for the rest of this session (that answer was the original's).");
            InterlockedExchange(&g_mskOn, 0); return w; }
    }
    if (!g_mskOn) return mskSlow(flags, set, clr);
    return r;
}
static void installMaskFast() {
    char mk[MAX_PATH];
    if (!g_engineHooks) return;
    if (GetFileAttributesA(aotrPath(mk, "MASKFAST_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("maskfast: MASKFAST_OFF is present - the engine's own loop stays."); return; }
    static const BYTE kFn[] = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x55, 0x08, 0x83, 0x65, 0xFC, 0x00, 0x53, 0x56, 0x8B, 0x75, 0x0C, 0x8B, 0xC1, 0x2B, 0xF0, 0x57,
                                0x2B, 0xD0, 0x8B, 0x08, 0x85, 0x0C, 0x06, 0x75, 0x20, 0x8B, 0x3C, 0x02, 0x8B, 0xDF, 0x23, 0xD9, 0x3B, 0xDF, 0x75, 0x15,
                                0xFF, 0x45, 0xFC, 0x83, 0xC0, 0x04, 0x83, 0x7D, 0xFC, 0x13, 0x72, 0xE2, 0xB0, 0x01, 0x5F, 0x5E, 0x5B, 0xC9, 0xC2, 0x08, 0x00,
                                0x32, 0xC0, 0xEB, 0xF5 };   // 0x5DF56A..0x5DF5AC
    __try { if (memcmp((void*)(ULONG_PTR)MSK_FN, kFn, sizeof(kFn))) { logf("maskfast: the mask test differs from the analysed build - not installed."); return; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)MSK_FN, (void*)&mskTest, 7);
    resumeAll(fz, nf);
    InterlockedExchange(&g_mskOn, 1);
    logf("maskfast: the 19-word flag mask test (0x5DF56A) runs in SSE steps (same answer; self-proving; MASKFAST_OFF).");
}
