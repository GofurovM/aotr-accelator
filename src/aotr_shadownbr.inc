// ================================================================ shadow mesh edge neighbours, exact and fast (v32, 2026-10-21)
// What the test-12 log measured (accel-test-12\bfme2_accel.log, the long-frame profiler, 18:22:34 - 18:28:47): of the
// 17.1 s the game thread spent in frames of 80 ms or more after the map had loaded, 13.6 s (80 %) were in two game.dat
// functions, always on the same path:
//
//     load:   path 65.0%  game.dat+0F2252 < game.dat+0F25EB < game.dat+0F2614 < game.dat+0F28E9
//     load:   path 29.2%  game.dat+0F08BC < game.dat+0F2252 < game.dat+0F25EB < game.dat+0F2614
//
// 0x4F2252 is the stencil-shadow mesh's buildPolygonNeighbors: the first time a model casts its volume shadow, the
// shadow code asks for a triangle's neighbours (0x4F25EB: "if the table is not there, build it"), and the build
// compares EVERY triangle with EVERY other one - n x n pairs, each through 0x4F08BC (the triangle's three welded vertex
// numbers) and nine 16-bit compares. A 3000-triangle model is nine million pairs: the 1-2 s freezes of test-12 when a
// new kind of unit or building came into view. It is drawing-side code (the W3D shadow volume), not the simulation:
// nothing in it is read by the logic, and it never runs in the game's logic step.
//
// The table it makes, per triangle i (22 bytes at [this+0x24] + i*22):
//     +0  WORD  i
//     +4  three slots { WORD neighbour j, WORD shared vertex 1, WORD shared vertex 2 }, empty = j 0xFFFF
// filled, for j = 0, 1, 2 ... n-1 in that order, with each j whose triangle shares an edge with i walked the other way
// round and does not lie back to back with it (|dot(normal i, normal j) + 1| > 0.01, computed on the x87); the first
// three such j win. The bytes +2/+3 and the vertex words of slots that stay empty are not written.
//
// This replacement writes the same bytes, found a different way: a triangle can only share an edge with triangles that
// share a vertex with it, so each vertex number gets the list of triangles that use it (a counting sort), and for each i
// only those triangles are tried - still in increasing j, through the very same per-pair test (the same compares in the
// same order, the same sign-extended 16-bit values, the same orientation formula with abs, and the facing test as the
// identical x87 instruction sequence under the game thread's own FPU control word). Pairs that share no vertex can not
// produce a neighbour in the original either (every one of its branches needs a shared vertex), so nothing is lost.
// Each triangle's entry depends only on read-only data, so large models are split over the worker cores.
// n x n becomes n x (a dozen): a 3000-triangle model goes from about a second to well under a millisecond.
//
// Exactness: (1) shadownbr_test (game.dat mapped at its base in a child process) runs the engine's own 0x4F2252 and this
// one over thousands of made-up meshes - closed grids, triangle soups, fans, doubled and degenerate triangles, back-to-
// back faces, 0xFFFF and negative vertex numbers, every x87 precision, one to eight thousand triangles, one thread and
// many - and compares every byte; (2) in the game the first 48 small models are built a second time by the engine's own
// code and compared (any difference: the engine's code for the rest of the session); (3) the code it mirrors is hashed
// at start - a different game.dat leaves the engine's own code in charge.

#include <float.h>
#include <math.h>
#define SNB_BUILD   0x004F2252
#define SNB_NORMALS 0x004F21B2
#define SNB_GETIDX  0x004F08BC
#define SNB_ALLOC   0x004EF711
#define SNB_FREE    0x004EF73A
#define SNB_ONE     0x00BD1908      // float 1.0  (fadd in the facing test)
#define SNB_EPS     0x00BE5600      // float 0.01 (the facing test's limit)
#define SNB_ENTRY   22
#define SNB_MAXHELP 15

typedef void (__fastcall* tSnbThis)(BYTE* self, void* edx);
typedef char (__fastcall* tSnbAlloc)(BYTE* self, void* edx, int n);
typedef void (__fastcall* tSnbGetIdx)(BYTE* self, void* edx, int i, short* out);
static tSnbThis   o_snbBuild = NULL;                 // trampoline: the engine's own builder
static tSnbThis   p_snbNormals = (tSnbThis)SNB_NORMALS, p_snbFree = (tSnbThis)SNB_FREE;
static tSnbAlloc  p_snbAlloc = (tSnbAlloc)SNB_ALLOC;
static tSnbGetIdx p_snbGetIdx = (tSnbGetIdx)SNB_GETIDX;

