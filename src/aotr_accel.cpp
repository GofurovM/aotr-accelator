// aotr_accel.dll - stage 1: locate and measure the pathfinding phase of the SAGE main thread.
//
// Design constraints (must survive Age of the Ring / 2.02 updates):
//   * Nothing is written into the game folder. The AotR launcher deletes stray files and
//     verifies checksums on every start, so we inject at runtime and touch only memory.
//   * game.dat on disk is never modified, so the launcher's checksum always passes.
//   * Functions are found by wildcarded byte signature, never by hardcoded address.
//     If a signature is missing or ambiguous we install nothing and log it: after a game
//     update the worst case is "no acceleration", never a crash.
//
// Stage 1 measures AIUpdateInterface::doPathfind (positively identified by its 13
// "CritterDesync: doPathfindN" strings) and records the caller's return address, which
// resolves Pathfinder::processPathfindQueue - the offload target for stage 2.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
extern "C" {
#include "rpmalloc.h"
}

// ---------------------------------------------------------------- where this DLL lives
// The log, the rotated logs, the marker files and the frame captures all sit in the folder the DLL itself
// was loaded from, so the whole thing can be dropped anywhere. Resolved once in DllMain, before any logging.
static char g_dir[MAX_PATH] = "";
static char kLogPath[MAX_PATH] = "";
static volatile LONG g_engineHooks = 0;   // may this build's absolute addresses be patched?
static void (__stdcall* g_twDrawTag)(DWORD mesh, DWORD count) = NULL;   // in-between frames (aotr_tween.inc): which meshes a draw is
static void (*g_twViewsEnd)() = NULL;                                    // in-between frames: the views are drawn, the interface follows
static void (*g_scrFrame)(LONG64 dt) = NULL;                              // steady scroll speed (aotr_scroll.inc): the time since the frame before
static void (*g_lpFrame)(LONG64 dt) = NULL;                               // the pan profiler (aotr_loadprof.inc): each frame's length
static volatile DWORD g_lpPanTick = 0;                                    // when the camera was last scrolled (GetTickCount; aotr_scroll.inc tells)
static void aotrSetDir(HMODULE self) {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, p, MAX_PATH);
    if (!n || n >= MAX_PATH) lstrcpynA(g_dir, ".", MAX_PATH);
    else {
        while (n && p[n - 1] != '\\' && p[n - 1] != '/') --n;
        p[n ? n - 1 : 0] = 0;
        lstrcpynA(g_dir, p, MAX_PATH);
    }
    wsprintfA(kLogPath, "%s\\bfme2_accel.log", g_dir);
}
static void logf(const char* fmt, ...);
static volatile LONG g_invParam = 0;
static void __cdecl aotrInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    if (InterlockedIncrement(&g_invParam) <= 3) logf("crt: a text buffer in this DLL was too small - the line was cut instead of closing the game (please report this line).");
}
static const char* aotrPath(char* out, const char* name) { wsprintfA(out, "%s\\%s", g_dir, name); return out; }

// [Engine] <key> from the INI that sits beside this DLL, or `def` when the file is not there.
// Every portable component reads its switch through here, so each one can be bisected from the INI alone.
static int iniWant(const char* key, int def) {
    char p[MAX_PATH];
    if (GetFileAttributesA(aotrPath(p, "bfme2_accel.ini")) == INVALID_FILE_ATTRIBUTES) return def;
    return (int)GetPrivateProfileIntA("Engine", key, def, p);
}
// [Engine] <key> as text (v28: Engine = core / full). Empty when the key is not there.
static bool iniStr(const char* key, char* out, DWORD cap) {
    char p[MAX_PATH];
    out[0] = 0;
    if (GetFileAttributesA(aotrPath(p, "bfme2_accel.ini")) == INVALID_FILE_ATTRIBUTES) return false;
    GetPrivateProfileStringA("Engine", key, "", out, cap, p);
    return out[0] != 0;
}

// ---------------------------------------------------------------- production build switch
// AOTR_PROD removes everything that exists only to be reported: the counters on the hot paths (85,000 queued
// records and 37,000 effect parameter writes per battle frame, three volatile stores each) and the background
// threads that print them every few seconds. Nothing that changes what the game does is behind this switch -
// every optimisation, and every self-check that keeps one honest, stays in. Install lines and the crash log stay
// too: they are written once, not per frame.
#ifdef AOTR_PROD
#define PSTAT(...) ((void)0)
#else
#define PSTAT(...) do { __VA_ARGS__; } while (0)
#endif

// ---------------------------------------------------------------- release build and diagnostics build (v70)
// The release build (build.bat) has no read-out on the screen, no session report and no measurement hooks; the log
// keeps the start-up lines (what was installed) and, after that, only problems (a self-check that turned a part off,
// the freeze guard, a crash). The diagnostics build (build.bat diag, AOTR_DIAG) puts all of that back.
// Self-checks: every optimisation still proves itself against the engine's own code for its first answers of the
// session (the "proof" counts). Only the diagnostics build keeps re-checking a sample (1 in 64 and so on) afterwards.
#ifdef AOTR_DIAG
#define AOTR_STEADY(x) (x)
#else
#define AOTR_STEADY(x) (false)
#endif

// ---------------------------------------------------------------- logging
static CRITICAL_SECTION g_logCs;

// v71: a line that says a part switched itself off (or the render thread stepped aside) makes the status badge yellow
// (aotr_badge.inc). These phrases are used by exactly those lines - checked against every logf format in the tree.
static volatile LONG g_bdgProblems = 0;                  // such lines so far this session
static char g_bdgWhy[48] = "";                           // the part that wrote the first one ("equiv-memo", "rt", ...)
static bool logIsProblem(const char* fmt) {
    static const char* const kProblem[] = { "rest of this session", "rest of the session", "for this session", "SELF-CHECK", "MISMATCH",
                                            "FREEZE GUARD", "SAFE MODE" };
    for (unsigned i = 0; i < sizeof(kProblem) / sizeof(kProblem[0]); ++i) if (strstr(fmt, kProblem[i])) return true;
    return false;
}
#ifndef AOTR_DIAG
// v70: after start-up the release log takes only lines that report a problem, at most 2000 of them a session.
static volatile LONG g_logQuiet = 0, g_logQuietLines = 0;
static bool logWorthKeeping(const char* fmt) {
    static const char* const kKeep[] = { "crash:", "HANG?", "FATAL", "GUARD:" };
    if (logIsProblem(fmt)) return true;
    for (unsigned i = 0; i < sizeof(kKeep) / sizeof(kKeep[0]); ++i) if (strstr(fmt, kKeep[i])) return true;
    return false;
}
#endif
static void logf(const char* fmt, ...) {
    if (logIsProblem(fmt) && InterlockedIncrement(&g_bdgProblems) == 1) {
        int k = 0; while (fmt[k] && fmt[k] != ':' && k < (int)sizeof(g_bdgWhy) - 1) { g_bdgWhy[k] = fmt[k]; ++k; } g_bdgWhy[k] = 0;
    }
#ifndef AOTR_DIAG
    if (g_logQuiet && (!logWorthKeeping(fmt) || InterlockedIncrement(&g_logQuietLines) > 2000)) return;
#endif
    char line[1100];                                 // wvsprintfA writes at most 1024 characters: a longer line is cut short, never past the buffer
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = wsprintfA(line, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(line + n, fmt, ap);
    va_end(ap);
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileA(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

#include "aotr_capture.inc"

// ---------------------------------------------------------------- signature scanning
// Mask: 'x' = byte must match, '?' = wildcard.
static BYTE* scanOnce(BYTE* base, SIZE_T size, const BYTE* pat, const char* mask, int* outCount) {
    SIZE_T patLen = strlen(mask);
    BYTE* first = NULL;
    int count = 0;
    if (patLen == 0 || size < patLen) { *outCount = 0; return NULL; }
    for (SIZE_T i = 0; i + patLen <= size; ++i) {
        BYTE* p = base + i;
        SIZE_T j = 0;
        for (; j < patLen; ++j) {
            if (mask[j] == 'x' && p[j] != pat[j]) break;
        }
        if (j == patLen) {
            if (!first) first = p;
            if (++count > 1) break;   // ambiguity is fatal; stop early
        }
    }
    *outCount = count;
    return first;
}

// AIUpdateInterface::doPathfind prologue.
//   B8 ?? ?? ?? ??   mov  eax, <SEH scope table>
//   E8 ?? ?? ?? ??   call __EH_prolog
//   83 EC 4C         sub  esp,4Ch
//   53 56            push ebx / push esi
//   8B D9            mov  ebx,ecx
//   57               push edi
//   8B 7B 08         mov  edi,[ebx+8]
//   8B 47 04         mov  eax,[edi+4]
// The two immediates are wildcarded; the rest is strict so a changed build fails closed.
static const BYTE kSigDoPathfind[] = {
    0xB8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0,
    0x83, 0xEC, 0x4C, 0x53, 0x56, 0x8B, 0xD9, 0x57, 0x8B, 0x7B, 0x08, 0x8B, 0x47, 0x04
};
static const char kMaskDoPathfind[] = "x????x????xxxxxxxxxxxxxx";

// ---------------------------------------------------------------- inline detour (5-byte jmp rel32)
static BYTE* makeTrampoline(BYTE* target, int stolen) {
    BYTE* t = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, target, stolen);
    t[stolen] = 0xE9;
    *(DWORD*)(t + stolen + 1) = (DWORD)(ULONG_PTR)(target + stolen) - (DWORD)(ULONG_PTR)(t + stolen + 5);
    return t;
}

static BOOL patchJmp(BYTE* target, void* dest, int stolen) {
    DWORD oldProt = 0;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProt)) return FALSE;
    target[0] = 0xE9;
    *(DWORD*)(target + 1) = (DWORD)(ULONG_PTR)dest - (DWORD)(ULONG_PTR)(target + 5);
    for (int i = 5; i < stolen; ++i) target[i] = 0x90;
    VirtualProtect(target, stolen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return TRUE;
}

// Freeze every other thread while the 5 bytes are swapped, so we can never patch
// underneath a thread that is mid-instruction at the target.
static void suspendOthers(HANDLE* out, int* n, int max) {
    *n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me && *n < max) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (h) { SuspendThread(h); out[(*n)++] = h; }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
static void resumeAll(HANDLE* h, int n) {
    for (int i = 0; i < n; ++i) { ResumeThread(h[i]); CloseHandle(h[i]); }
}


// ---------------------------------------------------------------- D3DX9 effect parameter-apply dedup
// d3dx9_27 sub_10863A applies ONE effect parameter's current value to the device, called per param
// per draw batch. Camera/projection/lights are re-applied every batch unchanged. We resolve the
// parameter (via the effect's own handle resolver sub_1085EF), hash its value bytes, and SKIP the
// apply when (collection,handle,value) is identical to last time. SEH-guarded; global auto-off.
typedef void* (__fastcall* tResolve)(void*, void*, void*, void*); // (ecx=collection, edx unused, handle, 0)
typedef int   (__stdcall*  tApply)(void*, void*);            // (collection, handle) -> HRESULT
static tResolve o_resolve = NULL;
static tApply   o_apply   = NULL;
static volatile LONG g_apHit=0, g_apMiss=0, g_apOff=0;
#define AP_CACHE 32768
struct ApEnt { void* coll; void* handle; DWORD hash; };
static ApEnt g_ap[AP_CACHE];

static DWORD apHash(void* p){
    DWORD h=2166136261u;
    __try {
        const BYTE* b=(const BYTE*)p;
        for(int i=0;i<64;i++) h=(h^b[i])*16777619u;             // inline value / header
        DWORD q=*(DWORD*)((const BYTE*)p+0x14);                  // some types hold value via [p+0x14]
        if(q>0x10000 && q<0x7F000000){ const BYTE* r=(const BYTE*)q; for(int i=0;i<64;i++) h=(h^r[i])*16777619u; }
    } __except(EXCEPTION_EXECUTE_HANDLER){ return 0; }
    return h ? h : 1;
}

static int __stdcall my_apply(void* coll, void* handle){
    if (InterlockedCompareExchange(&g_apOff,0,0)) return o_apply(coll,handle);
    ApEnt* e=NULL; DWORD hh=0, key=0; void* param=NULL;
    __try {
        param = o_resolve(coll, 0, handle, 0);   // handle + extra 0 flag = 2 stack args (ret 8)
        if (param){ hh = apHash(param);
            if (hh){ key = ((DWORD)(ULONG_PTR)coll) ^ (((DWORD)(ULONG_PTR)handle)*2654435761u); e=&g_ap[key%AP_CACHE]; } }
    } __except(EXCEPTION_EXECUTE_HANDLER){ e=NULL; }
    if (e && e->coll==coll && e->handle==handle && e->hash==hh){ InterlockedIncrement(&g_apHit); return 0; }
    int r = o_apply(coll,handle);
    if (e){ e->coll=coll; e->handle=handle; e->hash=hh; }
    InterlockedIncrement(&g_apMiss);
    return r;
}

static DWORD WINAPI applyReport(LPVOID){
    for(;;){ Sleep(2000);
        LONG hit=InterlockedExchange(&g_apHit,0), miss=InterlockedExchange(&g_apMiss,0);
        int tot=hit+miss; int hp=tot?(int)(100*hit/tot):0;
        logf("apply-dedup: %d applies/2s, %d%% skipped (%d skip/%d ran) %s", tot, hp,(int)hit,(int)miss,
             InterlockedCompareExchange(&g_apOff,0,0)?"[OFF]":"[live]");
    }
    return 0;
}

static void installApplyDedup(){
    HMODULE h = GetModuleHandleA("d3dx9_27.dll");
    if (!h){ logf("apply-dedup: d3dx9_27 not loaded - OFF."); return; }
    BYTE* apply   = (BYTE*)h + 0x10863A;
    o_resolve = (tResolve)((BYTE*)h + 0x1085EF);
    static const BYTE sig[] = {0x8B,0xFF,0x55,0x8B,0xEC};   // mov edi,edi; push ebp; mov ebp,esp
    if (memcmp(apply, sig, sizeof(sig)) != 0){ logf("apply-dedup: prologue mismatch - OFF."); return; }
    o_apply = (tApply)makeTrampoline(apply, 5);
    if (!o_apply){ logf("apply-dedup: trampoline failed - OFF."); return; }
    HANDLE susp[256]; int n=0; suspendOthers(susp,&n,256);
    patchJmp(apply,(void*)&my_apply,5);
    resumeAll(susp,n);
    CreateThread(NULL,0,applyReport,NULL,0,NULL);
    logf("apply-dedup: LIVE @ d3dx9+0x10863A. Skips unchanged per-batch parameter applies.");
}

// ---------------------------------------------------------------- measurement state
typedef int (__fastcall* tDoPathfind)(void* ecx, void* edx, void* pathfinder);
static tDoPathfind o_doPathfind = NULL;

static volatile LONG  g_calls = 0;         // doPathfind invocations
static volatile LONG64 g_ticks = 0;        // QPC ticks spent inside it
static volatile LONG  g_reentry = 0;       // nested invocations seen
static DWORD          g_tlsDepth = TLS_OUT_OF_INDEXES;

#define MAX_CALLERS 24
static struct { DWORD addr; LONG count; } g_callers[MAX_CALLERS];
static CRITICAL_SECTION g_cs;

static void recordCaller(DWORD ra) {
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAX_CALLERS; ++i) {
        if (g_callers[i].addr == ra) { g_callers[i].count++; break; }
        if (g_callers[i].addr == 0)  { g_callers[i].addr = ra; g_callers[i].count = 1; break; }
    }
    LeaveCriticalSection(&g_cs);
}

// The detour is reached by JMP, so [esp] still holds the original caller's return address:
// _ReturnAddress() here is an address inside Pathfinder::processPathfindQueue.
static int __fastcall hk_doPathfind(void* ecx, void* edx, void* pathfinder) {
    DWORD ra = (DWORD)(ULONG_PTR)_ReturnAddress();
    LARGE_INTEGER a, b;

    // Only time the outermost invocation so nested calls are not double counted.
    INT_PTR depth = (INT_PTR)TlsGetValue(g_tlsDepth);
    TlsSetValue(g_tlsDepth, (LPVOID)(depth + 1));
    if (depth != 0) InterlockedIncrement(&g_reentry);

    QueryPerformanceCounter(&a);
    int r = o_doPathfind(ecx, edx, pathfinder);
    QueryPerformanceCounter(&b);

    TlsSetValue(g_tlsDepth, (LPVOID)depth);
    if (depth == 0) {
        InterlockedExchangeAdd64((volatile LONG64*)&g_ticks, b.QuadPart - a.QuadPart);
    }
    InterlockedIncrement(&g_calls);
    recordCaller(ra);
    return r;
}

// ---------------------------------------------------------------- particle subsystem (offload target)
// W3DFXParticleSystem.cpp per-frame entry at 0044C813 (2.02/AotR): __thiscall(this, arg1) -
// prologue push ebp/mov ebp,esp/push esi/mov esi,ecx, terminates in 'leave; ret 4' (1 arg).
// Stage 1 (here): time it. Stage 2 (next) splits its per-particle work across worker cores.
// 24 template-clone functions share the generic prologue and differ only in the call's rel32,
// so for the measurement build we pin the exact bytes (rel32 included) - unique to 0044C813 on
// this game.dat. If an update moves it, the scan fails closed (no measurement, never a crash).
static const BYTE kSigParticle[] = {
    0x55,0x8B,0xEC,0x56,0x8B,0xF1,0x8B,0x46,0x04,0x3B,0x46,0x08,0x74,0x11,0xFF,0x75,0x08,0x50,0xE8,
    0xF6,0xFC,0xFF,0xFF,0x83,0x46,0x04,0x0C,0x59,0x59,0xEB,0x13,0x6A
};
static const char kMaskParticle[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

typedef int (__fastcall* tParticle)(void* ecx, void* edx, void* arg1);
static tParticle o_particle = NULL;
static volatile LONG   g_partCalls = 0;
static volatile LONG64 g_partTicks = 0;   // written only from the main thread; plain ops are safe

static int __fastcall hk_particle(void* ecx, void* edx, void* arg1) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    int r = o_particle(ecx, edx, arg1);
    QueryPerformanceCounter(&b);
    g_partTicks += (b.QuadPart - a.QuadPart);
    g_partCalls++;
    return r;
}

// ---------------------------------------------------------------- reporting
static DWORD WINAPI reportThread(LPVOID) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    LONG   lastCalls = 0;
    LONG64 lastTicks = 0;
    LONG   lastPCalls = 0;
    LONG64 lastPTicks = 0;
    for (;;) {
        Sleep(2000);
        LONG   c = g_calls;
        LONG64 t = g_ticks;
        LONG   dc = c - lastCalls;
        LONG64 dt = t - lastTicks;
        lastCalls = c; lastTicks = t;

        double ms = (double)dt * 1000.0 / (double)freq.QuadPart;
        double pct = ms / 2000.0 * 100.0;                       // share of wall time
        double avgUs = dc ? (ms * 1000.0 / dc) : 0.0;

        char callers[512]; callers[0] = 0;
        EnterCriticalSection(&g_cs);
        for (int i = 0; i < MAX_CALLERS && g_callers[i].addr; ++i) {
            char one[64];
            wsprintfA(one, "%08X x%d  ", g_callers[i].addr, g_callers[i].count);
            if (strlen(callers) + strlen(one) < sizeof(callers) - 1) strcat(callers, one);
        }
        LeaveCriticalSection(&g_cs);

        logf("doPathfind: %d calls/2s (%d total), %d.%03d ms in window = %d.%02d%% of wall, avg %d.%02d us, reentry=%d",
             dc, c,
             (int)ms, (int)((ms - (int)ms) * 1000),
             (int)pct, (int)((pct - (int)pct) * 100),
             (int)avgUs, (int)((avgUs - (int)avgUs) * 100),
             g_reentry);
        if (callers[0]) logf("   callers (return addresses inside processPathfindQueue): %s", callers);

        LONG64 pt = g_partTicks; LONG pc = g_partCalls;
        LONG64 dpt = pt - lastPTicks; LONG dpc = pc - lastPCalls;
        lastPTicks = pt; lastPCalls = pc;
        if (o_particle) {
            double pms  = (double)dpt * 1000.0 / (double)freq.QuadPart;
            double ppct = pms / 2000.0 * 100.0;
            double pavg = dpc ? (pms * 1000.0 / dpc) : 0.0;
            logf("particle: %d calls/2s, %d.%03d ms in window = %d.%02d%% of wall, avg %d.%02d us/frame",
                 dpc, (int)pms, (int)((pms - (int)pms) * 1000),
                 (int)ppct, (int)((ppct - (int)ppct) * 100),
                 (int)pavg, (int)((pavg - (int)pavg) * 100));
        }
    }
    return 0;
}

// ================================================================ allocator swap (stage 1b)
// game.dat imports exactly malloc/calloc/realloc/free from msvcr71.dll (verified from its
// import table). We reroute those four to rpmalloc, a modern lock-free thread-caching
// allocator, by overwriting their slots in game.dat's import table (IAT). Every block we
// hand back is preceded by a 16-byte tag, so free/realloc can tell our pointers apart from
// ones the game allocated before we installed - those are forwarded to the original msvcr71
// routine. Determinism-safe: heap addresses never feed the per-frame sync CRC. Nothing on
// disk changes; this is pure in-memory redirection undone when the process exits.

static const unsigned kA0 = 0x52504D41u;   // "RPMA"
static const unsigned kA1 = 0x6C6C6F63u;   // "lloc"  -> 8-byte ownership tag
#define AHDR 16

typedef void* (__cdecl* t_malloc )(size_t);
typedef void* (__cdecl* t_calloc )(size_t, size_t);
typedef void* (__cdecl* t_realloc)(void*, size_t);
typedef void  (__cdecl* t_free   )(void*);
static t_malloc  o_malloc  = 0;
static t_calloc  o_calloc  = 0;
static t_realloc o_realloc = 0;
static t_free    o_free    = 0;

static volatile LONG g_aAllocs = 0, g_aFrees = 0, g_aForwarded = 0;

static __forceinline void aThread() {
    if (!rpmalloc_is_thread_initialized()) rpmalloc_thread_initialize();
}
static __forceinline int aOwns(void* p) {
    unsigned* h = (unsigned*)((char*)p - AHDR);
    return h[0] == kA0 && h[1] == kA1;
}
static __forceinline void* aTag(void* base) {
    if (!base) return 0;
    unsigned* h = (unsigned*)base;
    h[0] = kA0; h[1] = kA1;
    return (char*)base + AHDR;
}

static void* __cdecl my_malloc(size_t n) {
    if (n > (size_t)-1 - AHDR) return 0;
    aThread();
    InterlockedIncrement(&g_aAllocs);
    return aTag(rpmalloc(n + AHDR));
}
static void* __cdecl my_calloc(size_t a, size_t b) {
    size_t n = a * b;
    if (a && n / a != b) return 0;             // multiply overflow
    if (n > (size_t)-1 - AHDR) return 0;
    aThread();
    InterlockedIncrement(&g_aAllocs);
    void* p = aTag(rpmalloc(n + AHDR));
    if (p) memset(p, 0, n);
    return p;
}
static void* __cdecl my_realloc(void* p, size_t n) {
    if (!p) return my_malloc(n);
    if (aOwns(p)) {
        // msvcr71's realloc(p, 0) frees the block and returns NULL. With the tag's 16 bytes added, a zero used to
        // go on as a request for 16 bytes: the caller got a block back that it takes to be gone.
        if (!n) { aThread(); InterlockedIncrement(&g_aFrees); rpfree((char*)p - AHDR); return 0; }
        if (n > (size_t)-1 - AHDR) return 0;
        aThread();
        return aTag(rprealloc((char*)p - AHDR, n + AHDR));
    }
    InterlockedIncrement(&g_aForwarded);
    return o_realloc(p, n);                     // allocated before our hook -> leave with CRT
}
static void __cdecl my_free(void* p) {
    if (!p) return;
    if (aOwns(p)) { aThread(); InterlockedIncrement(&g_aFrees); rpfree((char*)p - AHDR); }
    else { InterlockedIncrement(&g_aForwarded); o_free(p); }
}
// The four routines above at their edges, where msvcr71's answers are known (edge_test.exe shows them side by side).
// Returns the number of failures; a line for every check goes to out.
#ifdef AOTR_SELFTEST   // offline-test export: built only into the test DLL
extern "C" __declspec(dllexport) int __cdecl AotrAllocTest(char* out, int n) {
    int fail = 0, at = 0; if (out && n > 0) out[0] = 0;
    #define AT_CHECK(cond, what) do { bool ok_ = (cond); if (!ok_) ++fail; if (out && at < n - 160) at += wsprintfA(out + at, "%s  %s\n", ok_ ? "ok  " : "FAIL", what); } while (0)
    rpmalloc_initialize();                                   // in the game this was done at install; a second call changes nothing
    unsigned char* a = (unsigned char*)my_calloc(8, 8);
    bool zero = a != NULL; if (a) for (int i = 0; i < 64; ++i) if (a[i]) zero = false;
    AT_CHECK(zero && aOwns(a), "calloc(8, 8): a block of ours, 64 zero bytes");
    if (a) memset(a, 0x5A, 64);
    unsigned char* b = a ? (unsigned char*)my_realloc(a, 4096) : NULL;
    bool kept = b != NULL; if (b) for (int i = 0; i < 64; ++i) if (b[i] != 0x5A) kept = false;
    AT_CHECK(kept && aOwns(b), "realloc to 4096 bytes: the first 64 kept");
    void* big = b ? my_realloc(b, (size_t)-1) : (void*)1;
    bool still = b != NULL; if (b) for (int i = 0; i < 64; ++i) if (b[i] != 0x5A) still = false;
    AT_CHECK(big == NULL && still, "realloc to a size that cannot be: NULL, and the block is still whole");
    LONG frees = g_aFrees;
    void* gone = b ? my_realloc(b, 0) : (void*)1;
    AT_CHECK(gone == NULL && g_aFrees == frees + 1, "realloc(block, 0): the block is freed and NULL comes back");
    if (gone && gone != (void*)1) my_free(gone);
    AT_CHECK(my_calloc((size_t)-1, 2) == NULL, "calloc whose product overflows: NULL");
    AT_CHECK(my_malloc((size_t)-1) == NULL, "malloc of a size that cannot be: NULL");
    void* m0 = my_malloc(0); AT_CHECK(m0 != NULL && aOwns(m0), "malloc(0): a block"); my_free(m0);
    void* r0 = my_realloc(NULL, 0); AT_CHECK(r0 != NULL && aOwns(r0), "realloc(NULL, 0): a block, as malloc(0)"); my_free(r0);
    my_free(NULL);
    #undef AT_CHECK
    return fail;
}
#endif

// Locate the writable IAT slot for base!dll.fn (the pointer to patch). We match by the
// function's RESOLVED address, not by walking import-name strings: at runtime the FirstThunk
// (IAT) already holds resolved addresses, and if a descriptor has no INT (OriginalFirstThunk
// == 0) there are no name RVAs to read - trying to would dereference an address as an RVA and
// crash. Reading the IAT values and comparing to GetProcAddress is safe and INT-independent.
static FARPROC* iatSlot(BYTE* base, const char* dll, const char* fn) {
    HMODULE hmod = GetModuleHandleA(dll);
    if (!hmod) return 0;
    FARPROC tgt = GetProcAddress(hmod, fn);
    if (!tgt) return 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; ft->u1.Function; ++ft) {
            if ((FARPROC)(ULONG_PTR)ft->u1.Function == tgt) return (FARPROC*)&ft->u1.Function;
        }
    }
    return 0;
}
static bool hookSlot(FARPROC* slot, void* fn, void** orig) {
    DWORD op;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &op)) return false;
    if (orig) *orig = (void*)*slot;
    *slot = (FARPROC)fn;
    VirtualProtect(slot, sizeof(void*), op, &op);
    return true;
}

static DWORD WINAPI allocReport(LPVOID) {
    LONG last = 0;
    for (;;) {
        Sleep(2000);
        LONG a = g_aAllocs, fr = g_aFrees, fw = g_aForwarded;
        LONG d = a - last; last = a;
        logf("alloc: %d/2s via rpmalloc (%d total, frees=%d, forwarded-to-CRT=%d)", d, a, fr, fw);
    }
    return 0;
}

static void installAllocatorSwap(BYTE* base) {
    const char* M = "msvcr71.dll";
    HANDLE frozen[64]; int nf = 0; bool suspended = false;
    // Belt-and-suspenders: any fault while parsing the game's import table or bringing up
    // rpmalloc is caught here, threads are resumed, and we run without the swap. The
    // allocator can never take the game down - worst case is "no speedup".
    __try {
        FARPROC* sMalloc  = iatSlot(base, M, "malloc");
        FARPROC* sCalloc  = iatSlot(base, M, "calloc");
        FARPROC* sRealloc = iatSlot(base, M, "realloc");
        FARPROC* sFree    = iatSlot(base, M, "free");
        if (!sMalloc || !sFree || !sRealloc) {
            logf("alloc: malloc/free/realloc imports not found - allocator swap SKIPPED (game runs normally).");
            return;
        }
        if (rpmalloc_initialize() != 0) { logf("alloc: rpmalloc_initialize failed - SKIPPED."); return; }
        rpmalloc_thread_initialize();

        // Freeze other threads so no allocation happens mid-swap. Install consumers (free,
        // realloc) before producers (malloc, calloc) so our tagged blocks can never reach the
        // real free/realloc even for an instant.
        suspendOthers(frozen, &nf, 64); suspended = true;
        bool ok = hookSlot(sFree,    (void*)&my_free,    (void**)&o_free)
               && hookSlot(sRealloc, (void*)&my_realloc, (void**)&o_realloc)
               && hookSlot(sMalloc,  (void*)&my_malloc,  (void**)&o_malloc);
        if (ok && sCalloc) ok = hookSlot(sCalloc, (void*)&my_calloc, (void**)&o_calloc);
        resumeAll(frozen, nf); suspended = false;

        if (!ok) { logf("alloc: IAT patch failed - leaving allocator as-is."); return; }
        logf("alloc: rpmalloc installed on malloc/calloc/realloc/free (froze %d threads). Live.", nf);
#ifndef AOTR_PROD
        CreateThread(NULL, 0, allocReport, NULL, 0, NULL);
#endif
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (suspended) resumeAll(frozen, nf);
        logf("alloc: install faulted (0x%08lX) - allocator swap skipped, game runs normally.",
             (unsigned long)GetExceptionCode());
    }
}

