// ================================================================ shadow mesh vertex weld, exact and fast (v32, 2026-10-21)
// The second game.dat function of the test-12 long frames (accel-test-12\bfme2_accel.log 18:22:20 and 18:26:10, the
// long-frame profiler): 0x4F0907, 6.5 % + 1.8 % + 1.4 % + ... of the samples, always under 0x4F184F / 0x4F24A8 - the
// stencil-shadow code adding a model's mesh to its shadow geometry. Its loop at 0x4F099A..0x4F0A52 welds the mesh's
// vertices (up to 0x4000 of them): for every vertex i that is not yet welded it walks EVERY later vertex j and, when
//     (zi - zj)^2 + (xi - xj)^2 + (yi - yj)^2 == 0.0f      (SSE, ucomiss against the 0.0f at 0xC1B594)
// writes remap[j] = i and counts one vertex less; then remap[i] = i. n x n / 2 compares - 134 million for the
// largest mesh. The result (the remap table on its stack and the vertex count in ebx) feeds the edge neighbours of
// aotr_shadownbr.inc. Drawing-side code: the W3D shadow volume, never read by the simulation.
//
// The replacement gives the same table and the same count, found a different way. Two vertices can only pass that
// test if every coordinate difference squares to zero, i.e. |difference| < 2^-63 (2^-75 without flush-to-zero).
// Two different floats that close are both below 1e-10 in size (above it neighbouring floats are at least 6.9e-18
// apart). So each vertex gets a key - per coordinate the exact bits, or 0 for anything below 1e-10 (+0, -0,
// denormals) - and only vertices with the same key are tried, still in the original order (i ascending; for each i
// every later j of its key), with the very same SSE instruction sequence under the game thread's own MXCSR.
// A vertex with a NaN or infinite coordinate never passes the test (the difference is NaN or the square infinite),
// so it stays alone, as it does in the original. Keys are grouped by a sort; n log n instead of n x n.
//
// Exactness: shadowweld_test (game.dat mapped at its base in a child process) runs the engine's whole 0x4F0907 with
// and without this patch on made-up meshes (seams, exact copies, -0, denormals, values around 1e-10, NaN, infinity,
// 1 to 16384 vertices, flush-to-zero and denormals-are-zero on and off) and compares the remap table byte for byte
// and the count. In the game the first 32 meshes are also welded by the engine's own loop and compared (any
// difference: the engine's loop for the rest of the session), and the code it mirrors is hashed at start.

#define SWD_PATCH   0x004F099A      // mov ebx,[esi+18h]; add esp,0Ch   (6 bytes)
#define SWD_LOOP    0x004F09A0      // xor edi,edi - the original loop
#define SWD_AFTER   0x004F0A52      // after the loop: ebx = vertex count
#define SWD_ZERO    0x00C1B594      // float 0.0
#ifndef SWD_TINY
#define SWD_TINY    0x2EDBE6FFu     // 1e-10f
#endif

static volatile LONG g_swdOn = 0, g_swdBusy = 0, g_swdChecksLeft = 32, g_swdChecked = 0, g_swdBad = 0, g_swdStock = 0;
static volatile LONG g_swdN = 0, g_swdMaxVerts = 0;
static volatile LONG64 g_swdVerts = 0, g_swdTicks = 0, g_swdMaxTicks = 0, g_swdPairs = 0, g_swdCheckMine = 0, g_swdCheckEngine = 0;
static const float* g_swdZero = (const float*)SWD_ZERO;

// The test of 0x4F09D7..0x4F0A24, instruction for instruction. 1 = weld (equal and ordered).
static __declspec(noinline) int swdEqual(const float* pi, const float* pj) {
    int r;
    const float* z = g_swdZero;
    __asm {
        mov ecx, pi
        mov eax, pj
        mov edx, z
        movss xmm2, dword ptr [ecx+8]
        subss xmm2, dword ptr [eax+8]
        movss xmm0, dword ptr [ecx]
        subss xmm0, dword ptr [eax]
        movss xmm1, dword ptr [ecx+4]
        subss xmm1, dword ptr [eax+4]
        movaps xmm3, xmm2
        mulss xmm3, xmm2
        movaps xmm2, xmm0
        mulss xmm2, xmm0
        movaps xmm0, xmm1
        addss xmm3, xmm2
        mulss xmm0, xmm1
        addss xmm3, xmm0
        ucomiss xmm3, dword ptr [edx]
        lahf
        xor ecx, ecx
        test ah, 44h
        setnp cl
        mov r, ecx
    }
    return r;
}

