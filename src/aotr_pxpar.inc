// aotr_pxpar.inc - v74: the particles' own update spread over the spare cores (pictures only; exact by construction,
// and it checks itself while the game runs).
//
// WHAT IT TAKES OFF THE GAME THREAD: every frame ParticleSystemManager::update (0x5F5123) walks its systems (list at
// manager+0x4C) and calls each one's update (0x5FA2CE). That moves the system, emits new particles (0x5F3CCF), runs the
// system's own modules (+0x1AC.., 0x5F9F6D) and then the container's update (vtable slot 0x28, 0x7B09A1 for the
// default container 0xC33AE8): for every particle, Particle::update (0x5FA0E6) and, when it returns false, delete.
// Particle::update is the part that grows with the number of particles (about 2 us each in AOTR's battles).
//
// HOW: at the start of the manager's loop (0x5F51BC) the particles that can be updated early are picked, updated on
// the helper threads and the game thread together, and their results (alive or not) are kept. When the engine then
// reaches each system's container update - unchanged order, unchanged point in the frame - the replacement of slot
// 0x28 (pxCont) takes the kept result instead of calling Particle::update again, and deletes the dead particles
// exactly as 0x7B09A1 does, at the same moment. Every other particle is updated there, by the engine's code, as before.
//
// WHY THE RESULT IS THE SAME (the code read in game.dat, the same bytes in the 2.02 delayfix and AOTR 10 builds):
//  - Particle::update reads and writes only the particle and its own modules (+0x94 C32A64, +0x98 C329B0,
//    +0x9C C32BD0, +0xA0 C32B18; their sizes 0xB0, 0x5C, 0x38, 0x18), calls CRT sqrt/atan, and reads its system's
//    kind (+8). The whole call tree (31 functions, listed in kPxCode) has no write outside those objects, no lock, no
//    random number. The only shared thing it touches is the system's handle list (sys+0x9C/+0xA0): the physics module
//    links a stack handle to the particle's system and unlinks it again (0x44C4BE / 0x44C465). So all particles of one
//    system are done by one thread - systems are never split between threads.
//  - Picked are only particles whose update cannot depend on what runs between the loop's start and their system's
//    container update: not a model particle (kind 2) and no render object (+0x8C); no system that follows the
//    particle (+0x78: it reads the particle's position in its own update); no drawable-attached module (+0xAC) and no
//    extra modules (+0xB0); a physics module that neither follows the system's position (+0x15: reads sys+0xEC, which
//    the system update sets) nor a drawable (+0x14); a drift module at sys+0x1B8 only of the class whose getters
//    return its own constant fields (C32AC8, v75); and the system's own modules must be ones whose update cannot
//    reach a particle (pxSysModOk). The system must be one the loop really updates
//    this frame: the loop's own skip test (sys+0x98, the flag at [ebp-0xD]), the system update's own early returns
//    (global +0x9AF, delay counter sys+0x120), the system and container classes the code was read for.
//  - Nothing else can see the particle early: the eviction (0x5F3F18) picks by list position, not by particle state,
//    and the particle count changes at the same moments as before (deletes happen where they always happened).
//  - A particle deleted before its result is used (eviction by another system's emission) is forgotten by the hook on
//    the particle's deleting destructor (vtable 0xBF7B70 slot 0, the only way 0x5FB7FC is reached), so a new particle
//    at the same address can never take an old result.
//  - The helpers run with the game thread's x87 control word and MXCSR, so the arithmetic is bit for bit the same.
//
// IT CHECKS ITSELF, all session long (release too, it is cheap):
//  - every 16th frame it uses, up to 8 particles that would have been picked are left to the engine; their bytes
//    (the particle without its list links, and all four modules) are copied at the loop's start and compared just
//    before the engine updates them. Any difference means something did change them in between - then the early
//    update would not be exact: it switches itself off for the rest of the session (yellow light).
//  - a kept result that was never used (the system was not updated after all) is counted; more than 64 in one frame
//    or more than 1 in 1000 also switch it off.
//  - an exception on a helper switches it off.
// Particles are pictures only (client side): the logic and multiplayer are not touched either way. Off: PXPAR_OFF.