// ============================================================ shadow-volume offload (stage 2)
// Detour renderShadows (0x4F43A6): pre-compute every shadow's Update (0x4F3906) across a thread
// pool, then let the original renderShadows run - its per-shadow Update() calls early-out via
// g_shadowsDone, and it only does the D3D stencil render of the now-ready volumes. Cold shadows
// are warmed serially first (the shared per-geometry caches m_polygonNormals/m_polyNeighbors are
// lazily built once); warm shadows update in parallel (read-only on shared geometry). rpmalloc
// (stage 1b) makes the per-Update allocations thread-safe. Determinism-safe: shadow geometry is
// visual only, never in the lockstep sync CRC. SEH-guarded: any fault -> stock path, never a crash.
// All addresses/offsets verified against rotwk/game.dat (see SHADOW-OFFLOAD.md).

static const BYTE kSigRenderShadows[] = {0xB8,0xCF,0x7E,0xB7,0x00,0xE8,0x40,0x8B,0x54,0x00,0x83,0xEC,0x24,0x53,0x56,0x57};
static const char kMaskRenderShadows[] = "xxxxxxxxxxxxxxxx";
static const BYTE kSigShadowUpdate[]  = {0x55,0x8B,0xEC,0x83,0xEC,0x18,0xF6,0x05,0x14,0x1A,0xDD,0x00,0x01,0x53,0x56,0x57};
static const char kMaskShadowUpdate[] = "xxxxxxxxxxxxxxxx";
#define SHADOW_NEXT_OFF 0x68
#define SHADOW_MAX      4096

typedef void (__fastcall* tRenderShadows)(void* ecx, void* edx, void* arg);
typedef void (__fastcall* tShadowUpdate )(void* ecx, void* edx, int arg);
static tRenderShadows o_renderShadows = NULL;
static tShadowUpdate  o_shadowUpdate  = NULL;
static volatile LONG  g_shadowsDone   = 0;   // set while the stock renderShadows runs -> its Update()s skip

// warmed-geometry set (open addressing, pointer keys) - touched only from the main render thread
#define WARM_SLOTS 32768
static void* g_warm[WARM_SLOTS];
static bool warmContains(void* p) {
    unsigned h = (unsigned)(((ULONG_PTR)p) >> 4) & (WARM_SLOTS - 1);
    for (unsigned i = 0; i < WARM_SLOTS; ++i) { unsigned j = (h + i) & (WARM_SLOTS - 1);
        if (g_warm[j] == p) return true; if (!g_warm[j]) return false; }
    return false;
}
static void warmAdd(void* p) {
    unsigned h = (unsigned)(((ULONG_PTR)p) >> 4) & (WARM_SLOTS - 1);
    for (unsigned i = 0; i < WARM_SLOTS; ++i) { unsigned j = (h + i) & (WARM_SLOTS - 1);
        if (!g_warm[j] || g_warm[j] == p) { g_warm[j] = p; return; } }
}

// thread pool
#define SHADOW_WORKERS_MAX 15
static HANDLE g_shReady = NULL;
static HANDLE g_shWorkers[SHADOW_WORKERS_MAX];
static int    g_shNumWorkers = 0;
static void** g_shJobs = NULL;
static volatile LONG g_shHead = 0, g_shCount = 0, g_shFinished = 0;

