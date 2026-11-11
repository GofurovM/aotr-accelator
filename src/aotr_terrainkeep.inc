// aotr_terrainkeep.inc - v46/v48: ground patch textures parked while out of view or far (turning the camera = 15 fps).
//
// WHAT WAS SLOW: the ground is drawn in patches (W3DTerrainBackground, 0xD4 bytes each, in a grid). Every frame the
// engine updates each patch's camera status (0x511CF9) and then its textures (updateTexture 0x511F6B). A patch out of
// every camera's view, or in view but too far for a texture of its own, has its one or two textures (+0x38, +0x40)
// released at once (0x44D000, from 0x511D75 / 0x511F3B / 0x511F8D); when it is near and in view again without one,
// updateTexture builds it again from the map (getFlatTexture 0x4AE3CD: a new 512 x 512 texture, created and filled
// cell by cell). Turning the camera moves patches in and out of view and near/far all the time: test-28 / test-29
// built 1800-3000 patch textures in 5 s while the player turned the camera - 44 ms of client per frame, 15-16 fps.
// (v46 kept them only at updateTexture's own release, which the camera update had always done first: no effect.)
//
// WHAT THIS DOES (v48): at those three releases the textures are PARKED instead - the patch's fields are emptied
// exactly as the release leaves them, so the engine sees what it always sees, and the references are held here
// (up to 160 MB, oldest out first; nothing parked above 2.6 GB of address space, everything released above 2.9 GB
// or with less than 400 MB of free memory). Each texture's build records a hash of the cells it was made from (tile,
// blend and extra-blend indices, the blend records, the cell flags, the tile bitmaps' addresses). When updateTexture
// is about to build a patch's textures again, a parked pair whose cells still hash the same is put back instead - the
// pixels the engine would have built; otherwise it is released and the engine builds as it always did.
// Pictures only (client side). Off: TERRAINKEEP_OFF next to the DLL.
#define TK_UPD_CULL 0x00511D75          // in the camera/culling update (0x511CF9): out of every camera's view -> release both
#define TK_UPD_FAR  0x00511F3B          // ... in view but too far for its own texture -> release both
#define TK_TEX_CULL 0x00511F8D          // in updateTexture (0x511F6B): out of view -> release both
#define TK_BUILD    0x00511FD6          // in updateTexture: lea edi,[esi+38h] / cmp [edi],ecx / jne 0x511FC5 - build when it has none
#define TK_BUILD_GO 0x00511FDD
#define TK_RET_NO   0x00511FC5          // xor al,al / pop edi / ... (return false)
#define TK_CLEAR    0x0044D000
#define TK_SLOTS    4096
// A patch whose textures the engine releases has them PARKED here instead: the patch's two fields are set to empty
// exactly as the release would leave them (so the engine sees what it always sees), and this table holds the two
// references. When updateTexture is about to build textures for a patch that has none, a parked pair for it whose
// cells still hash to what they were built from is put back instead (the engine would build the same pixels).
struct TkEnt { DWORD p, t1, t2, hash, bytes, tick; };    // p: 0 empty, 1 removed
static TkEnt g_tk[TK_SLOTS];
static LONG g_tkN = 0, g_tkDead = 0;
static DWORD g_tkMap = 0, g_tkFrameLast = 0, g_tkTick = 0, g_tkMemTick = 0;
static LONG64 g_tkBytes = 0;
static LONG64 g_tkBudget = 160ll << 20;            // v79: set by the PC's memory at install (tkBudgetForThisPc)
static volatile LONG g_tkOn = 0, g_tkMemHigh = 0, g_tkInstalled = 0;
static volatile LONG g_tkUnknown = 0, g_tkKept = 0, g_tkReused = 0, g_tkStale = 0, g_tkEvicted = 0, g_tkResets = 0, g_tkRefused = 0;
typedef void (__fastcall* tTkClear)(void* ecx, void* edx);
static __forceinline void tkRelease(DWORD tex) { if (tex) { DWORD h = tex; ((tTkClear)(ULONG_PTR)TK_CLEAR)(&h, NULL); } }   // the engine's own RefCountPtr release
static __forceinline DWORD tkSlot(DWORD p) { return ((p >> 2) * 2654435761u) >> 20; }   // 12 bits
static TkEnt* tkFind(DWORD p) {
    for (DWORD i = tkSlot(p), k = 0; k < TK_SLOTS; ++k, i = (i + 1) & (TK_SLOTS - 1)) {
        TkEnt& e = g_tk[i];
        if (e.p == p) return &e;
        if (!e.p) return NULL;
    }
    return NULL;
}
static void tkRemove(TkEnt* e) { g_tkBytes -= e->bytes; e->p = 1; e->t1 = e->t2 = 0; g_tkN--; g_tkDead++; }
static void tkRehash() {                                   // drop the "removed" marks
    static TkEnt old[TK_SLOTS]; memcpy(old, g_tk, sizeof(g_tk)); memset(g_tk, 0, sizeof(g_tk)); g_tkDead = 0;
    for (int i = 0; i < TK_SLOTS; ++i) if (old[i].p > 1) {
        for (DWORD j = tkSlot(old[i].p); ; j = (j + 1) & (TK_SLOTS - 1)) if (!g_tk[j].p) { g_tk[j] = old[i]; break; }
    }
}
static void tkDropAll() {                                  // every parked texture released (a new world, memory short)
    for (int i = 0; i < TK_SLOTS; ++i) if (g_tk[i].p > 1) { DWORD a = g_tk[i].t1, b = g_tk[i].t2; g_tk[i].t1 = g_tk[i].t2 = 0;
        __try { tkRelease(a); tkRelease(b); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    memset(g_tk, 0, sizeof(g_tk)); g_tkN = 0; g_tkDead = 0; g_tkBytes = 0;
}
// the map data a patch's texture is made from (what tpCell reads), hashed
static DWORD tkHashCells(const BYTE* map, int x0, int y0, int w) {
    if (!map || w <= 0 || w > 128) return 0;
    TpMap s; tpSnap(s, map);
    DWORD h = 2166136261u;
#define TKMIX(v) { h = (h ^ (DWORD)(v)) * 16777619u; }
    TKMIX(x0); TKMIX(y0); TKMIX(w); TKMIX(s.extraOn); TKMIX(s.nBlend); TKMIX(s.w); TKMIX(s.h); TKMIX(s.originX); TKMIX(s.originY);
    for (int y = y0; y < y0 + w; ++y) for (int x = x0; x < x0 + w; ++x) {
        int ndx = s.w * y + x;
        if (y < 0 || x < 0 || x >= s.w || y >= s.h || ndx < 0 || ndx >= s.size) { TKMIX(0xFFFFFFFF); continue; }
        short tn = s.tile[ndx]; int bl = s.blend[ndx], ex = s.extra[ndx];
        TKMIX((WORD)tn); TKMIX(bl); TKMIX(ex); TKMIX(tpFlag(s, x, y));
        int idx = (int)tn / 4;
        if (idx >= 0 && idx < 0x1000) TKMIX(*(const DWORD*)(s.m + 0xB0 + idx * 4));
        if (bl > 0 && (unsigned)bl < (unsigned)s.nBlend) { const DWORD* b = (const DWORD*)(s.blBegin + bl * 16); TKMIX(b[0]); TKMIX(b[1]); TKMIX(b[2]); TKMIX(b[3]); }
        if (ex > 0 && (unsigned)ex < (unsigned)s.nBlend) { const DWORD* b = (const DWORD*)(s.blBegin + ex * 16); TKMIX(b[0]); TKMIX(b[1]); TKMIX(b[2]); TKMIX(b[3]); }
    }
#undef TKMIX
    return h ? h : 1;
}
static DWORD tkHash(DWORD p) {
    return tkHashCells(*(const BYTE* const*)(ULONG_PTR)(p + 0x5C), *(int*)(ULONG_PTR)(p + 0x50), *(int*)(ULONG_PTR)(p + 0x54), *(int*)(ULONG_PTR)(p + 0x58));
}
// What each texture was BUILT from: TerrainTextureClass::update (aotr_terrain.inc's hook) notes the hash of the cells
// it is about to fill the texture from, keyed by the texture object. A patch is kept out of view only if its
// textures have such a note for its own place - so what is kept is known to be what the cells said at build time,
// and back in view it is drawn only if the cells still say the same.
struct TkBuilt { DWORD tex, hash; int x0, y0, w, upd; };    // upd: it went into a patch through updateTexture's own build (not another path's)
#define TK_BUILT 8192
static TkBuilt g_tkBuilt[TK_BUILT];
static void tkNoteBuilt(void* holder, const BYTE* map, int x0, int y0, int w) {     // holder: getFlatTexture's RefCountPtr<TextureClass>
    if (!g_tkOn || !holder) return;
    DWORD t = 0, h = 0;
    __try { t = *(DWORD*)holder; h = t ? tkHashCells(map, x0, y0, w) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { t = 0; }
    if (!t) return;
    TkBuilt& b = g_tkBuilt[((t >> 3) * 2654435761u) >> 19];
    b.tex = t; b.hash = h; b.x0 = x0; b.y0 = y0; b.w = w; b.upd = 0;
}
// the build hash of the texture a patch holds (+0x38 / +0x40: RefCountPtr<TextureClass>), 0 if unknown or not this place
static DWORD tkBuiltHash(DWORD p, DWORD tex) {
    if (!tex) return 0;
    const TkBuilt& b = g_tkBuilt[((tex >> 3) * 2654435761u) >> 19];
    if (b.tex != tex || !b.upd || b.x0 != *(int*)(ULONG_PTR)(p + 0x50) || b.y0 != *(int*)(ULONG_PTR)(p + 0x54) || b.w != *(int*)(ULONG_PTR)(p + 0x58)) return 0;
    return b.hash;
}
static void tkEvictOldest() {
    TkEnt* best = NULL;
    for (int i = 0; i < TK_SLOTS; ++i) if (g_tk[i].p > 1 && (!best || g_tk[i].tick < best->tick)) best = &g_tk[i];
    if (!best) { g_tkBytes = 0; g_tkN = 0; return; }
    DWORD a = best->t1, b = best->t2; tkRemove(best); g_tkEvicted++;
    __try { tkRelease(a); tkRelease(b); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// a new match or another map: release what was parked for the old one (the references are ours, the objects alive)
static void tkWorldCheck(DWORD p) {
    DWORD map = *(DWORD*)(ULONG_PTR)(p + 0x5C);
    DWORD gl = *(DWORD*)(ULONG_PTR)0x00DE412C, fr = gl ? *(DWORD*)(ULONG_PTR)(gl + 0x40) : 0;
    if (map != g_tkMap || fr < g_tkFrameLast) { if (g_tkN) { g_tkResets++; tkDropAll(); } g_tkMap = map; }
    g_tkFrameLast = fr;
}
static bool tkMemOk() {                                    // checked every 256 calls
    if ((++g_tkMemTick & 255) == 1) {
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            ULONG64 used = ms.ullTotalVirtual - ms.ullAvailVirtual;
            g_tkMemHigh = used > (2600ull << 20) ? (used > (2900ull << 20) ? 2 : 1) : 0;
            if (ms.ullAvailPhys < (400ull << 20)) g_tkMemHigh = 2;                           // the machine itself is short of memory
        }
    }
    if (g_tkMemHigh == 2 && g_tkN) tkDropAll();
    return g_tkMemHigh == 0;
}
static __forceinline bool tkGameThread() { DWORD gt = g_frTidEarly; return gt && __readfsdword(0x24) == gt; }
// the engine is about to release both textures of patch p (any of the three places)
static void __stdcall tkRelease2(DWORD p) {
    DWORD t1 = *(DWORD*)(ULONG_PTR)(p + 0x38), t2 = *(DWORD*)(ULONG_PTR)(p + 0x40);
    if (!t1 && !t2) return;                                // nothing to release: as the engine
    bool park = g_tkOn && tkGameThread();
    if (park) {
        tkWorldCheck(p);
        DWORD h = t1 ? tkBuiltHash(p, t1) : tkBuiltHash(p, t2);
        if (!h || (t1 && t2 && tkBuiltHash(p, t2) != h)) { park = false; g_tkUnknown++; }
        else if (!tkMemOk() || g_tkN >= TK_SLOTS / 2) { park = false; g_tkRefused++; }
        else {
            TkEnt* old = tkFind(p);                         // (cannot normally happen: a parked patch has empty fields)
            if (old) { DWORD a = old->t1, b = old->t2; tkRemove(old); tkRelease(a); tkRelease(b); }
            int w = *(int*)(ULONG_PTR)(p + 0x58); DWORD px = 1; while (px < (DWORD)w * 32 && px < 4096) px <<= 1;
            DWORD bytes = px * px * 2 / 3 * 4 * ((t1 ? 1 : 0) + (t2 ? 1 : 0));               // 16-bit, with the smaller levels
            if (g_tkDead > TK_SLOTS / 4) tkRehash();
            for (DWORD i = tkSlot(p); ; i = (i + 1) & (TK_SLOTS - 1))
                if (g_tk[i].p <= 1) { TkEnt& e = g_tk[i]; e.p = p; e.t1 = t1; e.t2 = t2; e.hash = h; e.bytes = bytes; e.tick = ++g_tkTick; break; }
            g_tkN++; g_tkBytes += bytes; g_tkKept++;
            *(DWORD*)(ULONG_PTR)(p + 0x38) = 0; *(DWORD*)(ULONG_PTR)(p + 0x40) = 0;   // what the release leaves; the references are now ours
            while (g_tkBytes > g_tkBudget && g_tkN > 0) tkEvictOldest();
            return;
        }
    }
    ((tTkClear)(ULONG_PTR)TK_CLEAR)((void*)(ULONG_PTR)(p + 0x38), NULL);                 // the engine's own release
    ((tTkClear)(ULONG_PTR)TK_CLEAR)((void*)(ULONG_PTR)(p + 0x40), NULL);
}
// updateTexture is about to build p's textures (it has none): 1 = a parked pair went back instead
static int __stdcall tkRestore(DWORD p) {
    if (!g_tkOn || !g_tkN || !tkGameThread()) return 0;
    tkWorldCheck(p);
    TkEnt* e = tkFind(p);
    if (!e) return 0;
    DWORD t1 = e->t1, t2 = e->t2, h0 = e->hash; tkRemove(e);
    if (*(DWORD*)(ULONG_PTR)(p + 0x38) || *(DWORD*)(ULONG_PTR)(p + 0x40)) { tkRelease(t1); tkRelease(t2); return 0; }
    DWORD h = 0;
    __try { h = tkHash(p); } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
    bool want2 = (*(BYTE*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)0x00DE4364 + 0x49)) != 0;     // the engine builds the second texture only with this set
    if (!h || h != h0 || !t1 || (t2 != 0) != want2) { tkRelease(t1); tkRelease(t2); g_tkStale++; return 0; }
    *(DWORD*)(ULONG_PTR)(p + 0x38) = t1; *(DWORD*)(ULONG_PTR)(p + 0x40) = t2;             // our references go back to the patch
    g_tkReused++;
    return 1;
}
// updateTexture has just put a texture it built into the patch (0x511FFE / 0x51206A assign): only such textures are
// ever parked and put back - another path (doTesselatedUpdate) builds other sizes into the same field
static void __stdcall tkMarkUpd(DWORD tex) {
    if (!tex) return;
    TkBuilt& b = g_tkBuilt[((tex >> 3) * 2654435761u) >> 19];
    if (b.tex == tex) b.upd = 1;
}
static __declspec(naked) void tkMarkStub1() {
    __asm { push dword ptr [edi] } __asm { call tkMarkUpd } __asm { mov ecx, dword ptr [ebp - 0x10] } __asm { or dword ptr [ebp - 4], -1 } __asm { mov eax, 0x0051200A } __asm { jmp eax }
}
static __declspec(naked) void tkMarkStub2() {
    __asm { push dword ptr [edi] } __asm { call tkMarkUpd } __asm { mov ecx, dword ptr [ebp - 0x14] } __asm { or dword ptr [ebp - 4], -1 } __asm { mov eax, 0x00512076 } __asm { jmp eax }
}
static __declspec(naked) void tkRelStub1() { __asm { push esi } __asm { call tkRelease2 } __asm { mov eax, 0x00511D85 } __asm { jmp eax } }
static __declspec(naked) void tkRelStub2() { __asm { push esi } __asm { call tkRelease2 } __asm { mov eax, 0x00511F4B } __asm { jmp eax } }
static __declspec(naked) void tkRelStub3() { __asm { push esi } __asm { call tkRelease2 } __asm { mov eax, 0x00511F9D } __asm { jmp eax } }
static __declspec(naked) void tkBuildStub() {
    __asm {
        push esi
        call tkRestore
        xor  ecx, ecx                                      // the engine's code here compares against a zero ecx
        test eax, eax
        jnz  tkB_done
        lea  edi, [esi + 0x38]
        cmp  dword ptr [edi], ecx
        jne  tkB_done
        mov  eax, TK_BUILD_GO
        jmp  eax
    tkB_done:
        mov  eax, TK_RET_NO
        jmp  eax
    }
}
static void installTerrainKeep() {
    static const BYTE kRel[] = { 0x8D, 0x4E, 0x38, 0xE8, 0, 0, 0, 0, 0x8D, 0x4E, 0x40, 0xE8, 0, 0, 0, 0 };
    static const BYTE kAfter1[] = { 0x89, 0x7E, 0x44, 0xE9 };                              // 0x511D85 mov [esi+44h],edi / jmp
    static const BYTE kAfter2[] = { 0x83, 0x4D, 0xFC, 0xFF, 0xFF, 0x4B, 0x04 };            // 0x511F4B
    static const BYTE kAfter3[] = { 0x32, 0xC0, 0xEB, 0x27 };                              // 0x511F9D xor al,al / jmp 0x511FC8
    static const BYTE kBuild[] = { 0x8D, 0x7E, 0x38, 0x39, 0x0F, 0x75, 0xE8, 0x51, 0x8B, 0x4E, 0x5C, 0x6A, 0x19 };
    static const BYTE kRetNo[] = { 0x32, 0xC0, 0x5F, 0x8B, 0x4D, 0xF4 };
    static const BYTE kClr[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x0E, 0x85, 0xC9, 0x74, 0x08 };
    static const BYTE kMark1[] = { 0x8B, 0xCF, 0xE8, 0xDD, 0xFD, 0xF2, 0xFF, 0x8B, 0x4D, 0xF0, 0x83, 0x4D, 0xFC, 0xFF, 0x85, 0xC9 };   // 0x511FFC
    static const BYTE kMark2[] = { 0x8B, 0xCF, 0x89, 0x5D, 0xFC, 0xE8, 0x71, 0xFD, 0xF2, 0xFF, 0x8B, 0x4D, 0xEC, 0x83, 0x4D, 0xFC, 0xFF, 0x85, 0xC9 };   // 0x512065
    static const DWORD sites[3] = { TK_UPD_CULL, TK_UPD_FAR, TK_TEX_CULL };
    char mk[MAX_PATH];
    // (v51 had switched it off; test-33's A/B gave no sign against it - the four settings differed by less than the
    //  battle did from one minute to the next - and it keeps 2000+ patches a minute from being rebuilt. On again in v52.)
    if (GetFileAttributesA(aotrPath(mk, "TERRAINKEEP_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("terrain-keep: TERRAINKEEP_OFF is present - patches release their textures, as the engine does."); return; }
    if (!g_tpOn) { logf("terrain-keep: the terrain patch code was not recognised - off."); return; }
    // v79: the budget follows the PC's memory. The parked textures live in the game (and DXVK keeps a copy of each);
    // on a 4 GB PC the game itself already needs most of it, so they would be paid for with paging.
    { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); ULONG64 total = GlobalMemoryStatusEx(&ms) ? (ms.ullTotalPhys >> 20) : 8192;
      if (total < 3500) { logf("terrain-keep: off on this PC (%u MB of memory - the game needs it all).", (unsigned)total); return; }
      g_tkBudget = total < 6000 ? (48ll << 20) : total < 10000 ? (112ll << 20) : (160ll << 20); }
    __try {
        for (int i = 0; i < 3; ++i) {
            const BYTE* q = (const BYTE*)(ULONG_PTR)sites[i];
            for (int k = 0; k < 16; ++k) if ((k != 4 && k != 5 && k != 6 && k != 7 && k != 12 && k != 13 && k != 14 && k != 15) && q[k] != kRel[k]) { logf("terrain-keep: release site %d differs - off.", i); return; }
            if (sites[i] + 8 + *(const LONG*)(q + 4) != TK_CLEAR || sites[i] + 16 + *(const LONG*)(q + 12) != TK_CLEAR) { logf("terrain-keep: release site %d calls something else - off.", i); return; }
        }
        if (memcmp((void*)0x00511D85, kAfter1, sizeof(kAfter1)) || memcmp((void*)0x00511F4B, kAfter2, sizeof(kAfter2)) || memcmp((void*)0x00511F9D, kAfter3, sizeof(kAfter3)) ||
            memcmp((void*)0x00511FFC, kMark1, sizeof(kMark1)) || memcmp((void*)0x00512065, kMark2, sizeof(kMark2)) ||
            memcmp((void*)(ULONG_PTR)TK_BUILD, kBuild, sizeof(kBuild)) || memcmp((void*)(ULONG_PTR)TK_RET_NO, kRetNo, sizeof(kRetNo)) || memcmp((void*)(ULONG_PTR)TK_CLEAR, kClr, sizeof(kClr))) {
            logf("terrain-keep: the terrain patch code differs from the analysed build - off."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)(ULONG_PTR)TK_UPD_CULL, (void*)&tkRelStub1, 16);
    patchJmp((BYTE*)(ULONG_PTR)TK_UPD_FAR, (void*)&tkRelStub2, 16);
    patchJmp((BYTE*)(ULONG_PTR)TK_TEX_CULL, (void*)&tkRelStub3, 16);
    patchJmp((BYTE*)(ULONG_PTR)TK_BUILD, (void*)&tkBuildStub, 7);
    patchJmp((BYTE*)0x00512003, (void*)&tkMarkStub1, 7);
    patchJmp((BYTE*)0x0051206F, (void*)&tkMarkStub2, 7);
    resumeAll(fz, nf);
    g_tkOn = 1; g_tkInstalled = 1;
    logf("terrain-keep: v48 - ground patch textures the engine releases (out of view, or too far for their own texture) are parked (up to %d MB, oldest out first) and put back when the engine would build them again, if the cells they were made from hash the same. TERRAINKEEP_OFF = off.", (int)(g_tkBudget >> 20));
}
static void tkReport() {                                   // 30 s product report
    static LONG lk = 0, lr = 0, ls = 0, le = 0, lf = 0, lu = 0;
    if (!g_tkOn) return;
    LONG k = g_tkKept, r = g_tkReused, s = g_tkStale, e = g_tkEvicted, f = g_tkRefused, u = g_tkUnknown;
    if (k == lk && r == lr && s == ls && e == le && f == lf && u == lu) return;
    logf("terrain-keep: %d patches parked (~%d MB)%s | last 30 s: %d parked, %d put back instead of built, %d built again (ground changed), %d released for the budget, %d not parked (memory), %d not parked (no build record) | new worlds %d",
         (int)g_tkN, (int)(g_tkBytes >> 20), g_tkMemHigh ? " - memory high" : "", (int)(k - lk), (int)(r - lr), (int)(s - ls), (int)(e - le), (int)(f - lf), (int)(u - lu), (int)g_tkResets);
    lk = k; lr = r; ls = s; le = e; lf = f; lu = u;
}