#define PX_LOOP      0x005F51BC                  // the manager's loop: mov eax,[edi+4Ch] / push esi / mov esi,[eax] / cmp esi,eax
#define PX_LOOP_NEXT 0x005F51C2
#define PX_PUPD      0x005FA0E6                  // Particle::update (thiscall, returns al = alive)
#define PX_FREE      0x0042F6A0                  // the game's operator delete (cdecl)
#define PX_SYS_VT    0x00BF7B48                  // ParticleSystem (its update 0x5FA2CE is slot 4)
#define PX_CONT_VT   0x00C33AE8                  // the default particle container
#define PX_CONT_UPD  0x007B09A1                  // its update (slot 0x28)
#define PX_CONT_FIRST 0x009F9F37                 // its first particle (slot 0x20): mov eax,[ecx+14h]
#define PX_PART_VT   0x00BF7B70                  // the 0xC0-byte particle
#define PX_PART_DEL  0x005FBACA                  // its deleting destructor (slot 0)
#define PX_MAXP      16384
#define PX_TABN      65536
#define PX_MIN       192                          // fewer particles than this: the helpers cost more than they save
#define PX_CHECKS    8

struct PxEnt { DWORD p, stamp, v; };              // v: 0 dead, 1 alive, 2 + k: check copy k
static PxEnt* g_pxTab = NULL;
static DWORD* g_pxParts = NULL; static BYTE* g_pxAlive = NULL; static int* g_pxSlot = NULL;
static int g_pxSys[PX_MAXP / 2 + 1];              // chunk boundaries (indices into g_pxParts), always between systems
static volatile LONG g_pxOn = 0, g_pxHas = 0, g_pxFault = 0, g_pxHold = 0;   // g_pxHold: diagnostics glitch hunt (aotr_accel.cpp) - spread nothing for now
static DWORD g_pxStamp = 0;
static int g_pxN = 0, g_pxPrevN = 0;              // results kept this frame / the previous one (their table slots in g_pxSlot)
static volatile LONG g_pxNext = 0, g_pxNChunks = 0, g_pxOpen = 0, g_pxInside = 0;
static HANDLE g_pxSem = NULL; static int g_pxHelpers = 0;
static unsigned short g_pxCw = 0x027F; static unsigned int g_pxCsr = 0x1F80;
// counters (the diagnostics report reads them; the release build only keeps them)
static volatile LONG64 g_pxDone = 0, g_pxUnused = 0, g_pxTicksPar = 0, g_pxTicksPick = 0;
static volatile LONG g_pxFrames = 0, g_pxFramesPar = 0, g_pxChecked = 0, g_pxCheckBad = 0, g_pxLoops = 0;
static char g_pxOffWhy[160] = "";
// v80: the trial - is spreading faster on THIS PC? Both ways give the same particles (that is what the self-checks
// prove), so the DLL may simply time them. Frames alternate in blocks of 64: spread / the engine alone. The cost of a
// frame is the loop start (pick + spread) plus every container update, per particle the manager holds; only frames
// with PX_MIN particles or more count. After 900 counted frames of each kind it decides once: spreading stays when it
// costs at most 92% of the engine's own way, otherwise the engine updates the particles itself for the session.
static volatile LONG g_pxTrial = 1, g_pxAutoOff = 0;
static LONG g_pxTrialMode = 0, g_pxTrialCounts = 0, g_pxTrialMgrN = 0;
static ULONG64 g_pxTrialAcc = 0, g_pxTrialTicks[2] = { 0, 0 }; static LONG64 g_pxTrialParts[2] = { 0, 0 }; static LONG g_pxTrialFrames[2] = { 0, 0 };
static void pxTrialCommit() {                         // game thread, at the next loop start: the last frame's cost
    if (g_pxTrialCounts && g_pxTrialMgrN > 0) {
        g_pxTrialTicks[g_pxTrialMode] += g_pxTrialAcc; g_pxTrialParts[g_pxTrialMode] += g_pxTrialMgrN; g_pxTrialFrames[g_pxTrialMode]++;
    }
    g_pxTrialAcc = 0; g_pxTrialCounts = 0;
    if (g_pxTrialFrames[0] >= 900 && g_pxTrialFrames[1] >= 900) {
        double a = (double)g_pxTrialTicks[0] / (double)(g_pxTrialParts[0] ? g_pxTrialParts[0] : 1);
        double b = (double)g_pxTrialTicks[1] / (double)(g_pxTrialParts[1] ? g_pxTrialParts[1] : 1);
        int pct = b > 0 ? (int)(a * 100.0 / b) : 100;
        InterlockedExchange(&g_pxTrial, 0);
        if (pct > 92) { InterlockedExchange(&g_pxAutoOff, 1);
            logf("pxpar: trial - on this PC spreading the particles took %d%% of the engine's own time; the engine updates them itself from now on.", pct); }
        else logf("pxpar: trial - on this PC spreading the particles takes %d%% of the engine's own time; it stays on.", pct);
    }
}