static DWORD WINAPI shadowWorker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_shReady, INFINITE);
        for (;;) {
            LONG i = InterlockedIncrement(&g_shHead) - 1;
            if (i >= g_shCount) break;
            __try { o_shadowUpdate(g_shJobs[i], 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            InterlockedIncrement(&g_shFinished);
        }
    }
    return 0;
}
static void shadowRunBatch(void** jobs, int n) {
    if (n <= 0) return;
    g_shJobs = jobs; g_shCount = n; g_shHead = 0; g_shFinished = 0;
    if (g_shNumWorkers > 0) ReleaseSemaphore(g_shReady, g_shNumWorkers, NULL);
    for (;;) {                                    // main thread pulls jobs too
        LONG i = InterlockedIncrement(&g_shHead) - 1;
        if (i >= n) break;
        __try { o_shadowUpdate(jobs[i], 0, 0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        InterlockedIncrement(&g_shFinished);
    }
    while (g_shFinished < n) YieldProcessor();     // barrier
}

// live stats (read by reportThread)
static volatile LONG   g_shFrames = 0, g_shLastCount = 0, g_shLastSerial = 0, g_shLastParallel = 0;
static volatile LONG64 g_shLastMs10 = 0;           // parallel-phase wall time in 0.1ms units

static void __fastcall hk_shadowUpdate(void* ecx, void* edx, int arg) {
    if (g_shadowsDone) return;                      // already computed in the parallel pass this frame
    o_shadowUpdate(ecx, edx, arg);
}

static void __fastcall hk_renderShadows(void* ecx, void* edx, void* arg) {
    static void* list[SHADOW_MAX];
    static void* batch[SHADOW_MAX];
    // diagnostic: scan manager offsets x candidate next-offsets, track the LONGEST chain seen
    // over the battle -> reveals the real (head offset, next offset, peak shadow count).
    static int  g_diagBest = 0;
    static bool g_diagLock = false;
    if (!g_diagLock) {
        char* mgr = (char*)ecx;
        static const int nexts[] = {0x68, 0x00, 0x04, 0x08, 0x0C, 0x10, 0x64, 0x6C};
        for (int oi = 0; oi <= 0x40; oi += 4) {
            for (int k = 0; k < 8; ++k) {
                __try {
                    void* head = *(void**)(mgr + oi);
                    int cnt = 0; void* s = head; void* prev = NULL;
                    while (s && cnt < 20000 && s != prev) { prev = s; s = *(void**)((char*)s + nexts[k]); ++cnt; }
                    if (cnt > g_diagBest) {
                        g_diagBest = cnt;
                        logf("shadow-diag: NEW MAX chain=%d  head=[mgr+0x%02X]=%08X  next+0x%02X",
                             cnt, oi, (DWORD)(ULONG_PTR)head, nexts[k]);
                        if (cnt >= 50) { g_diagLock = true; logf("shadow-diag: locked (found a real list)."); }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }
    __try {
        int n = 0;
        for (void* s = *(void**)ecx; s && n < SHADOW_MAX; s = *(void**)((char*)s + SHADOW_NEXT_OFF))
            list[n++] = s;
        LARGE_INTEGER t0, t1, fr; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&t0);
        int m = 0, serial = 0;
        for (int i = 0; i < n; ++i) {
            if (warmContains(list[i])) { batch[m++] = list[i]; }
            else { warmAdd(list[i]); o_shadowUpdate(list[i], 0, 0); ++serial; }   // warm cold ones serially
        }
        shadowRunBatch(batch, m);                   // parallel-update the warm ones
        QueryPerformanceCounter(&t1);
        g_shLastCount = n; g_shLastSerial = serial; g_shLastParallel = m;
        g_shLastMs10 = (LONG64)((t1.QuadPart - t0.QuadPart) * 10000 / fr.QuadPart);
        InterlockedIncrement(&g_shFrames);
        g_shadowsDone = 1;                          // stock renderShadows' Update()s will now skip
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_shadowsDone = 0; }
    o_renderShadows(ecx, edx, arg);                 // stock: skips Update(), does the D3D stencil render
    g_shadowsDone = 0;
}

static DWORD WINAPI shadowReport(LPVOID) {
    LONG last = 0;
    for (;;) {
        Sleep(2000);
        LONG f = g_shFrames; LONG d = f - last; last = f;
        logf("shadow: %d frames/2s | last: %d shadows (%d warm-serial, %d parallel), parallel-phase %d.%d ms, %d workers",
             d, g_shLastCount, g_shLastSerial, g_shLastParallel,
             (int)(g_shLastMs10 / 10), (int)(g_shLastMs10 % 10), g_shNumWorkers);
    }
    return 0;
}

static void installShadowOffload(BYTE* textBase, SIZE_T textSize) {
    int c1 = 0, c2 = 0;
    BYTE* rs = scanOnce(textBase, textSize, kSigRenderShadows, kMaskRenderShadows, &c1);
    BYTE* up = scanOnce(textBase, textSize, kSigShadowUpdate,  kMaskShadowUpdate,  &c2);
    if (c1 != 1 || !rs || c2 != 1 || !up) {
        logf("shadow: signatures matched rs=%d up=%d (need 1 each) - offload OFF (game runs normally).", c1, c2);
        return;
    }
    o_shadowUpdate  = (tShadowUpdate) makeTrampoline(up, 6);
    o_renderShadows = (tRenderShadows)makeTrampoline(rs, 5);
    if (!o_shadowUpdate || !o_renderShadows) { logf("shadow: trampoline alloc failed - offload OFF."); return; }

    SYSTEM_INFO si; GetSystemInfo(&si);
    int workers = (int)si.dwNumberOfProcessors - 1;
    if (workers > SHADOW_WORKERS_MAX) workers = SHADOW_WORKERS_MAX;
    if (workers < 1) workers = 1;
    g_shReady = CreateSemaphoreA(NULL, 0, 1 << 20, NULL);
    for (int i = 0; i < workers; ++i) {
        g_shWorkers[i] = CreateThread(NULL, 0, shadowWorker, NULL, 0, NULL);
        if (g_shWorkers[i]) ++g_shNumWorkers;
    }

    HANDLE fz[64]; int nf = 0;
    suspendOthers(fz, &nf, 64);
    BOOL a = patchJmp(up, (void*)&hk_shadowUpdate,  6);
    BOOL b = patchJmp(rs, (void*)&hk_renderShadows, 5);
    resumeAll(fz, nf);
    if (!a || !b) { logf("shadow: patch failed (up=%d rs=%d) - offload OFF.", a, b); return; }
    logf("shadow: offload installed - renderShadows@%08X update@%08X, %d worker threads. Live.",
         (DWORD)(ULONG_PTR)rs, (DWORD)(ULONG_PTR)up, g_shNumWorkers);
    CreateThread(NULL, 0, shadowReport, NULL, 0, NULL);
}

// ============================================================ scene-render MAPPER (collect stage 1)
// One-shot, comprehensive render-path map so the parallel-collect offload can be built from a
// SINGLE play session. The 3D scene render (0x470176 on 2.02/AotR) walks a render-object list and
// issues each object's virtual methods through its vtable (e.g. call [vtable+0x15C]). A stripped
// binary + vtable dispatch hides those addresses statically, so we recover them live and dump
// enough context to identify every hookable function offline against the C&C Generals source.
//
// In one battle frame it logs: (1) the scene 'this' object field map (all render-object lists);
// (2) the render-object list base+count; (3) for EVERY distinct render-object type present, the
// FULL vtable (128 slots) + object field dump + a scan of sub-object (mesh) vtables; (4) the
// RenderInfoClass argument dump; (5) a bounded direct-call-graph scan from each type's render
// method. Everything is read-only, per-word SEH-guarded, captured for the richest few frames,
// then the stock render runs unchanged. Fail-closed: any fault aborts the dump, never the game.
//
// Signature @0x470176: push ebp; mov ebp,esp; mov eax,<scope>; call <EH_prolog>; push ebx;
//   mov ebx,[global]; test ebx,ebx; push esi; mov esi,ecx   (scope imm / call rel / global wildcarded)
static const BYTE kSigSceneRender[] = {
    0x55,0x8B,0xEC,0xB8,0,0,0,0, 0xE8,0,0,0,0, 0x53,0x8B,0x1D,0,0,0,0, 0x85,0xDB,0x56,0x8B,0xF1
};
static const char kMaskSceneRender[] = "xxxx????x????xxx????xxxxx";

typedef void (__fastcall* tSceneRender)(void* ecx, void* edx, void* rinfo);
static tSceneRender  o_sceneRender = NULL;
static volatile LONG g_srFrames   = 0;
static volatile LONG g_srCaptures = 0;
static int           g_srMaxCount = 0;
static int           g_srLastCount= 0;
static DWORD         g_objRender  = 0;   // recovered per-object Render() VA (first type's vtable[0x15C])

// module ranges (game.dat fixed base 0x400000): .text code, .rdata (vtables), .data
static __forceinline bool inText (DWORD a){ return a >= 0x00401000 && a < 0x00BD0000; }
static __forceinline bool inRdata(DWORD a){ return a >= 0x00BD0000 && a < 0x00D89000; }
static __forceinline bool looksProlog(DWORD a){
    if (!inText(a)) return false;
    BYTE b = 0; __try { b = *(BYTE*)a; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return b==0x55||b==0x51||b==0x53||b==0x56||b==0x57||b==0x8B||b==0x83||b==0x81||
           b==0x6A||b==0xB8||b==0xE9||b==0xFF||b==0xA1||b==0x33||b==0x68;
}

// per-word SEH hex dump: never aborts the whole dump on one unmapped word
static void dumpWords(const char* label, DWORD base, int nwords) {
    char line[700];
    for (int i = 0; i < nwords; i += 8) {
        int p = wsprintfA(line, "%s+%03X:", label, i * 4);
        for (int j = 0; j < 8 && (i + j) < nwords; ++j) {
            DWORD w = 0; BOOL ok = TRUE;
            __try { w = *(DWORD*)(base + (i + j) * 4); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
            if (ok) {
                const char* tag = inText(w) ? "t" : (inRdata(w) ? "r" : " ");
                p += wsprintfA(line + p, " %08X%s", w, tag);
            } else p += wsprintfA(line + p, " --------?");
        }
        logf("%s", line);
    }
}

// bounded recursive direct-call (E8 rel32) scanner -> maps the collect's direct call graph live
#define SC_MAX 200
static DWORD g_scSeen[SC_MAX]; static int g_scN;
static bool scSeen(DWORD a){ for (int i=0;i<g_scN;++i) if (g_scSeen[i]==a) return true; return false; }
static void scanCalls(DWORD fn, int depth) {
    if (depth < 0 || !inText(fn) || scSeen(fn) || g_scN >= SC_MAX) return;
    g_scSeen[g_scN++] = fn;
    DWORD targets[48]; int nt = 0; int rets = 0;
    for (DWORD k = 0; k < 0x1400 && nt < 48; ++k) {
        BOOL bad = FALSE; BYTE b = 0;
        __try { b = *(BYTE*)(fn + k); } __except (EXCEPTION_EXECUTE_HANDLER) { bad = TRUE; }
        if (bad) break;
        if (b == 0xC3 || b == 0xC2) { if (++rets >= 8) break; continue; }
        if (b == 0xE8) {
            int rel = 0; BOOL rok = TRUE;
            __try { rel = *(int*)(fn + k + 1); } __except (EXCEPTION_EXECUTE_HANDLER) { rok = FALSE; }
            if (rok) {
                DWORD tgt = fn + k + 5 + (DWORD)rel;
                if (looksProlog(tgt)) {
                    bool dup = false; for (int i=0;i<nt;++i) if (targets[i]==tgt) dup=true;
                    if (!dup) targets[nt++] = tgt;
                }
            }
        }
    }
    char line[600]; int p = wsprintfA(line, "  callscan %08X d%d ->", fn, depth);
    for (int i=0;i<nt && p<560;++i) p += wsprintfA(line + p, " %08X", targets[i]);
    logf("%s", line);
    for (int i=0;i<nt;++i) scanCalls(targets[i], depth - 1);
}

// full-vtable dedup across the whole run (dump each class's vtable only once)
static DWORD g_vtDumped[32]; static int g_nVtDumped;
static bool vtAlreadyDumped(DWORD vt){ for (int i=0;i<g_nVtDumped;++i) if (g_vtDumped[i]==vt) return true;
    if (g_nVtDumped<32) g_vtDumped[g_nVtDumped++]=vt; return false; }

static void sceneCaptureRich(DWORD ecx, DWORD rinfo) {
    __try {
        logf("==== SCENE-RENDER RICH CAPTURE #%d (this=%08X rinfo=%08X base=0x400000) ====",
             g_srCaptures, ecx, rinfo);

        // (1) scene 'this' field map -> reveals ALL render-object lists (ptr,count pairs), not just [0x800]
        logf("-- scene 'this' fields [0x000..0x820] (t=code r=vtable/rdata) --");
        dumpWords("this", ecx, 0x820 / 4);

        // (2) the render-object list this loop walks
        DWORD arr = 0; int count = 0;
        __try { arr = *(DWORD*)(ecx + 0x800); count = *(int*)(ecx + 0x80C); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        logf("-- render list @this+0x800: base=%08X  count(@+0x80C)=%d --", arr, count);

        if (arr > 0x10000 && count > 0 && count < 100000) {
            DWORD vts[12]; int nvt = 0;
            DWORD subvts[10]; int nsub = 0;
            for (int i = 0; i < count; ++i) {
                DWORD obj = 0;
                __try { obj = *(DWORD*)(arr + i * 4); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (obj < 0x10000) continue;
                DWORD vt = 0; __try { vt = *(DWORD*)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (!inRdata(vt)) continue;
                bool known = false; for (int k=0;k<nvt;++k) if (vts[k]==vt) known=true;
                if (known || nvt >= 12) continue;
                vts[nvt++] = vt;
                DWORD rn = 0; __try { rn = *(DWORD*)(vt + 0x15C); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                if (!g_objRender) g_objRender = rn;
                logf("-- render-object TYPE #%d: obj=%08X vtable=%08X vt[0x15C]=%08X (count-of-this-type follows) --",
                     nvt-1, obj, vt, rn);
                if (!vtAlreadyDumped(vt)) {
                    logf("   full vtable [slot 0x000..0x200]:");
                    dumpWords("   vt", vt, 128);
                    logf("   object instance fields [0x000..0x140]:");
                    dumpWords("   obj", obj, 0x140 / 4);
                    // sub-object (mesh/HLod child) discovery: any object field that points at a
                    // struct whose first word is an .rdata vtable is a child render object -> dump
                    // its vtable too, so MeshClass::Render etc. are recoverable in the same run.
                    for (int f = 1; f < 0x140/4; ++f) {
                        DWORD w = 0; __try { w = *(DWORD*)(obj + f*4); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                        if (w <= 0x10000) continue;
                        DWORD svt = 0; __try { svt = *(DWORD*)w; } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                        if (!inRdata(svt)) continue;
                        bool sk = false; for (int s=0;s<nsub;++s) if (subvts[s]==svt) sk=true;
                        if (sk || nsub >= 10) continue;
                        subvts[nsub++] = svt;
                        DWORD srn = 0; __try { srn = *(DWORD*)(svt + 0x15C); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                        logf("   sub-object @obj+0x%03X ptr=%08X vtable=%08X vt[0x15C]=%08X", f*4, w, svt, srn);
                        if (!vtAlreadyDumped(svt)) { logf("     sub full vtable:"); dumpWords("     svt", svt, 128); }
                    }
                    // direct call-graph of this type's render method
                    g_scN = 0; scanCalls(rn, 3);
                }
            }
            logf("-- distinct render-object types this frame: %d --", nvt);
        }

        // (3) RenderInfoClass argument (camera ref + material-pass stack + override flags) -> sizing per-worker copies
        if (rinfo > 0x10000) { logf("-- rinfo (RenderInfoClass) fields [0x000..0x0C0] --"); dumpWords("rinfo", rinfo, 0xC0/4); }

        logf("==== END RICH CAPTURE #%d ====", g_srCaptures);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("scene-render: rich capture faulted (0x%08lX) - stock render untouched.",
             (unsigned long)GetExceptionCode());
    }
}

// ============================================================ parallel skeleton-pose warm (stage 3 v1)
// Each visible HLOD's skeleton pose update (0x5A4A70 on 2.02/AotR) is pure per-object matrix math:
// it self-gates on [obj+0x100]==2 (no-op otherwise), computes into the object's OWN fields
// ([obj+0x108/0x114/0x118]), clears its dirty flag [obj+0xF4]=0, stamps the frame time [obj+0x114]
// from global [0xDD1E0C], and calls the 768-byte leaf evaluator 0x5A4770. No shared writes, no
// rinfo, no D3D. So we pre-compute every visible object's pose across the worker pool BEFORE the
// stock render runs; the render's own gate ([obj+0xF4] dirty) then finds them clean and SKIPS the
// serial pose eval. Disjoint per object, determinism-safe (visual only), fail-closed: any fault
// flips a session kill-switch and we run pure stock forever after.
// prologue: push esi; mov esi,ecx; cmp dword ptr [esi+0x100],2; jne +; lea eax,[esi+0x118]; push eax; call
static const BYTE kSigPoseUpdate[] = {
    0x56,0x8B,0xF1,0x83,0xBE,0x00,0x01,0x00,0x00,0x02,0x75,0x00,0x8D,0x86,0x18,0x01,0x00,0x00,0x50,0xE8
};
static const char kMaskPoseUpdate[] = "xxxxxxxxxxx?xxxxxxxx";
#define POSE_FRAMETIME_ADDR 0x00DD1E0C   // global stamped into [obj+0x114] each frame
#define POSE_STAMP_OFF      0x114        // per-object "last posed frame time"

typedef void (__fastcall* tPoseUpdate)(void* ecx, void* edx);
static tPoseUpdate   o_poseUpdate = NULL;      // 0x5A4A70, called directly (never patched)
static volatile LONG g_pcKill = 0;
static volatile LONG g_pcLastEligible = 0, g_pcLastWarmed = 0;

#define PC_WORKERS_MAX 15
#define PC_MAXJOBS 8192
static HANDLE g_pcReady = NULL;
static HANDLE g_pcWk[PC_WORKERS_MAX];
static int    g_pcNW = 0;
static void** g_pcJobs = NULL;
static volatile LONG g_pcHead = 0, g_pcN = 0, g_pcDone = 0;

static DWORD WINAPI pcWorker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_pcReady, INFINITE);
        for (;;) {
            LONG i = InterlockedIncrement(&g_pcHead) - 1;
            if (i >= g_pcN) break;
            __try { o_poseUpdate(g_pcJobs[i], 0); } __except (EXCEPTION_EXECUTE_HANDLER) { g_pcKill = 1; }
            InterlockedIncrement(&g_pcDone);
        }
    }
    return 0;
}
// main thread pulls jobs too, then barriers
static void pcRunBatch(void** jobs, int n) {
    if (n <= 0) return;
    g_pcJobs = jobs; g_pcN = n; g_pcHead = 0; g_pcDone = 0;
    if (g_pcNW > 0) ReleaseSemaphore(g_pcReady, g_pcNW, NULL);
    for (;;) {
        LONG i = InterlockedIncrement(&g_pcHead) - 1;
        if (i >= n) break;
        __try { o_poseUpdate(jobs[i], 0); } __except (EXCEPTION_EXECUTE_HANDLER) { g_pcKill = 1; }
        InterlockedIncrement(&g_pcDone);
    }
    while (g_pcDone < n) YieldProcessor();
}

static void prSample(void* scene);                  // aotr_posrate.inc, included further down
static void __fastcall hk_sceneRender(void* ecx, void* edx, void* rinfo) {
    InterlockedIncrement(&g_srFrames);
    prSample(ecx);                                  // measurement only, and only when the ini asks for it
#ifndef AOTR_PROD
    // Diagnostics only, and expensive where it fires: each capture writes about 140 lines from the game thread
    // while the frame waits (measured 41, 21, 20 and 20 ms in one battle). It is off in the production build.
    if (g_srCaptures < 4) {
        int count = 0; BOOL ok = TRUE;
        __try { count = *(int*)((char*)ecx + 0x80C); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
        if (ok) {
            g_srLastCount = count;
            if (count > g_srMaxCount + 4) {          // capture only meaningfully richer frames (menu -> battle)
                g_srMaxCount = count;
                InterlockedIncrement(&g_srCaptures);
                sceneCaptureRich((DWORD)(ULONG_PTR)ecx, (DWORD)(ULONG_PTR)rinfo);
            }
        }
    }
#endif

    // stage 3 v1: pre-compute every visible object's skeleton pose across the worker pool before
    // the stock render. 0x5A4A70 self-gates (no-op on non-animated objects), so it is safe to call
    // on every render object in the list; disjoint per-object writes; SEH + kill-switch fail-closed.
    if (o_poseUpdate && !g_pcKill) {
        __try {
            static void* jobs[PC_MAXJOBS];
            void** arr   = *(void***)((char*)ecx + 0x800);
            int    count = *(int*)   ((char*)ecx + 0x80C);
            int n = 0;
            DWORD nowT = *(DWORD*)POSE_FRAMETIME_ADDR;   // current frame time
            if (arr && count > 0 && count < PC_MAXJOBS) {
                for (int i = 0; i < count; ++i) {
                    void* o = arr[i];
                    if (!o) continue;
                    DWORD vt = *(DWORD*)o;
                    if (vt < 0x00BD0000 || vt >= 0x00D89000) continue;   // sane RenderObj vtable only
                    if (*(DWORD*)((char*)o + POSE_STAMP_OFF) == nowT) continue;  // already posed this frame
                    jobs[n++] = o;
                }
                g_pcLastEligible = n;
                pcRunBatch(jobs, n);
                g_pcLastWarmed = n;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_pcKill = 1; }
    }

    o_sceneRender(ecx, edx, rinfo);
}

static DWORD WINAPI sceneReport(LPVOID) {
    LONG last = 0;
    for (;;) {
        Sleep(2000);
        LONG f = g_srFrames; LONG d = f - last; last = f;
        logf("scene-render: %d calls/2s | lastCount=%d maxCount=%d captures=%d | pose-warm elig=%d warmed=%d workers=%d kill=%d",
             d, g_srLastCount, g_srMaxCount, g_srCaptures, g_pcLastEligible, g_pcLastWarmed, g_pcNW, g_pcKill);
    }
    return 0;
}

static void installSceneRenderMapper(BYTE* textBase, SIZE_T textSize) {
    int c = 0;
    BYTE* fn = scanOnce(textBase, textSize, kSigSceneRender, kMaskSceneRender, &c);
    if (c != 1 || !fn) {
        logf("scene-render: signature matched %d times (need 1) - mapper OFF (game runs normally).", c);
        return;
    }
    o_sceneRender = (tSceneRender)makeTrampoline(fn, 8);   // steal push ebp; mov ebp,esp; mov eax,imm32
    if (!o_sceneRender) { logf("scene-render: trampoline alloc failed - mapper OFF."); return; }
    HANDLE fz[64]; int nf = 0;
    suspendOthers(fz, &nf, 64);
    BOOL ok = patchJmp(fn, (void*)&hk_sceneRender, 8);
    resumeAll(fz, nf);
    if (!ok) { logf("scene-render: patch failed - mapper OFF."); return; }
    logf("scene-render: MAPPER installed at %08X (expected 00470176). Will dump the render-object "
         "vtables + lists + rinfo on the richest battle frames.", (DWORD)(ULONG_PTR)fn);

    // stage 3 v1: locate the per-object skeleton-pose update and spin up the pose worker pool.
    {
        int pc = 0;
        BYTE* pf = scanOnce(textBase, textSize, kSigPoseUpdate, kMaskPoseUpdate, &pc);
        char pwe[8]; pwe[0] = 0;
        bool poseWarm = GetEnvironmentVariableA("AOTR_POSEWARM", pwe, sizeof(pwe)) && pwe[0] == '1';
        if (pc == 1 && pf && !poseWarm) {
            logf("pose-warm: off (it warmed about one object per frame while waking 15 threads; AOTR_POSEWARM=1 enables it).");
        } else if (pc == 1 && pf) {
            o_poseUpdate = (tPoseUpdate)pf;      // called directly, not detoured
            SYSTEM_INFO si; GetSystemInfo(&si);
            int w = (int)si.dwNumberOfProcessors - 1;
            if (w > PC_WORKERS_MAX) w = PC_WORKERS_MAX;
            if (w < 1) w = 1;
            g_pcReady = CreateSemaphoreA(NULL, 0, 1 << 20, NULL);
            for (int i = 0; i < w; ++i) { g_pcWk[i] = CreateThread(NULL, 0, pcWorker, NULL, 0, NULL); if (g_pcWk[i]) ++g_pcNW; }
            logf("pose-warm: pose fn @%08X (expected 005A4A70), %d workers. Parallel skeleton pre-pass LIVE.",
                 (DWORD)(ULONG_PTR)pf, g_pcNW);
        } else {
            logf("pose-warm: signature matched %d times (need 1) - pose warm OFF (game runs normally).", pc);
        }
    }

#ifndef AOTR_PROD
    CreateThread(NULL, 0, sceneReport, NULL, 0, NULL);
#endif
}

// ================================================================ instrumentation v3 (2026-09-14)
// Exact counters for one battle, measurement only (every hook forwards unchanged):
//  (1) engine stage timers through the perf-event hook pointers (0xDD361C begin / 0xDD3620 end), now
//      also qualified by top-level pass, e.g. "UpdateShadowMap>RenderFXShaderBatch";
//  (2) exact call counts and exclusive time on 26 ID3DXEffect methods in d3dx9_27 (hot-patch prologue
//      detours; the internal vtable at d3dx9+0x13EB0 is verified slot by slot before patching);
//  (3) IDirect3DDevice9 vtable hooks on the live DXVK device, plus lazy vertex/index buffer Lock,
//      texture LockRect and state-block Capture/Apply hooks: counts, exclusive time, redundant state
//      and redundant shader-constant registers, all split by pass;
//  (4) the TEB-bounded sampler, now tagging every sample with the pass/stage the render thread is in,
//      so the unmarked time (logic, client update, audio, Present) gets module, leaf and deep-inclusive
//      attribution.
// Output: one compact line per 5 s window; a full BATTLE REPORT every 60 s of battle, where a battle
// window has procCPU >= 100% of one core and >= 20 rendered frames.

#include <stdlib.h>

static LARGE_INTEGER g_pqpf;
static __forceinline LONG64 qpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static LONG64 qpcHz() {                                  // the counter's frequency, asked for once
    static LONG64 hz = 0;
    if (!hz) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); hz = f.QuadPart ? f.QuadPart : 1; }
    return hz;
}
static __forceinline LONG64 qpcMs() { return (qpcNow() * 1000) / qpcHz(); }   // a monotonic millisecond clock
static LONG64 rd64(volatile LONG64* p) { for (;;) { LONG64 a = *p; LONG64 b = *p; if (a == b) return a; } }

enum { P_NONE = 0, P_SHADOW, P_VIEWS, P_UI, P_OTHER, NPASS };
static const char* const kPassName[NPASS] = { "unmarked", "shadow", "views", "ui", "other" };
static int g_curTop = P_NONE;                      // written only by the render thread

// ---------------------------------------------------------------- stage markers
typedef int (__stdcall* tPerfBegin)(DWORD color, const wchar_t* name);
typedef int (__stdcall* tPerfEnd)(void);
static void** const g_perfSlotBegin = (void**)0x00DD361C;
static void** const g_perfSlotEnd   = (void**)0x00DD3620;
static tPerfBegin o_perfBegin = NULL;
static tPerfEnd   o_perfEnd   = NULL;
static bool       g_mkLive    = false;

#define MK_ENTS 1024
struct MkEnt {
    DWORD hash; char name[60];
    volatile LONG64 incl, excl; volatile LONG count; LONG top;
    LONG64 lIncl, lExcl; LONG lCount;
    LONG64 aIncl, aExcl, aCount;
};
static MkEnt g_mk[MK_ENTS];
struct MkFrame { MkEnt* e; MkEnt* q; LONG64 t0; LONG64 child; };
static MkFrame g_mkStack[64];
static volatile LONG g_mkDepth = 0;
static DWORD  g_mkTid = 0;
static volatile LONG g_mkFrames = 0, g_mkBegins = 0, g_mkEnds = 0, g_mkUnderflow = 0;
static volatile LONG64 g_mkPeriod = 0;
static LONG64 g_mkLastViews = 0;
static MkEnt* g_mkMeshEnt = NULL, *g_mkBatchEnt = NULL, *g_mkPartEnt = NULL;
static MkEnt* g_mkPassEnt[NPASS];
#define MK_QTOPS 8
static MkEnt* g_qTop[MK_QTOPS]; static int g_nQTop = 0;
static MkEnt* g_qCache[MK_QTOPS][MK_ENTS];

static MkEnt* mkFindName(const char* nm, int n) {
    DWORD h = 2166136261u;
    for (int i = 0; i < n; ++i) h = (h ^ (BYTE)nm[i]) * 16777619u;
    if (!h) h = 1;
    for (int i = 0; i < MK_ENTS; ++i) {
        MkEnt* e = &g_mk[(h + i) & (MK_ENTS - 1)];
        if (e->hash == h && strcmp(e->name, nm) == 0) return e;
        if (e->hash == 0) {
            memcpy(e->name, nm, n + 1); e->hash = h;          // name before hash: readers see complete entries
            if      (!strcmp(nm, "UpdateShadowMap"))      g_mkPassEnt[P_SHADOW] = e;
            else if (!strcmp(nm, "RenderViews"))          g_mkPassEnt[P_VIEWS]  = e;
            else if (!strcmp(nm, "RenderUI"))             g_mkPassEnt[P_UI]     = e;
            else if (!strcmp(nm, "RenderFXShaderBatch"))  g_mkBatchEnt = e;
            else if (!strcmp(nm, "RenderParticles"))      g_mkPartEnt  = e;
            else if (!strcmp(nm, "Rendering mesh (all)")) g_mkMeshEnt  = e;
            return e;
        }
    }
    return NULL;
}
static MkEnt* mkFind(const wchar_t* w) {
    static const char kMesh[] = "Rendering mesh\t";               // per-mesh events: hot fast path
    if (g_mkMeshEnt) { int i = 0; while (i < 15 && w[i] == (wchar_t)(BYTE)kMesh[i]) ++i; if (i == 15) return g_mkMeshEnt; }
    char nm[60]; int n = 0;
    for (; n < 59 && w[n]; ++n) nm[n] = (char)(w[n] & 0xFF);
    nm[n] = 0;
    if (n > 10 && memcmp(nm, "Rendering ", 10) == 0) {             // any per-object event -> one bucket per kind
        int t = 10; while (t < n && nm[t] != '\t') ++t;
        if (t < n && t + 6 < 60) { memcpy(nm + t, " (all)", 7); n = t + 6; }
    }
    return mkFindName(nm, n);
}
static MkEnt* mkQualified(MkEnt* top, MkEnt* e) {
    int ti = 0;
    for (; ti < g_nQTop; ++ti) if (g_qTop[ti] == top) break;
    if (ti == g_nQTop) { if (g_nQTop >= MK_QTOPS) return NULL; g_qTop[g_nQTop++] = top; }
    int ei = (int)(e - g_mk);
    if (ei < 0 || ei >= MK_ENTS) return NULL;
    MkEnt* q = g_qCache[ti][ei];
    if (!q) {
        char nm[60]; int n = wsprintfA(nm, "%.22s>%.36s", top->name, e->name);
        q = mkFindName(nm, n);
        g_qCache[ti][ei] = q;
    }
    return q;
}
static int __stdcall my_perfBegin(DWORD color, const wchar_t* name) {
    if (o_perfBegin) o_perfBegin(color, name);
    DWORD tid = GetCurrentThreadId();
    if (!g_mkTid) g_mkTid = tid;
    if (tid != g_mkTid || !name) return 0;
    MkEnt* e = NULL;
    __try { e = mkFind(name); } __except (EXCEPTION_EXECUTE_HANDLER) { e = NULL; }
    InterlockedIncrement(&g_mkBegins);
    LONG64 t = qpcNow();
    int d = g_mkDepth;
    MkEnt* q = NULL;
    if (d == 0) {
        int p = P_OTHER;
        if (e) for (int i = P_SHADOW; i <= P_UI; ++i) if (g_mkPassEnt[i] == e) p = i;
        if (p == P_VIEWS) {
            if (g_mkLastViews) InterlockedAdd64(&g_mkPeriod, t - g_mkLastViews);
            g_mkLastViews = t;
            InterlockedIncrement(&g_mkFrames);
        }
        g_curTop = p;
    } else if (e && d < 64 && g_mkStack[0].e && e != g_mkStack[0].e) {
        q = mkQualified(g_mkStack[0].e, e);
    }
    if (d < 64) { g_mkStack[d].e = e; g_mkStack[d].q = q; g_mkStack[d].t0 = t; g_mkStack[d].child = 0; }
    g_mkDepth = d + 1;                               // stack entry written before depth: the sampler sees whole frames
    return 0;
}
static int __stdcall my_perfEnd(void) {
    if (o_perfEnd) o_perfEnd();
    if (GetCurrentThreadId() != g_mkTid) return 0;
    InterlockedIncrement(&g_mkEnds);
    int d = g_mkDepth;
    if (d <= 0) { InterlockedIncrement(&g_mkUnderflow); return 0; }
    --d;
    if (d < 64) {
        MkFrame& f = g_mkStack[d];
        LONG64 dt = qpcNow() - f.t0, ex = dt - f.child;
        if (f.e) { InterlockedAdd64(&f.e->incl, dt); InterlockedAdd64(&f.e->excl, ex); InterlockedIncrement(&f.e->count); if (d == 0) f.e->top = 1; }
        if (f.q) { InterlockedAdd64(&f.q->incl, dt); InterlockedAdd64(&f.q->excl, ex); InterlockedIncrement(&f.q->count); }
        if (d > 0) g_mkStack[d - 1].child += dt;
    }
    g_mkDepth = d;
    if (d == 0) g_curTop = P_NONE;
    return 0;
}
// v29: the frame boundary. A lean build installs no engine stage markers, so nothing else counts frames; Present
// calls rtFrameBoundary() once per real frame on the game thread (aotr_rt.inc, rt_dev_17 / rt_sc_3), so the counter,
// the stall detector and the A/B segments work in CORE mode exactly as they do in FULL mode.
void rtFrameBoundary();
static volatile LONG g_mkFrameMark = 0;                  // the engine stage markers are live: Present keeps out of the way
static void installPerfMarkers(bool first) {
    __try {
        static const BYTE sigBegin[] = {0x8B,0x35,0x1C,0x36,0xDD,0x00};
        static const BYTE sigEnd[]   = {0xA1,0x20,0x36,0xDD,0x00};
        static const BYTE sigCtor[]  = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x56,0x8B,0xF1};
        if (memcmp((void*)0x0051ECE7, sigBegin, sizeof(sigBegin)) != 0 ||
            memcmp((void*)0x0051ED60, sigEnd,   sizeof(sigEnd))   != 0 ||
            memcmp((void*)0x00517690, sigCtor,  sizeof(sigCtor))  != 0) {
            if (first) logf("stages: perf-event code mismatch (different game.dat build) - stage timers OFF.");
            return;
        }
        void* curB = *g_perfSlotBegin; void* curE = *g_perfSlotEnd;
        if (curB == (void*)&my_perfBegin && curE == (void*)&my_perfEnd) return;
        if (curB != (void*)&my_perfBegin) o_perfBegin = (tPerfBegin)curB;
        if (curE != (void*)&my_perfEnd)   o_perfEnd   = (tPerfEnd)curE;
        DWORD op = 0; VirtualProtect((void*)g_perfSlotBegin, 12, PAGE_READWRITE, &op);
        *g_perfSlotBegin = (void*)&my_perfBegin;
        *g_perfSlotEnd   = (void*)&my_perfEnd;
        VirtualProtect((void*)g_perfSlotBegin, 12, op, &op);
        g_mkLive = true;
        g_mkFrameMark = 1;                          // v29: the stage markers count frames now, Present must not
        logf("stages: perf-event hooks %s at 0xDD361C/0xDD3620 (previous begin=%08X end=%08X).",
             first ? "installed" : "RE-installed", (DWORD)(ULONG_PTR)curB, (DWORD)(ULONG_PTR)curE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("stages: install faulted (0x%08lX) - stage timers OFF.", (unsigned long)GetExceptionCode());
    }
}

// ---------------------------------------------------------------- stage timers for the RT build (call-site hooks)
// The render-thread build times only the three top-level stages, by redirecting the calls to their perf-event
// object constructor/destructor. The engine's perf-event hook pointers stay NULL, so the ~6500 per-mesh and
// per-batch events a battle frame emits no longer widen their names and call a hook.
// engine phases seen by the game-thread sampler: logic update, client update (outside the render stages), render stages
enum { PH_OTHER = 0, PH_LOGIC, PH_CLIENT, PH_SHADOW, PH_VIEWS, PH_UI, PH_N };
static const char* const kPhaseName[PH_N] = { "other", "logic", "client", "shadow", "views", "ui" };
static volatile LONG g_phase = PH_OTHER;                 // written by the game thread only
// v28: measurement costs. The v26 battle log has ~35,000 calls a frame going through timing thunks (effect wrapper
// Begin/BeginPass/Commit/EndPass/End, the batch test, MeshClass::Render, the mesh draw, render-list push/erase), each
// two rdtsc reads and a few memory updates on top of a detour: about 2 ms of a 43 ms battle frame, more than any
// optimization still on the list. Measurement hooks are therefore only installed, and timing inside the functional
// hooks only taken, when AOTR_DIAG=1. The frame / logic / stage figures of the report stay (they cost a few reads per frame).
static LONG g_diag = 0;
static volatile LONG64 g_phTicks[PH_N];
static LONG g_stPrevPhase[3];
static volatile LONG64 g_stTicks[3];                    // UpdateShadowMap, RenderViews, RenderUI
static LPTHREAD_START_ROUTINE g_gsStart = NULL;          // game-thread sampler, started lazily by the RT report thread
static LONG64 g_stT0[3];
// frame-time histogram (RenderViews to RenderViews) and frame time by the logic sub-frame index that preceded it
static volatile LONG g_frHist[9];
static LONG64 g_frEdge[8];                                  // 20, 25, 33.4, 40, 50, 66.7, 100, 200 ms in QPC ticks
static volatile LONG64 g_frMax = 0;
static volatile LONG g_frMaxTenths = 0;                     // the longest frame since the budget line last asked, in tenths of a millisecond
// v28: the A/B record needs the same frame gaps the report already measures - total stall time and the longest
// single frame - so they are accumulated here, where the gap is known, and read by the A/B block (aotr_rt.inc).
static volatile LONG64 g_abStallTicks = 0;                  // total time in frames of 100 ms or more, QPC ticks
static volatile LONG64 g_abWorstTicks = 0;                  // the longest single frame of the session, QPC ticks
static volatile LONG64 g_abGapTicks = 0;                    // every frame gap, summed: the segment's true denominator
static volatile LONG64 g_abSegWorst = 0;                    // the longest frame in the CURRENT A/B segment (reset on a switch)
static volatile LONG g_lgLastN = 0;
static volatile LONG g_frByN[8];
static volatile LONG64 g_frTByN[8];
typedef void* (__fastcall* tStageCtor)(void*, void*, DWORD, DWORD, DWORD);
typedef void  (__fastcall* tStageDtor)(void*, void*);
static const DWORD kStageCtor = 0x00517690, kStageDtor = 0x00517740;
static __forceinline void stBegin(int i) {
    LONG64 t = qpcNow(); g_stT0[i] = t;
    g_stPrevPhase[i] = g_phase; g_phase = PH_SHADOW + i;
    if (g_capOn) { capPrintf("P stage-begin %d\n", i); g_capFullLeft = 60; }
    if (!g_mkTid) g_mkTid = GetCurrentThreadId();       // the game's render thread
    if (i == 1) {
        if (g_mkLastViews) {
            LONG64 dt = t - g_mkLastViews; g_mkPeriod += dt;
            if (g_scrFrame) g_scrFrame(dt);
            if (g_lpFrame) g_lpFrame(dt);
            int b = 0; while (b < 8 && dt >= g_frEdge[b]) ++b;
            g_frHist[b]++; if (dt > g_frMax) g_frMax = dt;
            g_abGapTicks += dt;
            if (b >= 6) g_abStallTicks += dt;                 // 100 ms or more: what the player calls a freeze
            if (dt > g_abWorstTicks) g_abWorstTicks = dt;
            if (dt > g_abSegWorst) g_abSegWorst = dt;
            { LONG tenths = (LONG)(dt * 10000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1)); if (tenths > g_frMaxTenths) g_frMaxTenths = tenths; }
            LONG k = g_lgLastN; g_frByN[k]++; g_frTByN[k] += dt;
        }
        g_mkLastViews = t; InterlockedIncrement(&g_mkFrames);
    }
}
// ---- v29: the frame boundary when the engine stage markers are NOT installed (a lean build, and
// every build that ships). Present calls this once per real frame on the game thread (aotr_rt.inc,
// rtFrameB in rt_dev_17 / rt_sc_3), so g_mkFrames, the frame-gap histogram, the stall detector and
// the A/B segments all work in CORE mode exactly as they do in FULL mode. It is a no-op once the
// markers are live (g_mkFrameMark) so a diagnostic build cannot count a frame twice.
void rtFrameBoundary() {
    if (g_mkFrameMark) return;                          // the stage markers own this
    LONG64 t = qpcNow();
    // One picture, one frame. The game presents through the device's Present on some sessions and through the swap
    // chain's Present on others; when the device's Present forwards into its chain, BOTH of our entries run for the
    // same picture and the frame would be counted twice - halving the reported fps and inventing stalls at the gap
    // between them. A real frame gap is never below ~1 ms (the engine cannot present two pictures inside a
    // millisecond), while a forwarding pair is microseconds apart, so anything under 1 ms is the same picture.
    if (g_mkLastViews && t - g_mkLastViews < (g_pqpf.QuadPart / 1000)) return;
    if (g_mkLastViews) {
        LONG64 dt = t - g_mkLastViews;
        if (dt <= 0) { g_mkLastViews = t; return; }
        g_mkPeriod += dt;
        if (g_scrFrame) g_scrFrame(dt);                 // steady scroll speed reads the same gaps
        if (g_lpFrame) g_lpFrame(dt);
        int b = 0; while (b < 8 && dt >= g_frEdge[b]) ++b;
        g_frHist[b]++; if (dt > g_frMax) g_frMax = dt;
        g_abGapTicks += dt;
        if (b >= 6) g_abStallTicks += dt;               // 100 ms or more: what the player calls a freeze
        if (dt > g_abWorstTicks) g_abWorstTicks = dt;
        if (dt > g_abSegWorst) g_abSegWorst = dt;
        { LONG tenths = (LONG)(dt * 10000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1)); if (tenths > g_frMaxTenths) g_frMaxTenths = tenths; }
        LONG k = g_lgLastN; g_frByN[k]++; g_frTByN[k] += dt;
    }
    g_mkLastViews = t;
    InterlockedIncrement(&g_mkFrames);
}
static void* __fastcall stCtor0(void* ecx, void* edx, DWORD a, DWORD b, DWORD c) { stBegin(0); return ((tStageCtor)(ULONG_PTR)kStageCtor)(ecx, edx, a, b, c); }
static void* __fastcall stCtor1(void* ecx, void* edx, DWORD a, DWORD b, DWORD c) { stBegin(1); return ((tStageCtor)(ULONG_PTR)kStageCtor)(ecx, edx, a, b, c); }
static void* __fastcall stCtor2(void* ecx, void* edx, DWORD a, DWORD b, DWORD c) { stBegin(2); return ((tStageCtor)(ULONG_PTR)kStageCtor)(ecx, edx, a, b, c); }
static void __fastcall stDtor0(void* ecx, void* edx) { ((tStageDtor)(ULONG_PTR)kStageDtor)(ecx, edx); g_stTicks[0] += qpcNow() - g_stT0[0]; g_phase = g_stPrevPhase[0]; if (g_capOn) capPrintf("P stage-end 0\n"); }
static void __fastcall stDtor1(void* ecx, void* edx) { ((tStageDtor)(ULONG_PTR)kStageDtor)(ecx, edx); g_stTicks[1] += qpcNow() - g_stT0[1]; g_phase = g_stPrevPhase[1]; if (g_capOn) capPrintf("P stage-end 1\n"); if (g_twViewsEnd) g_twViewsEnd(); }
static void __fastcall stDtor2(void* ecx, void* edx) { ((tStageDtor)(ULONG_PTR)kStageDtor)(ecx, edx); g_stTicks[2] += qpcNow() - g_stT0[2]; g_phase = g_stPrevPhase[2]; if (g_capOn) capPrintf("P stage-end 2\n"); }

// GameEngine vtable 0xBFE260: +0x98 logic update (0x6329B0, runs one or more logic frames), +0x9C client update
// (0x632409: radar, game client update, display draw). Timed by swapping the two vtable entries.
typedef void (__fastcall* tEngLogic)(void*, void*, DWORD);
typedef void (__fastcall* tEngClient)(void*, void*);
static const DWORD kEngLogic = 0x006329B0, kEngClient = 0x00632409;
// The engine runs one logic step per six render frames and calls the logic update every frame with the sub-frame
// index 1..6 (heavy work only at 1); calls, total and worst time are kept per index (slot 0: any other value).
static volatile LONG g_lgN[8];
static volatile LONG64 g_lgT[8], g_lgMax[8];
static volatile LONG g_lgCurN = 0;
static volatile ULONG64 g_lgEnterTsc = 0;                // when this frame's logic update was entered
static void lsFrameBegin();
static void lsFrameDone(DWORD ne, ULONG64 ticks);
static void capFrameTick();
static ULONG64 g_lgTscSum = 0;                               // time in the logic update so far, in rdtsc ticks (the logic limiter reads it: aotr_logicrate.inc)
static void __fastcall phLogic(void* ecx, void* edx, DWORD n) {
    g_lgCurN = n < 7 ? (LONG)n : 0;
    if (g_capWanted) capFrameTick();
    lsFrameBegin();
    g_lgEnterTsc = __rdtsc();
    LONG prev = g_phase; g_phase = PH_LOGIC; LONG64 t0 = qpcNow();
    ((tEngLogic)(ULONG_PTR)kEngLogic)(ecx, edx, n);
    { ULONG64 d_ = __rdtsc() - g_lgEnterTsc; g_lgTscSum += d_; lsFrameDone(n, d_); }
    LONG64 dt = qpcNow() - t0;
    g_phTicks[PH_LOGIC] += dt; g_phase = prev;
    LONG k = n < 8 ? (LONG)n : 0;
    g_lgN[k]++; g_lgT[k] += dt; if (dt > g_lgMax[k]) g_lgMax[k] = dt;
    g_lgLastN = k;
}
static void __fastcall phClient(void* ecx, void* edx) {
    LONG prev = g_phase; g_phase = PH_CLIENT; LONG64 t0 = qpcNow();
    ((tEngClient)(ULONG_PTR)kEngClient)(ecx, edx);
    g_phTicks[PH_CLIENT] += qpcNow() - t0; g_phase = prev;
}
// Animation timers (measurement): Single_Anim_Progress 0x5A4A70, HLod sub-object transform update 0x59CD70 (vtable
// +0xA8, includes the tree update) and Animatable3DObj tree update 0x5A5050 - calls and inclusive time per phase.
enum { AN_PROGRESS = 0, AN_HLOD, AN_TREE, AN_N };
static volatile LONG g_anN[AN_N][PH_N];
static volatile LONG64 g_anT[AN_N][PH_N];                 // rdtsc ticks
static double g_tscPerQpc = 0;
typedef void (__fastcall* tThis0)(void*, void*);
static tThis0 o_an[AN_N];
static void __fastcall hkAnProgress(void* ecx, void* edx) { ULONG64 t = __rdtsc(); o_an[AN_PROGRESS](ecx, edx); LONG p = g_phase; g_anT[AN_PROGRESS][p] += (LONG64)(__rdtsc() - t); g_anN[AN_PROGRESS][p]++; }
static void __fastcall hkAnHLod(void* ecx, void* edx);     // after the parallel tree-update code below
static void __fastcall hkAnTree(void* ecx, void* edx);     // after the parallel tree-update code below
static void installAnimTimers() {
    { LONG64 q0 = qpcNow(); ULONG64 t0 = __rdtsc(); Sleep(60); LONG64 q1 = qpcNow(); ULONG64 t1 = __rdtsc();
      g_tscPerQpc = q1 > q0 ? (double)(LONG64)(t1 - t0) / (double)(q1 - q0) : 0; }
    static const BYTE kProg[] = {0x56,0x8B,0xF1,0x83,0xBE,0x00,0x01,0x00,0x00,0x02};
    static const BYTE kHLod[] = {0x83,0xEC,0x4C,0x53,0x55,0x56,0x57};
    static const BYTE kTree[] = {0x83,0xEC,0x30,0x56,0x8B,0xF1};
    __try {
        if (g_tscPerQpc <= 0 || memcmp((void*)0x005A4A70, kProg, sizeof(kProg)) || memcmp((void*)0x0059CD70, kHLod, sizeof(kHLod)) || memcmp((void*)0x005A5050, kTree, sizeof(kTree))) {
            logf("anim: animation functions differ - animation timers OFF."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    o_an[AN_PROGRESS] = (tThis0)makeTrampoline((BYTE*)0x005A4A70, sizeof(kProg));
    o_an[AN_HLOD]     = (tThis0)makeTrampoline((BYTE*)0x0059CD70, sizeof(kHLod));
    o_an[AN_TREE]     = (tThis0)makeTrampoline((BYTE*)0x005A5050, sizeof(kTree));
    if (!o_an[AN_PROGRESS] || !o_an[AN_HLOD] || !o_an[AN_TREE]) { logf("anim: trampoline alloc failed - animation timers OFF."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    if (g_diag) patchJmp((BYTE*)0x005A4A70, (void*)&hkAnProgress, sizeof(kProg));
    patchJmp((BYTE*)0x0059CD70, (void*)&hkAnHLod, sizeof(kHLod));
    patchJmp((BYTE*)0x005A5050, (void*)&hkAnTree, sizeof(kTree));
    resumeAll(fz, nf);
    logf("anim: timers on Single_Anim_Progress 0x5A4A70, HLod transform update 0x59CD70, tree update 0x5A5050.");
}
#include "aotr_posewarm2.inc"
static void __fastcall hkAnHLod(void* ecx, void* edx) {
    if (GetCurrentThreadId() == g_mkTid) {
        if (g_pwSeen) pwSeenMark((DWORD)(ULONG_PTR)ecx, g_mkFrames);       // drawn this frame: a candidate for warming next frame
        if (g_pwGenLive && pwTrySkipFull((DWORD)(ULONG_PTR)ecx)) return;
    }
    if (!g_diag) { o_an[AN_HLOD](ecx, edx); return; }
    ULONG64 t = __rdtsc(); o_an[AN_HLOD](ecx, edx); LONG p = g_phase; g_anT[AN_HLOD][p] += (LONG64)(__rdtsc() - t); g_anN[AN_HLOD][p]++;
}
static void __fastcall hkAnTree(void* ecx, void* edx) {
    DWORD o = (DWORD)(ULONG_PTR)ecx;
    if (GetCurrentThreadId() == g_mkTid) {
        if (g_pwSeen) pwSeenMark(o, g_mkFrames);
        if (g_pwGenLive && pwTrySkip(o)) return;
    }
    if (!g_diag) { o_an[AN_TREE](ecx, edx); return; }
    ULONG64 t = __rdtsc(); o_an[AN_TREE](ecx, edx); LONG p = g_phase; g_anT[AN_TREE][p] += (LONG64)(__rdtsc() - t); g_anN[AN_TREE][p]++;
}
static volatile DWORD g_frTidEarly = 0;            // the game thread, once known (aotr_frameprof.inc sets it)
#include "aotr_equivmemo.inc"
#include "aotr_psfind.inc"
#include "aotr_pevict.inc"                       // v42: the particle eviction loop stops at its first empty scan (exact)                       // v41: findParticleSystemByID from the remembered node (self-proving)
#include "aotr_fastcrt.inc"
#define FDB_LOG(...) logf(__VA_ARGS__)
static volatile LONG g_vanilla = 0;               // v51: VANILLA beside the DLL - nothing of the game is changed, only frames are timed and reported
static volatile DWORD g_frLastLogicTick = 0;       // v34: GetTickCount of the last logic step begun (aotr_frameprof.inc); the freeze guard tells play from loading by it
#include "aotr_fdbuf.inc"
static DWORD WINAPI fdbReport(LPVOID) {
    LONG lRead = 0, lReal = 0, lSeek = 0, lSeekReal = 0, lPass = 0; LONG64 lBytes = 0;
    LONG lWN = 0, lWSys = 0, lWSH = 0, lWSR = 0; LONG64 lWBytes = 0, lWTicks = 0;
    for (;;) {
        for (int s = 0; s < 5; ++s) { Sleep(1000); fdbWFlushOld(); }      // v34: collected writes older than a second go out
        { LONG wn = g_fdbWN, ws = g_fdbWSys; LONG64 wb = rd64(&g_fdbWBytes), wt = rd64(&g_fdbWTicks);
          if (wn - lWN >= 200)
              logf("files: last 5 s - %d writes (%d KB) collected into %d system writes, %d seeks answered from the write window (%d by the system), taking %d.%02d ms in all | so far %d files written through the window, %d failures, %d times the disk had under 256 MB free",
                   (int)(wn - lWN), (int)((wb - lWBytes) >> 10), (int)(ws - lWSys), (int)(g_fdbWSeekHere - lWSH), (int)(g_fdbWSeekReal - lWSR), (int)((wt - lWTicks) * 1000 / qpcHz()), (int)(((wt - lWTicks) * 100000 / qpcHz()) % 100), (int)g_fdbWFiles, (int)g_fdbWErr, (int)g_fdbWLow);
          lWN = wn; lWSys = ws; lWBytes = wb; lWTicks = wt; lWSH = g_fdbWSeekHere; lWSR = g_fdbWSeekReal; }
        LONG rd = g_fdbNRead, real = g_fdbNReal, sk = g_fdbNSeek, skr = g_fdbNSeekReal, pass = g_fdbNPass; LONG64 by = rd64(&g_fdbBytes);
        if (rd - lRead >= 2000 || pass - lPass >= 20000)
            logf("files: last 5 s - %d reads answered from read-ahead (%d KB) by %d system reads; %d seeks answered here, %d by the system | %d reads left to the C runtime as they were | so far %d files read ahead of (%d of them open before this was installed), %d windows of 64 KB",
                 (int)(rd - lRead), (int)((by - lBytes) >> 10), (int)(real - lReal), (int)(sk - lSeek), (int)(skr - lSeekReal), (int)(pass - lPass), (int)g_fdbNAdopt, (int)g_fdbNLate, (int)g_fdbNWin);
        lRead = rd; lReal = real; lSeek = sk; lSeekReal = skr; lPass = pass; lBytes = by;
    }
}
static void installFdBuf(BYTE* base) {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = 1; (void)ini;                         // v30: always on (same bytes, offline-tested); not an ini switch any more
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_FDBUF", env, sizeof(env)) && env[0]) want = env[0] - '0';
    if (want <= 0) { logf("files: read-ahead off ([Engine] FileReadAhead = 0)."); return; }
    HMODULE crt = GetModuleHandleA("msvcr71.dll");
    if (!crt || !fdbInit(crt)) { logf("files: msvcr71's file functions were not found - read-ahead off."); return; }
    // _read goes last: until it is hooked nothing is read ahead of, so no half-hooked state can mix positions
    static const struct { const char* name; void* hook; } k[7] = { {"_wopen", (void*)&fdb_wopen}, {"_open", (void*)&fdb_open}, {"_close", (void*)&fdb_close}, {"_write", (void*)&fdb_write},
        {"_get_osfhandle", (void*)&fdb_get_osfhandle}, {"_lseek", (void*)&fdb_lseek}, {"_read", (void*)&fdb_read} };
    int n[7] = { 0, 0, 0, 0, 0, 0, 0 }; bool all = true;
    for (int i = 0; i < 7; ++i) {
        for (int guard = 0; guard < 8; ++guard) { FARPROC* slot = iatSlot(base, "msvcr71.dll", k[i].name); if (!slot || !hookSlot(slot, k[i].hook, NULL)) break; n[i]++; }
        if (!n[i] && i >= 2 && i != 4) all = false;        // _close, _write, _lseek and _read are needed; the game may not import the others
        if (!all) break;
    }
    if (!all || !n[6]) { logf("files: the game does not import msvcr71's _read / _lseek / _close / _write as expected - read-ahead stays off (hooks pass through)."); return; }
    InterlockedExchange(&g_fdbOn, 1);
    { wchar_t self[MAX_PATH]; bool ok = GetModuleFileNameW(crt, self, MAX_PATH) && fdbSelfTest(self);       // here, in this process, against msvcr71 itself
      if (!ok) { InterlockedExchange(&g_fdbOn, 0); fdbAllRaw(); logf("files: the read-ahead self-test did NOT match msvcr71 - read-ahead is off for this session (every call passes through)."); return; } }
    // v34 (ISH-8 #1): the write buffer - its own self-test against msvcr71 (two files in %TEMP%); AOTR_FDBUFW=0 leaves writes as they were
    { char we[8]; we[0] = 0; GetEnvironmentVariableA("AOTR_FDBUFW", we, sizeof(we));
      if (we[0] == '0') logf("files: write buffer OFF (AOTR_FDBUFW=0) - every _write goes to the disk at once, as before.");
      else {
          InterlockedExchange(&g_fdbWOn, 1);
          LONG f0 = g_fdbWFiles; int ops = 0; LONG64 t0 = qpcNow();
          bool ok = fdbWSelfTest(&ops); bool used = g_fdbWFiles - f0 >= 2;
          int tms = (int)((qpcNow() - t0) * 1000 / qpcHz());
          g_fdbWFiles = f0;
          if (!ok || !used) { InterlockedExchange(&g_fdbWOn, 0);
              logf("files: write buffer self-test %s - write buffer OFF for this session (every _write goes to the disk at once, as before).", !ok ? "did NOT match msvcr71" : "could not use the buffer (the temp folder's disk has under 256 MB free?)"); }
          else logf("files: write buffer ON - self-test against msvcr71 passed (%d writes / seeks / reads on two temp files, every result and every byte of the closed files equal, %d ms): files opened for writing in binary mode get a 1 MB write window: writes and seeks inside it are answered here, it goes to the disk in one write (a save game: millions of system calls -> about a thousand).", ops, tms);
      } }
    CreateThread(NULL, 0, fdbReport, NULL, 0, NULL);
    logf("files: read-ahead installed (self-test against msvcr71 passed) - the engine's unbuffered file reads (one system call per few bytes while models and animations are parsed) are answered from 4-64 KB windows; "
         "text-mode files and files open for writing are left alone. Import slots: _read %d, _lseek %d, _close %d, _write %d, _wopen %d, _open %d, _get_osfhandle %d. Files opened earlier are %s.",
         n[6], n[5], n[2], n[3], n[0], n[1], n[4], g_fdbPio ? "taken up at their first read" : "left as they are (msvcr71's handle table was not understood)");
}
static void installPhaseTimers() {
    __try {
        static const BYTE kLogicPre[] = {0x55,0x56,0x57,0x68,0x50,0xE3,0xBF,0x00};
        static const BYTE kClientPre[] = {0xB8,0x04,0x46,0xB8,0x00,0xE8};
        if (memcmp((void*)(ULONG_PTR)kEngLogic, kLogicPre, sizeof(kLogicPre)) || memcmp((void*)(ULONG_PTR)kEngClient, kClientPre, sizeof(kClientPre))) {
            logf("phases: engine update functions differ - phase timers OFF."); return;
        }
        static const DWORD kVt[2] = { 0x00BFE260, 0x00BD84E0 };     // GameEngine and the derived engine the game instantiates
        int n = 0;
        for (int i = 0; i < 2; ++i) {
            DWORD* vt = (DWORD*)(ULONG_PTR)kVt[i];
            if (vt[0x98 / 4] != kEngLogic || vt[0x9C / 4] != kEngClient) continue;
            DWORD op = 0;
            if (!VirtualProtect(&vt[0x98 / 4], 8, PAGE_READWRITE, &op)) continue;
            vt[0x98 / 4] = (DWORD)(ULONG_PTR)&phLogic;
            vt[0x9C / 4] = (DWORD)(ULONG_PTR)&phClient;
            VirtualProtect(&vt[0x98 / 4], 8, op, &op);
            ++n;
        }
        static const int kEdgeUs[8] = { 20000, 25000, 33400, 40000, 50000, 66700, 100000, 200000 };
        for (int e = 0; e < 8; ++e) g_frEdge[e] = g_pqpf.QuadPart * kEdgeUs[e] / 1000000;
        logf("phases: logic / client update timers installed in %d engine vtable(s) (+0x98/+0x9C).", n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("phases: install faulted."); }
}

// ---------------------------------------------------------------- engine device lock: kernel mutex -> user-mode recursive lock
// DX8Wrapper serializes device use with a kernel mutex (created at 0x52506B into [0xDD1FD8]) that it takes and releases
// hundreds of times per frame: two system calls per pair, ~2% of a battle frame. The handle is read at exactly four
// places - WaitForSingleObject at 0x51EECB (20 s) and 0x51EF5C (caller timeout), ReleaseMutex at 0x520920 and
// 0x525291 - so those four calls are redirected to a recursive user-mode lock with the same results: owner recursion,
// WAIT_TIMEOUT after the timeout, ERROR_NOT_OWNER for a release by a non-owner. Installed only before the game
// creates the mutex, so the kernel object is never owned.
static volatile LONG g_dxlOwner = 0;
static LONG g_dxlDepth = 0;
static volatile LONG g_dxlWaiters = 0, g_dxlContended = 0;
static HANDLE g_dxlEvent = NULL;
static DWORD WINAPI dxlWait(HANDLE, DWORD ms) {
    LONG me = (LONG)GetCurrentThreadId();
    if (g_dxlOwner == me) { g_dxlDepth++; return WAIT_OBJECT_0; }
    if (InterlockedCompareExchange(&g_dxlOwner, me, 0) == 0) { g_dxlDepth = 1; return WAIT_OBJECT_0; }
    if (ms == 0) return WAIT_TIMEOUT;
    InterlockedIncrement(&g_dxlContended);
    LONG64 t0 = qpcNow(), limit = (ms == INFINITE) ? 0 : (LONG64)ms * g_pqpf.QuadPart / 1000;
    for (;;) {
        for (int i = 0; i < 2000; ++i) {
            _mm_pause();
            if (g_dxlOwner == 0 && InterlockedCompareExchange(&g_dxlOwner, me, 0) == 0) { g_dxlDepth = 1; return WAIT_OBJECT_0; }
        }
        LONG64 el = qpcNow() - t0;
        if (limit && el >= limit) return WAIT_TIMEOUT;
        InterlockedIncrement(&g_dxlWaiters);
        if (InterlockedCompareExchange(&g_dxlOwner, me, 0) == 0) { InterlockedDecrement(&g_dxlWaiters); g_dxlDepth = 1; return WAIT_OBJECT_0; }
        DWORD slice = 10;
        if (limit) { LONG64 left = (limit - el) * 1000 / g_pqpf.QuadPart; if (left < (LONG64)slice) slice = (DWORD)(left > 0 ? left : 0); }
        WaitForSingleObject(g_dxlEvent, slice);
        InterlockedDecrement(&g_dxlWaiters);
        if (InterlockedCompareExchange(&g_dxlOwner, me, 0) == 0) { g_dxlDepth = 1; return WAIT_OBJECT_0; }
    }
}
static BOOL WINAPI dxlRelease(HANDLE) {
    if (g_dxlOwner != (LONG)GetCurrentThreadId()) { SetLastError(ERROR_NOT_OWNER); return FALSE; }
    if (--g_dxlDepth == 0) { g_dxlOwner = 0; if (g_dxlWaiters) SetEvent(g_dxlEvent); }
    return TRUE;
}
static void installDeviceLock() {
    static const BYTE kWait[] = {0xFF,0x15,0x34,0x02,0xBD,0x00}, kRelease[] = {0xFF,0x15,0x24,0x02,0xBD,0x00}, kCreate[] = {0xFF,0x15,0x1C,0x02,0xBD,0x00};
    static const DWORD kWaits[2] = { 0x0051EECB, 0x0051EF5C }, kReleases[2] = { 0x00520920, 0x00525291 };
    __try {
        for (int i = 0; i < 2; ++i) if (memcmp((void*)(ULONG_PTR)kWaits[i], kWait, 6) || memcmp((void*)(ULONG_PTR)kReleases[i], kRelease, 6)) { logf("devlock: call sites differ - kernel mutex kept."); return; }
        if (memcmp((void*)0x0052506B, kCreate, 6)) { logf("devlock: mutex creation differs - kernel mutex kept."); return; }
        if (*(DWORD*)0x00DD1FD8 != 0) { logf("devlock: the game already created its device mutex - kernel mutex kept."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_dxlEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_dxlEvent) return;
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    bool late = *(DWORD*)0x00DD1FD8 != 0;
    if (!late) {
        for (int i = 0; i < 2; ++i) {
            BYTE* w = (BYTE*)(ULONG_PTR)kWaits[i], *r = (BYTE*)(ULONG_PTR)kReleases[i]; DWORD op = 0;
            VirtualProtect(w, 6, PAGE_EXECUTE_READWRITE, &op); w[0] = 0xE8; *(DWORD*)(w + 1) = (DWORD)(ULONG_PTR)&dxlWait - (kWaits[i] + 5); w[5] = 0x90; VirtualProtect(w, 6, op, &op);
            VirtualProtect(r, 6, PAGE_EXECUTE_READWRITE, &op); r[0] = 0xE8; *(DWORD*)(r + 1) = (DWORD)(ULONG_PTR)&dxlRelease - (kReleases[i] + 5); r[5] = 0x90; VirtualProtect(r, 6, op, &op);
            FlushInstructionCache(GetCurrentProcess(), w, 6); FlushInstructionCache(GetCurrentProcess(), r, 6);
        }
    }
    resumeAll(fz, nf);
    logf(late ? "devlock: the game created its device mutex meanwhile - kernel mutex kept." : "devlock: engine device mutex replaced by a user-mode recursive lock (4 call sites).");
}
static bool callSiteOk(DWORD site, DWORD target) {
    const BYTE* p = (const BYTE*)(ULONG_PTR)site;
    return p[0] == 0xE8 && site + 5 + *(const DWORD*)(p + 1) == target;
}
static void setCall(DWORD site, void* hook) {
    BYTE* p = (BYTE*)(ULONG_PTR)site; DWORD op = 0;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &op);
    *(DWORD*)(p + 1) = (DWORD)(ULONG_PTR)hook - (site + 5);
    VirtualProtect(p, 5, op, &op);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}
static const DWORD kEvHook = 0x00DD361C;               // engine perf-event BEGIN hook pointer (NULL unless a tool sets it)
static BYTE* emitCmpHookNull(BYTE* p) { p[0] = 0x83; p[1] = 0x3D; *(DWORD*)(p + 2) = kEvHook; p[6] = 0x00; return p + 7; }
static BYTE* emitJmp(BYTE* p, DWORD target) { p[0] = 0xE9; *(DWORD*)(p + 1) = target - ((DWORD)(ULONG_PTR)p + 5); return p + 5; }
static BYTE* emitJe(BYTE* p, DWORD target) { p[0] = 0x0F; p[1] = 0x84; *(DWORD*)(p + 2) = target - ((DWORD)(ULONG_PTR)p + 6); return p + 6; }
static void emitEventStubs(BYTE* stub, BYTE** o0, BYTE** o1, BYTE** o2) {
    static const BYTE kCtorOrig[]  = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x56,0x8B,0xF1};
    static const BYTE kMeshOrig[]  = {0x8D,0x85,0xCC,0xFE,0xFF,0xFF};
    static const BYTE kMultiOrig[] = {0x8B,0x07,0x8B,0x88,0xC4,0x00,0x00,0x00};
    // perf-event constructor: with no hook installed, construct nothing (the destructor already checks its own pointer)
    BYTE* q = emitCmpHookNull(stub);
    q[0] = 0x75; q[1] = 0x05;                        // jne +5 -> original body
    q[2] = 0x8B; q[3] = 0xC1;                        // mov eax, ecx
    q[4] = 0xC2; q[5] = 0x0C; q[6] = 0x00;           // ret 0Ch
    q += 7; memcpy(q, kCtorOrig, 9); q += 9; q = emitJmp(q, 0x00517699);
    // per-mesh event: skip building "Rendering mesh\tFXShader\t<name>" and its event object
    *o1 = q; q = emitCmpHookNull(q); q = emitJe(q, 0x00573D9A); memcpy(q, kMeshOrig, 6); q += 6; q = emitJmp(q, 0x00573D40);
    // instanced group event: skip its sprintf and event object
    *o2 = q; q = emitCmpHookNull(q); q = emitJe(q, 0x00573E1D); memcpy(q, kMultiOrig, 8); q += 8; q = emitJmp(q, 0x00573DCE);
    *o0 = stub;
}
#ifdef AOTR_SELFTEST   // offline-test export: built only into the test DLL
extern "C" __declspec(dllexport) DWORD __cdecl AotrTestEventStubs(BYTE* out, int n, DWORD* offs) {   // offline check of the emitted bytes
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf || n < 128) return 0;
    BYTE* s0, *s1, *s2; emitEventStubs(buf, &s0, &s1, &s2);
    memcpy(out, buf, 128); offs[0] = (DWORD)(s0 - buf); offs[1] = (DWORD)(s1 - buf); offs[2] = (DWORD)(s2 - buf);
    return (DWORD)(ULONG_PTR)buf;
}
#endif
static void installStageTimersAndEventSkips() {
    __try {
        static const DWORD kSites[6][2] = { {0x00449DE0, kStageCtor}, {0x00449E03, kStageDtor},     // UpdateShadowMap
                                            {0x00449FBA, kStageCtor}, {0x0044A008, kStageDtor},     // RenderViews
                                            {0x0044A049, kStageCtor}, {0x0044A0A8, kStageDtor} };   // RenderUI
        static const BYTE kNameUsm[] = {0x68,0xEC,0x9D,0xBD,0x00}, kNameRv[] = {0x68,0xC8,0x9D,0xBD,0x00}, kNameUi[] = {0x68,0xBC,0x9D,0xBD,0x00};
        static const BYTE kCtorOrig[]  = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x56,0x8B,0xF1};
        static const BYTE kMeshOrig[]  = {0x8D,0x85,0xCC,0xFE,0xFF,0xFF};
        static const BYTE kMultiOrig[] = {0x8B,0x07,0x8B,0x88,0xC4,0x00,0x00,0x00};
        static const BYTE kMeshName[]  = {0x68,0x74,0xA7,0xBE,0x00}, kMultiName[] = {0x68,0x44,0xA7,0xBE,0x00};
        static const BYTE kMeshSkipTo[] = {0x6A,0x01,0x51}, kMultiSkipTo[] = {0x3B,0x7D,0xE4};
        bool stagesOk = true;
        for (int i = 0; i < 6; ++i) stagesOk = stagesOk && callSiteOk(kSites[i][0], kSites[i][1]);
        stagesOk = stagesOk && !memcmp((void*)0x00449DD5, kNameUsm, 5) && !memcmp((void*)0x00449FAF, kNameRv, 5) && !memcmp((void*)0x0044A03E, kNameUi, 5);
        bool skipsOk = !memcmp((void*)0x00517690, kCtorOrig, 9) && !memcmp((void*)0x00573D3A, kMeshOrig, 6) && !memcmp((void*)0x00573DC6, kMultiOrig, 8)
                    && !memcmp((void*)0x00573D40, kMeshName, 5) && !memcmp((void*)0x00573DF5, kMultiName, 5)
                    && !memcmp((void*)0x00573D9A, kMeshSkipTo, 3) && !memcmp((void*)0x00573E1D, kMultiSkipTo, 3)
                    && *(DWORD*)(ULONG_PTR)kEvHook == 0;
        BYTE* stub = skipsOk ? (BYTE*)VirtualAlloc(NULL, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) : NULL;
        BYTE* s0 = stub, *s1 = NULL, *s2 = NULL;
        if (stub) {
            emitEventStubs(stub, &s0, &s1, &s2);
            DWORD op = 0; VirtualProtect(stub, 256, PAGE_EXECUTE_READ, &op);
        }
        HANDLE fz[256]; int nf = 0;
        suspendOthers(fz, &nf, 256);
        if (stagesOk) {
            setCall(0x00449DE0, (void*)&stCtor0); setCall(0x00449E03, (void*)&stDtor0);
            setCall(0x00449FBA, (void*)&stCtor1); setCall(0x0044A008, (void*)&stDtor1);
            setCall(0x0044A049, (void*)&stCtor2); setCall(0x0044A0A8, (void*)&stDtor2);
        }
        if (stub) {
            patchJmp((BYTE*)0x00517690, s0, 5);
            patchJmp((BYTE*)0x00573D3A, s1, 6);
            patchJmp((BYTE*)0x00573DC6, s2, 8);
        }
        resumeAll(fz, nf);
        logf("stages: RT-build stage timers %s (UpdateShadowMap / RenderViews / RenderUI call sites); engine per-mesh perf events %s.",
             stagesOk ? "installed" : "NOT installed (call sites differ)", stub ? "skipped while no perf hook is set" : "left as they are (code differs or a hook is set)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("stages: install faulted (0x%08lX).", (unsigned long)GetExceptionCode());
    }
}

// ---------------------------------------------------------------- shared call timer (render thread only)
struct Stat {
    volatile LONG   cnt[NPASS];
    volatile LONG64 incl[NPASS], excl[NPASS], x1[NPASS], x2[NPASS];
    LONG   lcnt[NPASS]; LONG64 lincl[NPASS], lexcl[NPASS], lx1[NPASS], lx2[NPASS];
    LONG64 acnt[NPASS], aincl[NPASS], aexcl[NPASS], ax1[NPASS], ax2[NPASS];
};
static int    g_hkDepth = 0;
static LONG64 g_hkChild[32];
static LONG64 g_hkOver = 0;                          // calibrated cost of one timer pair, charged to the child
struct HkTimer {
    Stat* s; LONG64 t0; int d; int p; bool on;
    __forceinline explicit HkTimer(Stat* st) : s(st), t0(0), d(0), p(0) {
        on = (g_mkTid != 0 && GetCurrentThreadId() == g_mkTid);
        if (on) { d = g_hkDepth++; if (d < 32) g_hkChild[d] = 0; p = g_curTop; t0 = qpcNow(); }
    }
    __forceinline ~HkTimer() {
        if (!on) return;
        LONG64 dt = qpcNow() - t0;
        g_hkDepth = d;
        LONG64 ch = (d < 32) ? g_hkChild[d] : 0;
        s->cnt[p]++; s->incl[p] += dt; s->excl[p] += dt - ch;
        if (d > 0 && d - 1 < 32) g_hkChild[d - 1] += dt + g_hkOver;
    }
};
static void calibrateTimer() {
    Stat dummy; memset(&dummy, 0, sizeof(dummy));
    const int N = 200000;
    LONG64 a = qpcNow();
    for (int i = 0; i < N; ++i) {
        volatile DWORD tid = GetCurrentThreadId(); (void)tid;
        LONG64 t0 = qpcNow(); LONG64 t1 = qpcNow();
        dummy.cnt[0]++; dummy.incl[0] += t1 - t0; dummy.excl[0] += t1 - t0;
    }
    LONG64 b = qpcNow();
    g_hkOver = (b - a) / N;
}

// ---------------------------------------------------------------- ID3DXEffect method timers (d3dx9_27)
struct FxDef { const char* name; DWORD rva; int slot; int hArg; };
static const FxDef kFxDefs[] = {
    {"SetValue",               0x1069E8, 20, 1}, {"SetBool",                0x104C03, 22, 1},
    {"SetInt",                 0x104A52, 26, 1}, {"SetFloat",               0x1057F8, 30, 1},
    {"SetFloatArray",          0x105438, 32, 1}, {"SetVector",              0x104470, 34, 1},
    {"SetVectorArray",         0x103E10, 36, 1}, {"SetMatrix",              0x1042E9, 38, 1},
    {"SetMatrixArray",         0x103FCD, 40, 1}, {"SetMatrixTranspose",     0x1043A8, 44, 1},
    {"SetMatrixTransposeArray",0x10415B, 46, 1}, {"SetTexture",             0x1047B5, 52, 1},
    {"SetTechnique",           0x0FEC78, 58, 1}, {"IsParameterUsed",        0x0FAA99, 62, 1},
    {"Begin",                  0x1062F5, 63, 0}, {"BeginPass",              0x108148, 64, 0},
    {"CommitChanges",          0x107FD6, 65, 0}, {"EndPass",                0x0FDB91, 66, 0},
    {"End",                    0x0FDA18, 67, 0}, {"BeginParameterBlock",    0x109BD7, 73, 0},
    {"EndParameterBlock",      0x108931, 74, 0}, {"ApplyParameterBlock",    0x10863A, 75, 0},
    {"SetRawValue",            0x103D7B, 78, 1}, {"GetParameterByName",     0x0FCED9,  9, 0},
    {"GetParameterBySemantic", 0x0FCEF7, 10, 0}, {"GetTechniqueByName",     0x0F9E8B, 13, 0},
};
#define NFX ((int)(sizeof(kFxDefs) / sizeof(kFxDefs[0])))
enum { FX_BEGIN = 14, FX_BEGINPASS = 15, FX_SETRAWVALUE = 22 };
static Stat  g_fx[32];
static void* g_fxTramp[32];
static bool  g_devTried = false;
static void  installDeviceHooks(void* fx);
static __forceinline bool isNameHandle(DWORD h) { return h != 0 && (h & 0x80000003u) != 0x80000003u; }

#define FXNAMEH(I, a) if (t.on && kFxDefs[I].hArg && isNameHandle(a)) g_fx[I].x2[t.p]++
#define FXW0(I) static DWORD __stdcall fxw##I(void* o) { HkTimer t(&g_fx[I]); \
    return ((DWORD(__stdcall*)(void*))g_fxTramp[I])(o); }
#define FXW1(I) static DWORD __stdcall fxw##I(void* o, DWORD a) { HkTimer t(&g_fx[I]); FXNAMEH(I, a); \
    return ((DWORD(__stdcall*)(void*, DWORD))g_fxTramp[I])(o, a); }
#define FXW2(I) static DWORD __stdcall fxw##I(void* o, DWORD a, DWORD b) { HkTimer t(&g_fx[I]); FXNAMEH(I, a); \
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_fxTramp[I])(o, a, b); }
#define FXW3(I) static DWORD __stdcall fxw##I(void* o, DWORD a, DWORD b, DWORD c) { HkTimer t(&g_fx[I]); FXNAMEH(I, a); \
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))g_fxTramp[I])(o, a, b, c); }
FXW3(0) FXW2(1) FXW2(2) FXW2(3) FXW3(4) FXW2(5) FXW3(6) FXW2(7) FXW3(8) FXW2(9) FXW3(10) FXW2(11) FXW1(12) FXW2(13)
FXW0(16) FXW0(17) FXW0(18) FXW0(19) FXW0(20) FXW1(21) FXW2(23) FXW2(24) FXW1(25)
static DWORD __stdcall fxw14(void* o, DWORD pPasses, DWORD flags) {           // Begin: state-save flags
    HkTimer t(&g_fx[FX_BEGIN]);
    if (t.on) { if (flags & 1) g_fx[FX_BEGIN].x1[t.p]++; if (flags & 6) g_fx[FX_BEGIN].x2[t.p]++; }
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD))g_fxTramp[FX_BEGIN])(o, pPasses, flags);
}
static DWORD __stdcall fxw15(void* o, DWORD pass) {                           // BeginPass: lazy device hooks
    if (!g_devTried && g_mkTid && GetCurrentThreadId() == g_mkTid) installDeviceHooks(o);
    HkTimer t(&g_fx[FX_BEGINPASS]);
    return ((DWORD(__stdcall*)(void*, DWORD))g_fxTramp[FX_BEGINPASS])(o, pass);
}
static DWORD __stdcall fxw22(void* o, DWORD h, DWORD p, DWORD off, DWORD bytes) {   // SetRawValue: bytes, name handles
    HkTimer t(&g_fx[FX_SETRAWVALUE]);
    if (t.on) { g_fx[FX_SETRAWVALUE].x1[t.p] += bytes; if (isNameHandle(h)) g_fx[FX_SETRAWVALUE].x2[t.p]++; }
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))g_fxTramp[FX_SETRAWVALUE])(o, h, p, off, bytes);
}
static void* const kFxWrap[] = {
    (void*)&fxw0, (void*)&fxw1, (void*)&fxw2, (void*)&fxw3, (void*)&fxw4, (void*)&fxw5, (void*)&fxw6, (void*)&fxw7,
    (void*)&fxw8, (void*)&fxw9, (void*)&fxw10, (void*)&fxw11, (void*)&fxw12, (void*)&fxw13, (void*)&fxw14, (void*)&fxw15,
    (void*)&fxw16, (void*)&fxw17, (void*)&fxw18, (void*)&fxw19, (void*)&fxw20, (void*)&fxw21, (void*)&fxw22, (void*)&fxw23,
    (void*)&fxw24, (void*)&fxw25,
};
static void installFxTimers() {
    HMODULE hx = GetModuleHandleA("d3dx9_27.dll");
    if (!hx) { logf("fx-timers: d3dx9_27 not loaded - OFF."); return; }
    if ((int)(sizeof(kFxWrap) / sizeof(kFxWrap[0])) != NFX) { logf("fx-timers: wrapper table size mismatch - OFF."); return; }
    DWORD base = (DWORD)(ULONG_PTR)hx;
    static const BYTE kHot[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    __try {
        DWORD* vt = (DWORD*)(ULONG_PTR)(base + 0x13EB0);
        for (int i = 0; i < NFX; ++i) {
            DWORD fn = base + kFxDefs[i].rva;
            if (vt[kFxDefs[i].slot] != fn || memcmp((void*)(ULONG_PTR)fn, kHot, 5) != 0) {
                logf("fx-timers: %s (slot %d) does not match this d3dx9_27 build - OFF.", kFxDefs[i].name, kFxDefs[i].slot);
                return;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("fx-timers: vtable verify faulted - OFF."); return; }
    for (int i = 0; i < NFX; ++i) {
        g_fxTramp[i] = makeTrampoline((BYTE*)(ULONG_PTR)(base + kFxDefs[i].rva), 5);
        if (!g_fxTramp[i]) { logf("fx-timers: trampoline alloc failed - OFF."); return; }
    }
    HANDLE fz[256]; int nf = 0; int ok = 0;
    suspendOthers(fz, &nf, 256);
    for (int i = 0; i < NFX; ++i) ok += patchJmp((BYTE*)(ULONG_PTR)(base + kFxDefs[i].rva), kFxWrap[i], 5) ? 1 : 0;
    resumeAll(fz, nf);
    logf("fx-timers: %d/%d ID3DXEffect methods timed (vtable d3dx9+0x13EB0 verified; timer pair %d ticks, qpf %d).",
         ok, NFX, (int)g_hkOver, (int)g_pqpf.QuadPart);
}

// ---------------------------------------------------------------- IDirect3DDevice9 hooks (DXVK)
#define DV_SLOTS 119
static void*  g_devOrig[DV_SLOTS];
static Stat   g_dv[DV_SLOTS];
static BYTE   g_dvKind[DV_SLOTS];     // 0 plain, 1 x1=redundant calls, 2 x1=registers x2=redundant registers, 3 x1=primitives
static DWORD  g_d3d9Lo = 0, g_d3d9Hi = 0;
static const char* const kDevSlot[DV_SLOTS] = {"QueryInterface","AddRef","Release","TestCooperativeLevel","GetAvailableTextureMem","EvictManagedResources","GetDirect3D","GetDeviceCaps","GetDisplayMode","GetCreationParameters","SetCursorProperties","SetCursorPosition","ShowCursor","CreateAdditionalSwapChain","GetSwapChain","GetNumberOfSwapChains","Reset","Present","GetBackBuffer","GetRasterStatus","SetDialogBoxMode","SetGammaRamp","GetGammaRamp","CreateTexture","CreateVolumeTexture","CreateCubeTexture","CreateVertexBuffer","CreateIndexBuffer","CreateRenderTarget","CreateDepthStencilSurface","UpdateSurface","UpdateTexture","GetRenderTargetData","GetFrontBufferData","StretchRect","ColorFill","CreateOffscreenPlainSurface","SetRenderTarget","GetRenderTarget","SetDepthStencilSurface","GetDepthStencilSurface","BeginScene","EndScene","Clear","SetTransform","GetTransform","MultiplyTransform","SetViewport","GetViewport","SetMaterial","GetMaterial","SetLight","GetLight","LightEnable","GetLightEnable","SetClipPlane","GetClipPlane","SetRenderState","GetRenderState","CreateStateBlock","BeginStateBlock","EndStateBlock","SetClipStatus","GetClipStatus","GetTexture","SetTexture","GetTextureStageState","SetTextureStageState","GetSamplerState","SetSamplerState","ValidateDevice","SetPaletteEntries","GetPaletteEntries","SetCurrentTexturePalette","GetCurrentTexturePalette","SetScissorRect","GetScissorRect","SetSoftwareVertexProcessing","GetSoftwareVertexProcessing","SetNPatchMode","GetNPatchMode","DrawPrimitive","DrawIndexedPrimitive","DrawPrimitiveUP","DrawIndexedPrimitiveUP","ProcessVertices","CreateVertexDeclaration","SetVertexDeclaration","GetVertexDeclaration","SetFVF","GetFVF","CreateVertexShader","SetVertexShader","GetVertexShader","SetVertexShaderConstantF","GetVertexShaderConstantF","SetVertexShaderConstantI","GetVertexShaderConstantI","SetVertexShaderConstantB","GetVertexShaderConstantB","SetStreamSource","GetStreamSource","SetStreamSourceFreq","GetStreamSourceFreq","SetIndices","GetIndices","CreatePixelShader","SetPixelShader","GetPixelShader","SetPixelShaderConstantF","GetPixelShaderConstantF","SetPixelShaderConstantI","GetPixelShaderConstantI","SetPixelShaderConstantB","GetPixelShaderConstantB","DrawRectPatch","DrawTriPatch","DeletePatch","CreateQuery"};

// shadow device state (render thread only) for redundancy counting; invalidated by state-block Apply and Reset
static DWORD g_rs[256];        static BYTE g_rsOk[256];
static DWORD g_ss[20][16];     static BYTE g_ssOk[20][16];
static DWORD g_tx[20];         static BYTE g_txOk[20];
static DWORD g_vsh, g_psh, g_decl, g_fvf, g_ibuf; static BYTE g_vshOk, g_pshOk, g_declOk, g_fvfOk, g_ibufOk;
static DWORD g_strm[8][3];     static BYTE g_strmOk[8];
static DWORD g_vsc[256 * 4];   static BYTE g_vscOk[256];
static DWORD g_psc[224 * 4];   static BYTE g_pscOk[224];
static volatile LONG g_dvInvalidations = 0;
static void dvInvalidate() {
    memset(g_rsOk, 0, sizeof(g_rsOk)); memset(g_ssOk, 0, sizeof(g_ssOk)); memset(g_txOk, 0, sizeof(g_txOk));
    g_vshOk = g_pshOk = g_declOk = g_fvfOk = g_ibufOk = 0; memset(g_strmOk, 0, sizeof(g_strmOk));
    memset(g_vscOk, 0, sizeof(g_vscOk)); memset(g_pscOk, 0, sizeof(g_pscOk));
    InterlockedIncrement(&g_dvInvalidations);
}
static __forceinline int smpIdx(DWORD s) { return s < 16 ? (int)s : ((s >= 257 && s <= 260) ? (int)(s - 257 + 16) : -1); }

#define DVO(S, SIG) ((SIG)g_devOrig[S])
#define DVW0(S) static DWORD __stdcall dvw##S(void* d) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*))(d); }
#define DVW1(S) static DWORD __stdcall dvw##S(void* d, DWORD a) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD))(d, a); }
#define DVW2(S) static DWORD __stdcall dvw##S(void* d, DWORD a, DWORD b) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD, DWORD))(d, a, b); }
#define DVW3(S) static DWORD __stdcall dvw##S(void* d, DWORD a, DWORD b, DWORD c) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))(d, a, b, c); }
#define DVW4(S) static DWORD __stdcall dvw##S(void* d, DWORD a, DWORD b, DWORD c, DWORD e) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))(d, a, b, c, e); }
#define DVW5(S) static DWORD __stdcall dvw##S(void* d, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD))(d, a, b, c, e, f); }
#define DVW6(S) static DWORD __stdcall dvw##S(void* d, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f, DWORD g) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))(d, a, b, c, e, f, g); }
#define DVW8(S) static DWORD __stdcall dvw##S(void* d, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f, DWORD g, DWORD h, DWORD i) { HkTimer t(&g_dv[S]); return DVO(S, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))(d, a, b, c, e, f, g, h, i); }
DVW4(17) DVW8(23) DVW6(26) DVW6(27) DVW4(30) DVW2(31) DVW2(32) DVW5(34) DVW3(35) DVW2(37) DVW1(39) DVW0(41) DVW0(42)
DVW6(43) DVW2(44) DVW1(47) DVW1(49) DVW2(51) DVW2(53) DVW2(55) DVW3(67) DVW1(75) DVW3(96) DVW3(98) DVW3(111) DVW3(113)

static DWORD __stdcall dvw16(void* d, DWORD pp) {                                  // Reset
    HkTimer t(&g_dv[16]); if (t.on) dvInvalidate();
    return DVO(16, DWORD(__stdcall*)(void*, DWORD))(d, pp);
}
static DWORD __stdcall dvw57(void* d, DWORD st, DWORD v) {                         // SetRenderState
    HkTimer t(&g_dv[57]);
    if (t.on && st < 256) { if (g_rsOk[st] && g_rs[st] == v) g_dv[57].x1[t.p]++; g_rs[st] = v; g_rsOk[st] = 1; }
    return DVO(57, DWORD(__stdcall*)(void*, DWORD, DWORD))(d, st, v);
}
static DWORD __stdcall dvw69(void* d, DWORD s, DWORD ty, DWORD v) {                // SetSamplerState
    HkTimer t(&g_dv[69]);
    int si = smpIdx(s);
    if (t.on && si >= 0 && ty < 16) { if (g_ssOk[si][ty] && g_ss[si][ty] == v) g_dv[69].x1[t.p]++; g_ss[si][ty] = v; g_ssOk[si][ty] = 1; }
    return DVO(69, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))(d, s, ty, v);
}
#define DV_SIMPLE_RED(S, VAR, OK) static DWORD __stdcall dvw##S(void* d, DWORD a) { \
    HkTimer t(&g_dv[S]); if (t.on) { if (OK && VAR == a) g_dv[S].x1[t.p]++; VAR = a; OK = 1; } \
    return DVO(S, DWORD(__stdcall*)(void*, DWORD))(d, a); }
DV_SIMPLE_RED(87, g_decl, g_declOk) DV_SIMPLE_RED(89, g_fvf, g_fvfOk) DV_SIMPLE_RED(92, g_vsh, g_vshOk) DV_SIMPLE_RED(107, g_psh, g_pshOk)
static DWORD __stdcall dvw94(void* d, DWORD start, DWORD data, DWORD count) {      // SetVertexShaderConstantF
    HkTimer t(&g_dv[94]);
    if (t.on && data && start < 256) {
        DWORD n = count; if (start + n > 256) n = 256 - start;
        const DWORD* src = (const DWORD*)(ULONG_PTR)data; LONG red = 0;
        for (DWORD r = 0; r < n; ++r) {
            DWORD* dst = &g_vsc[(start + r) * 4];
            if (g_vscOk[start + r] && dst[0] == src[r*4] && dst[1] == src[r*4+1] && dst[2] == src[r*4+2] && dst[3] == src[r*4+3]) ++red;
            else { dst[0] = src[r*4]; dst[1] = src[r*4+1]; dst[2] = src[r*4+2]; dst[3] = src[r*4+3]; g_vscOk[start + r] = 1; }
        }
        g_dv[94].x1[t.p] += count; g_dv[94].x2[t.p] += red;
    }
    return DVO(94, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))(d, start, data, count);
}
static DWORD __stdcall dvw109(void* d, DWORD start, DWORD data, DWORD count) {     // SetPixelShaderConstantF
    HkTimer t(&g_dv[109]);
    if (t.on && data && start < 224) {
        DWORD n = count; if (start + n > 224) n = 224 - start;
        const DWORD* src = (const DWORD*)(ULONG_PTR)data; LONG red = 0;
        for (DWORD r = 0; r < n; ++r) {
            DWORD* dst = &g_psc[(start + r) * 4];
            if (g_pscOk[start + r] && dst[0] == src[r*4] && dst[1] == src[r*4+1] && dst[2] == src[r*4+2] && dst[3] == src[r*4+3]) ++red;
            else { dst[0] = src[r*4]; dst[1] = src[r*4+1]; dst[2] = src[r*4+2]; dst[3] = src[r*4+3]; g_pscOk[start + r] = 1; }
        }
        g_dv[109].x1[t.p] += count; g_dv[109].x2[t.p] += red;
    }
    return DVO(109, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))(d, start, data, count);
}
static DWORD __stdcall dvw81(void* d, DWORD ty, DWORD sv, DWORD pc) {             // DrawPrimitive
    HkTimer t(&g_dv[81]); if (t.on) g_dv[81].x1[t.p] += pc;
    return DVO(81, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))(d, ty, sv, pc);
}
static DWORD __stdcall dvw82(void* d, DWORD ty, DWORD bv, DWORD mi, DWORD nv, DWORD si, DWORD pc) {   // DrawIndexedPrimitive
    HkTimer t(&g_dv[82]); if (t.on) g_dv[82].x1[t.p] += pc;
    return DVO(82, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))(d, ty, bv, mi, nv, si, pc);
}
static DWORD __stdcall dvw83(void* d, DWORD ty, DWORD pc, DWORD p, DWORD st) {    // DrawPrimitiveUP
    HkTimer t(&g_dv[83]); if (t.on) g_dv[83].x1[t.p] += pc;
    return DVO(83, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))(d, ty, pc, p, st);
}
static DWORD __stdcall dvw84(void* d, DWORD ty, DWORD mi, DWORD nv, DWORD pc, DWORD ix, DWORD fmt, DWORD vd, DWORD st) {  // DrawIndexedPrimitiveUP
    HkTimer t(&g_dv[84]); if (t.on) g_dv[84].x1[t.p] += pc;
    return DVO(84, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))(d, ty, mi, nv, pc, ix, fmt, vd, st);
}

