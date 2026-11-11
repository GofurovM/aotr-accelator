// aotr_partfast.inc - v54: the partition manager's whole-map object loop with its memory read ahead (exact).
//
// WHAT WAS SLOW (test-35, v53 wmq lines): PartitionManager::GetAllObjects (0xA3C6F0 = 1.06 0x628980) walks EVERY object
// on the map (list at manager+0x114: node = { mgr, item, back, next@+0xC, ... }, item = Object+0x6C) and asks a filter
// chain (0xA39450) about each one. AI::findClosestEnemy uses it ~70 times a second in a big battle (range >= 9000), about
// 1 ms a call: 13 ms of an 85 ms logic step, 14.5% of the logic samples. Inside, almost nothing is computed - the time
// is the wait for memory: node -> object -> object+0x264 -> [+8] -> status bits (0x792973 alone was 1.6% of all logic
// samples for 7 instructions), the template, the team. Each wait depends on the one before.
//
// WHAT THIS DOES: the same loop on the game thread - the same filter chain called for the same objects in the same order,
// the same results appended (0xA3A3A0) in the same order - but the node list is first read into an array, and while
// object i is asked, the lines object i+12 / i+6 / i+3 will need are requested from memory (prefetch: no fault, no
// change to anything). getThing is Object's own `lea eax,[ecx-0x6C]` (0x69A6F0) for objects; any other item kind is
// asked through its own virtual call at the same point of the loop as the original. Nothing the filters see changes.
// SELF-PROOF: the first 400 queries, then 1 in 64, are also run the original way (getThing through the virtual call,
// the chain through 0xA39450, node by node); the engine-way list is the one appended, and one difference turns this off
// for the session. Off: PARTFAST_OFF next to the DLL.
static const DWORD kPfSite = 0x00A3C71D, kPfBack = 0x00A3C766, kPfAllows = 0x00A39450, kPfAppend = 0x00A3A3A0, kPfObjThing = 0x0069A6F0;
static volatile LONG g_pfOn = 0, g_pfBad = 0, g_pfProof = 400, g_pfTick = 0;
static volatile LONG g_pfQ = 0, g_pfChecked = 0; static volatile LONG64 g_pfObjs = 0, g_pfPass = 0, g_pfWalkT = 0, g_pfEvalT = 0;
static DWORD* g_pfItem = NULL; static DWORD* g_pfThing = NULL; static DWORD* g_pfOut = NULL; static DWORD* g_pfRef = NULL; static int g_pfCap = 0;
typedef char  (__fastcall* tPfAllows)(DWORD filter, DWORD edx, DWORD obj);
typedef void  (__fastcall* tPfAppend)(DWORD res, DWORD edx, DWORD thing, DWORD zero);
typedef DWORD (__fastcall* tPfThing)(DWORD item, DWORD edx);