// ------------------------------------------------------------- what may be picked
// 0 = may be updated early; otherwise the reason (the census in aotr_fxprobe.inc counts them)
enum { PXW_OK = 0, PXW_CLASS, PXW_HANDLE, PXW_SLAVE, PXW_RENDEROBJ, PXW_MODCLASS, PXW_PHYSFOLLOW, PXW_DRIFT, PXW_ATTACHED,
       PXW_EXTRA, PXW_BACKPTR, PXW_SYSCLASS, PXW_SYSMODEL, PXW_SYSMODS, PXW_CONT, PXW_N };
static const char* const kPxWhy[PXW_N] = { "pickable", "other particle class", "handle not its own system", "a system follows it (+0x78)",
    "render object (+0x8C)", "a module class not verified", "physics follows the system or a drawable", "a drift module (sys+0x1B8) of another class",
    "drawable-attached module (+0xAC)", "extra modules (+0xB0)", "a module points to another particle", "other system class",
    "model particles (kind 2)", "a system module not verified", "other container class" };
// v75: the system modules AOTR's effects use (the v74 census: classes at +0x1AC..+0x1C8) whose update cannot touch a
// particle: `ret` (most of them); 0x9686C0 (adds to its own +0x24, clamped); 0x9669D8 (C32C48: its own +0x30/+0x34,
// client random numbers - drawn in the engine's own order, it still runs serially); 0x96BED6 (C32E28: when the
// system reaches the ground it starts an FXList (0x494615) and sets a system's +0x128 - new particles go to the end
// of their containers and are left to the engine, a particle the new emission evicts is forgotten by pxDel).
static __forceinline bool pxSysModOk(DWORD m) {
    if (!m) return true;
    DWORD u = *(DWORD*)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)m + 4);
    return u == 0x0063F3BF || u == 0x009686C0 || u == 0x009669D8 || u == 0x0096BED6;
}
// v75: the drift module at sys+0x1B8 (C32AC8, on most of AOTR's systems): no update (`ret`), slot 4 = fld [ecx+2Ch],
// slot 5 = lea eax,[ecx+20h] - the physics module reads two constants. Its bytes are in the check copies too.
#define PX_DRIFT_VT 0x00C32AC8
static int pxSysWhy(DWORD sys) {                   // the parts of the test that do not change within a frame
    if (*(DWORD*)(ULONG_PTR)sys != PX_SYS_VT) return PXW_SYSCLASS;
    DWORD c = *(DWORD*)(ULONG_PTR)(sys + 0xA4); if (!c || *(DWORD*)(ULONG_PTR)c != PX_CONT_VT) return PXW_CONT;
    if (*(DWORD*)(ULONG_PTR)(sys + 0xC) == 2) return PXW_SYSMODEL;
    for (int k = 0; k < 8; ++k) if (!pxSysModOk(*(DWORD*)(ULONG_PTR)(sys + 0x1AC + 4 * k))) return PXW_SYSMODS;
    DWORD vb = *(DWORD*)(ULONG_PTR)(sys + 0x1CC), ve = *(DWORD*)(ULONG_PTR)(sys + 0x1D0);
    if (vb != ve) return PXW_SYSMODS;               // the module vector the chain ends with (0x5F3A19)
    return PXW_OK;
}
static int pxPartWhy(DWORD p, DWORD sys) {
    if (*(DWORD*)(ULONG_PTR)p != PX_PART_VT) return PXW_CLASS;
    if (*(DWORD*)(ULONG_PTR)(p + 0x3C) != sys) return PXW_HANDLE;
    if (*(DWORD*)(ULONG_PTR)(p + 0x78)) return PXW_SLAVE;
    if (*(DWORD*)(ULONG_PTR)(p + 0x8C)) return PXW_RENDEROBJ;
    if (*(DWORD*)(ULONG_PTR)(p + 0xAC)) return PXW_ATTACHED;
    if (*(DWORD*)(ULONG_PTR)(p + 0xB0) != *(DWORD*)(ULONG_PTR)(p + 0xB4)) return PXW_EXTRA;
    static const DWORD kVt[4] = { 0x00C32A64, 0x00C329B0, 0x00C32BD0, 0x00C32B18 };
    for (int s = 0; s < 4; ++s) {
        DWORD m = *(DWORD*)(ULONG_PTR)(p + 0x94 + 4 * s); if (!m) continue;
        if (*(DWORD*)(ULONG_PTR)m != kVt[s]) return PXW_MODCLASS;
        if (*(DWORD*)(ULONG_PTR)(m + 4) != p) return PXW_BACKPTR;
    }
    DWORD ph = *(DWORD*)(ULONG_PTR)(p + 0xA0);
    if (ph) {
        if (*(BYTE*)(ULONG_PTR)(ph + 0x14) || *(BYTE*)(ULONG_PTR)(ph + 0x15)) return PXW_PHYSFOLLOW;
        DWORD dr = *(DWORD*)(ULONG_PTR)(sys + 0x1B8);
        if (dr && *(DWORD*)(ULONG_PTR)dr != PX_DRIFT_VT) return PXW_DRIFT;
    }
    return PXW_OK;
}