// lazy hooks on objects handed to the device: vertex/index buffers (Lock 11 / Unlock 12), 2D textures
// (LockRect 19 / UnlockRect 20), state blocks (Capture 4 / Apply 5). One entry per distinct vtable.
#define OBJ_KINDS 4
enum { OK_VB = 0, OK_IB, OK_TEX, OK_SB };
static const char* const kObjName[OBJ_KINDS] = {"VertexBuffer", "IndexBuffer", "Texture2D", "StateBlock"};
static const int kObjSlotA[OBJ_KINDS] = {11, 11, 19, 4}, kObjSlotB[OBJ_KINDS] = {12, 12, 20, 5};
static DWORD* g_objVt[OBJ_KINDS][4]; static void* g_objOrigA[OBJ_KINDS][4]; static void* g_objOrigB[OBJ_KINDS][4]; static int g_nObjVt[OBJ_KINDS];
static Stat g_objA[OBJ_KINDS], g_objB[OBJ_KINDS];
static __forceinline int objIdx(int k, void* o) {
    DWORD* vt = *(DWORD**)o;
    for (int i = 0; i < g_nObjVt[k]; ++i) if (g_objVt[k][i] == vt) return i;
    return -1;
}
static DWORD __stdcall ovbLock(void* o, DWORD off, DWORD sz, DWORD pp, DWORD fl) {
    int i = objIdx(OK_VB, o); HkTimer t(&g_objA[OK_VB]);
    if (t.on) { if (fl & 0x2000) g_objA[OK_VB].x1[t.p]++; if (fl & 0x1000) g_objA[OK_VB].x2[t.p]++; }
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))g_objOrigA[OK_VB][i < 0 ? 0 : i])(o, off, sz, pp, fl);
}
static DWORD __stdcall ovbUnlock(void* o) { int i = objIdx(OK_VB, o); HkTimer t(&g_objB[OK_VB]); return ((DWORD(__stdcall*)(void*))g_objOrigB[OK_VB][i < 0 ? 0 : i])(o); }
static DWORD __stdcall oibLock(void* o, DWORD off, DWORD sz, DWORD pp, DWORD fl) {
    int i = objIdx(OK_IB, o); HkTimer t(&g_objA[OK_IB]);
    if (t.on) { if (fl & 0x2000) g_objA[OK_IB].x1[t.p]++; if (fl & 0x1000) g_objA[OK_IB].x2[t.p]++; }
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))g_objOrigA[OK_IB][i < 0 ? 0 : i])(o, off, sz, pp, fl);
}
static DWORD __stdcall oibUnlock(void* o) { int i = objIdx(OK_IB, o); HkTimer t(&g_objB[OK_IB]); return ((DWORD(__stdcall*)(void*))g_objOrigB[OK_IB][i < 0 ? 0 : i])(o); }
static DWORD __stdcall otxLock(void* o, DWORD lvl, DWORD lr, DWORD rc, DWORD fl) {
    int i = objIdx(OK_TEX, o); HkTimer t(&g_objA[OK_TEX]);
    if (t.on) { if (fl & 0x2000) g_objA[OK_TEX].x1[t.p]++; if (fl & 0x10) g_objA[OK_TEX].x2[t.p]++; }   // DISCARD, READONLY
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))g_objOrigA[OK_TEX][i < 0 ? 0 : i])(o, lvl, lr, rc, fl);
}
static DWORD __stdcall otxUnlock(void* o, DWORD lvl) { int i = objIdx(OK_TEX, o); HkTimer t(&g_objB[OK_TEX]); return ((DWORD(__stdcall*)(void*, DWORD))g_objOrigB[OK_TEX][i < 0 ? 0 : i])(o, lvl); }
static DWORD __stdcall osbCapture(void* o) { int i = objIdx(OK_SB, o); HkTimer t(&g_objA[OK_SB]); return ((DWORD(__stdcall*)(void*))g_objOrigA[OK_SB][i < 0 ? 0 : i])(o); }
static DWORD __stdcall osbApply(void* o) {
    int i = objIdx(OK_SB, o); HkTimer t(&g_objB[OK_SB]);
    DWORD r = ((DWORD(__stdcall*)(void*))g_objOrigB[OK_SB][i < 0 ? 0 : i])(o);
    if (t.on) dvInvalidate();
    return r;
}
static void* const kObjHookA[OBJ_KINDS] = {(void*)&ovbLock, (void*)&oibLock, (void*)&otxLock, (void*)&osbCapture};
static void* const kObjHookB[OBJ_KINDS] = {(void*)&ovbUnlock, (void*)&oibUnlock, (void*)&otxUnlock, (void*)&osbApply};
static void hookObject(int k, void* o) {                   // render thread only
    if (!o) return;
    __try {
        DWORD* vt = *(DWORD**)o;
        for (int i = 0; i < g_nObjVt[k]; ++i) if (g_objVt[k][i] == vt) return;
        if (g_nObjVt[k] >= 4) return;
        DWORD fa = vt[kObjSlotA[k]], fb = vt[kObjSlotB[k]];
        if ((DWORD)(ULONG_PTR)vt < g_d3d9Lo || (DWORD)(ULONG_PTR)vt >= g_d3d9Hi || fa < g_d3d9Lo || fa >= g_d3d9Hi || fb < g_d3d9Lo || fb >= g_d3d9Hi) return;
        int n = g_nObjVt[k];
        g_objOrigA[k][n] = (void*)(ULONG_PTR)fa; g_objOrigB[k][n] = (void*)(ULONG_PTR)fb; g_objVt[k][n] = vt;
        g_nObjVt[k] = n + 1;                                // published before the vtable write
        DWORD op = 0;
        if (VirtualProtect(&vt[kObjSlotA[k]], 4 * (kObjSlotB[k] - kObjSlotA[k] + 1), PAGE_READWRITE, &op)) {
            vt[kObjSlotA[k]] = (DWORD)(ULONG_PTR)kObjHookA[k];
            vt[kObjSlotB[k]] = (DWORD)(ULONG_PTR)kObjHookB[k];
            VirtualProtect(&vt[kObjSlotA[k]], 4 * (kObjSlotB[k] - kObjSlotA[k] + 1), op, &op);
            logf("dev-hooks: %s vtable %08X hooked (slots %d/%d).", kObjName[k], (DWORD)(ULONG_PTR)vt, kObjSlotA[k], kObjSlotB[k]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static DWORD* g_texChecked[16]; static int g_nTexChecked = 0;
static void texProbe(DWORD tex) {                           // no C++ objects here: SEH allowed
    __try {
        DWORD* tvt = *(DWORD**)(ULONG_PTR)tex;
        for (int i = 0; i < g_nTexChecked; ++i) if (g_texChecked[i] == tvt) return;
        if (g_nTexChecked >= 16) return;
        g_texChecked[g_nTexChecked++] = tvt;
        DWORD ty = ((DWORD(__stdcall*)(void*))tvt[10])((void*)(ULONG_PTR)tex);   // IDirect3DResource9::GetType
        if (ty == 3) hookObject(OK_TEX, (void*)(ULONG_PTR)tex);                  // D3DRTYPE_TEXTURE
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void sbProbe(DWORD ppSB) { __try { hookObject(OK_SB, *(void**)(ULONG_PTR)ppSB); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static DWORD __stdcall dvw65(void* d, DWORD stage, DWORD tex) {                    // SetTexture (+ lazy LockRect hook)
    HkTimer t(&g_dv[65]);
    int si = smpIdx(stage);
    if (t.on) {
        if (si >= 0) { if (g_txOk[si] && g_tx[si] == tex) g_dv[65].x1[t.p]++; g_tx[si] = tex; g_txOk[si] = 1; }
        if (tex && g_nObjVt[OK_TEX] < 4) texProbe(tex);
    }
    return DVO(65, DWORD(__stdcall*)(void*, DWORD, DWORD))(d, stage, tex);
}
static DWORD __stdcall dvw100(void* d, DWORD n, DWORD vb, DWORD off, DWORD stride) {   // SetStreamSource (+ lazy VB hook)
    HkTimer t(&g_dv[100]);
    if (t.on && n < 8) {
        if (g_strmOk[n] && g_strm[n][0] == vb && g_strm[n][1] == off && g_strm[n][2] == stride) g_dv[100].x1[t.p]++;
        g_strm[n][0] = vb; g_strm[n][1] = off; g_strm[n][2] = stride; g_strmOk[n] = 1;
        if (vb) hookObject(OK_VB, (void*)(ULONG_PTR)vb);
    }
    return DVO(100, DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))(d, n, vb, off, stride);
}
static DWORD __stdcall dvw104(void* d, DWORD ib) {                                 // SetIndices (+ lazy IB hook)
    HkTimer t(&g_dv[104]);
    if (t.on) { if (g_ibufOk && g_ibuf == ib) g_dv[104].x1[t.p]++; g_ibuf = ib; g_ibufOk = 1; if (ib) hookObject(OK_IB, (void*)(ULONG_PTR)ib); }
    return DVO(104, DWORD(__stdcall*)(void*, DWORD))(d, ib);
}
static DWORD __stdcall dvw59(void* d, DWORD type, DWORD ppSB) {                    // CreateStateBlock (+ lazy SB hook)
    HkTimer t(&g_dv[59]);
    DWORD r = DVO(59, DWORD(__stdcall*)(void*, DWORD, DWORD))(d, type, ppSB);
    if (t.on && r == 0 && ppSB) sbProbe(ppSB);
    return r;
}
static DWORD __stdcall dvw61(void* d, DWORD ppSB) {                                // EndStateBlock (+ lazy SB hook)
    HkTimer t(&g_dv[61]);
    DWORD r = DVO(61, DWORD(__stdcall*)(void*, DWORD))(d, ppSB);
    if (t.on && r == 0 && ppSB) sbProbe(ppSB);
    return r;
}

struct DvHookDef { int slot; void* fn; BYTE kind; };
static const DvHookDef kDvHooks[] = {
    {16, (void*)&dvw16, 0}, {17, (void*)&dvw17, 0}, {23, (void*)&dvw23, 0}, {26, (void*)&dvw26, 0}, {27, (void*)&dvw27, 0},
    {30, (void*)&dvw30, 0}, {31, (void*)&dvw31, 0}, {32, (void*)&dvw32, 0}, {34, (void*)&dvw34, 0}, {35, (void*)&dvw35, 0},
    {37, (void*)&dvw37, 0}, {39, (void*)&dvw39, 0}, {41, (void*)&dvw41, 0}, {42, (void*)&dvw42, 0}, {43, (void*)&dvw43, 0},
    {44, (void*)&dvw44, 0}, {47, (void*)&dvw47, 0}, {49, (void*)&dvw49, 0}, {51, (void*)&dvw51, 0}, {53, (void*)&dvw53, 0},
    {55, (void*)&dvw55, 0}, {57, (void*)&dvw57, 1}, {59, (void*)&dvw59, 0}, {61, (void*)&dvw61, 0}, {65, (void*)&dvw65, 1},
    {67, (void*)&dvw67, 0}, {69, (void*)&dvw69, 1}, {75, (void*)&dvw75, 0}, {81, (void*)&dvw81, 3}, {82, (void*)&dvw82, 3},
    {83, (void*)&dvw83, 3}, {84, (void*)&dvw84, 3}, {87, (void*)&dvw87, 1}, {89, (void*)&dvw89, 1}, {92, (void*)&dvw92, 1},
    {94, (void*)&dvw94, 2}, {96, (void*)&dvw96, 0}, {98, (void*)&dvw98, 0}, {100, (void*)&dvw100, 1}, {104, (void*)&dvw104, 1},
    {107, (void*)&dvw107, 1}, {109, (void*)&dvw109, 2}, {111, (void*)&dvw111, 0}, {113, (void*)&dvw113, 0},
};
#define NDVH ((int)(sizeof(kDvHooks) / sizeof(kDvHooks[0])))
static void installDeviceHooks(void* fx) {                // render thread, from the first BeginPass
    g_devTried = true;
    __try {
        HMODULE h9 = GetModuleHandleA("d3d9.dll");
        if (!h9) { logf("dev-hooks: d3d9.dll not loaded - OFF."); return; }
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)h9 + ((IMAGE_DOS_HEADER*)h9)->e_lfanew);
        g_d3d9Lo = (DWORD)(ULONG_PTR)h9; g_d3d9Hi = g_d3d9Lo + nt->OptionalHeader.SizeOfImage;
        void* dev = NULL;
        ((DWORD(__stdcall*)(void*, void**))(*(DWORD**)fx)[68])(fx, &dev);     // ID3DXEffect::GetDevice
        if (!dev) { logf("dev-hooks: GetDevice returned NULL - OFF."); return; }
        ((DWORD(__stdcall*)(void*))(*(DWORD**)dev)[2])(dev);                  // Release the reference GetDevice added
        DWORD* vt = *(DWORD**)dev;
        if ((DWORD)(ULONG_PTR)vt < g_d3d9Lo || (DWORD)(ULONG_PTR)vt >= g_d3d9Hi) {
            logf("dev-hooks: device vtable %08X is outside d3d9.dll (%08X-%08X) - wrapped device, OFF.", (DWORD)(ULONG_PTR)vt, g_d3d9Lo, g_d3d9Hi);
            return;
        }
        for (int i = 0; i < NDVH; ++i) {
            DWORD f = vt[kDvHooks[i].slot];
            if (f < g_d3d9Lo || f >= g_d3d9Hi) { logf("dev-hooks: slot %d target %08X outside d3d9.dll - OFF.", kDvHooks[i].slot, f); return; }
        }
        DWORD op = 0;
        if (!VirtualProtect(vt, 4 * DV_SLOTS, PAGE_READWRITE, &op)) { logf("dev-hooks: VirtualProtect failed - OFF."); return; }
        for (int i = 0; i < NDVH; ++i) {
            int s = kDvHooks[i].slot;
            g_devOrig[s] = (void*)(ULONG_PTR)vt[s];
            g_dvKind[s] = kDvHooks[i].kind;
            vt[s] = (DWORD)(ULONG_PTR)kDvHooks[i].fn;        // original published first, then the slot
        }
        VirtualProtect(vt, 4 * DV_SLOTS, op, &op);
        logf("dev-hooks: %d IDirect3DDevice9 methods hooked on device %08X (vtable %08X in d3d9.dll %08X-%08X).",
             NDVH, (DWORD)(ULONG_PTR)dev, (DWORD)(ULONG_PTR)vt, g_d3d9Lo, g_d3d9Hi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("dev-hooks: install faulted (0x%08lX) - OFF.", (unsigned long)GetExceptionCode());
    }
}

// ---------------------------------------------------------------- sampler v3 (stage-tagged)
#define NCLASS 12
enum { C_UNMARKED = 0, C_SH_OWN, C_SH_BATCH, C_SH_MESH, C_SH_SUB, C_VW_OWN, C_VW_BATCH, C_VW_MESH, C_VW_SUB, C_VW_PART, C_UI, C_OTHER };
static const char* const kClassName[NCLASS] = {
    "unmarked (logic, client update, audio, Present)", "shadow pass: own code", "shadow pass: FX batch own", "shadow pass: inside mesh events",
    "shadow pass: other sub-stages", "main view: own code", "main view: FX batch own", "main view: inside mesh events",
    "main view: other sub-stages", "main view: particles", "UI", "other top-level stage" };
static const bool kDeep[NCLASS] = { true, true, false, false, false, true, false, false, true, true, true, true };
#define LTW 16384
#define LTA 65536
struct LeafEnt { DWORD key; DWORD count; };
struct ClassTab { LeafEnt* leafW; LeafEnt* leafA; LeafEnt* inclW; LeafEnt* inclA; LONG nW; LONG64 nA; LONG drop; };
static ClassTab g_ct[NCLASS];
#define PMODS 160
struct PMod { DWORD base, size; char name[40]; };
static PMod  g_pm[PMODS]; static int g_npm = 0;
static DWORD g_gameBase = 0x400000, g_gameEnd = 0, g_textLo = 0, g_textHi = 0;
static DWORD g_profSelfTid = 0;
static LONG  g_winSamples = 0, g_winBadStack = 0;
typedef struct _PROF_TBI { LONG ExitStatus; PVOID TebBaseAddress; HANDLE cidProcess; HANDLE cidThread; ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority; } PROF_TBI;
typedef LONG (NTAPI* tNtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
static tNtQIT p_NtQIT = NULL;

static void tabAdd(LeafEnt* tab, int size, DWORD key, DWORD count, LONG* drop) {
    DWORD h = (key * 2654435761u) >> 7;
    for (int i = 0; i < 128; ++i) {
        LeafEnt* e = &tab[(h + i) & (size - 1)];
        if (e->key == key) { e->count += count; return; }
        if (e->key == 0)   { e->key = key; e->count = count; return; }
    }
    if (drop) ++*drop;
}
static void profRefreshModules() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof(me); int n = 0;
    if (Module32First(snap, &me)) do {
        if (n < PMODS) { g_pm[n].base = (DWORD)(ULONG_PTR)me.modBaseAddr; g_pm[n].size = me.modBaseSize; lstrcpynA(g_pm[n].name, me.szModule, 40); ++n; }
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    g_npm = n;
}
static int pmOf(DWORD a) { for (int i = 0; i < g_npm; ++i) if (a >= g_pm[i].base && a < g_pm[i].base + g_pm[i].size) return i; return -1; }
struct ExpSym { DWORD rva; const char* name; };
struct ExpMod { DWORD base; int n; ExpSym* syms; };
static ExpMod g_exp[32]; static int g_nexp = 0;
static int __cdecl expCmp(const void* a, const void* b) { DWORD x = ((const ExpSym*)a)->rva, y = ((const ExpSym*)b)->rva; return x < y ? -1 : (x > y ? 1 : 0); }
static ExpMod* expFor(DWORD base) {
    for (int i = 0; i < g_nexp; ++i) if (g_exp[i].base == base) return &g_exp[i];
    if (g_nexp >= 32) return NULL;
    ExpMod* m = &g_exp[g_nexp++]; m->base = base; m->n = 0; m->syms = NULL;
    __try {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)(ULONG_PTR)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return m;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)dos + dos->e_lfanew); if (nt->Signature != IMAGE_NT_SIGNATURE) return m;
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dd.VirtualAddress || !dd.Size) return m;
        IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(ULONG_PTR)(base + dd.VirtualAddress);
        DWORD* funcs = (DWORD*)(ULONG_PTR)(base + ed->AddressOfFunctions);
        DWORD* names = (DWORD*)(ULONG_PTR)(base + ed->AddressOfNames);
        WORD*  ords  = (WORD*) (ULONG_PTR)(base + ed->AddressOfNameOrdinals);
        int nn = (int)ed->NumberOfNames; if (nn <= 0 || nn > 20000) return m;
        ExpSym* s = (ExpSym*)VirtualAlloc(NULL, nn * sizeof(ExpSym), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (!s) return m;
        int k = 0;
        for (int i = 0; i < nn; ++i) {
            DWORD o = ords[i]; if (o >= ed->NumberOfFunctions) continue;
            DWORD rva = funcs[o];
            if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) continue;
            s[k].rva = rva; s[k].name = (const char*)(ULONG_PTR)(base + names[i]); ++k;
        }
        qsort(s, k, sizeof(ExpSym), expCmp); m->syms = s; m->n = k;
    } __except (EXCEPTION_EXECUTE_HANDLER) { m->n = 0; }
    return m;
}
#ifdef AOTR_DIAG
// v77: this DLL's own functions by name in the diagnostics lines - from the map file build.bat writes next to it
// (out\bfme2_accel.map: "0001:00012345  ?name@@...  10013345 f   aotr_accel.obj", static functions included).
struct OurSym { DWORD rva; char name[44]; };
static OurSym* g_ourSyms = NULL; static int g_nOurSyms = 0; static DWORD g_ourBase = 0, g_ourEnd = 0; static volatile LONG g_ourMapTried = 0;
static int ourSymCmp(const void* a, const void* b) { DWORD x = ((const OurSym*)a)->rva, y = ((const OurSym*)b)->rva; return x < y ? -1 : x > y; }
static void ourMapLoad() {
    if (InterlockedExchange(&g_ourMapTried, 1)) return;
    HMODULE me = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ourMapLoad, &me)) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)me + ((IMAGE_DOS_HEADER*)me)->e_lfanew);
    g_ourBase = (DWORD)(ULONG_PTR)me; g_ourEnd = g_ourBase + nt->OptionalHeader.SizeOfImage;
    char path[MAX_PATH]; aotrPath(path, "bfme2_accel.map");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL);
    char* b = (size && size < (32u << 20)) ? (char*)VirtualAlloc(NULL, size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    DWORD got = 0; if (b) ReadFile(f, b, size, &got, NULL); CloseHandle(f);
    if (!b) return;
    b[got] = 0;
    DWORD pref = 0x10000000; char* pl = strstr(b, "Preferred load address is "); if (pl) pref = strtoul(pl + 26, NULL, 16);
    int cap = 1 << 16; OurSym* t = (OurSym*)VirtualAlloc(NULL, cap * sizeof(OurSym), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); int n = 0;
    for (char* l = b; t && *l && n < cap; ) {
        char* e = l; while (*e && *e != '\n') ++e;
        char sv = *e; *e = 0;
        // " 0001:00012345       ?name@@YAXXZ               10013345 f   aotr_accel.obj"
        if (l[0] == ' ' && l[1] == '0' && l[5] == ':') {
            char nm[256]; unsigned va = 0; char fl = 0;
            if (sscanf(l + 15, "%255s %x %c", nm, &va, &fl) >= 2 && va > pref && va - pref < (64u << 20) && (fl == 'f' || fl == 'F' || fl == 'i')) {
                const char* q = nm[0] == '?' ? nm + 1 : (nm[0] == '_' ? nm + 1 : nm);
                int k = 0; while (q[k] && q[k] != '@' && k < 43) { t[n].name[k] = q[k]; ++k; } t[n].name[k] = 0;
                t[n].rva = va - pref; ++n;
            }
        }
        *e = sv; l = *e ? e + 1 : e;
    }
    VirtualFree(b, 0, MEM_RELEASE);
    if (!n) { if (t) VirtualFree(t, 0, MEM_RELEASE); return; }
    qsort(t, n, sizeof(OurSym), ourSymCmp);
    g_ourSyms = t; g_nOurSyms = n;
    logf("symbols: %d functions of this DLL read from bfme2_accel.map - the diagnostics lines name them.", n);
}
#endif
static void symName(DWORD addr, char* out) {
    if (addr >= g_gameBase && addr < g_gameEnd) { wsprintfA(out, "game.dat+%06X", addr - g_gameBase); return; }
#ifdef AOTR_DIAG
    if (!g_ourMapTried) ourMapLoad();
    if (g_nOurSyms && addr >= g_ourBase && addr < g_ourEnd) {
        DWORD rva = addr - g_ourBase; int lo = 0, hi = g_nOurSyms - 1, best = -1;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (g_ourSyms[mid].rva <= rva) { best = mid; lo = mid + 1; } else hi = mid - 1; }
        if (best >= 0 && rva - g_ourSyms[best].rva < 0x8000) { wsprintfA(out, "accel!%s+%X", g_ourSyms[best].name, rva - g_ourSyms[best].rva); return; }
    }