static volatile LONG g_snbOn = 0, g_snbBusy = 0, g_snbChecksLeft = 48, g_snbChecked = 0, g_snbBad = 0, g_snbFaults = 0, g_snbStock = 0;
static volatile LONG g_snbN = 0, g_snbPar = 0, g_snbMaxPolys = 0;
static volatile LONG64 g_snbPolys = 0, g_snbPairsSaved = 0, g_snbTicks = 0, g_snbMaxTicks = 0, g_snbCheckMine = 0, g_snbCheckEngine = 0;
static LONG g_snbParMin = 1500;                       // triangles from which a build is split over the workers

// ---- the per-build data: read-only while the entries are filled
struct SnbJob {
    BYTE* arr; int n;
    const short* P;            // 3 welded vertex numbers per triangle (as 0x4F08BC returns them)
    const float* N;            // the triangle normals [this+0x10]
    const int* start;          // counting sort: triangles of vertex value v are list[start[v-minV] .. start[v-minV+1])
    const int* list;
    int minV;
    unsigned short cw;         // the game thread's x87 control word
    volatile LONG next, done, nChunks, chunk, open, inJob, fault;
};
static SnbJob g_snbJob;

// The facing test, instruction for instruction as 0x4F2388..0x4F23C1 does it (operands, order, the double round trip
// through memory before fabs), so that every x87 precision rounds it the same way. True = the neighbour is taken.
static const float* g_snbOne = (const float*)SNB_ONE;
static const float* g_snbEps = (const float*)SNB_EPS;
static __declspec(noinline) int snbFacing(const float* nJ, const float* nI) {
    int r;
    const float* one = g_snbOne; const float* eps = g_snbEps;
    __asm {
        mov eax, nJ
        mov ecx, nI
        mov edx, one
        fld dword ptr [eax+8]
        fmul dword ptr [ecx+8]
        fld dword ptr [eax+4]
        fmul dword ptr [ecx+4]
        faddp st(1), st
        fld dword ptr [ecx]
        fmul dword ptr [eax]
        faddp st(1), st
        fadd dword ptr [edx]
        sub esp, 8
        fstp qword ptr [esp]
        fld qword ptr [esp]
        fabs
        add esp, 8
        mov edx, eps
        fld dword ptr [edx]
        fcomip st, st(1)
        fstp st(0)
        setb al
        movzx eax, al
        mov r, eax
    }
    return r;
}

#ifdef AOTR_SELFTEST
static volatile LONG g_snbTFacingYes = 0, g_snbTFacingNo = 0, g_snbTFull = 0;
#endif
// One (i, j) pair, the original's two nested loops over the three vertices of each (0x4F232C..0x4F23ED).
static __forceinline bool snbPair(const short* pi, const short* pj, const float* nI, const float* nJ, int* o1, int* o2) {
    int idx1 = -1, idx2 = -1, aP = 0, bP = 0;
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) {
            if ((WORD)pi[a] != (WORD)pj[b]) continue;
            if (idx1 == -1) { idx1 = (int)pi[a]; aP = a; bP = b; }
            else if (idx2 == -1) {
                int da = a - aP, db = b - bP;
                unsigned x = (unsigned)(abs(db) ^ abs(da));
                x <<= 30; x ^= (unsigned)db; x ^= (unsigned)da;
                if ((int)x < 0) {
#ifdef AOTR_SELFTEST
                    int fz = snbFacing(nJ, nI); if (fz) g_snbTFacingYes++; else g_snbTFacingNo++;
                    if (fz) idx2 = (int)pi[a];
#else
                    if (snbFacing(nJ, nI)) idx2 = (int)pi[a];
#endif
                }
            } else { idx1 = -1; idx2 = -1; }
        }
    *o1 = idx1; *o2 = idx2;
    return idx1 != -1 && idx2 != -1;
}

