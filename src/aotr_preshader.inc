// aotr_preshader.inc - v33 (ISH-7): the D3DX preshader cache, back - with a key that holds EVERY byte the VM can read.
//
// d3dx9_27.dll+0xE8943 is the preshader VM (stdcall, 14 arguments, always returns 0). Read from its own code
// (work\ish7\d3dx.asm, 0x4E8943..0x4E926A):
//    a0  the program: [count] then per instruction: op, nsrc, per source (rel count, rel count x (type, index),
//        type, index), then the destination (one skipped dword, type, index). op & 0xFFFFF = the component count.
//    a1  literals (type 1): [count] then count doubles, index taken modulo count
//    a2  bank type 3, mask a8      a3  bank type 2, mask a9      a4  bank type 9 (4-byte stride), mask a10
//    a5  bank type 4, mask a11     a6  bank type 6, mask a12     a7  bank type 5, mask a13
//    type 7 = temporaries on its own stack. Destinations are types 4, 5, 6 (the banks it writes) or 7.
//    Index = (index + relative terms) & mask, then op-dependent component runs from there (x87, the thread's control word).
// v30 keyed on a0 + a3 only and compared one hit in 32: two calls with the same a3 and another a2 got the first call's
// output (v31 removed it). This version keys on all of it:
//    the 14 arguments, the x87 control word, the program's bytes (its length parsed exactly as the VM walks it), the
//    literals, and the WHOLE of every bank the arguments name (mask + 1 entries, plus the widest component run past the
//    end) - so whatever index a relative term produces, the byte it reads is in the key. The output banks' contents
//    before the call are in the key too (a program may read what it later writes).
// The value is the list of bytes the call CHANGED in the output banks. A hit means every input byte is the same as
// when that entry was made, so the VM would write exactly those bytes again: the hook writes them and returns 0.
// Every hit compares the whole key byte for byte (not a hash). On top of that the first 4096 hits, and one in 32 after
// them, run the VM anyway and compare its result with the entry; one difference turns the cache off for the session
// (logged as MISMATCH). Drawing only: these are shader constants of the picture; logic never reads them.
// AOTR_PRESHADER=1 installs it (measured: no gain, see installPreshaderCache), AOTR_PSCHECK=1 checks every hit.

typedef int (__stdcall* tPsVm)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
static tPsVm o_psVm = NULL;
#define PS_SLOTS   16384
#define PS_ARENA   (48u << 20)
#define PS_BLOBMAX (48u << 10)
struct PsSlot { DWORD hash, blobLen, blobOff, outOff, outLen; };
static PsSlot* g_psTab = NULL;
static BYTE* g_psArena = NULL; static DWORD g_psArenaUsed = 0;
static BYTE* g_psBlob = NULL;                       // the key of the current call (one caller at a time)
static BYTE* g_psPost = NULL;
static CRITICAL_SECTION g_psCs;
static volatile LONG g_psOn = 0, g_psKill = 0, g_psCheckAll = 0, g_psCheckLeft = 4096;
static volatile LONG g_psCalls = 0, g_psHits = 0, g_psMiss = 0, g_psUncache = 0, g_psBusy = 0, g_psChecked = 0, g_psMismatch = 0, g_psResets = 0, g_psFaults = 0;
static volatile LONG64 g_psVmTicks = 0, g_psVmN = 0, g_psHitTicks = 0, g_psMissTicks = 0;
static volatile DWORD g_psLastTid = 0;