#endif
    int mi = pmOf(addr);
    if (mi < 0) { wsprintfA(out, "?%08X", addr); return; }
    DWORD rva = addr - g_pm[mi].base;
    ExpMod* m = expFor(g_pm[mi].base);
    if (m && m->n) {
        int lo = 0, hi = m->n - 1, best = -1;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (m->syms[mid].rva <= rva) { best = mid; lo = mid + 1; } else hi = mid - 1; }
        if (best >= 0 && rva - m->syms[best].rva < 0x4000) {
            char nm[96]; lstrcpynA(nm, m->syms[best].name, 96);
            wsprintfA(out, "%s!%s+%X", g_pm[mi].name, nm, rva - m->syms[best].rva); return;
        }
    }
    wsprintfA(out, "%s+%06X", g_pm[mi].name, rva);
}
static char* msStr(LONG64 ticks, LONG64 div, char* buf) {
    if (div <= 0) div = 1;
    LONG64 us = ticks * 1000000 / g_pqpf.QuadPart / div;
    if (us < 0) { wsprintfA(buf, "-%d.%03d", (int)(-us / 1000), (int)(-us % 1000)); return buf; }
    wsprintfA(buf, "%d.%03d", (int)(us / 1000), (int)(us % 1000)); return buf;
}
static char* pctStr(LONG64 num, LONG64 den, char* buf) {
    if (den <= 0) den = 1; LONG64 p10 = num * 1000 / den;
    wsprintfA(buf, "%d.%d", (int)(p10 / 10), (int)(p10 % 10)); return buf;
}
static char* perStr(LONG64 num, LONG64 den, char* buf) {       // num/den with one decimal
    if (den <= 0) den = 1; LONG64 v10 = num * 10 / den;
    wsprintfA(buf, "%d.%d", (int)(v10 / 10), (int)(v10 % 10)); return buf;
}
static DWORD profPickBusy() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), best = 0; ULONG64 bestCpu = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_profSelfTid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) {
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                ULONG64 t = (((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime);
                if (t > bestCpu) { bestCpu = t; best = te.th32ThreadID; }
            }
            CloseHandle(h);
        }
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return best;
}
static __forceinline int sampleClass() {
    int d = g_mkDepth;
    if (d <= 0) return C_UNMARKED;
    int top = g_curTop;
    MkEnt* inner = (d - 1 < 64) ? g_mkStack[d - 1].e : NULL;
    int base;
    if (top == P_SHADOW) base = C_SH_OWN; else if (top == P_VIEWS) base = C_VW_OWN; else if (top == P_UI) return C_UI; else return C_OTHER;
    if (d == 1) return base;
    if (inner && inner == g_mkMeshEnt) return base + 2;
    if (inner && inner == g_mkBatchEnt) return base + 1;
    if (top == P_VIEWS && inner && inner == g_mkPartEnt) return C_VW_PART;
    return base + 3;
}