// Entries lo..hi-1: the initial values (0x4F2299..0x4F22C5) and the neighbours (the outer loop), each entry on its own.
static void snbFillRange(const SnbJob* J, int lo, int hi, int* cand, int candCap) {
    for (int i = lo; i < hi; ++i) {
        BYTE* e = J->arr + i * SNB_ENTRY;
        *(WORD*)e = (WORD)i;
        *(WORD*)(e + 4) = 0xFFFF; *(WORD*)(e + 10) = 0xFFFF; *(WORD*)(e + 16) = 0xFFFF;
        const short* pi = J->P + i * 3;
        // the triangles that use any of i's vertex values, merged in increasing order without repeats
        const int* s[3]; const int* t[3]; int ns = 0;
        for (int k = 0; k < 3; ++k) {
            int v = (int)(WORD)pi[k];
            bool dup = false;
            for (int q = 0; q < k; ++q) if ((WORD)pi[q] == (WORD)pi[k]) dup = true;
            if (dup) continue;
            s[ns] = J->list + J->start[v - J->minV]; t[ns] = J->list + J->start[v - J->minV + 1]; ++ns;
        }
        int m = 0;
        for (;;) {
            int best = 0x7FFFFFFF;
            for (int k = 0; k < ns; ++k) if (s[k] < t[k] && *s[k] < best) best = *s[k];
            if (best == 0x7FFFFFFF) break;
            for (int k = 0; k < ns; ++k) while (s[k] < t[k] && *s[k] == best) ++s[k];
            if (best != i && m < candCap) cand[m++] = best;
        }
        const float* nI = J->N + i * 3;
#ifdef SNB_MUTATE
        for (int lo2 = 0, hi2 = m - 1; lo2 < hi2; ++lo2, --hi2) { int tq = cand[lo2]; cand[lo2] = cand[hi2]; cand[hi2] = tq; }   // test only: the wrong order
#endif
        for (int c = 0; c < m; ++c) {
            WORD* slot = NULL;
            for (int k = 0; k < 3; ++k) if (*(WORD*)(e + 4 + k * 6) == 0xFFFF) { slot = (WORD*)(e + 4 + k * 6); break; }
            if (!slot) {
#ifdef AOTR_SELFTEST
                g_snbTFull++;
#endif
                break;                                                   // all three taken: nothing more can be written
            }
            int j = cand[c], i1, i2;
            if (snbPair(pi, J->P + j * 3, nI, J->N + j * 3, &i1, &i2)) { slot[0] = (WORD)j; slot[1] = (WORD)i1; slot[2] = (WORD)i2; }
        }
    }
}

// ---- grow-only buffers (one build at a time: g_snbBusy)
static short* g_snbP = NULL; static int g_snbPCap = 0;
static int* g_snbStart = NULL; static int g_snbStartCap = 0;
static int* g_snbList = NULL; static int g_snbListCap = 0;
static int* g_snbCand[SNB_MAXHELP + 1]; static int g_snbCandCap[SNB_MAXHELP + 1];
static BYTE* g_snbCopy = NULL; static int g_snbCopyCap = 0;
static int g_snbMaxBucket = 0;

static void* snbGrow(void* p, int* cap, int need, int elem) {
    if (p && *cap >= need) return p;
    int c = need < 1024 ? 1024 : need + need / 2;
    void* q = VirtualAlloc(NULL, (SIZE_T)c * elem, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!q) return NULL;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    *cap = c;
    return q;
}
static int* snbCandFor(int w) {
    int need = g_snbMaxBucket * 3 + 4;
    int* p = (int*)snbGrow(g_snbCand[w], &g_snbCandCap[w], need, sizeof(int));
    g_snbCand[w] = p;
    return p;
}