// ------------------------------------------------------------- the kept results (game thread only)
static __forceinline DWORD pxHash(DWORD p) { return ((p >> 3) * 2654435761u) >> 16; }
static PxEnt* pxFind(DWORD p) {
    for (DWORD h = pxHash(p), k = 0; k < PX_TABN; ++k, h = (h + 1) & (PX_TABN - 1)) {
        PxEnt& e = g_pxTab[h];
        if (e.stamp != g_pxStamp) return NULL;
        if (e.p == p) return &e;
    }
    return NULL;
}
static int pxPut(DWORD p, DWORD v) {
    for (DWORD h = pxHash(p), k = 0; k < PX_TABN; ++k, h = (h + 1) & (PX_TABN - 1)) {
        PxEnt& e = g_pxTab[h];
        if (e.stamp != g_pxStamp) { e.p = p; e.stamp = g_pxStamp; e.v = v; return (int)h; }
    }
    return -1;
}
// check copies: the particle (list links left out - they change when neighbours come and go) and its four modules
static const int kPxModSize[4] = { 0xB0, 0x5C, 0x38, 0x18 };
#define PX_SNAP (0xC0 + 0xB0 + 0x5C + 0x38 + 0x18 + 0x30)
static BYTE g_pxSnap[PX_CHECKS][PX_SNAP]; static DWORD g_pxSnapP[PX_CHECKS]; static DWORD g_pxPick = 0;
static void pxCopy(DWORD p, BYTE* out) {
    memcpy(out, (void*)(ULONG_PTR)p, 0xC0);
    memset(out + 0x40, 0, 8); memset(out + 0x64, 0, 0x10); memset(out + 0x7C, 0, 8);   // handle +0x3C links, container +0x64/+0x68, priority +0x6C/+0x70, handle +0x78 links
    int o = 0xC0;
    for (int s = 0; s < 4; ++s) {
        DWORD m = *(DWORD*)(ULONG_PTR)(p + 0x94 + 4 * s);
        if (m) memcpy(out + o, (void*)(ULONG_PTR)m, kPxModSize[s]); else memset(out + o, 0, kPxModSize[s]);
        o += kPxModSize[s];
    }
    DWORD sys = *(DWORD*)(ULONG_PTR)(p + 0x3C), dr = sys ? *(DWORD*)(ULONG_PTR)(sys + 0x1B8) : 0;   // the drift module the physics reads
    if (dr) memcpy(out + o, (void*)(ULONG_PTR)dr, 0x30); else memset(out + o, 0, 0x30);
}
static void pxOff(const char* why) {
    if (!InterlockedExchange(&g_pxOn, 0)) return;
    InterlockedExchange(&g_pxTrial, 0);              // nothing more to time
    // g_pxHas stays: the results already kept this frame are still used - their particles were updated already and
    // must not be updated a second time. The next loop start clears it and, with g_pxOn 0, keeps nothing more.
    lstrcpynA(g_pxOffWhy, why, sizeof(g_pxOffWhy));
    logf("pxpar: SELF-CHECK - %s; the particles are updated by the engine alone for the rest of this session.", why);
}
static __forceinline int pxTake(DWORD p) {          // -1: the engine updates it now; 0 / 1: the kept result
    PxEnt* e = pxFind(p);
    if (!e) return -1;
    DWORD v = e->v; e->p = 1;                       // used: a tombstone, so the probe chain stays whole
    if (v < 2) return (int)v;
    BYTE now[PX_SNAP]; int bad = 0;
    __try { pxCopy(p, now); bad = memcmp(now, g_pxSnap[v - 2], PX_SNAP) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { bad = 1; }
    InterlockedIncrement(&g_pxChecked);
    if (bad) { InterlockedIncrement(&g_pxCheckBad); pxOff("a particle changed between the manager loop's start and its own update"); }
    return -1;
}
typedef char  (__fastcall* tPxUpd)(DWORD, DWORD);
typedef DWORD (__fastcall* tPxDel)(DWORD, DWORD, int);
typedef DWORD (__fastcall* tPxFirst)(void*, void*);
typedef void  (__fastcall* tPxCont)(void*, void*);
static DWORD __fastcall pxDel(DWORD p, DWORD edx, int flags) {   // vtable 0xBF7B70 slot 0
    if (g_pxHas) { PxEnt* e = pxFind(p); if (e) e->p = 1; }
    return ((tPxDel)(ULONG_PTR)PX_PART_DEL)(p, edx, flags);
}
static void pxContRun(void* cont, void* edx);
static void __fastcall pxCont(void* cont, void* edx) {           // vtable 0xC33AE8 slot 0x28: 0x7B09A1, with the kept results
    if (!g_pxTrial) { pxContRun(cont, edx); return; }
    ULONG64 t = __rdtsc(); pxContRun(cont, edx); g_pxTrialAcc += __rdtsc() - t;   // the trial's timing (game thread)
}
static void pxContRun(void* cont, void* edx) {
    if (!g_pxHas) { ((tPxCont)(ULONG_PTR)PX_CONT_UPD)(cont, edx); return; }
    DWORD p = ((tPxFirst)(ULONG_PTR)(*(DWORD*)(ULONG_PTR)(*(DWORD*)cont + 0x20)))(cont, 0);
    while (p) {
        DWORD next = *(DWORD*)(ULONG_PTR)(p + 0x64);
        int r = pxTake(p);
        char alive = r >= 0 ? (char)r : ((tPxUpd)(ULONG_PTR)PX_PUPD)(p, 0);
        if (!alive) {
            DWORD o = ((tPxDel)(ULONG_PTR)(**(DWORD**)(ULONG_PTR)p))(p, 0, 0);
            ((void (__cdecl*)(DWORD))(ULONG_PTR)PX_FREE)(o);
        }
        p = next;
    }
}

// ------------------------------------------------------------- the helpers
static void pxWork() {
    LONG c;
    while ((c = InterlockedIncrement(&g_pxNext) - 1) < g_pxNChunks) {
        for (int i = g_pxSys[c], e = g_pxSys[c + 1]; i < e; ++i) {
            __try { g_pxAlive[i] = ((tPxUpd)(ULONG_PTR)PX_PUPD)(g_pxParts[i], 0) ? 1 : 0; }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_pxAlive[i] = 1; InterlockedIncrement(&g_pxFault); }
        }
    }
}
static DWORD WINAPI pxHelper(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_pxSem, INFINITE);
        InterlockedIncrement(&g_pxInside);
        if (g_pxOpen) {
            unsigned short cw = g_pxCw; __asm fldcw cw
            _mm_setcsr(g_pxCsr);
            pxWork();
        }
        InterlockedDecrement(&g_pxInside);
    }
}