// ---------------------------------------------------------------- battle accumulation + reports
static LONG64 g_bFrames = 0, g_bPeriod = 0, g_bSamples = 0; static int g_bWindows = 0, g_bReports = 0;
static void statWindow(Stat* s, bool battle) {
    for (int p = 0; p < NPASS; ++p) {
        LONG c = s->cnt[p]; LONG64 i = rd64(&s->incl[p]), e = rd64(&s->excl[p]), a = rd64(&s->x1[p]), b = rd64(&s->x2[p]);
        if (battle) { s->acnt[p] += c - s->lcnt[p]; s->aincl[p] += i - s->lincl[p]; s->aexcl[p] += e - s->lexcl[p]; s->ax1[p] += a - s->lx1[p]; s->ax2[p] += b - s->lx2[p]; }
        s->lcnt[p] = c; s->lincl[p] = i; s->lexcl[p] = e; s->lx1[p] = a; s->lx2[p] = b;
    }
}
static void reportStatLine(const char* name, Stat* s, int kind, LONG64 F) {
    LONG64 tc = 0, te = 0, ti = 0; for (int p = 0; p < NPASS; ++p) { tc += s->acnt[p]; te += s->aexcl[p]; ti += s->aincl[p]; }
    if (tc == 0) return;
    char c[NPASS][16], e[NPASS][16], b1[16], b2[16], b3[16], b4[16], b5[16];
    for (int p = 0; p < NPASS; ++p) { perStr(s->acnt[p], F, c[p]); msStr(s->aexcl[p], F, e[p]); }
    LONG64 x1 = 0, x2 = 0; for (int p = 0; p < NPASS; ++p) { x1 += s->ax1[p]; x2 += s->ax2[p]; }
    char extra[96]; extra[0] = 0;
    if (kind == 1) wsprintfA(extra, "  redundant %s%%", pctStr(x1, tc, b3));
    else if (kind == 2) wsprintfA(extra, "  reg/f %s  redundant reg %s%%", perStr(x1, F, b3), pctStr(x2, x1, b4));
    else if (kind == 3) wsprintfA(extra, "  prims/f %s", perStr(x1, F, b3));
    else if (kind == 4) wsprintfA(extra, "  flagA %s%%  flagB %s%%", pctStr(x1, tc, b3), pctStr(x2, tc, b4));
    else if (kind == 5) wsprintfA(extra, "  bytes/f %s  name-handles %s%%", perStr(x1, F, b3), pctStr(x2, tc, b4));
    else if (kind == 6) wsprintfA(extra, "  name-handles %s%%", pctStr(x2, tc, b3));
    logf("   %-26s calls/f %8s %8s %8s %8s | excl ms/f %7s %7s %7s %7s | incl ms/f %7s%s",
         name, c[P_SHADOW], c[P_VIEWS], c[P_NONE], c[P_UI], e[P_SHADOW], e[P_VIEWS], e[P_NONE], e[P_UI], msStr(ti, F, b5), extra);
    (void)b1; (void)b2;
}
static void battleReport() {
    LONG64 F = g_bFrames > 0 ? g_bFrames : 1;
    char m1[16], m2[16], m3[16], m4[16];
    ++g_bReports;
    logf("==== BATTLE REPORT #%d: %d battle windows, %d frames, %s ms/frame, %d samples ====",
         g_bReports, g_bWindows, (int)g_bFrames, msStr(g_bPeriod, F, m1), (int)g_bSamples);
    // stages
    LONG64 topSum = 0;
    for (int p = P_SHADOW; p <= P_UI; ++p) if (g_mkPassEnt[p]) topSum += g_mkPassEnt[p]->aIncl;
    logf("-- stages (ms per frame; incl = with children, excl = own; shadow+views+ui = %s, unmarked = %s) --",
         msStr(topSum, F, m2), msStr(g_bPeriod - topSum, F, m3));
    int order[MK_ENTS]; int n = 0;
    for (int i = 0; i < MK_ENTS; ++i) if (g_mk[i].hash && g_mk[i].aCount > 0) order[n++] = i;
    for (int r = 0; r < n; ++r) { int bi = r; for (int k = r + 1; k < n; ++k) if (g_mk[order[k]].aIncl > g_mk[order[bi]].aIncl) bi = k; int t = order[r]; order[r] = order[bi]; order[bi] = t; }
    for (int r = 0; r < n && r < 60; ++r) { MkEnt* e = &g_mk[order[r]];
        if (e->aIncl * 1000 / F < g_pqpf.QuadPart / 100) continue;          // hide < 0.01 ms/frame
        logf("   %-44s incl %8s  excl %8s  x%s/frame", e->name, msStr(e->aIncl, F, m1), msStr(e->aExcl, F, m2), perStr(e->aCount, F, m4)); }
    // D3DX
    logf("-- ID3DXEffect methods (calls per frame and exclusive ms per frame by pass: shadow / views / unmarked / ui) --");
    for (int i = 0; i < NFX; ++i) reportStatLine(kFxDefs[i].name, &g_fx[i], i == FX_BEGIN ? 4 : (i == FX_SETRAWVALUE ? 5 : (kFxDefs[i].hArg ? 6 : 0)), F);
    // device
    logf("-- IDirect3DDevice9 (DXVK) methods (same columns; redundant = same value as last set, since last state-block Apply) --");
    for (int s = 0; s < DV_SLOTS; ++s) if (g_devOrig[s]) reportStatLine(kDevSlot[s], &g_dv[s], g_dvKind[s], F);
    logf("-- object methods (flagA/flagB: buffers DISCARD/NOOVERWRITE, textures DISCARD/READONLY); state invalidations %d --", (int)g_dvInvalidations);
    for (int k = 0; k < OBJ_KINDS; ++k) {
        char na[48], nb[48];
        wsprintfA(na, "%s.%s", kObjName[k], k == OK_SB ? "Capture" : (k == OK_TEX ? "LockRect" : "Lock"));
        wsprintfA(nb, "%s.%s", kObjName[k], k == OK_SB ? "Apply" : (k == OK_TEX ? "UnlockRect" : "Unlock"));
        reportStatLine(na, &g_objA[k], k == OK_SB ? 0 : 4, F); reportStatLine(nb, &g_objB[k], 0, F);
    }
    // sampler classes
    LONG64 tot = 0; for (int c = 0; c < NCLASS; ++c) tot += g_ct[c].nA;
    if (tot <= 0) return;
    profRefreshModules();
    logf("-- sampler: where the render thread was (share of battle samples, ms/frame equivalent) --");
    for (int c = 0; c < NCLASS; ++c) {
        ClassTab& ct = g_ct[c];
        if (ct.nA * 1000 / tot < 5) continue;                                  // skip classes under 0.5%
        logf("   [%s] %s%% = %s ms/frame, %d samples, table drops %d", kClassName[c], pctStr(ct.nA, tot, m1),
             msStr(g_bPeriod * ct.nA / tot, F, m2), (int)ct.nA, (int)ct.drop);
        // module split from the leaf table
        struct { int mi; DWORD cnt; } ms[PMODS + 1]; int nm = 0; DWORD unk = 0;
        for (int i = 0; i < LTA; ++i) { if (!ct.leafA[i].key) continue;
            int mi = pmOf(ct.leafA[i].key); if (mi < 0) { unk += ct.leafA[i].count; continue; }
            int k = 0; for (; k < nm; ++k) if (ms[k].mi == mi) { ms[k].cnt += ct.leafA[i].count; break; }
            if (k == nm && nm < PMODS) { ms[nm].mi = mi; ms[nm].cnt = ct.leafA[i].count; ++nm; } }
        char line[700]; int p = wsprintfA(line, "      modules:");
        for (int r = 0; r < 8; ++r) { int bi = -1; DWORD bc = 0; for (int k = 0; k < nm; ++k) if (ms[k].cnt > bc) { bc = ms[k].cnt; bi = k; }
            if (bi < 0 || p > 620) break; p += wsprintfA(line + p, " %s %s%%", g_pm[ms[bi].mi].name, pctStr(bc, ct.nA, m1)); ms[bi].cnt = 0; }
        if (unk) wsprintfA(line + p, " <unattributed> %s%%", pctStr(unk, ct.nA, m1));
        logf("%s", line);
        // top leaves
        LeafEnt top[16]; int nt = 0;
        for (int i = 0; i < LTA; ++i) { if (!ct.leafA[i].key) continue; DWORD cnt = ct.leafA[i].count;
            int j = nt; if (nt < 16) nt++; else if (cnt <= top[15].count) continue; else j = 15;
            while (j > 0 && top[j - 1].count < cnt) { top[j] = top[j - 1]; --j; }
            top[j] = ct.leafA[i]; }
        for (int i = 0; i < nt; ++i) { char s[200]; symName(top[i].key, s); logf("      leaf %6s%%  %08X  %s", pctStr(top[i].count, ct.nA, m1), top[i].key, s); }
        if (kDeep[c]) {
            LeafEnt ti[24]; int ni = 0;
            for (int i = 0; i < LTA; ++i) { if (!ct.inclA[i].key) continue; DWORD cnt = ct.inclA[i].count;
                int j = ni; if (ni < 24) ni++; else if (cnt <= ti[23].count) continue; else j = 23;
                while (j > 0 && ti[j - 1].count < cnt) { ti[j] = ti[j - 1]; --j; }
                ti[j] = ct.inclA[i]; }
            char line2[900]; int q = wsprintfA(line2, "      on-stack game.dat return addresses:");
            for (int i = 0; i < ni && q < 840; ++i) q += wsprintfA(line2 + q, " %08X:%s%%", ti[i].key, pctStr(ti[i].count, ct.nA, m1));
            logf("%s", line2);
        }
    }
    (void)m4;
}