// the regions of one call: index 0 program, 1 literals, then the six banks
struct PsReg { const BYTE* p; DWORD len; };
enum { PSR_PROG, PSR_CLIT, PSR_B2, PSR_B3, PSR_B9, PSR_B4, PSR_B6, PSR_B5, PSR_N };
static int psRegions(const DWORD* a, PsReg* r) {
    const DWORD* p = (const DWORD*)(ULONG_PTR)a[0];
    if (!p) return 0;
    DWORD n = p[0]; if (n > 4096) return 0;
    const DWORD* q = p + 1; const DWORD* end = p + (PS_BLOBMAX / 4);
    DWORD maxComp = 1;
    for (DWORD i = 0; i < n; ++i) {
        if (q + 2 > end) return 0;
        DWORD comp = q[0] & 0xFFFFF, nsrc = q[1]; q += 2;
        if (comp > 64 || nsrc > 64) return 0;
        if (comp > maxComp) maxComp = comp;
        for (DWORD s = 0; s < nsrc; ++s) {
            if (q + 1 > end) return 0;
            DWORD cnt = q[0]; q += 1; if (cnt > 16) return 0;
            q += 2 * cnt + 2;
            if (q > end) return 0;
        }
        if (q + 3 > end) return 0;
        DWORD dt = q[1]; q += 3;
        if (dt != 4 && dt != 5 && dt != 6 && dt != 7) return 0;   // the VM would write through a stale pointer: not cached
    }
    DWORD ext = (maxComp + 8) * 8;
    r[PSR_PROG].p = (const BYTE*)p; r[PSR_PROG].len = (DWORD)((const BYTE*)q - (const BYTE*)p);
    const DWORD* cl = (const DWORD*)(ULONG_PTR)a[1];
    if (cl) { if (cl[0] > 8192) return 0; r[PSR_CLIT].p = (const BYTE*)cl; r[PSR_CLIT].len = 4 + cl[0] * 8 + ext; } else { r[PSR_CLIT].p = NULL; r[PSR_CLIT].len = 0; }
    struct { int reg; DWORD ptr, mask, stride; } b[6] = { { PSR_B2, a[3], a[9], 8 }, { PSR_B3, a[2], a[8], 8 }, { PSR_B9, a[4], a[10], 4 },
                                                          { PSR_B4, a[5], a[11], 8 }, { PSR_B6, a[6], a[12], 8 }, { PSR_B5, a[7], a[13], 8 } };
    for (int i = 0; i < 6; ++i) {
        if (!b[i].ptr) { r[b[i].reg].p = NULL; r[b[i].reg].len = 0; continue; }
        if (b[i].mask > 0xFFFF) return 0;
        r[b[i].reg].p = (const BYTE*)(ULONG_PTR)b[i].ptr; r[b[i].reg].len = (b[i].mask + 1) * b[i].stride + ext;
    }
    DWORD tot = 64; for (int i = 0; i < PSR_N; ++i) tot += r[i].len + 8;
    return tot <= PS_BLOBMAX ? 1 : 0;
}
static __forceinline DWORD psHash(const BYTE* b, DWORD len) {
    const DWORD* d = (const DWORD*)b; DWORD n = len / 4, h1 = 0x811C9DC5u, h2 = 0x9E3779B9u;
    DWORD i = 0;
    for (; i + 1 < n; i += 2) { h1 = (h1 ^ d[i]) * 16777619u; h2 = (h2 ^ d[i + 1]) * 0x85EBCA6Bu; h2 = (h2 << 13) | (h2 >> 19); }
    if (i < n) h1 = (h1 ^ d[i]) * 16777619u;
    return h1 ^ (h2 * 0xC2B2AE35u) ^ len;
}
// the key: header (14 arguments, x87 control word, region lengths) then each region's bytes
static DWORD psBuild(const DWORD* a, const PsReg* r, WORD fpcw, DWORD* outStart) {
    BYTE* b = g_psBlob; DWORD o = 0;
    memcpy(b, a, 56); o = 56;
    *(DWORD*)(b + o) = fpcw; o += 4;
    for (int i = 0; i < PSR_N; ++i) { *(DWORD*)(b + o) = r[i].len; o += 4; }
    for (int i = 0; i < PSR_N; ++i) {
        if (i == PSR_B4) *outStart = o;
        if (r[i].len) { memcpy(b + o, r[i].p, r[i].len); o += r[i].len; }
    }
    return o;
}
// The changes the call made in the output banks: runs of (region, offset, length, bytes) from the snapshot before.
static DWORD psDiff(const PsReg* r, const BYTE* pre, BYTE* out, DWORD cap) {
    DWORD o = 0;
    for (int i = PSR_B4; i < PSR_N; ++i) {
        const BYTE* now = r[i].p; DWORD len = r[i].len;
        if (!now || !len) continue;
        DWORD k = 0, nd = len / 4;
        const DWORD* a = (const DWORD*)pre; const DWORD* c = (const DWORD*)now;
        while (k < nd) {
            if (a[k] == c[k]) { ++k; continue; }
            DWORD s = k; while (k < nd && a[k] != c[k]) ++k;
            DWORD bytes = (k - s) * 4;
            if (o + 12 + bytes > cap) return 0xFFFFFFFF;
            *(DWORD*)(out + o) = (DWORD)i; *(DWORD*)(out + o + 4) = s * 4; *(DWORD*)(out + o + 8) = bytes;
            memcpy(out + o + 12, c + s, bytes); o += 12 + bytes;
        }
        pre += len;
    }
    return o;
}
static void psApply(const PsReg* r, const BYTE* d, DWORD len) {
    DWORD o = 0;
    while (o < len) {
        DWORD reg = *(const DWORD*)(d + o), off = *(const DWORD*)(d + o + 4), bytes = *(const DWORD*)(d + o + 8);
        memcpy((BYTE*)r[reg].p + off, d + o + 12, bytes); o += 12 + bytes;
    }
}
static BYTE* psAlloc(DWORD n) {
    n = (n + 7) & ~7u;
    if (g_psArenaUsed + n > PS_ARENA) {                            // full: forget everything, start again
        memset(g_psTab, 0, sizeof(PsSlot) * PS_SLOTS); g_psArenaUsed = 0; InterlockedIncrement(&g_psResets);
    }
    BYTE* p = g_psArena + g_psArenaUsed; g_psArenaUsed += n; return p;
}

