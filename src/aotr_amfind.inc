// aotr_amfind.inc - v55.2: TheAttributeModifierStore's index-by-name without walking the whole store.
//
// WHAT WAS SLOW (test-37/39 logic samples): 0x614470 was 1.7-2.1% of the logic step on its own (8 instructions): the
// store (TheAttributeModifierStore, 0xDE3C14: a vector of modifier pointers at +0x0C..+0x10) is searched front to back
// for the modifier whose name key (+0x14) is asked; the answer is its index, or -1. Every unit that gains or loses a
// bonus (leadership, spells, upgrades, banners - all factions) asks it, by name key.
// WHAT THIS DOES: the index found for a key is remembered (per store). The next question for that key takes it when it
// is still inside the vector and the modifier there still has that key; anything else walks as the original. The store
// is filled from the INI and a map's INI; nothing is removed during play. SELF-PROOF: the first 20000 remembered
// answers, then 1 in 64, are also found by the walk; a difference turns this off for the session. Game thread only.
// Off: AMFIND_OFF next to the DLL.
#define AMF_N 4096
struct AmfEnt { DWORD store, key; LONG idx; };
static AmfEnt g_amf[AMF_N];
static volatile LONG g_amfOn = 0, g_amfBad = 0, g_amfProof = 20000, g_amfTick = 0, g_amfCalls = 0, g_amfHits = 0, g_amfChecked = 0;
static LONG amfWalk(DWORD store, DWORD key) {                     // the original (0x614470)
    DWORD b = *(DWORD*)(ULONG_PTR)(store + 0x0C), e = *(DWORD*)(ULONG_PTR)(store + 0x10);
    LONG i = 0;
    for (DWORD p = b; p != e; p += 4, ++i) if (*(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)p + 0x14) == key) return i;
    return -1;
}
static LONG __fastcall amfFind(DWORD store, DWORD, DWORD key) {   // = 0x614470 (thiscall, one argument)
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!g_amfOn || !gt || __readfsdword(0x24) != gt) return amfWalk(store, key);
    g_amfCalls++;
    AmfEnt& e = g_amf[((key ^ (store >> 4)) * 2654435761u) >> 20];
    DWORD b = *(DWORD*)(ULONG_PTR)(store + 0x0C), en = *(DWORD*)(ULONG_PTR)(store + 0x10);
    LONG n = (LONG)((en - b) / 4);
    if (e.store == store && e.key == key && e.idx >= 0 && e.idx < n && *(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)(b + 4 * (DWORD)e.idx) + 0x14) == key) {
        LONG idx = e.idx;
        g_amfHits++;
        bool check = g_amfProof > 0 ? (g_amfProof--, true) : AOTR_STEADY((++g_amfTick & 63) == 0);
        if (check) {
            LONG w = amfWalk(store, key); g_amfChecked++;
            if (w != idx) { g_amfBad++; g_amfOn = 0; logf("amfind: the remembered index %d differed from the walk (%d) for key %08X - OFF for the rest of this session.", (int)idx, (int)w, key); return w; }
        }
        return idx;
    }
    LONG w = amfWalk(store, key);
    if (w >= 0) { e.store = store; e.key = key; e.idx = w; }
    return w;
}
static void installAmFind() {
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "AMFIND_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("amfind: AMFIND_OFF is present - the store is walked as before."); return; }
    static const BYTE k[] = { 0x8B, 0x51, 0x0C, 0x8B, 0x49, 0x10, 0x56, 0x33, 0xC0, 0xEB, 0x0F, 0x8B, 0x32, 0x8B, 0x76, 0x14, 0x3B, 0x74, 0x24, 0x08,
                              0x74, 0x0B, 0x40, 0x83, 0xC2, 0x04, 0x3B, 0xD1, 0x75, 0xED, 0x83, 0xC8, 0xFF, 0x5E, 0xC2, 0x04, 0x00 };
    __try { if (memcmp((void*)0x00614470, k, sizeof(k))) { logf("amfind: 0x614470 differs from the analysed build - off."); return; } }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x00614470, (void*)&amfFind, 5);
    resumeAll(fz, nf);
    g_amfOn = 1;
    logf("amfind: TheAttributeModifierStore's index by name key (0x614470) answers from the index it was last found at (checked in range and holding the key; self-proving; AMFIND_OFF).");
}
static void amfReport() {
    static LONG lC = 0, lH = 0;
    LONG c = g_amfCalls, h = g_amfHits;
    if (c == lC) return;
    logf("amfind: %s | last 30 s: %d questions, %d from the remembered index | proof left %d, compared %d, different %d",
         g_amfOn ? "on" : (g_amfBad ? "OFF (a difference was found)" : "off"), (int)(c - lC), (int)(h - lH), (int)g_amfProof, (int)g_amfChecked, (int)g_amfBad);
    lC = c; lH = h;
}
