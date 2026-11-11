// aotr_psfind.inc - v41: ParticleSystemManager::findParticleSystemByID (0x5F530A) answered from a remembered node.
//
// WHAT WAS SLOW (test-22, big battle): the client step grew 8-10 ms over the calm part, and the particle code
// (0x5F3xxx..0x5F9xxx) was the largest named part of it. 0x5F530A (named from Open-BFME-2's 1.06 twin) alone was ~3% of
// the client step: for every question "which system has this ID" it walks the manager's whole list of particle systems
// (STLport list at manager+0x4C: node = { next, prev, handle { system*, ... } }, the system's ID at system+0xA8) until
// the ID matches - and the list grows with the battle, like the sound request list did.
//
// WHAT THIS DOES: the node an ID was found at is remembered. The next question for that ID checks the remembered node
// (still linked both ways, still holding a system with this ID) and copies its handle with the engine's own copy
// (0x44C4BE) - what the original does when it finds it. Anything else (not remembered, not linked any more, another ID
// there) walks the list exactly as the original does. IDs are unique, so the node holding the ID is the one the walk
// finds. Particles are pictures only (client side, nothing of the simulation), so this cannot affect multiplayer.
// SELF-PROOF: the first 20000 remembered answers, then 1 in 64, are also found by the full walk; one difference turns
// the memory off for the session. Game thread only. Off: PSFIND_OFF next to the DLL.
static const DWORD kPsFind = 0x005F530A, kPsHandleCopy = 0x0044C4BE;
#define PSF_N 4096
struct PsfEnt { DWORD id, node; };
static PsfEnt g_psf[PSF_N];
static volatile LONG g_psfOn = 0, g_psfProof = 20000, g_psfTick = 0;
static volatile LONG g_psfCalls = 0, g_psfHit = 0, g_psfWalk = 0, g_psfMissing = 0, g_psfChecked = 0, g_psfBad = 0;
static volatile LONG64 g_psfNodes = 0;
typedef DWORD* (__fastcall* tPsFind)(void* mgr, void* edx, DWORD* out, DWORD id);
typedef DWORD* (__fastcall* tPsHCopy)(DWORD* out, void* edx, DWORD* src);
static tPsFind o_psFind = NULL;

static DWORD psfWalk(DWORD mgr, DWORD id) {                       // the original loop
    DWORD head = *(DWORD*)(ULONG_PTR)(mgr + 0x4C);
    LONG v = 0; DWORD hit = 0;
    for (DWORD n = *(DWORD*)(ULONG_PTR)head; n != head; n = *(DWORD*)(ULONG_PTR)n) {
        ++v;
        DWORD s = *(DWORD*)(ULONG_PTR)(n + 8);
        if (!s) continue;
        if (*(DWORD*)(ULONG_PTR)(s + 0xA8) == id) { hit = n; break; }
    }
    g_psfNodes += v;
    return hit;
}
static DWORD* __fastcall hkPsFind(void* mgr, void* edx, DWORD* out, DWORD id) {
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!g_psfOn || !id || !gt || __readfsdword(0x24) != gt) return o_psFind(mgr, edx, out, id);
    DWORD m = (DWORD)(ULONG_PTR)mgr;
    g_psfCalls++;
    PsfEnt& e = g_psf[(id * 2654435761u) >> 20];
    DWORD node = 0;
    if (e.id == id && e.node) {
        DWORD n = e.node;
        __try {
            DWORD nx = *(DWORD*)(ULONG_PTR)n, pv = *(DWORD*)(ULONG_PTR)(n + 4);
            if (nx && pv && *(DWORD*)(ULONG_PTR)(nx + 4) == n && *(DWORD*)(ULONG_PTR)pv == n) {
                DWORD s = *(DWORD*)(ULONG_PTR)(n + 8);
                if (s && *(DWORD*)(ULONG_PTR)(s + 0xA8) == id) node = n;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { node = 0; }
    }
    if (node) {
        g_psfHit++;
        bool check = g_psfProof > 0 ? (g_psfProof--, true) : AOTR_STEADY((++g_psfTick & 63) == 0);
        if (check) {
            DWORD w = psfWalk(m, id);
            g_psfChecked++;
            if (w != node) {
                g_psfBad++; g_psfOn = 0;
                logf("psfind: a remembered node (%08X) differed from the walk (%08X) for particle system ID %u - remembering OFF for the rest of this session.", node, w, id);
                node = w;
            }
        }
    } else {
        node = psfWalk(m, id);
        g_psfWalk++;
        if (node) { e.id = id; e.node = node; } else g_psfMissing++;
    }
    if (!node) { out[0] = 0; out[1] = 0; out[2] = 0; return out; }           // what the original leaves when nothing has the ID
    return ((tPsHCopy)(ULONG_PTR)kPsHandleCopy)(out, NULL, (DWORD*)(ULONG_PTR)(node + 8));
}
static void installPsFind() {
    static const BYTE kFind[] = { 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x33, 0xDB, 0x39, 0x5D, 0x0C, 0x57, 0x8B, 0xF9 };
    static const BYTE kLoop[] = { 0x8B, 0x47, 0x4C, 0x56, 0x8B, 0x30, 0x3B, 0xF0 };             // 0x5F5329: mov eax,[edi+4C]; push esi; mov esi,[eax]; cmp esi,eax
    static const BYTE kId[]   = { 0x8B, 0x4D, 0x0C, 0x39, 0x88, 0xA8, 0x00, 0x00, 0x00 };       // 0x5F5344: mov ecx,[ebp+C]; cmp [eax+0A8h],ecx
    static const BYTE kCopy[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x00, 0x85, 0xC0 };
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "PSFIND_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("psfind: PSFIND_OFF is present - the engine's own walk stays."); return; }
    __try {
        if (memcmp((void*)(ULONG_PTR)kPsFind, kFind, sizeof(kFind)) || memcmp((void*)0x005F5329, kLoop, sizeof(kLoop)) ||
            memcmp((void*)0x005F5344, kId, sizeof(kId)) || memcmp((void*)(ULONG_PTR)kPsHandleCopy, kCopy, sizeof(kCopy))) {
            logf("psfind: findParticleSystemByID differs from the analysed build - off."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    o_psFind = (tPsFind)makeTrampoline((BYTE*)(ULONG_PTR)kPsFind, 5);
    if (!o_psFind) { logf("psfind: no trampoline - off."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)(ULONG_PTR)kPsFind, (void*)&hkPsFind, 5);
    resumeAll(fz, nf);
    g_psfOn = 1;
    logf("psfind: ParticleSystemManager::findParticleSystemByID (0x5F530A) answers from the node it was last found at (checked linked and holding the ID; self-proving against the full walk).");
}
static void psfReport() {                                          // from the 30 s product report thread
    static LONG lC = 0, lH = 0, lW = 0, lM = 0; static LONG64 lN = 0;
    LONG c = g_psfCalls, h = g_psfHit, w = g_psfWalk, mi = g_psfMissing; LONG64 n = g_psfNodes;
    if (!o_psFind || c == lC) return;
    logf("psfind: %s | last 30 s: %d questions, %d answered from the remembered node, %d walked (%d found nothing), %d list nodes visited | proof left %d, compared %d, different %d",
         g_psfOn ? "on" : (g_psfBad ? "OFF (a difference was found)" : "off"), (int)(c - lC), (int)(h - lH), (int)(w - lW), (int)(mi - lM), (int)(n - lN),
         (int)g_psfProof, (int)g_psfChecked, (int)g_psfBad);
    lC = c; lH = h; lW = w; lM = mi; lN = n;
}