static bool pfGrow(int need) {
    if (need <= g_pfCap) return true;
    int cap = g_pfCap ? g_pfCap : 4096; while (cap < need) cap *= 2;
    DWORD* a = (DWORD*)VirtualAlloc(NULL, (SIZE_T)cap * 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!a) return false;
    if (g_pfItem) {                                                // keep what is already collected
        memcpy(a, g_pfItem, (size_t)g_pfCap * 4); memcpy(a + cap, g_pfThing, (size_t)g_pfCap * 4);
        VirtualFree(g_pfItem, 0, MEM_RELEASE);
    }
    g_pfItem = a; g_pfThing = a + cap; g_pfOut = a + 2 * cap; g_pfRef = a + 3 * cap; g_pfCap = cap;
    return true;
}
static __forceinline DWORD pfThingOf(DWORD item) {                // the thing of an item - its own virtual call
    return ((tPfThing)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)item + 0xC)))(item, 0);
}
// The engine's own loop (0xA3C734..0xA3C763) into an array.
static int pfPlain(DWORD pm, DWORD filter, DWORD* out, int max) {
    int n = 0;
    for (DWORD node = *(DWORD*)(ULONG_PTR)(pm + 0x114); node; node = *(DWORD*)(ULONG_PTR)(node + 0xC)) {
        if (n >= max) return -1;
        DWORD th = pfThingOf(*(DWORD*)(ULONG_PTR)(node + 4));
        if (!th) continue;
        if (filter && !((tPfAllows)(ULONG_PTR)kPfAllows)(filter, 0, th)) continue;
        if (out) out[n] = th;
        ++n;
    }
    return n;
}
// Loads guarded: an object leaving the map may hold a stale pointer; a prefetch never faults, a load might.
static __declspec(noinline) void pfDeep(DWORD o) {                // object -> its status holder and template lines
    __try {
        DWORD x = *(DWORD*)(ULONG_PTR)(o + 0x264); if (x) _mm_prefetch((const char*)(ULONG_PTR)(x + 8), _MM_HINT_T0);
        DWORD t = *(DWORD*)(ULONG_PTR)(o + 4); if (t) { _mm_prefetch((const char*)(ULONG_PTR)(t + 0x108), _MM_HINT_T0); _mm_prefetch((const char*)(ULONG_PTR)(t + 0x118), _MM_HINT_T0); }
        DWORD tm = *(DWORD*)(ULONG_PTR)(o + 0x31C); if (tm) _mm_prefetch((const char*)(ULONG_PTR)tm, _MM_HINT_T0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static __declspec(noinline) void pfDeeper(DWORD o) {              // the status holder -> its status bits (0x792973)
    __try {
        DWORD x = *(DWORD*)(ULONG_PTR)(o + 0x264);
        if (x) { DWORD y = *(DWORD*)(ULONG_PTR)(x + 8); if (y) { _mm_prefetch((const char*)(ULONG_PTR)(y + 0x114), _MM_HINT_T0); _mm_prefetch((const char*)(ULONG_PTR)(y + 0x118), _MM_HINT_T0); } }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static __forceinline void pfNear(DWORD o) {
    _mm_prefetch((const char*)(ULONG_PTR)o, _MM_HINT_T0); _mm_prefetch((const char*)(ULONG_PTR)(o + 0x264), _MM_HINT_T0);
    _mm_prefetch((const char*)(ULONG_PTR)(o + 0x31C), _MM_HINT_T0); _mm_prefetch((const char*)(ULONG_PTR)(o + 0x458), _MM_HINT_T0);
}
static LONG g_pfDepth = 0;                                         // a query asked from inside a filter of another (game thread) takes the plain path
static void pfFast(DWORD pm, DWORD filter, DWORD res);
static void __stdcall pfLoop(DWORD pm, DWORD filter, DWORD res) {
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!g_pfOn || !gt || __readfsdword(0x24) != gt || g_pfDepth) {  // the engine's loop, appending as it goes
        for (DWORD node = *(DWORD*)(ULONG_PTR)(pm + 0x114); node; node = *(DWORD*)(ULONG_PTR)(node + 0xC)) {
            DWORD th = pfThingOf(*(DWORD*)(ULONG_PTR)(node + 4));
            if (!th) continue;
            if (filter && !((tPfAllows)(ULONG_PTR)kPfAllows)(filter, 0, th)) continue;
            ((tPfAppend)(ULONG_PTR)kPfAppend)(res, 0, th, 0);
        }
        return;
    }
    g_pfDepth++;
    pfFast(pm, filter, res);
    g_pfDepth--;
}
static void pfFast(DWORD pm, DWORD filter, DWORD res) {
    ULONG64 t0 = __rdtsc();
    int n = 0;
    for (DWORD node = *(DWORD*)(ULONG_PTR)(pm + 0x114); node; node = *(DWORD*)(ULONG_PTR)(node + 0xC)) {
        if (n >= g_pfCap && !pfGrow(n + 1)) { g_pfOn = 0; logf("partfast: no memory for the object array - off."); n = -1; break; }
        DWORD item = *(DWORD*)(ULONG_PTR)(node + 4);
        g_pfItem[n] = item;
        g_pfThing[n] = (item && *(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)item + 0xC) == kPfObjThing) ? item - 0x6C : 0;   // 0: ask it in order
        ++n;
    }
    if (n < 0) {                                                     // no array: the engine's loop
        for (DWORD node = *(DWORD*)(ULONG_PTR)(pm + 0x114); node; node = *(DWORD*)(ULONG_PTR)(node + 0xC)) {
            DWORD th = pfThingOf(*(DWORD*)(ULONG_PTR)(node + 4));
            if (!th) continue;
            if (filter && !((tPfAllows)(ULONG_PTR)kPfAllows)(filter, 0, th)) continue;
            ((tPfAppend)(ULONG_PTR)kPfAppend)(res, 0, th, 0);
        }
        return;
    }
    ULONG64 t1 = __rdtsc();
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (i + 12 < n) { DWORD o = g_pfThing[i + 12]; if (o) pfNear(o); }
        if (i + 6 < n)  { DWORD o = g_pfThing[i + 6];  if (o) pfDeep(o); }
        if (i + 3 < n)  { DWORD o = g_pfThing[i + 3];  if (o) pfDeeper(o); }
        DWORD th = g_pfThing[i];
        if (!th) { DWORD item = g_pfItem[i]; th = item ? pfThingOf(item) : 0; if (!th) continue; }
        if (filter && !((tPfAllows)(ULONG_PTR)kPfAllows)(filter, 0, th)) continue;
        g_pfOut[k++] = th;
    }
    ULONG64 t2 = __rdtsc();
    DWORD* app = g_pfOut; int na = k;
    bool check = g_pfProof > 0 ? (g_pfProof--, true) : AOTR_STEADY((++g_pfTick & 63) == 0);
    if (check) {
        int m = pfPlain(pm, filter, g_pfRef, g_pfCap);
        g_pfChecked++;
        bool same = (m == k);
        for (int i = 0; same && i < k; ++i) if (g_pfRef[i] != g_pfOut[i]) same = false;
        if (!same) {
            g_pfBad++; g_pfOn = 0;
            logf("partfast: the read-ahead loop found %d objects, the engine's loop %d (or a different order) - read-ahead OFF for the rest of this session.", k, m);
        }
        if (m >= 0) { app = g_pfRef; na = m; }                          // the engine-way list is the one the query returns
    }
    for (int i = 0; i < na; ++i) ((tPfAppend)(ULONG_PTR)kPfAppend)(res, 0, app[i], 0);
    g_pfQ++; g_pfObjs += n; g_pfPass += k; g_pfWalkT += (LONG64)(t1 - t0); g_pfEvalT += (LONG64)(t2 - t1);
}
// At 0xA3C71D (mov edi,[esi+114h]; test edi,edi; mov [esp+18h],1; je ...): esi = the manager, [esp+24h] = the filter
// chain (or 0), esp+8 = the local result. The original sets the unwind state to 1 before its loop - so does this.
static __declspec(naked) void pfStub() {
    __asm {
        mov  dword ptr [esp+18h], 1
        lea  eax, [esp+8]
        push eax
        push dword ptr [esp+28h]
        push esi
        call pfLoop
        push 0x00A3C766
        ret
    }
}
static void installPartFast() {
    static const BYTE kSite[] = { 0x8B, 0xBE, 0x14, 0x01, 0x00, 0x00, 0x85, 0xFF, 0xC7, 0x44, 0x24, 0x18, 0x01, 0x00, 0x00, 0x00, 0x74, 0x37,
                                  0x55, 0x8B, 0x6C, 0x24, 0x28, 0x8B, 0x4F, 0x04, 0x8B, 0x01, 0xFF, 0x50, 0x0C, 0x8B, 0xF0, 0x85, 0xF6, 0x74, 0x1C,
                                  0x85, 0xED, 0x74, 0x0C, 0x56, 0x8B, 0xCD, 0xE8 };
    static const BYTE kTail[] = { 0x8B, 0x7F, 0x0C, 0x85, 0xFF, 0x75, 0xCF, 0x5D, 0x8B, 0x44, 0x24, 0x08 };   // 0xA3C75E: next; jne loop; pop ebp; 0xA3C766
    static const BYTE kAllows[] = { 0x56, 0x8B, 0xF1, 0x85, 0xF6, 0x57, 0x74, 0x1B };
    static const BYTE kAppend[] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x8B, 0x0E };
    static const BYTE kThing[] = { 0x8D, 0x41, 0x94, 0xC3 };
    static const BYTE kApp2[]  = { 0x6A, 0x00, 0x56, 0x8D, 0x4C, 0x24, 0x14, 0xE8 };                     // 0xA3C752: push 0; push esi; lea ecx,[esp+14h]; call append
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "PARTFAST_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("partfast: PARTFAST_OFF is present - the engine's own whole-map loop stays."); return; }
    __try {
        if (memcmp((void*)(ULONG_PTR)kPfSite, kSite, sizeof(kSite)) || memcmp((void*)0x00A3C75E, kTail, sizeof(kTail)) ||
            *(LONG*)(ULONG_PTR)(kPfSite + sizeof(kSite)) + kPfSite + sizeof(kSite) + 4 != kPfAllows ||
            memcmp((void*)0x00A3C752, kApp2, sizeof(kApp2)) || *(LONG*)(ULONG_PTR)0x00A3C75A + 0x00A3C75E != kPfAppend ||
            memcmp((void*)(ULONG_PTR)kPfAllows, kAllows, sizeof(kAllows)) || memcmp((void*)(ULONG_PTR)kPfAppend, kAppend, sizeof(kAppend)) ||
            memcmp((void*)(ULONG_PTR)kPfObjThing, kThing, sizeof(kThing)) || *(DWORD*)0x00C1239C != kPfObjThing) {
            logf("partfast: GetAllObjects differs from the analysed build - off."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!pfGrow(8192)) { logf("partfast: no memory - off."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)(ULONG_PTR)kPfSite, (void*)&pfStub, 6);
    resumeAll(fz, nf);
    g_pfOn = 1;
    logf("partfast: the whole-map object loop (0xA3C71D, GetAllObjects) reads its objects ahead - the same filter calls in the same order (self-proving; PARTFAST_OFF).");
}
static void pfReport() {                                           // 30 s report thread
    static LONG lQ = 0; static LONG64 lO = 0, lP = 0, lW = 0, lE = 0;
    LONG q = g_pfQ; LONG64 o = g_pfObjs, p = g_pfPass, w = g_pfWalkT, e = g_pfEvalT, tm = g_msTicksMs;
    if (q == lQ || !tm) { lQ = q; return; }
    LONG dq = q - lQ;
    logf("partfast: %s | last 30 s: %d queries, %d objects a query (%d passed), list walk %d us + filters %d us a query | proof left %d, compared %d, different %d",
         g_pfOn ? "on" : (g_pfBad ? "OFF (a difference was found)" : "off"), (int)dq, (int)((o - lO) / dq), (int)((p - lP) / dq),
         (int)((w - lW) * 1000 / tm / dq), (int)((e - lE) * 1000 / tm / dq), (int)g_pfProof, (int)g_pfChecked, (int)g_pfBad);
    lQ = q; lO = o; lP = p; lW = w; lE = e;
}