static DWORD WINAPI profThread(LPVOID) {
    g_profSelfTid = GetCurrentThreadId();
    DWORD target = 0, lastPick = 0, lastReport = GetTickCount(); void* teb = NULL;
    ULONG64 lastProcCpu = 0;
    LONG lastFrames = 0; LONG64 lastPeriod = 0; LONG lastBegins = 0, lastEnds = 0;
    { FILETIME c, e, k, u; if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) lastProcCpu = (((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime); }
    for (;;) {
        DWORD now = GetTickCount();
        if (now - lastPick > 750) {
            DWORD t = profPickBusy();
            if (t != target) { target = t; teb = NULL;
                if (target && p_NtQIT) { HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, target);
                    if (h) { PROF_TBI tbi; if (p_NtQIT(h, 0, &tbi, sizeof(tbi), NULL) == 0) teb = tbi.TebBaseAddress; CloseHandle(h); } } }
            lastPick = now;
        }
        if (target && teb) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, target);
            if (h) {
                if (SuspendThread(h) != (DWORD)-1) {
                    CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &ctx)) {
                        __try {
                            DWORD eip = ctx.Eip, esp = ctx.Esp, ebp = ctx.Ebp;
                            int cls = (target == g_mkTid) ? sampleClass() : C_OTHER;
                            ClassTab& ct = g_ct[cls];
                            ct.nW++; g_winSamples++;
                            tabAdd(ct.leafW, LTW, eip & ~(DWORD)15, 1, &ct.drop);
                            if (kDeep[cls]) {
                                NT_TIB* tib = (NT_TIB*)teb; DWORD sb = (DWORD)(ULONG_PTR)tib->StackBase, sl = (DWORD)(ULONG_PTR)tib->StackLimit;
                                DWORD lo = esp > sl ? esp : sl, hi = sb;
                                DWORD seen[40]; int ns = 0; DWORD fp = ebp;
                                for (int f = 0; f < 40; ++f) {
                                    if (fp < lo || fp + 8 > hi) break;
                                    DWORD ret = *(DWORD*)(ULONG_PTR)(fp + 4);
                                    if (ret >= g_textLo && ret < g_textHi) {
                                        bool dup = false; for (int k = 0; k < ns; ++k) if (seen[k] == ret) { dup = true; break; }
                                        if (!dup) { seen[ns++] = ret; tabAdd(ct.inclW, LTW, ret, 1, &ct.drop); }
                                    }
                                    DWORD ne = *(DWORD*)(ULONG_PTR)fp;
                                    if (ne <= fp || (ne & 3)) break;
                                    fp = ne;
                                }
                            }
                        } __except (EXCEPTION_EXECUTE_HANDLER) { g_winBadStack++; }
                    }
                    ResumeThread(h);
                }
                CloseHandle(h);
            }
        }
        Sleep(1);
        if (now - lastReport >= 5000) {
            lastReport = now;
            ULONG64 pc = 0; { FILETIME c, e, k, u; if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) pc = (((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime); }
            int procPct = (int)((double)(pc - lastProcCpu) / 10000.0 / 5000.0 * 100.0); lastProcCpu = pc;
            if (g_mkLive) installPerfMarkers(false);
            LONG frames = g_mkFrames; LONG dF = frames - lastFrames; lastFrames = frames;
            LONG64 per = rd64(&g_mkPeriod); LONG64 dPer = per - lastPeriod; lastPeriod = per;
            LONG b = g_mkBegins, en = g_mkEnds; LONG dB = b - lastBegins, dE = en - lastEnds; lastBegins = b; lastEnds = en;
            bool battle = procPct >= 100 && dF >= 20 && g_winSamples > 200;
            // draws per pass this window (before the snapshot moves)
            LONG drw[NPASS]; for (int p = 0; p < NPASS; ++p) { drw[p] = 0; for (int s = 81; s <= 84; ++s) if (g_devOrig[s]) drw[p] += g_dv[s].cnt[p] - g_dv[s].lcnt[p]; }
            // top-level stage times this window
            LONG64 wSh = 0, wVw = 0, wUi = 0;
            for (int i = 0; i < MK_ENTS; ++i) { MkEnt* e = &g_mk[i]; if (!e->hash) continue;
                LONG64 I = rd64(&e->incl), E = rd64(&e->excl); LONG C = e->count;
                LONG64 dI = I - e->lIncl, dEx = E - e->lExcl; LONG dC = C - e->lCount;
                if (e == g_mkPassEnt[P_SHADOW]) wSh = dI; else if (e == g_mkPassEnt[P_VIEWS]) wVw = dI; else if (e == g_mkPassEnt[P_UI]) wUi = dI;
                if (battle) { e->aIncl += dI; e->aExcl += dEx; e->aCount += dC; }
                e->lIncl = I; e->lExcl = E; e->lCount = C; }
            for (int i = 0; i < NFX; ++i) statWindow(&g_fx[i], battle);
            for (int s = 0; s < DV_SLOTS; ++s) if (g_devOrig[s]) statWindow(&g_dv[s], battle);
            for (int k = 0; k < OBJ_KINDS; ++k) { statWindow(&g_objA[k], battle); statWindow(&g_objB[k], battle); }
            // sampler tables: merge the window into the battle tables, then clear
            for (int c = 0; c < NCLASS; ++c) {
                ClassTab& ct = g_ct[c];
                if (battle) {
                    for (int i = 0; i < LTW; ++i) if (ct.leafW[i].key) tabAdd(ct.leafA, LTA, ct.leafW[i].key, ct.leafW[i].count, &ct.drop);
                    if (kDeep[c]) for (int i = 0; i < LTW; ++i) if (ct.inclW[i].key) tabAdd(ct.inclA, LTA, ct.inclW[i].key, ct.inclW[i].count, &ct.drop);
                    ct.nA += ct.nW; g_bSamples += ct.nW;
                }
                memset(ct.leafW, 0, LTW * sizeof(LeafEnt)); if (kDeep[c]) memset(ct.inclW, 0, LTW * sizeof(LeafEnt));
                ct.nW = 0;
            }
            char m1[16], m2[16], m3[16], m4[16], m5[16], d1[16], d2[16];
            LONG fr = dF > 0 ? dF : 1;
            logf("win: cpu=%d%% samples=%d frames=%d ms/f=%s | shadow %s views %s ui %s unmarked %s ms/f | draws/f shadow %s views %s | begins=%d ends=%d under=%d badstack=%d%s",
                 procPct, g_winSamples, dF, msStr(dPer, fr, m1), msStr(wSh, fr, m2), msStr(wVw, fr, m3), msStr(wUi, fr, m4),
                 msStr(dPer - wSh - wVw - wUi, fr, m5), perStr(drw[P_SHADOW], fr, d1), perStr(drw[P_VIEWS], fr, d2),
                 dB, dE, (int)g_mkUnderflow, g_winBadStack, battle ? "  [BATTLE]" : "");
            g_winSamples = 0; g_winBadStack = 0;
            if (battle) {
                g_bWindows++; g_bFrames += dF; g_bPeriod += dPer;
                if ((g_bWindows % 12) == 0) battleReport();
            }
        }
    }
    return 0;
}
static void startProfiler(BYTE* textBase, SIZE_T textSize, BYTE* imageBase, SIZE_T imageSize) {
    g_textLo = (DWORD)(ULONG_PTR)textBase; g_textHi = g_textLo + (DWORD)textSize;
    g_gameBase = (DWORD)(ULONG_PTR)imageBase; g_gameEnd = g_gameBase + (DWORD)imageSize;
    p_NtQIT = (tNtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    for (int c = 0; c < NCLASS; ++c) {
        g_ct[c].leafW = (LeafEnt*)VirtualAlloc(NULL, LTW * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_ct[c].leafA = (LeafEnt*)VirtualAlloc(NULL, LTA * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (kDeep[c]) {
            g_ct[c].inclW = (LeafEnt*)VirtualAlloc(NULL, LTW * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            g_ct[c].inclA = (LeafEnt*)VirtualAlloc(NULL, LTA * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        if (!g_ct[c].leafW || !g_ct[c].leafA || (kDeep[c] && (!g_ct[c].inclW || !g_ct[c].inclA))) { logf("prof: table alloc failed - profiler OFF."); return; }
    }
    if (!p_NtQIT) { logf("prof: NtQueryInformationThread missing - profiler OFF."); return; }
    profRefreshModules();
    logf("prof: sampler v3 started (%d modules, stage-tagged classes %d, stack reads bounded by TEB).", g_npm, NCLASS);
    CreateThread(NULL, 0, profThread, NULL, 0, NULL);
}
static void rotateLog() {
#ifndef AOTR_DIAG
    { char prev[MAX_PATH]; wsprintfA(prev, "%s\\bfme2_accel.prev.log", g_dir);   // v70: one log per game start, the one before kept
      MoveFileExA(kLogPath, prev, MOVEFILE_REPLACE_EXISTING); return; }
#endif
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(kLogPath, GetFileExInfoStandard, &fa)) return;
    ULONGLONG sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (sz < 64ull * 1024 * 1024) return;
    SYSTEMTIME st; GetLocalTime(&st); char dst[MAX_PATH];
    wsprintfA(dst, "%s\\bfme2_accel.%04d%02d%02d-%02d%02d%02d.old.log", g_dir,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    MoveFileA(kLogPath, dst);
}

#include "aotr_modtime.inc"
#include "aotr_w3dlock.inc"                     // v54: the W3D render lock in user mode (no kernel call per lock)
#include "aotr_partfast.inc"
#include "aotr_pswarm.inc"
#include "aotr_psremove.inc"
#include "aotr_amfind.inc"                      // v55.2: TheAttributeModifierStore index by name key without a walk (self-proving)                    // v55: a particle system leaves the FX manager without a whole-list search                      // v54: the particle system list read ahead before the engine walks it (prefetch only)                    // v54: the whole-map object loop with its memory read ahead (exact, self-proving)
#include "aotr_flushtime.inc"
#include "aotr_clienttime.inc"
#include "aotr_pick.inc"
#include "aotr_subsys.inc"
#include "aotr_logicspread.inc"
#include "aotr_logicrate.inc"
#include "aotr_posrate.inc"
#include "aotr_mutexcs.inc"
#include "aotr_audiolimit.inc"
#include "aotr_drawgen.inc"
#include "aotr_rlsort.inc"
#include "aotr_texmem.inc"
#include "aotr_rt.inc"
#include "aotr_merge.inc"
#include "aotr_screen.inc"
#include "aotr_pace.inc"
#include "aotr_tween.inc"
#include "aotr_texcache.inc"
#include "aotr_texjobs.inc"
#include "aotr_exitfix.inc"
#include "aotr_streamset.inc"
#include "aotr_terrain.inc"
#include "aotr_terrainkeep.inc"
#include "aotr_shadownbr.inc"
#include "aotr_shadowweld.inc"
#include "aotr_filewarm.inc"
#include "aotr_guard.inc"                       // v55.1: a load that never ends turns this build's new parts off at the next start
#include "aotr_preload.inc"
#define SP_RUNTIME 1
#include "aotr_shadowpcf.inc"
#include "aotr_gpupref.inc"
#include "aotr_badge.inc"                        // v71: the status light, aotr_status.txt, the refresh after every fifth match
#include "aotr_cbfind.inc"                     // v77: ControlBar::findNonConstCommandButton remembers its answers by name (self-proving)
#include "aotr_fmfind.inc"                     // v81: Object::findModule remembers its answers per object (self-proving)
#include "aotr_maskfast.inc"                   // v81: the 19-word flag mask test in SSE steps (self-proving)
#include "aotr_hashfind.inc"                   // v82: the unsigned-key hash map find without a divide (self-proving)
#include "aotr_overlay.inc"

// ---------------------------------------------------------------- game-thread sampler v2 (RT build)
// 100 samples/s of the game's render thread while frames are heavy (>= 30 ms) and the render thread is live.
// Every sample is tagged with the engine phase (logic, client, shadow pass, main view, UI, other) and attributed
// to whole functions (start-address table gd_funcs.inc, generated from game.dat). Callers come from the frame-
// pointer chain of a stack copy taken while the thread is suspended; time outside game.dat is charged to the first
// game.dat return address. Each report covers only the samples since the previous one.
#include "gd_funcs.inc"
#define GS_TAB 65536
#define PH_NX (PH_N + 7)                                    // + logic split by sub-frame index (0 = other, 1..6)
static const char* const kTabName[PH_NX] = { "other", "logic", "client", "shadow", "views", "ui", "logic-n0", "logic-n1", "logic-n2", "logic-n3", "logic-n4", "logic-n5", "logic-n6" };
static LeafEnt* g_gsSelfP[PH_NX], *g_gsInclP[PH_NX];
static LeafEnt* g_gsBound = NULL;
// Focus: samples whose call chain passes through one of these functions get their own leaf / inner-function tables, so
// the inside of the heaviest logic subtrees can be read (the phase-wide tables are flat: nothing above 1%).
#define GS_FOCUS 7
static const DWORD kGsFocus[GS_FOCUS] = { 0x0066E58F, 0x00872EFC, 0x008953BB, 0x006A96A0, 0x00B51970, 0x0060D77F, 0x005F5123 };
static const char* const kGsFocusName[GS_FOCUS] = { "unit-AI-update(C6A5A8)", "horde-update(C5D3E0)", "module-C64284", "SkirmishAI", "sub4358", "sub3BE8", "particle-manager" };
static LeafEnt* g_gsFocSelf[GS_FOCUS], *g_gsFocIncl[GS_FOCUS];
static LONG g_gsFocN[GS_FOCUS];
static LONG g_gsN = 0, g_gsDrop = 0, g_gsNP[PH_NX];
static LONG g_gsFrames = 0;
static LONG64 g_gsPeriod = 0;
static DWORD gsFunc(DWORD a) {
    if (a < 0x00401000 || a >= 0x00BD0000) return 0;
    int lo = 0, hi = kGameFuncCount - 1, best = -1;
    while (lo <= hi) { int mid = (lo + hi) >> 1; if (kGameFuncs[mid] <= a) { best = mid; lo = mid + 1; } else hi = mid - 1; }
    return best >= 0 ? kGameFuncs[best] : 0;
}
static __forceinline bool gsModrmCall(BYTE m) { return (m & 0x38) == 0x10; }
static bool gsIsRet(DWORD a) {
    if (a < g_textLo + 8 || a >= g_textHi) return false;
    const BYTE* p = (const BYTE*)(ULONG_PTR)a;
    if (p[-5] == 0xE8) return true;                                                                    // call rel32
    BYTE m;
    if (p[-2] == 0xFF && gsModrmCall(m = p[-1]) && ((m & 0xC0) == 0xC0 || ((m & 0xC0) == 0 && (m & 7) != 4 && (m & 7) != 5))) return true;
    if (p[-3] == 0xFF && gsModrmCall(m = p[-2]) && (((m & 0xC0) == 0x40 && (m & 7) != 4) || ((m & 0xC0) == 0 && (m & 7) == 4))) return true;
    if (p[-4] == 0xFF && gsModrmCall(m = p[-3]) && (m & 0xC0) == 0x40 && (m & 7) == 4) return true;
    if (p[-6] == 0xFF && gsModrmCall(m = p[-5]) && (((m & 0xC0) == 0x80 && (m & 7) != 4) || ((m & 0xC0) == 0 && (m & 7) == 5))) return true;
    if (p[-7] == 0xFF && gsModrmCall(m = p[-6]) && (m & 0xC0) == 0x80 && (m & 7) == 4) return true;
    return false;
}
static DWORD g_selfLo = 0, g_selfHi = 0;
#include "aotr_crashlog.inc"
struct PathEnt { DWORD f, c1, c2, n; };
#define GS_PATHS 32768
static PathEnt* g_gsPath[PH_NX];
static PathEnt g_gsPathTmp[GS_PATHS];
static void gsPathAdd(PathEnt* t, DWORD f, DWORD c1, DWORD c2) {
    DWORD h = ((f * 2654435761u) ^ (c1 * 2246822519u) ^ (c2 * 3266489917u)) >> 9;
    for (int i = 0; i < 64; ++i) {
        PathEnt* e = &t[(h + i) & (GS_PATHS - 1)];
        if (e->n && e->f == f && e->c1 == c1 && e->c2 == c2) { e->n++; return; }
        if (!e->n) { e->f = f; e->c1 = c1; e->c2 = c2; e->n = 1; return; }
    }
    g_gsDrop++;
}
static DWORD gsSymKey(DWORD a) {                       // whole function in game.dat, export (or module) elsewhere, exact address in this DLL
    if (a >= g_gameBase && a < g_gameEnd) { DWORD f = gsFunc(a); return f ? f : a; }
    if (a >= g_selfLo && a < g_selfHi) return a;
    int mi = pmOf(a);
    if (mi < 0) return a & ~0xFFFu;
    DWORD rva = a - g_pm[mi].base;
    ExpMod* m = expFor(g_pm[mi].base);
    if (m && m->n) {
        int lo = 0, hi = m->n - 1, best = -1;
        while (lo <= hi) { int mid = (lo + hi) >> 1; if (m->syms[mid].rva <= rva) { best = mid; lo = mid + 1; } else hi = mid - 1; }
        if (best >= 0 && rva - m->syms[best].rva < 0x100) return g_pm[mi].base + m->syms[best].rva;   // near an export: that export
        if (best >= 0 && rva - m->syms[best].rva < 0x4000) return a & ~0x3Fu;                          // an internal routine: keep it apart
    }
    return g_pm[mi].base;
}
static int __cdecl gsCmp(const void* a, const void* b) { DWORD x = ((const LeafEnt*)a)->count, y = ((const LeafEnt*)b)->count; return x < y ? 1 : (x > y ? -1 : 0); }
static LeafEnt g_gsTmp[GS_TAB], g_gsAgg[GS_TAB];
static void gsDumpAgg(LeafEnt* tab, bool aggregate, const char* what, int count, LONG denom) {
    memset(g_gsAgg, 0, sizeof(g_gsAgg)); LONG drop = 0;
    for (int i = 0; i < GS_TAB; ++i) if (tab[i].key) tabAdd(g_gsAgg, GS_TAB, aggregate ? gsSymKey(tab[i].key) : tab[i].key, tab[i].count, &drop);
    int m = 0;
    for (int i = 0; i < GS_TAB; ++i) if (g_gsAgg[i].key) g_gsTmp[m++] = g_gsAgg[i];
    qsort(g_gsTmp, m, sizeof(LeafEnt), gsCmp);
    char line[1000]; int lp = 0;
    for (int i = 0; i < m && i < count; ++i) {
        char sym[160]; symName(g_gsTmp[i].key, sym);
        char* plus = strrchr(sym, '+'); if (plus && strchr(sym, '!')) *plus = 0;   // exports: name only
        char pc[16]; pctStr(g_gsTmp[i].count, denom, pc);
        lp += wsprintfA(line + lp, " %s=%s", sym, pc);
        if (lp > 820 || i + 1 == count || i + 1 == m) { logf("gs: %s%s", what, line); lp = 0; }
    }
}
static int __cdecl gsPathCmp(const void* a, const void* b) { DWORD x = ((const PathEnt*)a)->n, y = ((const PathEnt*)b)->n; return x < y ? 1 : (x > y ? -1 : 0); }
static void gsDumpPaths(PathEnt* t, const char* phase, int count, LONG denom) {
    int m = 0;
    for (int i = 0; i < GS_PATHS; ++i) if (t[i].n) g_gsPathTmp[m++] = t[i];
    qsort(g_gsPathTmp, m, sizeof(PathEnt), gsPathCmp);
    for (int i = 0; i < m && i < count; ++i) {
        char s0[160], s1[160], s2[160], pc[16];
        symName(g_gsPathTmp[i].f, s0);
        if (g_gsPathTmp[i].c1) symName(g_gsPathTmp[i].c1, s1); else lstrcpyA(s1, "?");
        if (g_gsPathTmp[i].c2) symName(g_gsPathTmp[i].c2, s2); else lstrcpyA(s2, "?");
        pctStr(g_gsPathTmp[i].n, denom, pc);
        logf("gs: %s path %s %s < %s < %s", phase, pc, s0, s1, s2);
    }
}
static BOOL gsCopyStack(BYTE* dst, DWORD esp, DWORD n) { __try { memcpy(dst, (void*)(ULONG_PTR)esp, n); return TRUE; } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; } }
static DWORD WINAPI gsThread(LPVOID) {
    DWORD tid = g_rtMainTid;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h || !p_NtQIT) { logf("gs: cannot open the game thread - sampler OFF."); return 0; }
    PROF_TBI tbi; if (p_NtQIT(h, 0, &tbi, sizeof(tbi), NULL) != 0 || !tbi.TebBaseAddress) { logf("gs: no TEB - sampler OFF."); CloseHandle(h); return 0; }
    NT_TIB* tib = (NT_TIB*)tbi.TebBaseAddress;
    static BYTE stk[128 * 1024];
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    { HMODULE hs = NULL; if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&gsSymKey, &hs) && hs) {
        IMAGE_NT_HEADERS* nh = (IMAGE_NT_HEADERS*)((BYTE*)hs + ((IMAGE_DOS_HEADER*)hs)->e_lfanew); g_selfLo = (DWORD)(ULONG_PTR)hs; g_selfHi = g_selfLo + nh->OptionalHeader.SizeOfImage; } }
    logf("gs: game-thread sampler v4 armed (thread %u, 100 Hz in battle-like seconds: 30..250 ms per frame; %d function starts; callers by validated stack scan; logic split by sub-frame; a report per ~30 s).", tid, kGameFuncCount);
    LONG lastFrames = g_mkFrames; LONG64 lastPeriod = rd64(&g_mkPeriod);
    LONG64 lPh[PH_N]; for (int p = 0; p < PH_N; ++p) lPh[p] = rd64(&g_phTicks[p]);
    LONG64 lSt[3]; for (int s = 0; s < 3; ++s) lSt[s] = rd64(&g_stTicks[s]);
    DWORD lastCheck = GetTickCount(); bool heavy = false; int window = 0;
    for (;;) {
        Sleep(heavy ? 10 : 250);
        DWORD now = GetTickCount();
        if (now - lastCheck >= 1000) {
            LONG fr = g_mkFrames; LONG64 per = rd64(&g_mkPeriod);
            LONG dF = fr - lastFrames; LONG64 dP = per - lastPeriod;
            heavy = dF >= 4 && dP >= (LONG64)dF * g_pqpf.QuadPart * 30 / 1000 && dP <= (LONG64)dF * g_pqpf.QuadPart * 250 / 1000;
            if (heavy) { g_gsFrames += dF; g_gsPeriod += dP; }
            lastFrames = fr; lastPeriod = per; lastCheck = now;
            if (g_gsN >= 3000) {
                char m1[16], m2[16], m3[16], m4[16], m5[16], m6[16], m7[16];
                LONG64 dPh[PH_N]; for (int p = 0; p < PH_N; ++p) { LONG64 v = rd64(&g_phTicks[p]); dPh[p] = v - lPh[p]; lPh[p] = v; }
                LONG64 dSt[3]; for (int s = 0; s < 3; ++s) { LONG64 v = rd64(&g_stTicks[s]); dSt[s] = v - lSt[s]; lSt[s] = v; }
                profRefreshModules();
                LONG F = g_gsFrames > 0 ? g_gsFrames : 1;
                logf("gs: ==== profile window #%d: %d samples over %d heavy frames at %s ms/f | logic %s, client %s (shadow %s views %s ui %s) ms/f over all frames since the last window | samples logic %d client %d shadow %d views %d ui %d other %d | drops %d ====",
                     ++window, g_gsN, g_gsFrames, msStr(g_gsPeriod, F, m1), msStr(dPh[PH_LOGIC], F, m2), msStr(dPh[PH_CLIENT], F, m3), msStr(dSt[0], F, m4), msStr(dSt[1], F, m5), msStr(dSt[2], F, m6),
                     g_gsNP[PH_LOGIC], g_gsNP[PH_CLIENT], g_gsNP[PH_SHADOW], g_gsNP[PH_VIEWS], g_gsNP[PH_UI], g_gsNP[PH_OTHER], g_gsDrop);
                (void)m7;
                logf("gs: logic samples by sub-frame: n0 %d n1 %d n2 %d n3 %d n4 %d n5 %d n6 %d", g_gsNP[PH_N], g_gsNP[PH_N + 1], g_gsNP[PH_N + 2], g_gsNP[PH_N + 3], g_gsNP[PH_N + 4], g_gsNP[PH_N + 5], g_gsNP[PH_N + 6]);
                for (int p = 0; p < PH_NX; ++p) {
                    if (p == PH_LOGIC || g_gsNP[p] < 50) continue;             // logic is reported per sub-frame
                    char w1[48], w2[48]; wsprintfA(w1, "%s self:", kTabName[p]); wsprintfA(w2, "%s incl:", kTabName[p]);
                    gsDumpAgg(g_gsSelfP[p], true, w1, 60, g_gsN);
                    gsDumpAgg(g_gsInclP[p], false, w2, 60, g_gsN);
                    gsDumpPaths(g_gsPath[p], kTabName[p], 30, g_gsN);
                }
                gsDumpAgg(g_gsBound, false, "outside game.dat, by first game.dat return address:", 40, g_gsN);
                for (int k = 0; k < GS_FOCUS; ++k) {
                    if (g_gsFocN[k] >= 15 && g_gsFocSelf[k]) {
                        char w1[96], w2[96]; wsprintfA(w1, "focus %s: %d of %d samples; self (%% of the subtree):", kGsFocusName[k], (int)g_gsFocN[k], (int)g_gsN); wsprintfA(w2, "focus %s inner functions (%% of the subtree):", kGsFocusName[k]);
                        gsDumpAgg(g_gsFocSelf[k], true, w1, 28, g_gsFocN[k]);
                        gsDumpAgg(g_gsFocIncl[k], false, w2, 28, g_gsFocN[k]);
                    }
                    if (g_gsFocSelf[k]) { memset(g_gsFocSelf[k], 0, GS_TAB * sizeof(LeafEnt)); memset(g_gsFocIncl[k], 0, GS_TAB * sizeof(LeafEnt)); }
                    g_gsFocN[k] = 0;
                }
                for (int p = 0; p < PH_NX; ++p) { memset(g_gsSelfP[p], 0, GS_TAB * sizeof(LeafEnt)); memset(g_gsInclP[p], 0, GS_TAB * sizeof(LeafEnt)); memset(g_gsPath[p], 0, GS_PATHS * sizeof(PathEnt)); g_gsNP[p] = 0; }
                memset(g_gsBound, 0, GS_TAB * sizeof(LeafEnt));
                g_gsN = 0; g_gsFrames = 0; g_gsPeriod = 0; g_gsDrop = 0;
            }
        }
        if (!heavy || !g_rtActive) continue;
        if (SuspendThread(h) == (DWORD)-1) continue;
        CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
        DWORD eip = 0, ebp = 0, esp = 0, n = 0; LONG ph = PH_OTHER;
        if (GetThreadContext(h, &ctx)) {
            eip = ctx.Eip; ebp = ctx.Ebp; esp = ctx.Esp; ph = g_phase;
            DWORD sb = (DWORD)(ULONG_PTR)tib->StackBase, sl = (DWORD)(ULONG_PTR)tib->StackLimit;
            if (esp >= sl && esp < sb) { n = (sb - esp) & ~3u; if (n > sizeof(stk)) n = sizeof(stk); if (!gsCopyStack(stk, esp, n)) n = 0; }
        }
        ResumeThread(h);
        if (!eip) continue;
        if (ph < 0 || ph >= PH_N) ph = PH_OTHER;
        g_gsN++; g_gsNP[ph]++;
        if (ph == PH_LOGIC) { LONG sn = g_lgCurN; ph = PH_N + ((sn >= 0 && sn < 7) ? sn : 0); g_gsNP[ph]++; }
        tabAdd(g_gsSelfP[ph], GS_TAB, eip, 1, &g_gsDrop);
        // callers by stack scan: every game.dat return address (the byte before it ends a call) on the copied stack,
        // each function once per sample; the first two distinct callers also key the call-path table
        DWORD seen[128]; int ns = 0; DWORD firstRet = 0, cl[2] = { 0, 0 }; int nc = 0;
        DWORD f0 = (eip >= g_gameBase && eip < g_gameEnd) ? gsFunc(eip) : 0;
        if (f0) seen[ns++] = f0;
        (void)ebp;
        DWORD cur = f0; int miss = 0;                         // the function the next return address must have called
        for (DWORD o = 0; o + 4 <= n && ns < 128; o += 4) {
            DWORD v = *(DWORD*)(stk + o);
            if (v < g_textLo + 8 || v >= g_textHi || !gsIsRet(v)) continue;
            const BYTE* rp = (const BYTE*)(ULONG_PTR)v;
            if (cur && rp[-5] == 0xE8) {                          // a direct call must target the current function (or a jmp to it)
                DWORD t = v + *(const LONG*)(rp - 4);
                bool ok = t == cur;
                if (!ok && t >= g_textLo && t + 5 <= g_textHi && ((const BYTE*)(ULONG_PTR)t)[0] == 0xE9) ok = t + 5 + *(const LONG*)((const BYTE*)(ULONG_PTR)t + 1) == cur;
                if (!ok && ++miss <= 3) continue;                 // a stale return address; after three, resynchronize
            }
            miss = 0;
            if (!firstRet) firstRet = v;
            DWORD f = gsFunc(v);
            cur = f;
            if (!f) continue;
            bool dup = false; for (int q = 0; q < ns; ++q) if (seen[q] == f) { dup = true; break; }
            if (dup) continue;
            seen[ns++] = f;
            if (nc < 2) cl[nc++] = f;
        }
        for (int q = 0; q < ns; ++q) tabAdd(g_gsInclP[ph], GS_TAB, seen[q], 1, &g_gsDrop);
        for (int k = 0; k < GS_FOCUS; ++k) {                          // inside a focus subtree: the leaf, and every function between it and the root
            int at = -1; for (int q = 0; q < ns; ++q) if (seen[q] == kGsFocus[k]) { at = q; break; }
            if (at < 0 || !g_gsFocSelf[k]) continue;
            g_gsFocN[k]++;
            tabAdd(g_gsFocSelf[k], GS_TAB, eip, 1, &g_gsDrop);
            for (int q = 0; q < at; ++q) tabAdd(g_gsFocIncl[k], GS_TAB, seen[q], 1, &g_gsDrop);
        }
        gsPathAdd(g_gsPath[ph], gsSymKey(eip), cl[0], cl[1]);
        if (!f0 && firstRet) tabAdd(g_gsBound, GS_TAB, firstRet, 1, &g_gsDrop);   // outside game.dat: charge the first game.dat caller
    }
    return 0;
}
static void startGameSampler() {
    g_gsBound = (LeafEnt*)VirtualAlloc(NULL, GS_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int p = 0; p < PH_NX; ++p) {
        g_gsSelfP[p] = (LeafEnt*)VirtualAlloc(NULL, GS_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_gsInclP[p] = (LeafEnt*)VirtualAlloc(NULL, GS_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_gsPath[p] = (PathEnt*)VirtualAlloc(NULL, GS_PATHS * sizeof(PathEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_gsSelfP[p] || !g_gsInclP[p] || !g_gsPath[p]) { logf("gs: table alloc failed - sampler OFF."); return; }
    }
    if (!g_gsBound) { logf("gs: table alloc failed - sampler OFF."); return; }
    for (int k = 0; k < GS_FOCUS; ++k) {
        g_gsFocSelf[k] = (LeafEnt*)VirtualAlloc(NULL, GS_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_gsFocIncl[k] = (LeafEnt*)VirtualAlloc(NULL, GS_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_gsFocSelf[k] || !g_gsFocIncl[k]) { g_gsFocSelf[k] = NULL; g_gsFocIncl[k] = NULL; }
    }
    g_gsStart = gsThread;                               // the RT report thread starts it at the first heavy 5 s window
}

static volatile LONG g_frW = 0;                 // frames written by aotr_frameprof.inc (game thread)
#include "aotr_loadprof.inc"
#include "aotr_sched.inc"
#include "aotr_abtest.inc"
#include "aotr_frameprof.inc"
#include "aotr_taskwarm.inc"                   // v77: the FX renderer's task list read ahead before its neighbours are compared (prefetch only)
#include "aotr_pxpar.inc"                      // v74: the particles' own update on the spare cores (exact, self-checking)
#ifdef AOTR_DIAG
#include "aotr_vmprobe.inc"                     // v77 (diagnostics build): who commits and frees memory, working-set trims
#endif
#include "aotr_fxprobe.inc"                    // v73 (diagnostics build): the particle update measured
#include "aotr_preshader.inc"

// ---------------------------------------------------------------- init
// A second copy of this DLL in the same process (a second loader left running, or another accelerator started
// alongside us) must not install on top of the first. The first copy has already patched the engine's .text and has
// taken the import slots by name; the second would hash an engine whose code it has already changed (and so call it an
// unknown build) and would fight the first for every slot it finds free. Symptoms are exactly what a double install
// looks like: half the modules report "already hooked", the read-out never arms, and the picture is built by two
// halves of two builds at once. Count the copies loaded here and step aside unless we are the first.
static bool aotrSecondCopy() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    int n = 0;
    if (Module32First(snap, &me)) do {
        if (!lstrcmpiA(me.szModule, "bfme2_accel.dll")) ++n;
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    return n > 1;
}

// (the name of the release, and a build number that goes up with every build handed out: the first thing a log says)
// v29: this is the AOTR fork's own name, not upstream's ("BFME2 Accelerator 2.0"), because the two are installed side
// by side and a log has to say which one it is without opening the DLL. The banner carries the folder the DLL was
// loaded from, so a log can never be mistaken for another test folder's. v30: it is the first line of every session.
#ifdef AOTR_DIAG
#define AOTR_RELEASE "AOTR accel v85 (diagnostics)"
#else
#define AOTR_RELEASE "AOTR accel v85"
#endif
#define AOTR_BUILDNO 135                                  // (also written out in aotr_overlay.inc: the in-frame banner and ovSwitches)
static_assert(AOTR_BUILDNO == AOTR_BUILDNO_GUARD, "aotr_guard.inc: AOTR_BUILDNO_GUARD must equal AOTR_BUILDNO");
static void aotrBanner() {
    HMODULE selfMod = NULL;
    // v45 (test-27 crashed in the menu: a report line grew past its sprintf_s buffer, and the CRT's answer to that is to
    // end the process). This DLL's own C runtime (/MT, not the game's) now cuts such a line instead and says so once.
    _set_invalid_parameter_handler(aotrInvalidParam);
    char dllPath[MAX_PATH] = "", iniPath[MAX_PATH] = "", cachePath[MAX_PATH] = "";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&aotrSetDir, &selfMod);
    if (selfMod) GetModuleFileNameA(selfMod, dllPath, sizeof(dllPath));
    aotrPath(iniPath, "bfme2_accel.ini");
    aotrPath(cachePath, "texcache");
    // the one line that settles 'which build, from where, writing what': resolved DLL, ini and cache paths, the log
    // path, and the build id. (v30: the texture cache folder stays where it was so no older path can creep back in,
    // but this build does not fill it - see the init lines below.)
    logf("init: %s (build %d) - SELF-CONTAINED. dll=%s | ini=%s | cache=%s (texture cache not used in v30) | log=%s",
         AOTR_RELEASE, AOTR_BUILDNO,
         dllPath[0] ? dllPath : "(unknown)",
         iniPath[0] ? iniPath : "(none)",
         cachePath[0] ? cachePath : "(none)",
         kLogPath[0] ? kLogPath : "(none)");
}

// v37: the product build's 30-second line for the exact shortcuts (rtReport, which printed them, is diagnostic only)
// v44: RIGHT Ctrl (or Scroll Lock) puts a marker into the log ("the player saw something here"). The game's hotkeys use
// the left Ctrl with another key; the right Ctrl pressed alone does nothing in the game. A marker is written only when
// the right Ctrl is let go without any other key having been pressed meanwhile (so Ctrl+1 etc. never mark).
// And without any key: every second of play under 28 frames is written as it happens ("drop"), so a fall from 29 to 26
// is in the log too.
#ifdef AOTR_DIAG
// v76 / v82.1 (diagnostics build): GLITCH HUNT. The long upward shapes at full graphics (NOTES, open issues). The v82
// test (22 markers) cleared the two first suspects: they came with the logic slicer off and the particle spread off
// just as with them on. What they look like - dark columns stretched from units and flat dark walls - is stencil
// shadow volumes drawn into the picture. v82.1 therefore switches the RENDER THREAD itself: FULL (the game's Direct3D
// calls carried out on the worker) for 3 minutes, then CORE (every call made directly on the game thread, as the
// engine ships) for 3 minutes, and so on. The player presses the right Ctrl when a glitch is on the screen; each
// marker line says which mode was on, and the 30 s line counts the markers per mode. Markers in both modes mean the
// render thread is not the cause either (next: SHADOWNBR_OFF / SHADOWWELD_OFF, then VANILLA). Release never does this.
static volatile LONG g_ghCore = -1; static volatile LONG g_ghMarks[2], g_ghSecs[2];
static void ghTick(bool play) {
    static DWORD playSecs = 0;                         // called once a second (aotrMarkKeys)
    if (!play || !g_rtInstalled) return;
    if (g_rtBypassN > RT_BYP_RETRIES) return;          // v83.1: the freeze guard has stepped the render thread aside for good - never turned back on here
    LONG c = (LONG)((playSecs / 180) & 1);
    ++playSecs;
    if (g_ghCore >= 0) InterlockedIncrement(&g_ghSecs[g_rtActive ? 0 : 1]);   // v83.1: the mode really in effect, not the wish
    if (c != g_ghCore) {
        g_ghCore = c;
        // v83.1: only the wish. rtApplyToggle (at the next Present, on the game thread) drains the queue and then flips
        // g_rtActive and g_rtCoreMode together - "kept in step there, and only there". v83 also set g_rtCoreMode here, at
        // once, from this thread: for a moment the game thread called DXVK directly while the worker still ran queued
        // calls, and the v83 test's fifth switch crashed DXVK's own thread (d3d9.dll+EDB5) - the game hung.
        InterlockedExchange(&g_rtWantOn, c ? 0 : 1);
        logf("glitch-hunt: the render thread is now %s (play minute %u) - press the right Ctrl when a glitch is on the screen",
             c ? "OFF (CORE: every Direct3D call on the game thread)" : "ON (FULL)", playSecs / 60);
    }
}
static const char* ghState() {
    static char b[96];
    if (g_ghCore < 0) return "";
    LONG k = g_rtActive ? 0 : 1;                       // v83.1: the mode really in effect (the switch lands at the next frame)
    InterlockedIncrement(&g_ghMarks[k]);
    wsprintfA(b, " [glitch-hunt: render thread %s]", k ? "OFF (CORE)" : "ON (FULL)");
    return b;
}
static void ghReport() {
    if (g_ghCore < 0) return;
    logf("glitch-hunt: markers so far by state (minutes in that state) - render thread ON (FULL) %d (%d), render thread OFF (CORE) %d (%d); %d in all",
         (int)g_ghMarks[0], (int)g_ghSecs[0] / 60, (int)g_ghMarks[1], (int)g_ghSecs[1] / 60, (int)(g_ghMarks[0] + g_ghMarks[1]));
}
#endif
static DWORD WINAPI aotrMarkKeys(LPVOID) {
    bool rc = false, other = false, sl = false; LONG n = 0, lastW = g_frW; DWORD lastSec = GetTickCount();
    for (;;) {
        Sleep(30);
#ifdef AOTR_DIAG
        vmSampleWs();
#endif
        bool r = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
        if (r) { if (!rc) other = false;
                 for (int k = 0x01; k <= 0xFE && !other; ++k) {          // another key or a mouse button held with it: not a marker
                     if (k == VK_CONTROL || k == VK_RCONTROL || k == VK_LCONTROL) continue;
                     if (GetAsyncKeyState(k) & 0x8000) other = true; } }
#ifdef AOTR_DIAG
        else if (rc && !other) logf("MARKER %d: the player pressed the right Ctrl here (frame %d)%s%s", (int)++n, (int)g_frW, gdMarkNote(), ghState());
#else
        else if (rc && !other) logf("MARKER %d: the player pressed the right Ctrl here (frame %d)%s", (int)++n, (int)g_frW, gdMarkNote());
#endif
        rc = r;
        bool s2 = (GetAsyncKeyState(VK_SCROLL) & 0x8000) != 0;
        if (s2 && !sl) logf("MARKER %d: the player pressed Scroll Lock here (frame %d)%s", (int)++n, (int)g_frW, gdMarkNote());
        sl = s2;
        DWORD now = GetTickCount();
        if (now - lastSec >= 1000) {
            LONG w = g_frW, fr = w - lastW; lastW = w;
            bool play = g_frLastLogicTick && now - g_frLastLogicTick < 2000;
#ifdef AOTR_DIAG
            ghTick(play);
#endif
            if (play && fr > 0 && fr < 28) logf("drop: %d frames in the last second (frame %d)", (int)fr, (int)w);
            lastSec = now;
        }
    }
}
static DWORD WINAPI aotrProdReport30(LPVOID) {
    if (g_tscPerQpc <= 0) { LONG64 q0 = qpcNow(); ULONG64 t0 = __rdtsc(); Sleep(60); LONG64 q1 = qpcNow(); ULONG64 t1 = __rdtsc();
                            g_tscPerQpc = q1 > q0 ? (double)(LONG64)(t1 - t0) / (double)(q1 - q0) : 0; }
    LONG lF = g_frW, lH = 0, lM = 0, lFa = 0, lF5 = g_frW;
    for (int tick = 1; ; ++tick) {
        Sleep(5000);
        bdgWriteStatus(); bdgSaveShow(g_ovShow);         // v71: aotr_status.txt for the starter's tray icon; the F7 choice kept
        // v43: the client parts every 5 s (a camera jump or a big effect lasts seconds - 30 s averages hid them)
        { LONG f5 = g_frW - lF5; lF5 = g_frW; if (f5 > 0) { ctReport(f5); ftReport(f5); ssReport(f5); } }
        if (tick % 6) continue;
        LONG f = g_frW - lF; lF = g_frW;
        if (f <= 0) continue;
        alReport(f);
        clReport();
        fxpReport(f); pxReport(); tkwReport();
#ifdef AOTR_DIAG
        ghReport(); vmReport();
#endif
        psfReport(); peReport(); coreReport(); tkReport(); plReport(); plTrickleReport(); plLearnSave(); gdTurboReport(); abReport(); wmqReport(); pfReport(); wlReport(); pswReport(); psrReport(); amfReport(); cbfReport(); fmfReport();
        { static LONG lB = 0, lE = 0, lS = 0, lL = 0, lA = 0, lD = 0, lF = 0;
          LONG b = g_rtNPxBatch, e = g_rtNPxEntry, sh = g_rtNShroud, tl = g_rtNTMLock, ta = g_rtNTMAdopt, td = g_rtNTMDrop, tf = g_rtNTMFail;
          if (b != lB || sh != lS || tl != lL)
              logf("rt: radar - last 30 s: %d pixel writes queued as %d runs | %d shroud updates staged | %d radar texture locks answered from a CPU mirror (%d mirrors made, %d dropped, %d could not be made)",
                   (int)(e - lE), (int)(b - lB), (int)(sh - lS), (int)(tl - lL), (int)(ta - lA), (int)(td - lD), (int)(tf - lF));
          lB = b; lE = e; lS = sh; lL = tl; lA = ta; lD = td; lF = tf; }
        if (g_flt) {
            static LONG lc = 0, lh = 0, lm = 0;
            LONG c = g_fltCalls, h = g_fltHit, m = g_fltMiss;
            logf("filter-memo: %s | last 30 s: %d filter calls, %d template parts answered from the memo, %d run by the engine | proof left %d, compared %d, different %d, cleared %d",
                 g_fltOff ? "OFF (a difference was found)" : "on", (int)(c - lc), (int)(h - lh), (int)(m - lm), (int)g_fltProof, (int)g_fltChecked, (int)g_fltBad, (int)g_fltClears);
            lc = c; lh = h; lm = m;
        }
        if (g_eqMemo) {
            LONG h = g_eqHit, m = g_eqMiss, fa = g_eqFast;
            logf("equiv-memo: %s | last 30 s: %d answered from the memo, %d by the equal-overrides rule, %d computed | proof left %d, compared %d, different %d, cleared %d",
                 g_eqOff ? "OFF (a difference was found)" : "on", (int)(h - lH), (int)(fa - lFa), (int)(m - lM), (int)g_eqProofLeft, (int)g_eqChecked, (int)g_eqBad, (int)g_eqClears);
            lH = h; lM = m; lFa = fa;
        }
    }
}
// v72: the places in game.dat this DLL uses (tools/gen_anchors.py) - do they hold the verified build's bytes?
#include "aotr_anchors.inc"
// v85: checked per source file. An anchor that differs turns off only the parts whose own files name that address;
// the core files (the build's frame and logic plumbing) still decide for everything. The import address table is never
// compared: the loader fills it at run time (the tool leaves it out, and this skips it again in case it ever reappears).
static unsigned __int64 g_anchorBad = 0;               // bit i: kAnchorFiles[i] names a place this game.dat changed
static int aotrAnchorFile(const char* name) {
    for (int i = 0; i < (int)(sizeof(kAnchorFiles) / sizeof(kAnchorFiles[0])); ++i) if (!lstrcmpiA(kAnchorFiles[i], name)) return i;
    return -1;                                          // the file names no anchored address
}
static bool aotrPartOk(const char* file) { int i = aotrAnchorFile(file); return i < 0 || !((g_anchorBad >> i) & 1); }
// an engine part whose own places in game.dat are intact (both files, when it uses two)
static bool aotrPart(const char* name, const char* file, const char* file2 = NULL) {
    if (aotrPartOk(file) && (!file2 || aotrPartOk(file2))) return true;
    logf("%s: not installed - this game.dat changed a place it uses (the rest is not affected).", name);
    return false;
}
static bool aotrInIat(BYTE* base, DWORD va, DWORD n) {
    __try {
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dd.VirtualAddress) return false;
        for (IMAGE_IMPORT_DESCRIPTOR* id = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress); id->FirstThunk; ++id) {
            DWORD lo = (DWORD)(ULONG_PTR)base + id->FirstThunk, hi = lo;
            while (*(DWORD*)(ULONG_PTR)hi) hi += 4;
            if (va < hi + 4 && va + n > lo) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static bool aotrAnchorsHold(BYTE* base) {
    static const char* const kCore[] = { "aotr_accel.cpp", "aotr_frameprof.inc", "aotr_modtime.inc", "aotr_logicspread.inc", "aotr_sched.inc", "aotr_guard.inc" };
    int bad = 0, n = (int)(sizeof(kAnchors) / sizeof(kAnchors[0]));
    g_anchorBad = 0;
    for (int i = 0; i < n; ++i) {
        if (aotrInIat(base, kAnchors[i].va, kAnchors[i].n)) continue;
        bool same = false;
        __try { same = memcmp((const void*)(ULONG_PTR)kAnchors[i].va, kAnchors[i].b, kAnchors[i].n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
        if (!same) { ++bad; g_anchorBad |= kAnchors[i].files; }
    }
    if (!bad) return true;
    char ln[700]; int q = 0;
    for (int i = 0; i < (int)(sizeof(kAnchorFiles) / sizeof(kAnchorFiles[0])) && q < 600; ++i)
        if ((g_anchorBad >> i) & 1) q += wsprintfA(ln + q, " %s", kAnchorFiles[i]);
    ln[q] = 0;
    bool core = false;
    for (int k = 0; k < (int)(sizeof(kCore) / sizeof(kCore[0])); ++k) if (!aotrPartOk(kCore[k])) core = true;
    logf("init: this game.dat changed %d of the %d places this DLL uses - named in:%s.%s", bad, n, ln,
         core ? " The engine's frame and logic plumbing is among them: the parts tied to addresses stay off." :
                " Only the parts in those files stay off; the rest of them run.");
    return !core;
}
static DWORD WINAPI initThread(LPVOID) {
    rotateLog();                                     // keep the log readable: >64 MB is renamed, never deleted
    logf("---- %s (build %d) loaded (pid %d) ----", AOTR_RELEASE, AOTR_BUILDNO, GetCurrentProcessId());
    aotrBanner();

    if (aotrSecondCopy()) {
        logf("SKIPPED: a second copy of bfme2_accel.dll is already installed in this process. Two copies patch the "
             "same code twice and fight for every import slot, which corrupts the picture and breaks the read-out. "
             "Only the first copy runs. Close the other loader (aotr_start.exe) and start the game once.");
        return 0;
    }

    HMODULE hGame = GetModuleHandleA("game.dat");
    if (!hGame) hGame = GetModuleHandleA(NULL);
    if (!hGame) { logf("FATAL: cannot find game module"); return 0; }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hGame + dos->e_lfanew);
    BYTE* base = (BYTE*)hGame;
    SIZE_T imgSize = nt->OptionalHeader.SizeOfImage;
    logf("game module base %08X, size %08X, checksum %08X, timestamp %08X",
         (DWORD)(ULONG_PTR)base, (DWORD)imgSize,
         nt->OptionalHeader.CheckSum, nt->FileHeader.TimeDateStamp);

    // ---------------------------------------------------------------- which build is this, and what may be installed
    // The work here splits in two. One half does not care which executable it is running in: the render thread
    // hooks the live Direct3D9 device's own vtable, the preshader cache patches d3dx9_27.dll, and the heap swap and
    // the fast CRT patch entries in the import table by name. The other half - the render-list sort, the logic
    // slicer, the pose warm-up, the audio limit, the pick cache and the rest - is anchored to absolute addresses
    // inside one exact build, and patching those into a different one would hit whatever happens to sit there.
    //
    // So the build is identified by a content hash of its .text (header fields are not enough: AotR ships a
    // "delayfix" game.dat whose timestamp, size, checksum and entry point are all identical to the stock 2.02 one
    // while 61 bytes of code differ, two of them inside the frame dispatcher the slicer works with). A build we
    // know gets what it has been verified for; a build we do not know, but which is clearly the same engine, gets
    // the portable half only; anything else gets nothing.
    DWORD textHash = 0, textRva = 0, textLen = 0;
    {
        IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (memcmp(sh[i].Name, ".text", 5)) continue;
            textRva = sh[i].VirtualAddress;
            textLen = sh[i].Misc.VirtualSize < sh[i].SizeOfRawData ? sh[i].Misc.VirtualSize : sh[i].SizeOfRawData;
            const BYTE* q = base + textRva;
            DWORD h = 2166136261u;
            __try { for (DWORD k = 0; k < textLen; ++k) { h ^= q[k]; h *= 16777619u; } }
            __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
            textHash = h;
            break;
        }
    }
    {
        struct Build { DWORD hash, stamp, size, sum, entry; const char* name; bool engineHooks; };
        static const Build kBuilds[] = {
            { 0x5ED63115, 0x460DA09E, 0x00AD4000, 0x00ADC2F6, 0x0063D082, "RotWK 2.02 (Age of the Ring, Edain, stock 2.02)", true  },
            { 0xDE0F8163, 0x460DA09E, 0x00AD4000, 0x00ADC2F6, 0x0063D082, "RotWK 2.02 delayfix (2.02 + 19 bytes, all outside every anchor)", true },
            { 0x66C9806A, 0x460DA09E, 0x00AD4000, 0x00ADC2F6, 0x0063D082, "Age of the Ring 10 (the delayfix build + 15 bytes in the AI's target search, all outside every anchor)", true },
            { 0x8C81C601, 0x460DA09E, 0x00ACA000, 0x00BAF85F, 0x0063D082, "RotWK 2.02 build 820",                           false },
            { 0x32667B9B, 0x00564544, 0x00ADA000, 0x00BC0776, 0x00629306, "BFME2",                                          false },
        };
        const Build* hit = NULL;
        for (int i = 0; i < (int)(sizeof(kBuilds) / sizeof(kBuilds[0])); ++i)
            if (kBuilds[i].hash == textHash) { hit = &kBuilds[i]; break; }

        // the same engine generation: 32-bit, loaded where the hooks expect, and importing the three libraries
        // every BFME2-era build imports. The portable half checks its own preconditions again before it patches.
        bool family = ((DWORD)(ULONG_PTR)base == 0x00400000) && textLen &&
                      GetModuleHandleA("msvcr71.dll") && GetModuleHandleA("mss32.dll");
        if (hit) {
            g_engineHooks = hit->engineHooks ? 1 : 0;
            logf("init: %s (.text %08X). %s", hit->name, textHash,
                 g_engineHooks ? "Its verified addresses may be used (ground patch textures, render list)." :
                 "Only the parts that do not depend on this build's addresses are installed (render thread, heap, preshader cache, fast CRT).");
        } else if (family && nt->FileHeader.TimeDateStamp == 0x460DA09E && aotrAnchorsHold(base)) {
            // v85: the same 2.02 compile (its time stamp) is enough to look further - size, checksum and entry point may
            // change with an AOTR update; the anchors decide, part by part
            // v72: an AOTR update that changed game.dat only where this DLL never looks (AOTR 10: 15 bytes in the AI's
            // target search) - every address the source names still holds the verified build's bytes
            g_engineHooks = 1;
            logf("init: a newer AOTR game.dat (.text %08X) - its code differs from the verified build; %s",
                 textHash, g_anchorBad ? "the parts whose places are intact may run (see above)." : "every place this DLL uses holds the same bytes. Its verified addresses may be used.");
        } else if (family) {
            g_engineHooks = 0;
            logf("init: an unrecognised build of this engine (.text %08X, stamp %08X size %08X sum %08X entry %08X). "
                 "Only the parts that do not depend on a build's addresses are installed; the rest is left alone.",
                 textHash, nt->FileHeader.TimeDateStamp, (DWORD)imgSize, nt->OptionalHeader.CheckSum,
                 nt->OptionalHeader.AddressOfEntryPoint);
        } else {
            char mk[MAX_PATH];
            if (GetFileAttributesA(aotrPath(mk, "ANY_BINARY_OK")) != INVALID_FILE_ATTRIBUTES) {
                g_engineHooks = 1;
                logf("init: this is not a build this DLL knows (.text %08X, loaded at %08X) - installing EVERYTHING anyway, "
                     "ANY_BINARY_OK is present. A crash is the expected outcome.", textHash, (DWORD)(ULONG_PTR)base);
            } else {
                logf("init: this does not look like a BFME2-era game.dat (.text %08X, loaded at %08X). NOTHING installed - "
                     "the game runs exactly as it would without this DLL.", textHash, (DWORD)(ULONG_PTR)base);
                return 0;
            }
        }

        // [Engine] EngineHooks: -1 (default) = whatever the build table says, 0 = portable half only,
        // 1 = force the engine half - only on a build that was recognised, never on a guessed one.
        { int eh = -1;                               // v30: one fixed set - an old ini cannot switch the verified half off
          if (eh == 0 && g_engineHooks) { g_engineHooks = 0; logf("init: engine hooks OFF by [Engine] EngineHooks = 0 - portable half only."); }
          else if (eh > 0 && !g_engineHooks && hit) { g_engineHooks = 1; logf("init: engine hooks forced ON by [Engine] EngineHooks = 1."); } }
    }

    // ================================================================ v30 (test-11): one finished set, nothing to switch
    // What runs is decided here, on evidence, and is the same for every player - the ini holds only the panel's keys.
    //
    // ON:
    //   gpu        Windows' GPU preference for game.dat -> the GeForce (aotr_gpupref.inc). First, before the game's
    //              Direct3DCreate9 runs.
    //   terrain    each new ground patch texture built on every core (the camera-pan / minimap-jump hitch). Exact: on
    //              this player's game.dat, 260 textures, every byte the same as the engine's, 237.6 ms -> 39.8 ms
    //              (work\probe11\gamemap_test.exe). In game it checks itself on the first 24 and stops on a difference.
    //   rlsort     the mesh render list sort / push / clear without reference-count churn. Exact: on this game.dat,
    //              7220 lists, comparator call sequences, reference counts and release order all identical, 7-12x
    //              faster sorts (work\probe11\rlsort_test.exe). Checks itself in game too.
    //   readahead  the engine's byte-at-a-time file reads answered from 4-64 KB windows (aotr_fdbuf.inc; same bytes,
    //              offline-tested). It also keeps the list of files the game opens.
    //   filewarm   in read mode: the files of earlier sessions read into Windows' file cache, one thread, background
    //              disk priority, yielding to the game (aotr_filewarm.inc, v30).
    //   fastcrt    the game's hot msvcr71 imports with SSE versions that return identical bytes (crt tests).
    //   (preshader cache: removed in v31 - its key leaves out an input bank; see below where it used to be installed.)
    //   where      v31: the long-frame profiler (aotr_loadprof.inc) on the Present counter - in every frame of 80 ms or
    //              more it samples the game thread and logs the game.dat functions it was in. Read only.
    //   overlay    the panel (F7), the 5-second perf line and aotr_report.txt.
    //   crashlog   a fatal exception's place and call chain in the log; nothing handled or changed.
    //   v32 shadownbr  the shadow mesh's edge neighbours (game.dat 0x4F2252, n x n) found through shared vertices, big
    //              models split over the worker cores - 80-99 % of the game thread's time in the test-12 long frames
    //              after the map had loaded. Exact (shadownbr_test: 548 meshes, every byte the same), self-checked.
    //   v32 shadowweld the shadow mesh's vertex weld (0x4F0907, n x n) by grouping equal positions. Exact (shadowweld
    //              test: 184 meshes, four MXCSR modes, same table and count), self-checked.
    //   v32 render thread (FULL, on DXVK): the game's D3D9 / D3DX effect calls queued in order and carried out on a
    //              worker thread. rt_harness on DXVK: every picture byte-identical to the game without it (call de-dupe
    //              off - with it on 146 of 300 pictures differed). A freeze guard steps it aside for the session if the
    //              game thread ever waits 250 ms+ for the worker. On Windows' own d3d9 it stands aside (its method table
    //              is rewritten under it - the test-9 crash).
    // OFF, and not reachable from the ini any more:
    //   CallDedupe: a stale shadow drops a state change, which is a broken picture (measured in rt_harness, v32).
    //   texture cache / texture threads: belong to the render thread; on this HDD the cache was measured slower than
    //       decoding (test-9: "878 KB in 527 ms; decoding them took 41 ms").
    //   rpmalloc: ~250 allocations a second went through it in test-10 (the engine has its own pools) - nothing to
    //       gain, and another heap changes where the game's own use-after-free bugs land.
    //   everything that touches the simulation or its timing (FpsLimit, LogicHz, SmoothAnimation, logic spread,
    //       memo caches of logic queries, faction AI, script sort) and everything that changes the picture
    //       (in-between frames, merged draws, shadow map size, smooth shadows) - multiplayer and "no broken graphics".
    installGpuPref();
    logf("alloc: the game's own heap is kept (v30: rpmalloc carried ~250 allocations a second - the engine has its own pools).");

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    BYTE* textBase = base; SIZE_T textSize = imgSize;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            textBase = base + sec[i].VirtualAddress;
            textSize = sec[i].Misc.VirtualSize;
            break;
        }
    }
    QueryPerformanceFrequency(&g_pqpf);
    g_textLo = (DWORD)(ULONG_PTR)textBase; g_textHi = g_textLo + (DWORD)textSize;
    g_gameBase = (DWORD)(ULONG_PTR)base; g_gameEnd = g_gameBase + (DWORD)imgSize;
    p_NtQIT = (tNtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    profRefreshModules();
    // v32: the render thread is ON (FULL: the game's D3D9 / D3DX effect calls queued in order and carried out on a
    // worker thread). It hooks the D3DX imports here, before the overlay; it goes live at the game's first effect calls,
    // on DXVK only (on Windows' own d3d9 it stands aside - the test-9 crash). Call de-dupe is off for good (it changed
    // pictures in rt_harness); with it off rt_harness gives byte-identical pictures. Drawing only - no logic.
    // v33 (ISH-7): the texture workers (aotr_texjobs.inc) are back, repaired. The 30 of 629 differences and the crash of
    // v32's test had two causes, both found with work/ish7/tx texjob_test: (1) the freeze guard switched the game thread
    // to direct calls AT ONCE while older records were still queued - a Release ran before an upload record that named
    // the same texture (use after free; the 1x1 house-colour textures came out wrong the same way); it now only asks,
    // and the switch is made at the next Present after draining the queue; (2) a level written a second time went the
    // old way while the texture's chain stayed live, so the mip filter after it worked on the FIRST picture; such a
    // chain (and any texture the game locks for writing) is now dropped. Result: texjob_test dll 1 300 - 13 069 of
    // 13 069 sequences identical, no crash; the game thread's share 1.25 ms a texture instead of 15.23 ms. In the game
    // the first 24 results are compared again with D3DX on a real texture; one difference = off for the session.
    { char mk[MAX_PATH]; if (GetFileAttributesA(aotrPath(mk, "VANILLA")) != INVALID_FILE_ATTRIBUTES) g_vanilla = 1; }
    if (g_vanilla) {
        // v51: the game as it is, measured. Only the crash log, the panel and report, the long-frame profiler and the
        // frame timer (read only) are installed - no render thread, no texture workers, no file reading, no preload, no
        // logic slicer, no memo, no core of its own: the same saved game played with and without VANILLA gives the
        // numbers of the original AotR against this DLL, side by side.
#ifdef AOTR_DIAG
        logf("init: VANILLA is present - nothing of the game is changed: no render thread, no texture or file work, no preload, no slicer, no memos, no core of its own. Only frames are timed and reported.");
        installCrashLog();
        installOverlay();
        if (g_engineHooks) { installLoadProf(); installFrameProf(); }
        CreateThread(NULL, 0, aotrProdReport30, NULL, 0, NULL);
        CreateThread(NULL, 0, aotrMarkKeys, NULL, 0, NULL);
        logf("init: %s (build %d) ready - VANILLA (measurement only).", AOTR_RELEASE, AOTR_BUILDNO);
#else
        logf("init: VANILLA is present - nothing of the game is changed (the DLL stays idle this session).");
#endif
        return 0;
    }
    installTexJobs();
    rtInit();
    // v31: the D3DX preshader cache is NOT installed any more. Its key is the program (a0) and the constant bank (a3);
    // the other input bank a2 - "a2/a3 = input constant banks (the values that vary per call)" in its own notes - is
    // not in the key, so two calls with the same a3 and a different a2 share an entry and the second gets the first
    // one's output: shader constants that belong to another object. Only every 32nd hit was compared, and the product
    // build never logged the comparison (preshaderReport is not started under AOTR_PROD), so test-11 could not show it.
    // It was the one module of v30 that could put wrong numbers into a shader - a wrong shadow (test-11: the shadow of
    // a Harad tower). Removed, not repaired. It also gives back the 34 MB its table committed.
    // v33 (ISH-7): back, repaired - the key now holds every byte the VM can read (a2 and the other banks included),
    // every hit is compared in full, and hits are re-run against the VM (aotr_preshader.inc).
    installPreshaderCache();
    installFastCrt(base);                           // exact fast memcpy / memset / strlen / strcmp / floor ... for the game's imports
    installFdBuf(base);                              // the engine's unbuffered file reads answered from read-ahead windows
    installFileWarm(true);                           // the files of earlier sessions read into the file cache before the game asks
    installGuard();                                  // v55.1: aotr_guard.txt - a run that hangs while loading turns this build's new parts off next time
    installPreload();                                // v46: this match's models, textures and sounds found from the mod's INI and read ahead (PRELOAD_OFF)
    installCrashLog();                               // fatal exceptions: where, registers and the call chain into the log
#ifdef AOTR_DIAG
    installOverlay();                                // the panel, the perf line and the session report (diagnostics build only)
#else
    installOverlay(true);                            // v71: the status light only (green / yellow / red, F7)
#endif
    if (g_engineHooks) {                             // anchored to absolute addresses inside one verified build
        if (aotrPart("w3dlock", "aotr_w3dlock.inc")) installW3dLock();                            // v54: the W3D render lock (kernel mutex 0xDD1FD8) kept in user mode - before W3D makes it (W3DLOCK_OFF)
        if (aotrPart("rlsort", "aotr_rlsort.inc")) installRlSort();                             // render list sort / push / clear without reference-count churn (exact, self-checked)
        // v39: the radar (minimap) and the shroud through the render queue - lost from the install list in v30 (c51fe95).
        // test-20: the radar code (0x44C000..0x451000) was 13-17% of the client step and its per-pixel writes went through
        // a synchronous surface lock each (a wait for the whole render queue, ~650 a battle frame). Pixel writes queued in
        // runs, shroud rows staged, radar textures written through CPU mirrors - same calls on the device, same order.
        // Off: RADARHOOKS_OFF next to the DLL.
        { char mk[MAX_PATH];
          if (GetFileAttributesA(aotrPath(mk, "RADARHOOKS_OFF")) == INVALID_FILE_ATTRIBUTES) { if (aotrPart("rt radar", "aotr_rt.inc", "aotr_rtmirror.inc")) rtInstallEngineHooks(); }
          else logf("rt: RADARHOOKS_OFF is present - radar pixels, shroud and radar textures stay synchronous."); }
        if (aotrPart("terrain", "aotr_terrain.inc")) installTerrain();                            // each new ground patch texture built on every core (exact, self-checked)
        if (aotrPart("terrainkeep", "aotr_terrainkeep.inc", "aotr_terrain.inc")) installTerrainKeep();                        // v46: patches out of view keep their textures (camera turning: 2645 rebuilt in 5 s); hash-checked; TERRAINKEEP_OFF
#ifdef AOTR_DIAG
        if (aotrPart("abtest", "aotr_abtest.inc")) installAbTest();                             // v50 measurement tool: only with AB_ON beside the DLL
#endif
        if (aotrPart("pevict", "aotr_pevict.inc")) installPartEvict();                          // v42: particle limit eviction - no repeated empty scans (exact; PEVICT_OFF)
        if (!aotrPart("amfind", "aotr_amfind.inc")) {} else if (!gdNewOff()) installAmFind(); else logf("amfind: not installed (GUARD safe mode).");
        if (!aotrPart("cbfind", "aotr_cbfind.inc")) {} else if (!gdNewOff()) installCbFind(); else logf("cbfind: not installed (GUARD safe mode).");
        if (!aotrPart("fmfind", "aotr_fmfind.inc")) {} else if (!gdNewOff()) installFmFind(); else logf("fmfind: not installed (GUARD safe mode).");
        if (!aotrPart("maskfast", "aotr_maskfast.inc")) {} else if (!gdNewOff()) installMaskFast(); else logf("maskfast: not installed (GUARD safe mode).");
        if (!aotrPart("hashfind", "aotr_hashfind.inc")) {} else if (!gdNewOff()) installHashFind(); else logf("hashfind: not installed (GUARD safe mode).");   // v82: hash map find by multiply (self-proving; HASHFIND_OFF)   // v81: flag mask test in SSE (self-proving; MASKFAST_OFF)   // v81: Object::findModule per object (self-proving; FMFIND_OFF)   // v77: command buttons by name without the walk (self-proving; CBFIND_OFF)
        if (!aotrPart("psremove", "aotr_psremove.inc")) {} else if (!gdNewOff()) installPsRemove(); else logf("psremove: not installed (GUARD safe mode).");                           // v55: FX manager remove without walking every system (self-proving; PSREMOVE_OFF)
        if (aotrPart("pswarm", "aotr_pswarm.inc")) installPsWarm();                             // v54: the particle system list read ahead before update and draw walk it (PSWARM_OFF)
        if (aotrPart("psfind", "aotr_psfind.inc")) installPsFind();                             // v41: ParticleSystemManager::findParticleSystemByID without walking every system (self-proving; PSFIND_OFF)
        if (!aotrPart("pxpar", "aotr_pxpar.inc")) {} else if (!gdNewOff()) installPxPar(); else logf("pxpar: not installed (GUARD safe mode).");
        if (!aotrPart("taskwarm", "aotr_taskwarm.inc")) {} else if (!gdNewOff()) installTaskWarm(); else logf("taskwarm: not installed (GUARD safe mode).");   // v77: the FX renderer's task list read ahead (prefetch only; TASKWARM_OFF)   // v74: the particles' own update on the spare cores (exact, self-checking; PXPAR_OFF)
#ifdef AOTR_DIAG
        if (aotrPart("vmprobe", "aotr_vmprobe.inc")) installVmProbe();                            // v77: the page faults' source - commits / frees by caller, working-set trims
        if (aotrPart("fxprobe", "aotr_fxprobe.inc")) installFxProbe();                            // v73: the particle update measured - its time and the module classes used (read only)
#endif
        if (aotrPart("shadownbr", "aotr_shadownbr.inc")) installShadowNbr();                          // v32: shadow edge neighbours without comparing every triangle pair (exact, self-checked, big models on every core)
        if (aotrPart("shadowweld", "aotr_shadowweld.inc")) installShadowWeld();                         // v32: shadow mesh vertex weld by grouping equal positions instead of every pair (exact, self-checked)
        // v33: pose warm (skeleton tree updates on worker threads) measured in the Helm's Deep battle: exact (MISMATCH 0 in
        // ~50 000 updates / 5 s) but no gain - client+drawing 14.56 ms with it against 14.12 ms without (work\ish7 c1_pw /
        // b80p). Not installed; AOTR_POSEWARM2=1 turns it on for a measurement.
        { char pe[8]; pe[0] = 0; if (GetEnvironmentVariableA("AOTR_POSEWARM2", pe, sizeof(pe)) && pe[0] == '1') { if (aotrPart("pose-warm2", "aotr_posewarm2.inc")) { installAnimTimers(); installPoseWarm2(); } }
          else logf("pose-warm2: not installed (v33: measured, no gain in the battle frame; AOTR_POSEWARM2=1 for a measurement)."); }
#ifdef AOTR_DIAG
        if (aotrPart("loadprof", "aotr_loadprof.inc")) installLoadProf();                           // v31: frames of 80 ms+ - which game.dat functions the game thread was in (read only)
#endif
        installSched();                              // v33: the frame limiter sleeps instead of spinning (same limit, same frame end); measurement options
        installFrameProf();                          // v33: every frame timed in its parts, sampled 50x a second, 'frame:' and 'cores:' every 5 s (read only)
        // v35: the logic step evened out over its six frames. The six logical calls run unchanged, in order, each once per
        // step (the simulation is the stock one, bit for bit); only where the render frames fall inside that sequence moves.
        // test-15/16: sub-frame 5 alone took 35-45 ms while sub-frames 2 and 6 took ~0 - one frame in six was a hitch.
        // Off: a file named SLICER_OFF next to the DLL (out\SLICER_OFF), or AOTR_LOGICSPREAD=0.
        installModuleTimers();                       // the 12 sub-frame-5 subsystem updates timed (the slicer budgets that block by them)
#ifdef AOTR_DIAG
        installWholeMapTimer();                      // v53: the whole-map partition query timed at its six call sites (read only)
#endif
        // v64: installShadowHalf() removed for good - test-46: the shadows still flickered and spells (the Army of the Dead) looked wrong
        if (aotrPart("partfast", "aotr_partfast.inc")) installPartFast();                           // v54: its object loop reads memory ahead - same calls, same order (self-proving; PARTFAST_OFF)
        installLogicSpread();                        // needs the frame profiler's logic wrapper above (it measures the steps through it)
        // v37 (test-18 diag: in big battles the sound request limit check 0x456D8B was the single hottest client spot,
        // 8-17% of the client step, growing with the battle; ThingTemplate::isEquivalentTo ~9% of the logic). Both are
        // exact and self-proving (compared with the original, one difference -> off for the session).
        // Off: AUDIOLIMIT_OFF / EQUIV_OFF next to the DLL.
        { char mk[MAX_PATH];
          if (GetFileAttributesA(aotrPath(mk, "AUDIOLIMIT_OFF")) == INVALID_FILE_ATTRIBUTES) { if (aotrPart("audiolimit", "aotr_audiolimit.inc")) installAudioLimit(); }
          else logf("audiolimit: AUDIOLIMIT_OFF is present - the engine's own list walk stays.");
          if (GetFileAttributesA(aotrPath(mk, "EQUIV_OFF")) == INVALID_FILE_ATTRIBUTES) { if (aotrPart("equiv-memo", "aotr_equivmemo.inc")) installEquivMemo(); }
          else logf("equiv-memo: EQUIV_OFF is present - memo OFF.");
          // v45 (test-26: the equivalence memo was asked 1.8 million times a second, mostly from the object filter
          // 0x763543 looping over its template lists; the memo's own call cost was 3-4% of the logic): the filter's
          // template part per (filter, template), self-proving like the others. Off: FILTER_OFF next to the DLL.
          if (GetFileAttributesA(aotrPath(mk, "FILTER_OFF")) == INVALID_FILE_ATTRIBUTES) { if (aotrPart("filter-memo", "aotr_equivmemo.inc")) installFilterMemo(); }
          else logf("filter-memo: FILTER_OFF is present - memo OFF."); }
        // v40 (measurement): test-21 showed the slow quarter of the battle differs from the fast one by +2-3 ms of logic
        // and +8-10 ms of client step. These name where inside the client step that goes - the client update's call sites
        // and W3DDisplay::draw (clienttime), the scene render's parts (flushtime) and the 24 client subsystems (subsys).
        // Timing only: every call is made exactly as before.
        // v68: kept for the multiplayer measurement (where the client step's time goes; timing only)
#ifdef AOTR_DIAG
        if (aotrPart("clienttime", "aotr_clienttime.inc")) installClientTimers();
        if (aotrPart("flushtime", "aotr_flushtime.inc")) installFlushTimers();
        if (aotrPart("subsys", "aotr_subsys.inc")) installSubsysTimers();
        CreateThread(NULL, 0, aotrMarkKeys, NULL, 0, NULL);
#endif
        CreateThread(NULL, 0, aotrProdReport30, NULL, 0, NULL);   // every 30 s: the learned list saved, the game thread's core kept (its log lines only in the diagnostics build)
        // v34 (ISH-8 #4): both were written in v54/v55, are included above, and lost their install call in v30 (c51fe95)
        // when the ini switches went. Each checks the game's bytes first and changes nothing if they differ.
        if (aotrPart("exitfix", "aotr_exitfix.inc")) installExitFix();                            // the game's own crash on leaving a skirmish (test-14: C0000005 at game.dat+2B4EAE under the AI reset 0x5B42B0, scanner 0x7EDB5D) - only while that reset runs
        if (aotrPart("streamsets", "aotr_streamset.inc")) installStreamSets();                         // the asset streamer completes its wanted-asset sets without copying them (same inserts, same order; the first 48 checked against the engine's own walk)
    } else
        logf("init: this build is not one whose addresses were verified - the ground patch and render list parts stay off; the rest runs.");
    logf("init: multicore - render thread %s (the game's D3D9 / D3DX effect calls carried out on their own core, in the same order), texture decoding and mip chains on worker threads (%s), shadow edge neighbours of big models on %d cores, ground patch textures on %d cores, files read ahead on their own thread, DXVK's own threads; the game's logic tick stays on one thread, in step with every other player (multiplayer-safe: no logic code, no logic timing touched).",
         g_rtTried && !g_rtInstalled ? "OFF" : "ON (live from the first effect calls, on DXVK)", g_txjOn ? "on, with the render thread, self-checked" : "off", g_snbOn ? (int)g_snbHelpers + 1 : 1, g_tpOn ? (int)g_tpHelpers + 1 : 1);
#ifdef AOTR_DIAG
    logf("init: %s (build %d) ready - DIAGNOSTICS build: the panel (F7), aotr_report.txt and the perf: lines say how the game runs.", AOTR_RELEASE, AOTR_BUILDNO);
#else
    logf("init: %s (build %d) ready - from here on this log takes only problems (a part that switched itself off, a crash).", AOTR_RELEASE, AOTR_BUILDNO);
    bdgWriteStatus();                                // the starter learns at once that the DLL is up
    InterlockedExchange(&g_logQuiet, 1);
#endif
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        aotrSetDir(hMod);                                   // before anything can log
        InitializeCriticalSection(&g_logCs);
        InitializeCriticalSection(&g_cs);
#ifdef AOTR_V4_NOSPIN
        InitializeCriticalSection(&g_rtExecCs);
#else
        InitializeCriticalSectionAndSpinCount(&g_rtExecCs, 4000);     // short holds: spin before sleeping
#endif
        InitializeCriticalSection(&g_rtInitCs);
        CreateThread(NULL, 0, initThread, NULL, 0, NULL);   // keep work out of the loader lock
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        fdbWFlushAllNoLock(false);                          // v34: collected writes still waiting go to their files (nothing lost at exit)
#ifdef AOTR_DIAG
        ovSessionEnd();                                     // the game is closing: the session summary line, once (aotr_overlay.inc)
#endif
    }
    return TRUE;
}