struct SwdKey { DWORD k[3]; int idx; };
static int __cdecl swdCmp(const void* a, const void* b) {
    const SwdKey* x = (const SwdKey*)a; const SwdKey* y = (const SwdKey*)b;
    for (int c = 0; c < 3; ++c) if (x->k[c] != y->k[c]) return x->k[c] < y->k[c] ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}
static SwdKey g_swdKeys[0x4000];

// The weld of n vertices at pos (12 bytes each) into remap (already 0xFFFF everywhere). Returns the count the
// original leaves in ebx, or -1 if it could not run (the caller runs the original loop).
static int swdWeld(const float* pos, int n, WORD* remap, LONG64* pairs) {
    if (n <= 0) return n;
    if (n > 0x4000 || !pos) return -1;
    int m = 0;
    __try {
        for (int i = 0; i < n; ++i) {
            const DWORD* b = (const DWORD*)(pos + i * 3);
            bool finite = true;
            DWORD k[3];
            for (int c = 0; c < 3; ++c) {
                DWORD v = b[c];
                if ((v & 0x7F800000u) == 0x7F800000u) { finite = false; break; }
                k[c] = (v & 0x7FFFFFFFu) < SWD_TINY ? 0u : v;          // SWD_TINY = 1e-10f: below it, key 0
            }
            if (!finite) { remap[i] = (WORD)i; continue; }           // never equal to anything: alone, as in the original
            g_swdKeys[m].k[0] = k[0]; g_swdKeys[m].k[1] = k[1]; g_swdKeys[m].k[2] = k[2]; g_swdKeys[m].idx = i; ++m;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }            // nothing written that the original does not rewrite
    qsort(g_swdKeys, m, sizeof(SwdKey), swdCmp);
    int count = n;
    LONG64 tried = 0;
    for (int s = 0; s < m; ) {
        int e = s + 1;
        while (e < m && g_swdKeys[e].k[0] == g_swdKeys[s].k[0] && g_swdKeys[e].k[1] == g_swdKeys[s].k[1] && g_swdKeys[e].k[2] == g_swdKeys[s].k[2]) ++e;
        // one key: its vertices in increasing order, exactly the original's i / j walk restricted to them
        for (int a = s; a < e; ++a) {
            int i = g_swdKeys[a].idx;
            if (remap[i] != 0xFFFF) continue;
            const float* pi = pos + i * 3;
            for (int c = a + 1; c < e; ++c) {
                int j = g_swdKeys[c].idx;
                ++tried;
                if (swdEqual(pi, pos + j * 3)) { remap[j] = (WORD)i; --count; }
            }
            remap[i] = (WORD)i;
        }
        s = e;
    }
    *pairs += tried;
    return count;
}

// The original loop as written (0x4F09A0..0x4F0A4C), for the in-game check on real meshes: same order, same test.
static int swdWeldNN(const float* pos, int n, WORD* remap) {
    int count = n;
    for (int i = 0; i < n; ++i) {
        if (remap[i] != 0xFFFF) continue;
        for (int j = i + 1; j < n; ++j)
            if (swdEqual(pos + i * 3, pos + j * 3)) { remap[j] = (WORD)i; --count; }
        remap[i] = (WORD)i;
    }
    return count;
}

// Called from the patch at 0x4F099A with the shadow entry (esi) and the remap table on the original's stack (all
// 0xFFFF). Returns the vertex count for ebx, or -1: the original loop runs.
static WORD g_swdCopy[0x4000];
static int __cdecl swdHook(BYTE* entry, WORD* remap) {
    if (!g_swdOn || InterlockedCompareExchange(&g_swdBusy, 1, 0) != 0) { InterlockedIncrement(&g_swdStock); return -1; }
    int n = *(int*)(entry + 0x18);
    const float* pos = *(const float**)(entry + 8);
    LONG64 pairs = 0;
    if (n > 0 && n <= 2000 && g_swdChecksLeft > 0) {
        // the first meshes: the original's loop on a copy of the table, this one on the table, then compared
        memcpy(g_swdCopy, remap, n * 2);
        LONG64 t0 = qpcNow();
        int rn = -1;
        __try { rn = swdWeldNN(pos, n, g_swdCopy); } __except (EXCEPTION_EXECUTE_HANDLER) { rn = -1; }
        LONG64 t1 = qpcNow();
        int r = swdWeld(pos, n, remap, &pairs);
        LONG64 t2 = qpcNow();
        InterlockedDecrement(&g_swdChecksLeft);
        if (rn < 0 || r != rn || memcmp(g_swdCopy, remap, n * 2)) {
            if (!g_swdBad++) logf("shadow-weld: SELF-CHECK DIFFERENCE on a mesh of %d vertices (count %d, the original's way %d) - the engine's own loop welds the rest of this session.", n, r, rn);
            InterlockedExchange(&g_swdOn, 0);
            if (rn >= 0) { memcpy(remap, g_swdCopy, n * 2); InterlockedExchange(&g_swdBusy, 0); return rn; }   // the original's result
            memset(remap, 0xFF, n * 2);
            InterlockedExchange(&g_swdBusy, 0);
            return -1;
        }
        g_swdChecked++; g_swdCheckEngine += t1 - t0; g_swdCheckMine += t2 - t1;
        g_swdN++; g_swdVerts += n;
        InterlockedExchange(&g_swdBusy, 0);
        return r;
    }
    LONG64 t0 = qpcNow();
    int r = swdWeld(pos, n, remap, &pairs);
    LONG64 dt = qpcNow() - t0;
    if (r < 0 && n > 0) { InterlockedExchange(&g_swdBusy, 0); return -1; }
    g_swdN++; g_swdVerts += n; g_swdTicks += dt; g_swdPairs += (LONG64)n * (n - 1) / 2;
    if (n > g_swdMaxVerts) { g_swdMaxVerts = n; g_swdMaxTicks = dt; }
    InterlockedExchange(&g_swdBusy, 0);
    return r;
}

// The patch: 0x4F099A  jmp swdStub; nop     (the two instructions it replaces are redone here)
static __declspec(naked) void swdStub() {
    __asm {
        add esp, 0Ch                        // the original's add esp,0Ch (memset's arguments)
        pushad
        lea eax, [ebp-800Ch]                // the remap table
        push eax
        push esi                            // the shadow entry
        call swdHook
        add esp, 8
        cmp eax, -1
        je  orig
        mov [esp+16], eax                   // pushad frame: the saved ebx
        popad
        push SWD_AFTER
        ret
    orig:
        popad
        mov ebx, dword ptr [esi+18h]        // the original's mov ebx,[esi+18h]
        push SWD_LOOP
        ret
    }
}

static bool swdCodeIsKnown() {
    static const DWORD k[][3] = {
        { 0x004F0907, 0x004F0AAF, 0x322576C6 }, { 0x00C1B594, 0x00C1B598, 0x4B95F515 },
    };
    __try {
        for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
            if (snbFnv((const BYTE*)(ULONG_PTR)k[i][0], (const BYTE*)(ULONG_PTR)k[i][1]) != k[i][2]) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

static DWORD WINAPI swdReport(LPVOID) {
    LONG lastN = 0, saidCheck = 0; LONG64 lastV = 0, lastT = 0, lastP = 0;
    for (;;) {
        Sleep(5000);
        LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
        if (!saidCheck && g_swdChecked > 0 && (g_swdChecksLeft <= 0 || g_swdBad)) {
            saidCheck = 1;
            logf("shadow-weld: the first %d meshes were also welded the original way (every vertex against every later one): %s. The original way took %d.%03d ms for them, this one %d.%03d ms.",
                 (int)g_swdChecked, g_swdBad ? "NOT the same - switched off" : "same table, same count",
                 (int)(g_swdCheckEngine * 1000 / f), (int)((g_swdCheckEngine * 1000000 / f) % 1000), (int)(g_swdCheckMine * 1000 / f), (int)((g_swdCheckMine * 1000000 / f) % 1000));
        }
        LONG n = g_swdN; LONG64 v = g_swdVerts, t = g_swdTicks, p = g_swdPairs;
        if (n != lastN) {
            LONG64 dT = t - lastT;
            logf("shadow-weld: last 5 s - %d meshes welded (%d vertices) in %d.%03d ms of the game thread; the engine's own loop would have compared %I64d vertex pairs. Largest mesh so far %d vertices, %d.%03d ms.",
                 (int)(n - lastN), (int)(v - lastV), (int)(dT * 1000 / f), (int)((dT * 1000000 / f) % 1000), p - lastP,
                 (int)g_swdMaxVerts, (int)(g_swdMaxTicks * 1000 / f), (int)((g_swdMaxTicks * 1000000 / f) % 1000));
        }
        lastN = n; lastV = v; lastT = t; lastP = p;
    }
}

static void installShadowWeld() {
    char env[8]; env[0] = 0;
    { char mk[MAX_PATH]; if (GetFileAttributesA(aotrPath(mk, "SHADOWWELD_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("shadow-weld: SHADOWWELD_OFF is present - the engine's own loop welds the shadow mesh vertices."); return; } }   // v82.1: a file switch too
    if (GetEnvironmentVariableA("AOTR_SHADOWWELD", env, sizeof(env)) && env[0] == '0') { logf("shadow-weld: off (AOTR_SHADOWWELD=0) - the engine's own loop welds the shadow mesh vertices."); return; }
    if (!swdCodeIsKnown() || memcmp((const void*)SWD_PATCH, "\x8B\x5E\x18\x83\xC4\x0C", 6)) { logf("shadow-weld: the shadow weld code is not what this build has - the engine's own loop welds them."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)SWD_PATCH, (void*)&swdStub, 6);
    resumeAll(fz, nf);
    InterlockedExchange(&g_swdOn, 1);
    CreateThread(NULL, 0, swdReport, NULL, 0, NULL);
    logf("shadow-weld: installed - a shadow mesh's vertices are welded by grouping equal positions instead of comparing every vertex with every later one (test-12: game.dat+0F0907 in the long frames). The first %d meshes are also welded the original way and compared.", (int)g_swdChecksLeft);
}

#ifdef AOTR_SELFTEST   // offline-test export: built only into the test DLL. Run in gamemap_test's child (game.dat mapped).
static WORD g_swdCapTab[0x4000]; static int g_swdCapCount = 0, g_swdCapN = 0;
static void __cdecl swdCapture(const WORD* tab, int count) { memcpy(g_swdCapTab, tab, sizeof(g_swdCapTab)); g_swdCapCount = count; }
// 0x4F0A52 in the test process: hand the table and the count over, then leave the function the way it would
static __declspec(naked) void swdCapStub() {
    __asm {
        lea eax, [ebp-800Ch]
        push ebx
        push eax
        call swdCapture
        add esp, 8
        xor eax, eax
        pop edi
        pop ebx
        pop esi
        leave
        ret 0Ch
    }
}
static DWORD g_swdRng = 777;
static DWORD swdRand() { g_swdRng = g_swdRng * 1664525u + 1013904223u; return g_swdRng >> 8; }
static float swdBits(DWORD b) { float f; memcpy(&f, &b, 4); return f; }
static float swdSpecial() {
    static const DWORD s[] = { 0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x00400000, 0x2EDBE6FF, 0x2EDBE6FE, 0x2EDBE700,
                               0xAEDBE6FF, 0xAEDBE6FE, 0x1E3CE508 /*1e-20*/, 0x1EBCE508 /*2e-20*/, 0x1F800000 /*2^-64*/, 0x20000000 /*2^-63*/,
                               0x1F000000, 0x1C000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00001, 0x7F800001, 0x3F800000, 0x3F800001,
                               0x00800000, 0x00800001, 0x80800000 };
    return swdBits(s[swdRand() % (sizeof(s) / sizeof(s[0]))]);
}
typedef int (__fastcall* tSwdAdd)(BYTE* mgr, void* edx, BYTE* obj, int a2, int a3);
static int swdRunEngine(float* pos, int n, BYTE* mgr, BYTE* obj, BYTE* R) {
    *(int*)(mgr + 0x2094) = 0;
    *(int*)(R + 0x28) = n;
    ((tSwdAdd)0x004F0907)(mgr, NULL, obj, 0, 0);
    return g_swdCapCount;
}
extern "C" __declspec(dllexport) int __cdecl AotrShadowWeldTest(char* out, int outLen) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    int p = 0; out[0] = 0;
    if (!swdCodeIsKnown() || memcmp((const void*)SWD_PATCH, "\x8B\x5E\x18\x83\xC4\x0C", 6)) { sprintf_s(out, outLen, "the mapped game.dat is not the build this replacement mirrors"); return 1; }
    DWORD old;
    VirtualProtect((void*)0x00BD0000, 0x1000, PAGE_READWRITE, &old);
    *(void**)0x00BD06C4 = (void*)&memset;                 // the image is mapped without its imports: memset / memcpy for the engine's code
    *(void**)0x00BD06C8 = (void*)&memcpy;
    BYTE orig[6]; memcpy(orig, (void*)SWD_PATCH, 6);
    patchJmp((BYTE*)SWD_AFTER, (void*)&swdCapStub, 5);
    BYTE* mgr = (BYTE*)calloc(1, 0x3000); BYTE* obj = (BYTE*)calloc(1, 0x200); BYTE* R = (BYTE*)calloc(1, 0x100);
    BYTE* P = (BYTE*)calloc(1, 0x40); BYTE* Q = (BYTE*)calloc(1, 0x40);
    *(BYTE**)(obj + 0xC4) = R; *(DWORD*)(R + 0x18) = 0x1000; *(BYTE**)(R + 0x30) = P; *(BYTE**)(R + 0x2C) = Q;
    float* pos = (float*)malloc(0x4000 * 12 + 64);
    *(float**)(P + 0x0C) = pos;
    static const unsigned kCsr[4] = { 0x1F80, 0x9F80, 0x1FC0, 0x9FC0 };      // default, FTZ, DAZ, both
    static const int kSizes[] = { 0, 1, 2, 3, 5, 17, 64, 200, 777, 2000, 4096, 9000, 16384 };
    static WORD engTab[0x4000];
    int cases = 0, bad = 0, welded = 0; LONG64 tEng = 0, tMine = 0; LONG64 bigEng = 0, bigMine = 0; int bigN = 0;
    unsigned keepCsr = _mm_getcsr();
    for (int kind = 0; kind < 4; ++kind)
    for (int si = 0; si < (int)(sizeof(kSizes) / sizeof(kSizes[0])); ++si)
    for (int ci = 0; ci < 4; ++ci) {
        int n = kSizes[si];
        if (n > 4096 && ci > 0) continue;                  // the engine's way is n x n: keep the run short
        int pool = n / 3 + 1;
        for (int v = 0; v < n; ++v) {
            float* q = pos + v * 3;
            int r = swdRand() % 10;
            if (kind == 0 && v > 0 && r < 4) { const float* o = pos + (swdRand() % v) * 3; q[0] = o[0]; q[1] = o[1]; q[2] = o[2]; }           // seams: exact copies
            else if (kind == 1) { for (int c = 0; c < 3; ++c) q[c] = (swdRand() & 3) ? (float)(swdRand() % 4) : swdSpecial(); }               // few values + specials
            else if (kind == 2) { int b = swdRand() % pool; q[0] = (float)(b % 7) * 0.5f; q[1] = (float)(b / 7) * 0.25f; q[2] = (swdRand() & 7) ? 0.0f : swdSpecial(); }
            else { for (int c = 0; c < 3; ++c) q[c] = (float)((int)(swdRand() % 2001) - 1000) * 0.125f; if (r == 0) q[swdRand() % 3] = swdSpecial(); }
        }
        _mm_setcsr(kCsr[ci]);
        memcpy((void*)SWD_PATCH, orig, 6);                  // the engine's own loop
        LONG64 t0 = qpcNow();
        int ce = swdRunEngine(pos, n, mgr, obj, R);
        LONG64 t1 = qpcNow();
        memcpy(engTab, g_swdCapTab, sizeof(engTab));
        patchJmp((BYTE*)SWD_PATCH, (void*)&swdStub, 6);     // this one
        InterlockedExchange(&g_swdOn, 1);
        g_swdChecksLeft = (cases & 1) ? 1 : 0;             // every other case through the in-game self-check path too
        g_swdBad = 0;
        LONG64 t2 = qpcNow();
        int cm = swdRunEngine(pos, n, mgr, obj, R);
        LONG64 t3 = qpcNow();
        _mm_setcsr(keepCsr);
        ++cases; tEng += t1 - t0; tMine += t3 - t2; welded += n - ce;
        if (n > bigN) { bigN = n; bigEng = t1 - t0; bigMine = t3 - t2; }
        if (ce != cm || memcmp(engTab, g_swdCapTab, sizeof(engTab)) || g_swdBad) {
            ++bad;
            if (bad <= 6 && p < outLen - 600) {
                int first = -1; for (int k = 0; k < 0x4000; ++k) if (engTab[k] != g_swdCapTab[k]) { first = k; break; }
                p += sprintf_s(out + p, outLen - p, "DIFFERENT: kind %d n %d mxcsr %04X count %d/%d first entry %d (%04X/%04X) selfcheck-bad %d\n", kind, n, kCsr[ci], ce, cm, first,
                               first >= 0 ? engTab[first] : 0, first >= 0 ? g_swdCapTab[first] : 0, (int)g_swdBad);
            }
        }
    }
    memcpy((void*)SWD_PATCH, orig, 6);
    double f = (double)g_pqpf.QuadPart;
    p += sprintf_s(out + p, outLen - p, "%d meshes (4 kinds, 0-16384 vertices, MXCSR default/FTZ/DAZ/both), %d vertices welded away in all: %d different | engine %.1f ms, replacement %.1f ms | largest %d vertices: engine %.1f ms, replacement %.3f ms",
                   cases, welded, bad, tEng * 1000.0 / f, tMine * 1000.0 / f, bigN, bigEng * 1000.0 / f, bigMine * 1000.0 / f);
    return bad ? 1 : 0;
}
#endif