// ------------------------------------------------------------- the loop's start (game thread)
static void pxAudit() {                              // the previous frame's results that were never used
    if (!g_pxPrevN) return;
    DWORD prev = g_pxStamp; int un = 0;
    for (int i = 0; i < g_pxPrevN; ++i) { PxEnt& e = g_pxTab[g_pxSlot[i]]; if (e.stamp == prev && e.p > 1 && e.v < 2) ++un; }
    if (un) {
        g_pxUnused += un;
        if (un > 64) pxOff("more than 64 particles updated early were then not updated by their system");
        else if (g_pxDone > 20000 && g_pxUnused * 1000 > g_pxDone) pxOff("over 1 in 1000 particles updated early were then not updated by their system");
    }
    g_pxPrevN = 0;
}
static void pxSpread(DWORD mgr, DWORD flag);
static void __stdcall pxPrepass(DWORD mgr, DWORD flag) {
    DWORD gt = g_mkTid ? g_mkTid : g_frTidEarly;
    if (!gt || __readfsdword(0x24) != gt) return;
    if (g_pxHas) { pxAudit(); InterlockedExchange(&g_pxHas, 0); } else g_pxPrevN = 0;
    if (!g_pxOn || g_pxHold || g_pxAutoOff) return;
    if (g_pxTrial) {
        pxTrialCommit();
        if (g_pxAutoOff || !g_pxTrial) { if (g_pxAutoOff) return; }
        else {
            g_pxTrialMode = (LONG)((g_frW >> 6) & 1);
            __try { g_pxTrialMgrN = *(int*)(ULONG_PTR)(mgr + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) { g_pxTrialMgrN = 0; }
            g_pxTrialCounts = g_pxTrialMgrN >= PX_MIN;
            if (g_pxTrialMode == 1) return;                 // this block: the engine alone (its containers are timed in pxCont)
        }
    }
    if (!g_pxTrial) { pxSpread(mgr, flag); return; }
    ULONG64 t = __rdtsc(); pxSpread(mgr, flag); g_pxTrialAcc += __rdtsc() - t;
}
static void pxSpread(DWORD mgr, DWORD flag) {             // the pick and the spread (the loop start's real work)
    if (++g_pxStamp == 0) { memset(g_pxTab, 0, PX_TABN * sizeof(PxEnt)); g_pxStamp = 1; }
    g_pxFrames++;
    if (g_pxFault) { pxOff("an exception on a helper thread"); return; }
    ULONG64 t0 = __rdtsc();
    int n = 0, nc = 0, nSnap = 0;
    bool check = (g_pxFrames & 15) == 0;
    __try {
        DWORD gd = *(DWORD*)(ULONG_PTR)0x00DE4364;
        if (!gd || !*(BYTE*)(ULONG_PTR)(gd + 0x9AF)) return;          // the system update returns before its particles (0x5FA309)
        if (*(int*)(ULONG_PTR)(mgr + 0x50) < PX_MIN) return;          // the manager's particle count
        DWORD head = *(DWORD*)(ULONG_PTR)(mgr + 0x4C); if (!head) return;
        g_pxSys[0] = 0;
        int guard = 0;
        for (DWORD node = *(DWORD*)(ULONG_PTR)head; node != head && node && ++guard < 200000; node = *(DWORD*)(ULONG_PTR)node) {
            DWORD sys = *(DWORD*)(ULONG_PTR)(node + 8);
            if (!sys) continue;
            if (!*(DWORD*)(ULONG_PTR)(sys + 0x98) && !flag) continue;    // the loop's own skip (0x5F51D9)
            if (*(DWORD*)(ULONG_PTR)(sys + 0x120)) continue;             // delayed: no particle update this frame (0x5FA319)
            if (pxSysWhy(sys) != PXW_OK) continue;
            DWORD cont = *(DWORD*)(ULONG_PTR)(sys + 0xA4);
            int k = 0;
            for (DWORD p = *(DWORD*)(ULONG_PTR)(cont + 0x14); p && k < 20000; p = *(DWORD*)(ULONG_PTR)(p + 0x64), ++k) {
                if (pxPartWhy(p, sys) != PXW_OK) continue;
                if (check && nSnap < PX_CHECKS && (++g_pxPick % 29) == 0) { pxCopy(p, g_pxSnap[nSnap]); g_pxSnapP[nSnap++] = p; continue; }
                if (n >= PX_MAXP) break;
                g_pxParts[n++] = p;
            }
            if (n - g_pxSys[nc] >= 48 && nc < PX_MAXP / 2 - 1) g_pxSys[++nc] = n;   // a chunk ends with a whole system
            if (n >= PX_MAXP) break;
        }
        if (g_pxSys[nc] != n) g_pxSys[++nc] = n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { pxOff("an exception while picking particles"); return; }
    if (n < PX_MIN) { g_pxTicksPick += (LONG64)(__rdtsc() - t0); return; }
    ULONG64 t1 = __rdtsc();
    { unsigned short cw; __asm fnstcw cw
      g_pxCw = cw; g_pxCsr = _mm_getcsr(); }
    g_pxNext = 0; g_pxNChunks = nc;
    LONG wake = g_pxHelpers < nc - 1 ? g_pxHelpers : nc - 1;
    if (wake > 0) { InterlockedExchange(&g_pxOpen, 1); ReleaseSemaphore(g_pxSem, wake, NULL); }
    pxWork();
    if (wake > 0) { InterlockedExchange(&g_pxOpen, 0); while (g_pxInside) _mm_pause(); }
    if (g_pxFault) { pxOff("an exception on a helper thread"); }
    g_pxTicksPar += (LONG64)(__rdtsc() - t1);
    // keep the results; the check copies' particles are marked too, so their update compares them
    int m = 0;
    for (int i = 0; i < n; ++i) { int s = pxPut(g_pxParts[i], g_pxAlive[i]); if (s >= 0) g_pxSlot[m++] = s; }
    for (int k = 0; k < nSnap; ++k) pxPut(g_pxSnapP[k], 2 + k);     // the engine updates these; pxTake compares them first
    g_pxN = m; g_pxPrevN = m;
    g_pxDone += n; g_pxFramesPar++;
    g_pxTicksPick += (LONG64)(t1 - t0);
    InterlockedExchange(&g_pxHas, 1);
}

// ------------------------------------------------------------- install
static __declspec(naked) void pxLoopStub() {        // replaces 0x5F51BC: mov eax,[edi+4Ch] / push esi / mov esi,[eax]
    __asm {
        pushad
        movzx eax, byte ptr [ebp - 0Dh]             // the loop's flag (0x5F51B3 / 0x5F51B9)
        push eax
        push edi                                    // the manager
        call pxPrepass
        popad
        mov  eax, dword ptr [edi + 4Ch]
        push esi
        mov  esi, dword ptr [eax]
        push PX_LOOP_NEXT
        ret
    }
}
// The code the argument above was read from: every function of Particle::update's call tree, the container update,
// the manager loop's skip test, the system update's early returns and its last steps, the deleting destructor.
static const DWORD kPxCode[][2] = {
    {0x5FA0E6,0x5FA202}, {0x7B09A1,0x7B09D4}, {0x5F449F,0x5F44F4}, {0x5F9F0A,0x5F9F43}, {0x5F9C3D,0x5F9C76}, {0x5F8BAC,0x5F8BE5},
    {0x5F8559,0x5F8592}, {0x5F54F7,0x5F5538}, {0x5F39DB,0x5F39F5}, {0x96AAD2,0x96AC69}, {0x96A0F9,0x96A218}, {0x969A94,0x969B39},
    {0x96AF99,0x96AFF4}, {0x5F2D45,0x5F2D53}, {0x96A26A,0x96A314}, {0x9699F3,0x969A29}, {0x969978,0x9699B5}, {0x96A079,0x96A0F9},
    {0x96B86A,0x96B88A}, {0x44C4BE,0x44C4E2}, {0x44C465,0x44C49C}, {0x44C43F,0x44C465}, {0x5F47D0,0x5F47F2}, {0x5F47F2,0x5F4804},
    {0x4032DD,0x403312}, {0x42F4F0,0x42F4FA}, {0x5F2EEE,0x5F2F2D}, {0x5F332E,0x5F3397}, {0x9F9F37,0x9F9F3B}, {0x5F5198,0x5F51E6},
    {0x5FA2CE,0x5FA342}, {0x5FA91B,0x5FA92D}, {0x5FBACA,0x5FBAE6},
    {0x9669D8,0x966BBD}, {0x96BED6,0x96BF66}, {0x49A4C5,0x49A4C9}, {0x7ACC91,0x7ACC95}, {0x9686C0,0x9686E2}, {0x5F3A19,0x5F3A39} };
#define PX_CODE_FNV 0x2D4533CFu
static bool pxPatchPtr(DWORD at, DWORD expect, void* now) {
    DWORD* slot = (DWORD*)(ULONG_PTR)at; DWORD op = 0;
    if (*slot != expect || !VirtualProtect(slot, 4, PAGE_READWRITE, &op)) return false;
    *slot = (DWORD)(ULONG_PTR)now; VirtualProtect(slot, 4, op, &op);
    return true;
}
static void installPxPar() {
    char mk[MAX_PATH];
    if (!g_engineHooks) return;
    // v82.1: OFF by default. The v82 test's trial (a 6-core PC, a 26-minute battle at full graphics): spreading took 139%
    // of the engine's own time - the pick walk and the hand-over cost more than the ~2 us a particle saves at this
    // battle's particle counts. PXPAR_ON next to the DLL installs it (its trial then still decides per PC).
    if (GetFileAttributesA(aotrPath(mk, "PXPAR_ON")) == INVALID_FILE_ATTRIBUTES) { logf("pxpar: off (v82.1 default - measured slower than the engine in the v82 test; PXPAR_ON turns it on)."); return; }
    __try {
        DWORD h = 0x811C9DC5u;
        for (unsigned r = 0; r < sizeof(kPxCode) / sizeof(kPxCode[0]); ++r)
            for (const BYTE* b = (const BYTE*)(ULONG_PTR)kPxCode[r][0]; b < (const BYTE*)(ULONG_PTR)kPxCode[r][1]; ++b) h = (h ^ *b) * 0x01000193u;
        bool vt = *(DWORD*)(ULONG_PTR)(PX_SYS_VT + 0x10) == 0x005FA2CE && *(DWORD*)(ULONG_PTR)(PX_CONT_VT + 0x28) == PX_CONT_UPD &&
                  *(DWORD*)(ULONG_PTR)(PX_CONT_VT + 0x20) == PX_CONT_FIRST && *(DWORD*)(ULONG_PTR)PX_PART_VT == PX_PART_DEL &&
                  *(DWORD*)(ULONG_PTR)(0x00C32A64 + 4) == 0x0096A0F9 && *(DWORD*)(ULONG_PTR)(0x00C32A64 + 0x10) == 0x0096A26A &&
                  *(DWORD*)(ULONG_PTR)(0x00C329B0 + 4) == 0x00969A94 && *(DWORD*)(ULONG_PTR)(0x00C329B0 + 0x10) == 0x009699F3 &&
                  *(DWORD*)(ULONG_PTR)(0x00C32BD0 + 4) == 0x0096AF99 && *(DWORD*)(ULONG_PTR)(0x00C32BD0 + 0x24) == 0x005F2D45 &&
                  *(DWORD*)(ULONG_PTR)(0x00C32B18 + 4) == 0x0096AAD2 &&
                  *(DWORD*)(ULONG_PTR)(PX_DRIFT_VT + 4) == 0x0063F3BF && *(DWORD*)(ULONG_PTR)(PX_DRIFT_VT + 0x10) == 0x0049A4C5 &&
                  *(DWORD*)(ULONG_PTR)(PX_DRIFT_VT + 0x14) == 0x007ACC91 &&
                  !memcmp((void*)(ULONG_PTR)0x0063F3BF, "\xC3", 1) && !memcmp((void*)(ULONG_PTR)0x009686C0, "\xF3\x0F\x10\x41\x28\xF3\x0F\x58\x41\x24", 10) &&
                  !memcmp((void*)(ULONG_PTR)PX_LOOP, "\x8B\x47\x4C\x56\x8B\x30\x3B\xF0", 8);
        if (h != PX_CODE_FNV || !vt) { logf("pxpar: the particle update code differs from the analysed build (%08X) - not installed.", h); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    int want = tpAutoHelpers(); if (want > 6) want = 6;
    // v79: on two physical cores the one helper would share its core with the render worker and DXVK, and the game
    // thread waits for the slowest helper - there it could cost more than it saves. Three cores or more only.
    if (want < 2) { logf("pxpar: not installed - %d physical core(s): no spare core for the particles.", want + 1); return; }
    g_pxTab = (PxEnt*)VirtualAlloc(NULL, PX_TABN * sizeof(PxEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_pxParts = (DWORD*)VirtualAlloc(NULL, PX_MAXP * (sizeof(DWORD) + sizeof(int) + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_pxSem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    if (!g_pxTab || !g_pxParts || !g_pxSem) { logf("pxpar: no memory - not installed."); return; }
    g_pxSlot = (int*)(g_pxParts + PX_MAXP); g_pxAlive = (BYTE*)(g_pxSlot + PX_MAXP);
    for (int i = 0; i < want; ++i) {
        HANDLE t = CreateThread(NULL, 256 * 1024, pxHelper, NULL, 0, NULL);
        if (t) { SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(t); ++g_pxHelpers; }
    }
    if (!g_pxHelpers) { logf("pxpar: no helper thread - not installed."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    bool a = pxPatchPtr(PX_CONT_VT + 0x28, PX_CONT_UPD, (void*)&pxCont);
    bool b = a && pxPatchPtr(PX_PART_VT, PX_PART_DEL, (void*)&pxDel);
    if (a && b) patchJmp((BYTE*)PX_LOOP, (void*)&pxLoopStub, 6);
    else if (a) pxPatchPtr(PX_CONT_VT + 0x28, (DWORD)(ULONG_PTR)&pxCont, (void*)(ULONG_PTR)PX_CONT_UPD);
    resumeAll(fz, nf);
    if (!(a && b)) { logf("pxpar: the particle vtables could not be changed - not installed."); return; }
    InterlockedExchange(&g_pxOn, 1);
    logf("pxpar: the particles' own update runs on %d helper threads and the game thread when a frame has %d or more that can be updated early (exact; self-checking; PXPAR_OFF).",
         g_pxHelpers, PX_MIN);
}
static void pxReport() {                             // diagnostics: the 60 s report
    static LONG64 lD = 0, lU = 0, lTp = 0, lTk = 0; static LONG lF = 0, lFp = 0, lC = 0;
    LONG64 d = g_pxDone - lD, u = g_pxUnused - lU, tp = g_pxTicksPar - lTp, tk = g_pxTicksPick - lTk; LONG f = g_pxFrames - lF, fp = g_pxFramesPar - lFp, c = g_pxChecked - lC;
    lD = g_pxDone; lU = g_pxUnused; lTp = g_pxTicksPar; lTk = g_pxTicksPick; lF = g_pxFrames; lFp = g_pxFramesPar; lC = g_pxChecked;
    if (!f) return;
    double tpm = g_tscPerQpc > 0 ? g_tscPerQpc * (double)g_pqpf.QuadPart / 1000.0 : 0;
    logf("pxpar: last 30 s - %s; %d of %d frames spread their particles (%d particles a spread frame); picking %d us and the spread %d us a frame; %d particles checked against their copy (%d different in all), %d results not used",
         g_pxOn ? "on" : g_pxOffWhy, (int)fp, (int)f, fp ? (int)(d / fp) : 0, tpm > 0 ? (int)(tk / tpm * 1000.0 / f) : 0, tpm > 0 && fp ? (int)(tp / tpm * 1000.0 / fp) : 0,
         (int)c, (int)g_pxCheckBad, (int)u);
}
