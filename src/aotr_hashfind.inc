// aotr_hashfind.inc - v82: the engine's hash map find for unsigned keys without the `div`.
//
// WHAT WAS SLOW (v74 test): 0x6B4E8F was 2.2% of the logic step's own samples. It is STLport's
// hashtable<pair<const unsigned, void*>>::find (Open-BFME-2's name): bucket = key % bucket count (a 32-bit `div`,
// ~25 cycles), then the bucket's nodes { next, key, value } until the key. 79 places call it - GameLogic::
// findObjectByID (0x449681, the id map at GameLogic+0xB4) among them, for every object looked up by id.
// WHAT THIS DOES: the same bucket by a multiply: with m = floor(2^32 / n), q = (key * m) >> 32 is the quotient or one
// less, so r = key - q * n needs at most one subtraction of n to be key % n exactly (checked offline over 108 million
// keys and 36 bucket counts, prime and not). m is recomputed only when the bucket count changes (a rehash), on the game
// thread; other threads use the plain `%`. The walk over the bucket is the original's. A bucket count of 0 or 1 is left
// to the plain `%` (0 divides by zero exactly as the engine would). SELF-PROOF: the first 20000 buckets of the session
// are also computed with `%`. The answer (the node, or none) is the engine's. Off: HASHFIND_OFF.
#define HF_FN 0x006B4E8F
static volatile LONG g_hfOn = 0, g_hfProof = 20000, g_hfBad = 0, g_hfChecked = 0;
static DWORD g_hfN = 0, g_hfM = 0;
static DWORD __fastcall hfFind(DWORD tab, DWORD, const DWORD* keyp) {    // = 0x6B4E8F (thiscall, one argument: &key)
    DWORD key = *keyp;
    DWORD b = *(DWORD*)(ULONG_PTR)(tab + 4);
    DWORD n = (DWORD)((LONG)(*(DWORD*)(ULONG_PTR)(tab + 8) - b) >> 2);    // sar, as the original
    DWORD r;
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (n >= 2 && g_hfOn && gt && __readfsdword(0x24) == gt) {
        if (n != g_hfN) { g_hfM = (DWORD)(0x100000000ull / n); g_hfN = n; }
        DWORD q = (DWORD)(((unsigned __int64)key * g_hfM) >> 32);
        r = key - q * n; if (r >= n) r -= n;
        if (g_hfProof > 0) {
            g_hfProof--; g_hfChecked++;
            if (r != key % n) { g_hfBad++; InterlockedExchange(&g_hfOn, 0); r = key % n;
                logf("hashfind: SELF-CHECK - the multiplied bucket differed from key %% n - off for the rest of this session."); }
        }
    } else r = key % n;
    DWORD node = *(DWORD*)(ULONG_PTR)(b + r * 4);
    while (node && *(DWORD*)(ULONG_PTR)(node + 4) != key) node = *(DWORD*)(ULONG_PTR)node;
    return node;
}
static void installHashFind() {
    char mk[MAX_PATH];
    if (!g_engineHooks) return;
    // v82.1: OFF by default. In the v82 test the function kept the same share of the logic (2.2%): its cost is the
    // bucket's cold memory, not the divide. HASHFIND_ON turns it on.
    if (GetFileAttributesA(aotrPath(mk, "HASHFIND_ON")) == INVALID_FILE_ATTRIBUTES) { logf("hashfind: off (v82.1 default - no gain measured in the v82 test; HASHFIND_ON turns it on)."); return; }
    static const BYTE kFn[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0x30, 0x57, 0x8B, 0x79, 0x08, 0x2B, 0x79, 0x04, 0x8B, 0xC6, 0xC1, 0xFF, 0x02,
                                0x33, 0xD2, 0xF7, 0xF7, 0x8B, 0x41, 0x04, 0x8B, 0x04, 0x90, 0xEB, 0x07, 0x39, 0x70, 0x04, 0x74, 0x06, 0x8B, 0x00,
                                0x85, 0xC0, 0x75, 0xF5, 0x5F, 0x5E, 0xC2 };   // 0x6B4E8F..0x6B4EBB
    __try { if (memcmp((void*)(ULONG_PTR)HF_FN, kFn, sizeof(kFn))) { logf("hashfind: the hash map find differs from the analysed build - not installed."); return; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)HF_FN, (void*)&hfFind, 5);
    resumeAll(fz, nf);
    InterlockedExchange(&g_hfOn, 1);
    logf("hashfind: the unsigned-key hash map find (0x6B4E8F, findObjectByID's) takes its bucket by a multiply instead of a divide (exact; self-proving; HASHFIND_OFF).");
}
