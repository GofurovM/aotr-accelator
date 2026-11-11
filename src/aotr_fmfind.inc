// aotr_fmfind.inc - v81: Object::findModule by name key without asking every module (self-proving).
//
// WHAT WAS SLOW (v74 test, 50 samples a second): 0x68BDA5 was the second leaf of the logic step's samples (~2%):
// Object::findModule (Open-BFME-2: it compares [vtable+0x10]) walks the object's behaviour modules - a null-ended array
// at object+0x24C - and makes a virtual call on each (slot 4: the module class's name key) until one returns the key
// asked. 246 places in game.dat ask (AI, pathfinding, weapons, upgrades, every frame for many objects), and every
// question touches every module object of the object - cold memory, one virtual call each.
//
// WHY A REMEMBERED ANSWER IS THE ENGINE'S ANSWER:
//  - the array belongs to the object: allocated in its constructor (0x69990F, at 0x699E8B) and freed in its destructor
//    (0x69A760, at 0x69A922); nothing else writes object+0x24C, so its modules and their order never change while the
//    object lives;
//  - slot 4 of a module is its class's name key - the same answer every time for the same module;
//  - v82.1 CORRECTION (the v82 test's one self-check difference): the constructor allocates room for 7 more modules
//    than it fills ((n + 7 + 1) * 4 bytes at 0x699E7E), and AOTR adds modules to living objects - appended after the
//    existing ones. So "no module with this key" can stop being true, and is never remembered now; a module that was
//    found stays the first with its key (anything added later comes after it). A remembered module is used only when
//    the array still holds it at the same place.
// A remembered answer is used only when it is that same object: the same address, the same ObjectID (+0x74: what
// GameLogic::registerObject puts in its id map, 0x62BA6F; ids are not reused within a match) and the same module array.
// Everything is forgotten at every new match (ids start again). SELF-PROOF: the first 20000 remembered answers of every
// match, then (diagnostics build) 1 in 64, are also found by the walk; a difference turns this off for the session.
// Game thread only; other threads walk. The logic and multiplayer get the engine's own answer. Off: FMFIND_OFF.
#define FMF_FN 0x0068BDA5
#define FMF_N  16384
struct FmfEnt { DWORD obj, key, id, arr, mod, gen, idx; };
static FmfEnt* g_fmf = NULL;
static volatile LONG g_fmfOn = 0, g_fmfBad = 0, g_fmfProof = 20000, g_fmfTick = 0, g_fmfCalls = 0, g_fmfHits = 0, g_fmfChecked = 0;
static DWORD g_fmfGen = 1; static LONG g_fmfMatch = -1;
typedef DWORD (__fastcall* tFmfKey)(DWORD, DWORD);
static DWORD fmfWalk(DWORD obj, DWORD key, DWORD* idxOut = NULL) {   // the original (0x68BDA5)
    DWORD* a = *(DWORD**)(ULONG_PTR)(obj + 0x24C);
    for (DWORD* p = a; *p; ++p) {
        DWORD m = *p;
        if (((tFmfKey)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)m + 0x10)))(m, 0) == key) { if (idxOut) *idxOut = (DWORD)(p - a); return m; }
    }
    return 0;
}
static DWORD __fastcall fmfFind(DWORD obj, DWORD, DWORD key) {        // = 0x68BDA5 (thiscall, one argument)
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!g_fmfOn || !gt || __readfsdword(0x24) != gt) return fmfWalk(obj, key);
    g_fmfCalls++;
    if (g_bdgMatches != g_fmfMatch) { g_fmfGen++; g_fmfMatch = g_bdgMatches; InterlockedExchange(&g_fmfProof, 20000); }
    DWORD id = *(DWORD*)(ULONG_PTR)(obj + 0x74), arr = *(DWORD*)(ULONG_PTR)(obj + 0x24C);
    FmfEnt& e = g_fmf[(((obj >> 3) ^ (key * 0x9E3779B1u)) * 2654435761u) >> 18];
    if (e.gen == g_fmfGen && e.obj == obj && e.key == key && e.id == id && e.arr == arr && e.mod && ((DWORD*)(ULONG_PTR)arr)[e.idx] == e.mod &&
        ((tFmfKey)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)e.mod + 0x10)))(e.mod, 0) == key) {   // still there, still that key: one call, not the walk
        DWORD m = e.mod;
        g_fmfHits++;
        bool check = g_fmfProof > 0 ? (InterlockedDecrement(&g_fmfProof), true) : AOTR_STEADY((++g_fmfTick & 63) == 0);
        if (check) {
            DWORD w = fmfWalk(obj, key); g_fmfChecked++;
            if (w != m) { g_fmfBad++; InterlockedExchange(&g_fmfOn, 0);
                logf("fmfind: SELF-CHECK - the remembered module %08X differed from the walk (%08X) - OFF for the rest of this session.", m, w); return w; }
        }
        return m;
    }
    DWORD idx = 0, w = fmfWalk(obj, key, &idx);
    if (w) { e.gen = g_fmfGen; e.obj = obj; e.key = key; e.id = id; e.arr = arr; e.mod = w; e.idx = idx; }   // found modules only (see above)
    return w;
}
static void installFmFind() {
    char mk[MAX_PATH];
    if (!g_engineHooks) return;
    if (GetFileAttributesA(aotrPath(mk, "FMFIND_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("fmfind: FMFIND_OFF is present - the engine's own walk stays."); return; }
    static const BYTE kFn[] = { 0x56, 0x8B, 0xB1, 0x4C, 0x02, 0x00, 0x00, 0x57, 0x33, 0xFF, 0xEB, 0x0E, 0x8B, 0x01, 0xFF, 0x50, 0x10, 0x3B, 0x44, 0x24, 0x0C,
                                0x74, 0x0B, 0x83, 0xC6, 0x04, 0x8B, 0x0E, 0x85, 0xC9, 0x75, 0xEC, 0xEB, 0x02, 0x8B, 0x3E, 0x8B, 0xC7, 0x5F, 0x5E, 0xC2 };   // 0x68BDA5..
    static const BYTE kReg[] = { 0x8B, 0x46, 0x74, 0x89, 0x45, 0x08, 0x8D, 0x45, 0x08, 0x50, 0x81, 0xC1, 0xB4, 0x00, 0x00, 0x00 };   // 0x62BA6F: the id map's key is +0x74
    static const BYTE kArr[] = { 0x89, 0x86, 0x4C, 0x02, 0x00, 0x00 };                                                          // 0x699E8B: the constructor's array
    __try {
        if (memcmp((void*)(ULONG_PTR)FMF_FN, kFn, sizeof(kFn)) || memcmp((void*)(ULONG_PTR)0x0062BA6F, kReg, sizeof(kReg)) ||
            memcmp((void*)(ULONG_PTR)0x00699E8B, kArr, sizeof(kArr)) || memcmp((void*)(ULONG_PTR)0x0069A922, "\x89\xBE\x4C\x02\x00\x00", 6)) {
            logf("fmfind: Object::findModule differs from the analysed build - not installed."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_fmf = (FmfEnt*)VirtualAlloc(NULL, sizeof(FmfEnt) * FMF_N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_fmf) return;
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)FMF_FN, (void*)&fmfFind, 7);
    resumeAll(fz, nf);
    InterlockedExchange(&g_fmfOn, 1);
    logf("fmfind: Object::findModule (0x68BDA5) remembers its answers per object (same id, same module array; self-proving; FMFIND_OFF).");
}
static void fmfReport() {                                              // 30 s report thread
    static LONG lC = 0, lH = 0;
    LONG c = g_fmfCalls, h = g_fmfHits;
    if (c != lC) logf("fmfind: %s | last 30 s: %d questions, %d from the remembered answers | proof left %d, compared %d, different %d",
                      g_fmfOn ? "on" : "OFF", (int)(c - lC), (int)(h - lH), (int)g_fmfProof, (int)g_fmfChecked, (int)g_fmfBad);
    lC = c; lH = h;
}