// ---- workers
static HANDLE g_snbSem = NULL;
static LONG g_snbHelpers = 0;
static void snbChunks(int w) {
    SnbJob* J = &g_snbJob;
    int* cand = snbCandFor(w);
    if (!cand) { InterlockedExchange(&J->fault, 1); }
    for (;;) {
        LONG c = InterlockedIncrement(&J->next) - 1;
        if (c >= J->nChunks) break;
        int lo = c * J->chunk, hi = lo + J->chunk; if (hi > J->n) hi = J->n;
        if (cand) {
            __try { snbFillRange(J, lo, hi, cand, g_snbMaxBucket * 3 + 4); }
            __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&J->fault, 1); }
        }
        InterlockedIncrement(&J->done);
    }
}
static DWORD WINAPI snbHelper(LPVOID arg) {
    int w = (int)(ULONG_PTR)arg;
    for (;;) {
        WaitForSingleObject(g_snbSem, INFINITE);
        InterlockedIncrement(&g_snbJob.inJob);
        if (g_snbJob.open) {
            unsigned short mine, cw = g_snbJob.cw;
            __asm fnstcw mine
            __asm fldcw cw
            snbChunks(w);
            __asm fldcw mine
        }
        InterlockedDecrement(&g_snbJob.inJob);
    }
}
static void snbStartHelpers(int helpers) {
    if (g_snbSem || helpers <= 0) return;
    if (helpers > SNB_MAXHELP) helpers = SNB_MAXHELP;
    g_snbSem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    if (!g_snbSem) return;
    for (int i = 0; i < helpers; ++i) {
        HANDLE h = CreateThread(NULL, 64 * 1024, snbHelper, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
        if (h) { CloseHandle(h); ++g_snbHelpers; }
    }
}

// The whole fill of an already allocated table (what 0x4F2252 does after its first lines). 0 = done, -1 = could not
// (out of memory / fault: the caller lets the engine's own code do it - it rewrites every entry).
static int snbFill(BYTE* self, BYTE* arr, int n, bool mayParallel, bool* usedPar) {
    *usedPar = false;
    const float* N = *(const float**)(self + 0x10);
    if (!arr || !N || n <= 0) return -1;
    short* P = (short*)snbGrow(g_snbP, &g_snbPCap, n * 3 + 3, sizeof(short)); if (!P) return -1; g_snbP = P;
    for (int i = 0; i < n; ++i) p_snbGetIdx(self, NULL, i, P + i * 3);
    int lo = 0xFFFF, hi = 0;
    for (int k = 0; k < n * 3; ++k) { int v = (WORD)P[k]; if (v < lo) lo = v; if (v > hi) hi = v; }
    int R = hi - lo + 1;
    int* start = (int*)snbGrow(g_snbStart, &g_snbStartCap, R + 2, sizeof(int)); if (!start) return -1; g_snbStart = start;
    int* list = (int*)snbGrow(g_snbList, &g_snbListCap, n * 3 + 1, sizeof(int)); if (!list) return -1; g_snbList = list;
    memset(start, 0, (R + 2) * sizeof(int));
    for (int k = 0; k < n * 3; ++k) start[(WORD)P[k] - lo + 1]++;
    int mb = 0;
    for (int v = 1; v <= R; ++v) { if (start[v] > mb) mb = start[v]; start[v] += start[v - 1]; }
    g_snbMaxBucket = mb;
    {   // place each triangle under its vertex values, in increasing triangle order (a stable counting sort)
        int* pos = start + 0;  // reuse: fill with a running cursor per value, then shift back
        for (int k = 0; k < n * 3; ++k) list[pos[(WORD)P[k] - lo]++] = k / 3;
        for (int v = R; v > 0; --v) start[v] = start[v - 1];
        start[0] = 0;
    }
    SnbJob* J = &g_snbJob;
    J->arr = arr; J->n = n; J->P = P; J->N = N; J->start = start; J->list = list; J->minV = lo;
    unsigned short cw; __asm fnstcw cw
    J->cw = cw;
    J->fault = 0;
    int* cand0 = snbCandFor(0); if (!cand0) return -1;
    if (mayParallel && g_snbHelpers > 0 && n >= g_snbParMin) {
        J->chunk = 256; J->nChunks = (n + 255) / 256; J->done = 0;
        for (int w = 1; w <= g_snbHelpers; ++w) if (!snbCandFor(w)) return -1;
        InterlockedExchange(&J->next, 0);
        InterlockedExchange(&J->open, 1);
        LONG wake = g_snbHelpers < J->nChunks - 1 ? g_snbHelpers : J->nChunks - 1;
        if (wake > 0) ReleaseSemaphore(g_snbSem, wake, NULL);
        snbChunks(0);
        while (J->done < J->nChunks) YieldProcessor();
        InterlockedExchange(&J->open, 0);
        while (J->inJob) SwitchToThread();
        *usedPar = true;
        return J->fault ? -1 : 0;
    }
    __try { snbFillRange(J, 0, n, cand0, g_snbMaxBucket * 3 + 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 0;
}

// The detour at 0x4F2252 (this = the shadow mesh, no arguments, plain ret).
static void __fastcall hkSnbBuild(BYTE* self, void* edx) {
    if (!g_snbOn || InterlockedCompareExchange(&g_snbBusy, 1, 0) != 0) { InterlockedIncrement(&g_snbStock); o_snbBuild(self, edx); return; }
    // the original's first lines, through the engine's own functions: normals, then a table of [this+0x1C] entries
    p_snbNormals(self, NULL);
    int n = *(int*)(self + 0x1C);
    if (n == 0) { if (*(int*)(self + 0x28) != 0) p_snbFree(self, NULL); InterlockedExchange(&g_snbBusy, 0); return; }
    if (n != *(int*)(self + 0x28)) { p_snbFree(self, NULL); if (!p_snbAlloc(self, NULL, n)) { InterlockedExchange(&g_snbBusy, 0); return; } }
    n = *(int*)(self + 0x28);
    BYTE* arr = *(BYTE**)(self + 0x24);
    LONG64 t0 = qpcNow();
    bool par = false;
    bool check = n <= 400 && g_snbChecksLeft > 0;
    if (check) {
        // the engine's way and this one on the same starting bytes; the engine's result is the one kept
        BYTE* cp = (BYTE*)snbGrow(g_snbCopy, &g_snbCopyCap, n * SNB_ENTRY, 1);
        if (cp) {
            g_snbCopy = cp;
            memcpy(cp, arr, n * SNB_ENTRY);
            int r = snbFill(self, cp, n, false, &par);
            LONG64 t1 = qpcNow();
            o_snbBuild(self, edx);                                  // its first lines find the table ready and skip
            LONG64 t2 = qpcNow();
            InterlockedDecrement(&g_snbChecksLeft);
            if (r != 0 || memcmp(cp, *(BYTE**)(self + 0x24), n * SNB_ENTRY)) {
                if (!g_snbBad++) logf("shadow-nbr: SELF-CHECK DIFFERENCE on a model of %d triangles (%s) - the engine's own code builds the rest of this session.", n, r ? "the replacement could not finish" : "bytes differ");
                InterlockedExchange(&g_snbOn, 0);
            } else { g_snbChecked++; g_snbCheckMine += t1 - t0; g_snbCheckEngine += t2 - t1; }
            g_snbN++; g_snbPolys += n;
            InterlockedExchange(&g_snbBusy, 0);
            return;
        }
    }
    int r = snbFill(self, arr, n, true, &par);
    LONG64 dt = qpcNow() - t0;
    if (r != 0) {
        if (!g_snbFaults++) logf("shadow-nbr: the replacement could not finish a model of %d triangles - the engine's own code builds the rest of this session.", n);
        InterlockedExchange(&g_snbOn, 0);
        o_snbBuild(self, edx);                                      // rewrites every entry
        InterlockedExchange(&g_snbBusy, 0);
        return;
    }
    g_snbN++; g_snbPolys += n; g_snbTicks += dt; if (par) g_snbPar++;
    g_snbPairsSaved += (LONG64)n * (n - 1);
    if (n > g_snbMaxPolys) { g_snbMaxPolys = n; g_snbMaxTicks = dt; }
    InterlockedExchange(&g_snbBusy, 0);
}

static DWORD snbFnv(const BYTE* p, const BYTE* e) { DWORD h = 0x811C9DC5u; for (; p < e; ++p) h = (h ^ *p) * 0x01000193u; return h; }
static bool snbCodeIsKnown() {
    static const DWORD k[][3] = {
        { 0x004F2252, 0x004F2470, 0xE544D93F }, { 0x004F21B2, 0x004F2252, 0xA07E2713 }, { 0x004F08BC, 0x004F08FF, 0x7ADA5279 },
        { 0x004EF711, 0x004EF755, 0xABA35D80 }, { 0x004F25EB, 0x004F2614, 0x4987BE8E }, { 0x00A3D6B0, 0x00A3D6B6, 0x6F8B344B },
        { 0x00A3CF8A, 0x00A3CF90, 0xA0A1072A }, { 0x00BD1908, 0x00BD190C, 0x1B587698 }, { 0x00BE5600, 0x00BE5604, 0xA2D8CA81 },
    };
    __try {
        for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
            if (snbFnv((const BYTE*)(ULONG_PTR)k[i][0], (const BYTE*)(ULONG_PTR)k[i][1]) != k[i][2]) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static DWORD WINAPI snbReport(LPVOID) {
    LONG lastN = 0, lastPar = 0, saidCheck = 0; LONG64 lastP = 0, lastT = 0, lastS = 0;
    for (;;) {
        Sleep(5000);
        LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
        if (!saidCheck && g_snbChecked > 0 && (g_snbChecksLeft <= 0 || g_snbBad)) {
            saidCheck = 1;
            logf("shadow-nbr: the first %d models were built a second time by the engine's own code: %s. The engine's way took %d.%03d ms for them, this one %d.%03d ms.",
                 (int)g_snbChecked, g_snbBad ? "NOT the same - switched off" : "every byte the same",
                 (int)(g_snbCheckEngine * 1000 / f), (int)((g_snbCheckEngine * 1000000 / f) % 1000), (int)(g_snbCheckMine * 1000 / f), (int)((g_snbCheckMine * 1000000 / f) % 1000));
        }
        LONG n = g_snbN, par = g_snbPar; LONG64 p = g_snbPolys, t = g_snbTicks, s = g_snbPairsSaved;
        if (n != lastN) {
            LONG64 dT = t - lastT;
            logf("shadow-nbr: last 5 s - %d models got their shadow edge neighbours (%d triangles; %d of them split over %d worker cores) in %d.%03d ms of the game thread; the engine's own way compares every triangle with every other: %I64d pairs. Largest model so far %d triangles, %d.%03d ms.",
                 (int)(n - lastN), (int)(p - lastP), (int)(par - lastPar), (int)g_snbHelpers + 1, (int)(dT * 1000 / f), (int)((dT * 1000000 / f) % 1000), s - lastS,
                 (int)g_snbMaxPolys, (int)(g_snbMaxTicks * 1000 / f), (int)((g_snbMaxTicks * 1000000 / f) % 1000));
        }
        lastN = n; lastPar = par; lastP = p; lastT = t; lastS = s;
    }
}

static void installShadowNbr() {
    char env[8]; env[0] = 0;
    { char mk[MAX_PATH]; if (GetFileAttributesA(aotrPath(mk, "SHADOWNBR_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("shadow-nbr: SHADOWNBR_OFF is present - the engine's own code builds the shadow edge neighbours."); return; } }   // v82.1: a file switch too
    if (GetEnvironmentVariableA("AOTR_SHADOWNBR", env, sizeof(env)) && env[0] == '0') { logf("shadow-nbr: off (AOTR_SHADOWNBR=0) - the engine's own code builds the shadow edge neighbours."); return; }
    if (!snbCodeIsKnown()) { logf("shadow-nbr: the shadow neighbour code is not what this build has - the engine's own code builds them."); return; }
    o_snbBuild = (tSnbThis)makeTrampoline((BYTE*)SNB_BUILD, 6);             // push ebp; mov ebp, esp; sub esp, 44h
    if (!o_snbBuild) { logf("shadow-nbr: no trampoline - off."); return; }
    snbStartHelpers(tpAutoHelpers());
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)SNB_BUILD, (void*)&hkSnbBuild, 6);
    resumeAll(fz, nf);
    InterlockedExchange(&g_snbOn, 1);
    CreateThread(NULL, 0, snbReport, NULL, 0, NULL);
    logf("shadow-nbr: installed - a model's shadow edge neighbours are found through the triangles that share its vertices instead of comparing every triangle with every other (test-12: 80%% of the game thread's time in frames of 80 ms+ after the map had loaded); models of %d+ triangles are split over the game thread and %d worker cores. The first %d small models are built a second time by the engine's own code and compared.",
         (int)g_snbParMin, (int)g_snbHelpers, (int)g_snbChecksLeft);
}

#ifdef AOTR_SELFTEST   // offline-test export: built only into the test DLL. Run in gamemap_test's child (game.dat mapped).
static int __cdecl snbAbs(int x) { return x < 0 ? -x : x; }
static double __cdecl snbFabs(double x) { return x < 0 ? -x : x; }
static DWORD g_snbRng = 12345;
static DWORD snbRand() { g_snbRng = g_snbRng * 1664525u + 1013904223u; return g_snbRng >> 8; }
struct SnbMesh { BYTE self[0x40]; BYTE geo[0x20]; WORD* idx; short* remap; float* N; int n, nv; };
static void snbMeshFree(SnbMesh* m) { free(m->idx); free(m->remap); free(m->N); }
// kind: 0 closed grid (torus), 1 soup over few values, 2 fans + doubles + degenerates, 3 extreme values
static void snbMakeMesh(SnbMesh* m, int kind, int n) {
    memset(m, 0, sizeof(*m));
    int W = 2, H = 2;
    if (kind == 0) { while (W * H * 2 < n) { if (W <= H) ++W; else ++H; } n = W * H * 2; }
    m->n = n;
    m->nv = kind == 0 ? W * H : (kind == 1 ? (n / 2 + 3) : (kind == 2 ? n / 3 + 4 : n + 7));
    m->idx = (WORD*)malloc(n * 6); m->remap = (short*)malloc(m->nv * 2 + 2); m->N = (float*)malloc(n * 12 + 12);
    for (int v = 0; v < m->nv; ++v) {
        if (kind == 3) m->remap[v] = (short)((snbRand() & 7) == 0 ? 0xFFFF : (snbRand() & 3) == 0 ? (0x8000 | (snbRand() & 15)) : (snbRand() % (m->nv / 2 + 1)));
        else m->remap[v] = (short)((snbRand() & 15) == 0 ? (snbRand() % m->nv) : v);   // some vertices welded onto others
    }
    for (int t = 0; t < n; ++t) {
        WORD* x = m->idx + t * 3;
        if (kind == 0) {
            int q = t / 2, cx = q % W, cy = q / W, a = cy * W + cx, b = cy * W + (cx + 1) % W, c = ((cy + 1) % H) * W + cx, d = ((cy + 1) % H) * W + (cx + 1) % W;
            if (t & 1) { x[0] = (WORD)b; x[1] = (WORD)d; x[2] = (WORD)c; } else { x[0] = (WORD)a; x[1] = (WORD)b; x[2] = (WORD)c; }
        } else if (kind == 2 && t > 2 && (snbRand() % 5) == 0) {
            const WORD* y = m->idx + (snbRand() % t) * 3;      // a copy (same or reversed winding) or a fan member on an old edge
            int r = snbRand() % 3;
            if (r == 0) { x[0] = y[0]; x[1] = y[1]; x[2] = y[2]; } else if (r == 1) { x[0] = y[2]; x[1] = y[1]; x[2] = y[0]; }
            else { x[0] = y[1]; x[1] = y[0]; x[2] = (WORD)(snbRand() % m->nv); }
        } else {
            for (int k = 0; k < 3; ++k) x[k] = (WORD)(snbRand() % m->nv);
            if (kind == 2 && (snbRand() % 9) == 0) x[1] = x[0];  // degenerate
        }
        float* nn = m->N + t * 3;
        int s = snbRand() % 6;
        if (s == 0 && t > 0) { const float* o = m->N + (snbRand() % t) * 3; nn[0] = -o[0]; nn[1] = -o[1]; nn[2] = -o[2]; }      // back to back
        else if (s == 1 && t > 0) { const float* o = m->N + (snbRand() % t) * 3; nn[0] = o[0]; nn[1] = o[1]; nn[2] = o[2]; }
        else if (s == 2) { nn[0] = 0; nn[1] = 0; nn[2] = (snbRand() & 1) ? 1.0f : -1.0f; }
        else if (s == 3 && t > 0) { const float* o = m->N + (snbRand() % t) * 3; float e = ((int)(snbRand() % 2001) - 1000) * 1e-5f; nn[0] = -o[0] + e; nn[1] = -o[1]; nn[2] = -o[2] - e; }   // near the 0.01 limit
        else { float a = (snbRand() % 10000) * 0.001f, b = (snbRand() % 10000) * 0.001f - 5.0f, c = (snbRand() % 10000) * 0.001f - 5.0f; float l = sqrtf(a * a + b * b + c * c) + 1e-6f; nn[0] = a / l; nn[1] = b / l; nn[2] = c / l; }
    }
    *(BYTE**)(m->self + 0x00) = m->geo;
    *(WORD**)(m->geo + 0x0C) = m->idx;
    *(float**)(m->self + 0x10) = m->N;
    *(int*)(m->self + 0x1C) = n;
    *(short**)(m->self + 0x20) = m->remap;
}
extern "C" __declspec(dllexport) int __cdecl AotrShadowNbrTest(char* out, int outLen) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    int p = 0; out[0] = 0;
    if (!snbCodeIsKnown()) { sprintf_s(out, outLen, "the mapped game.dat is not the build this replacement mirrors"); return 1; }
    DWORD old;
    VirtualProtect((void*)0x00BD0000, 0x1000, PAGE_READWRITE, &old);
    *(void**)0x00BD0574 = (void*)&snbAbs;               // the image is mapped without its imports: abs and fabs for the engine's code
    *(void**)0x00BD06A8 = (void*)&snbFabs;
    snbStartHelpers(5);
    static const unsigned kCw[3] = { _PC_24, _PC_53, _PC_64 };
    static const int kSizes[] = { 1, 2, 3, 4, 7, 12, 31, 64, 100, 257, 600, 1499, 1500, 2100, 3000, 5200, 8000 };
    int cases = 0, bad = 0, par = 0; LONG64 tEng = 0, tMine = 0; int bigN = 0; LONG64 bigEng = 0, bigMine = 0;
    g_snbParMin = 1500;
    for (int round = 0; round < 3; ++round)
    for (int kind = 0; kind < 4; ++kind)
    for (int si = 0; si < (int)(sizeof(kSizes) / sizeof(kSizes[0])); ++si)
    for (int ci = 0; ci < 3; ++ci) {
        int n = kSizes[si];
        if (n > 3000 && (round > 0 || ci > 0)) continue;   // the engine's way is n x n: keep the run short
        SnbMesh m; snbMakeMesh(&m, kind, n); n = m.n;
        int bytes = n * SNB_ENTRY;
        BYTE* a = (BYTE*)malloc(bytes + 64); BYTE* b = (BYTE*)malloc(bytes + 64);
        for (int k = 0; k < bytes; ++k) a[k] = b[k] = (BYTE)snbRand();
        unsigned keep = _controlfp(0, 0);
        _controlfp(kCw[ci], _MCW_PC);
        *(BYTE**)(m.self + 0x24) = a; *(int*)(m.self + 0x28) = n;
        LONG64 t0 = qpcNow();
        ((tSnbThis)SNB_BUILD)(m.self, NULL);                // the engine's own builder (not patched in this process)
        LONG64 t1 = qpcNow();
        bool usedPar = false;
        int r = snbFill(m.self, b, n, true, &usedPar);
        LONG64 t2 = qpcNow();
        _controlfp(keep, _MCW_PC);
        ++cases; if (usedPar) ++par;
        tEng += t1 - t0; tMine += t2 - t1;
        if (n >= 5000 && n > bigN) { bigN = n; bigEng = t1 - t0; bigMine = t2 - t1; }
        if (r != 0 || memcmp(a, b, bytes)) {
            ++bad;
            if (bad <= 6 && p < outLen - 600) {
                int first = -1; for (int k = 0; k < bytes; ++k) if (a[k] != b[k]) { first = k; break; }
                p += sprintf_s(out + p, outLen - p, "DIFFERENT: kind %d n %d precision %d r %d first byte %d (entry %d)\n", kind, n, ci, r, first, first / SNB_ENTRY);
            }
        }
        free(a); free(b); snbMeshFree(&m);
    }
    // how many neighbours the made-up meshes produced (so the test is not vacuous)
    int withNb = 0, slots = 0;
    { SnbMesh m; snbMakeMesh(&m, 2, 600); int n = m.n; BYTE* a = (BYTE*)malloc(n * SNB_ENTRY); *(BYTE**)(m.self + 0x24) = a; *(int*)(m.self + 0x28) = n;
      ((tSnbThis)SNB_BUILD)(m.self, NULL);
      for (int i = 0; i < n; ++i) { int c = 0; for (int k = 0; k < 3; ++k) if (*(WORD*)(a + i * SNB_ENTRY + 4 + k * 6) != 0xFFFF) ++c; slots += c; if (c) ++withNb; }
      free(a); snbMeshFree(&m); }
    double f = (double)g_pqpf.QuadPart;
    {   SnbMesh m; snbMakeMesh(&m, 1, 8000); int n = m.n; BYTE* a = (BYTE*)malloc(n * SNB_ENTRY); BYTE* b = (BYTE*)malloc(n * SNB_ENTRY);
        bool u = false; LONG64 best1 = 1LL << 60, bestP = 1LL << 60;
        for (int rep = 0; rep < 20; ++rep) {
            memset(a, 0x5A, n * SNB_ENTRY); memset(b, 0x5A, n * SNB_ENTRY);     // same starting bytes: +2/+3 and empty slots are not written (as in the engine)
            LONG64 t0 = qpcNow(); snbFill(m.self, a, n, false, &u); LONG64 t1 = qpcNow(); snbFill(m.self, b, n, true, &u); LONG64 t2 = qpcNow();
            if (t1 - t0 < best1) best1 = t1 - t0; if (t2 - t1 < bestP) bestP = t2 - t1;
            if (memcmp(a, b, n * SNB_ENTRY)) ++bad;
        }
        p += sprintf_s(out + p, outLen - p, "soup of %d triangles, best of 20: one thread %.3f ms, split %.3f ms (same bytes)\n", n, best1 * 1000.0 / f, bestP * 1000.0 / f);
        free(a); free(b); snbMeshFree(&m); }
    p += sprintf_s(out + p, outLen - p, "facing test taken %d / refused %d, triangles that ran out of slots %d\n", (int)g_snbTFacingYes, (int)g_snbTFacingNo, (int)g_snbTFull);
    p += sprintf_s(out + p, outLen - p, "%d meshes (4 kinds, 1-8000 triangles, x87 precision 24/53/64), %d split over %d helper threads: %d different | engine %.1f ms, replacement %.1f ms | largest %d triangles: engine %.1f ms, replacement %.3f ms | sample soup of 600: %d triangles with neighbours, %d slots filled",
                   cases, par, (int)g_snbHelpers, bad, tEng * 1000.0 / f, tMine * 1000.0 / f, bigN, bigEng * 1000.0 / f, bigMine * 1000.0 / f, withNb, slots);
    return bad ? 1 : 0;
}
#endif