static int __stdcall psHook(DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7, DWORD a8, DWORD a9, DWORD a10, DWORD a11, DWORD a12, DWORD a13) {
    InterlockedIncrement(&g_psCalls);
    if (g_psKill || !g_psOn) return o_psVm(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    if (!TryEnterCriticalSection(&g_psCs)) { InterlockedIncrement(&g_psBusy); return o_psVm(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13); }
    g_psLastTid = GetCurrentThreadId();
    LONG64 t0 = qpcNow();
    DWORD a[14] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13 };
    PsReg r[PSR_N]; DWORD blobLen = 0, outStart = 0, hash = 0; WORD cw = 0; int ok = 0;
    __asm fnstcw cw
    __try {
        ok = psRegions(a, r);
        if (ok) { blobLen = psBuild(a, r, cw, &outStart); hash = psHash(g_psBlob, blobLen); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; InterlockedIncrement(&g_psFaults); }
    if (!ok) { InterlockedIncrement(&g_psUncache); LeaveCriticalSection(&g_psCs); return o_psVm(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13); }
    DWORD si = hash & (PS_SLOTS - 1); PsSlot* hit = NULL; PsSlot* freeS = NULL;
    for (int k = 0; k < 4; ++k) {
        PsSlot* s = &g_psTab[(si + k) & (PS_SLOTS - 1)];
        if (!s->blobLen) { if (!freeS) freeS = s; continue; }
        if (s->hash == hash && s->blobLen == blobLen && !memcmp(g_psArena + s->blobOff, g_psBlob, blobLen)) { hit = s; break; }
    }
    if (hit) {
        bool check = g_psCheckAll || (g_psCheckLeft > 0 && InterlockedDecrement(&g_psCheckLeft) >= 0) || ((g_psHits & 31) == 0);
        if (!check) {
            __try { psApply(r, g_psArena + hit->outOff, hit->outLen); }
            __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_psFaults); InterlockedExchange(&g_psKill, 1); }
            InterlockedIncrement(&g_psHits);
            g_psHitTicks += qpcNow() - t0;
            LeaveCriticalSection(&g_psCs);
            return 0;
        }
        // the check: the real VM, then its changes against the entry's
        int rv = o_psVm(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
        DWORD dl = 0;
        __try { dl = psDiff(r, g_psBlob + outStart, g_psPost, PS_BLOBMAX); } __except (EXCEPTION_EXECUTE_HANDLER) { dl = 0xFFFFFFFF; }
        InterlockedIncrement(&g_psChecked); InterlockedIncrement(&g_psHits);
        if (dl != hit->outLen || (dl && memcmp(g_psPost, g_psArena + hit->outOff, dl))) {
            InterlockedIncrement(&g_psMismatch); InterlockedExchange(&g_psKill, 1);
            logf("preshader: MISMATCH - a cached result differed from the VM's own (program %08X, %u changed bytes against %u) - the cache is OFF for this session.", a0, dl, hit->outLen);
        }
        LeaveCriticalSection(&g_psCs);
        return rv;
    }
    LONG64 v0 = qpcNow();
    int rv = o_psVm(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    LONG64 v1 = qpcNow();
    g_psVmTicks += v1 - v0; g_psVmN++;
    InterlockedIncrement(&g_psMiss);
    DWORD dl = 0;
    __try { dl = psDiff(r, g_psBlob + outStart, g_psPost, PS_BLOBMAX); } __except (EXCEPTION_EXECUTE_HANDLER) { dl = 0xFFFFFFFF; }
    if (dl != 0xFFFFFFFF) {
        PsSlot* s = freeS ? freeS : &g_psTab[si];
        BYTE* kb = psAlloc(blobLen + dl);                       // (a reset in there empties the table; s stays a valid slot)
        memcpy(kb, g_psBlob, blobLen); memcpy(kb + blobLen, g_psPost, dl);
        s->hash = hash; s->blobOff = (DWORD)(kb - g_psArena); s->outOff = s->blobOff + blobLen; s->outLen = dl; s->blobLen = blobLen;
    }
    g_psMissTicks += qpcNow() - t0 - (v1 - v0);
    LeaveCriticalSection(&g_psCs);
    return rv;
}

static DWORD WINAPI psReport(LPVOID) {
    LONG lc = 0, lh = 0, lm = 0, lu = 0, lb = 0, lk = 0; LONG64 lvt = 0, lvn = 0, lht = 0, lmt = 0;
    for (;;) {
        Sleep(30000);
        LONG c = g_psCalls, h = g_psHits, m = g_psMiss, u = g_psUncache, b = g_psBusy, k = g_psChecked; LONG64 vt = g_psVmTicks, vn = g_psVmN, ht = g_psHitTicks, mt = g_psMissTicks;
        if (c == lc) continue;
        LONG64 dvn = vn - lvn; LONG64 vmUs = dvn ? (vt - lvt) * 1000000 / qpcHz() / dvn * 1 : 0;      // microseconds a VM run, x1
        LONG64 vmNs = dvn ? (vt - lvt) * 1000000000 / qpcHz() / dvn : 0;
        LONG64 hitNs = (h - lh) ? (ht - lht) * 1000000000 / qpcHz() / (h - lh) : 0;
        LONG64 missNs = (m - lm) ? (mt - lmt) * 1000000000 / qpcHz() / (m - lm) : 0;
        LONG64 savedUs = (LONG64)(h - lh - (k - lk)) * (vmNs - hitNs) / 1000 - (LONG64)(m - lm) * missNs / 1000;   // over the 30 s
        const char* who = g_psLastTid == g_rtWorkerTid && g_rtWorkerTid ? "the render worker" : "the game thread";
        logf("preshader: last 30 s - %d VM calls (on %s): %d hits (every byte of the key compared), %d misses, %d not cacheable, %d while busy | checked against the VM %d, MISMATCH %d%s | one VM run %d.%03d us, a hit %d.%03d us, a miss adds %d.%03d us | net saved %d.%d ms a second on that thread | table resets %d",
             (int)(c - lc), who, (int)(h - lh), (int)(m - lm), (int)(u - lu), (int)(b - lb), (int)(k - lk), (int)g_psMismatch, g_psKill ? " - the cache is OFF" : "",
             (int)(vmNs / 1000), (int)(vmNs % 1000), (int)(hitNs / 1000), (int)(hitNs % 1000), (int)(missNs / 1000), (int)(missNs % 1000),
             (int)(savedUs / 30000), (int)((savedUs / 3000) % 10 < 0 ? -((savedUs / 3000) % 10) : (savedUs / 3000) % 10), (int)g_psResets);
        (void)vmUs;
        lc = c; lh = h; lm = m; lu = u; lb = b; lk = k; lvt = vt; lvn = vn; lht = ht; lmt = mt;
    }
}

static void installPreshaderCache() {
    char env[8]; env[0] = 0;
    // v33 measurement (work\ish7\r1, a real replay, 357 745 VM calls in 30 s, all on the render worker): the cache is exact
    // (MISMATCH 0 in 9 480 re-runs) but one VM run costs 0.626 us and a hit 0.647 us - the VM is cheaper than reading its
    // own inputs. It saves nothing (-5.1 ms a second on the worker, 0 on the game thread), so it is not installed.
    // AOTR_PRESHADER=1 installs it for a measurement.
    if (!(GetEnvironmentVariableA("AOTR_PRESHADER", env, sizeof(env)) && env[0] == '1')) { logf("preshader: the cache is not installed (v33: measured exact but slower than the VM itself - a hit 0.647 us against a VM run 0.626 us; AOTR_PRESHADER=1 for a measurement)."); return; }
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_PSCHECK", env, sizeof(env)) && env[0] == '1') g_psCheckAll = 1;
    HMODULE h = GetModuleHandleA("d3dx9_27.dll");
    if (!h) { logf("preshader: d3dx9_27.dll is not loaded - the cache is not installed."); return; }
    BYTE* target = (BYTE*)h + 0xE8943;
    static const BYTE sig[] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x1C, 0x0C, 0x00, 0x00, 0x8B, 0x45, 0x0C, 0x8B, 0x00 };
    if (memcmp(target, sig, sizeof(sig)) != 0) { logf("preshader: d3dx9_27.dll+E8943 is not the VM this cache was built from - not installed."); return; }
    g_psTab = (PsSlot*)VirtualAlloc(NULL, sizeof(PsSlot) * PS_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_psArena = (BYTE*)VirtualAlloc(NULL, PS_ARENA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_psBlob = (BYTE*)VirtualAlloc(NULL, PS_BLOBMAX * 2 + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_psTab || !g_psArena || !g_psBlob) { logf("preshader: no memory for the cache - not installed."); return; }
    g_psPost = g_psBlob + PS_BLOBMAX + 1024;
    InitializeCriticalSectionAndSpinCount(&g_psCs, 0);
    o_psVm = (tPsVm)makeTrampoline(target, 5);
    if (!o_psVm) { logf("preshader: no trampoline - not installed."); return; }
    HANDLE susp[256]; int ns = 0; suspendOthers(susp, &ns, 256);
    patchJmp(target, (void*)&psHook, 5);
    resumeAll(susp, ns);
    g_psOn = 1;
    CreateThread(NULL, 0, psReport, NULL, 0, NULL);
    logf("preshader: cache ON - key = the 14 arguments, the x87 control word, the program bytes, the literals and every byte of all six banks (a2 included); every hit compared in full; the first 4096 hits and 1 in 32 after them re-run the VM and compare (one difference = off for the session)%s.", g_psCheckAll ? "; AOTR_PSCHECK=1: EVERY hit re-run and compared" : "");
}
