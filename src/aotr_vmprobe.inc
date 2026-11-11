// aotr_vmprobe.inc - v77 (diagnostics build only): who makes the 15 000 page faults a second.
//
// The v74 test counted 14 000-19 000 page faults a second in calm play (none from the disk) and 10 000-80 000 in the
// slow frames, while the address space grew only ~0.5 MB a second. So the faults are not growth: either memory is
// committed and freed again and again (each new page is a demand-zero fault when first touched), or Windows trims the
// game's working set and the pages come back as soft faults. This tells the two apart and names the caller:
//  - NtAllocateVirtualMemory / NtFreeVirtualMemory (every VirtualAlloc, every large heap block, DXVK's own memory)
//    are wrapped in ntdll itself (the stub's first instruction, `mov eax, <service>`, moved to a trampoline). Each
//    commit / decommit / release of this process is added to the module that asked for it: the first frame of a short
//    stack walk outside ntdll, kernel32, kernelbase and the C runtime (game.dat, DXVK's d3d9.dll, this DLL, d3dx9,
//    other). The module table is rebuilt by the report thread, never inside the hook (no loader lock there).
//  - the working set is read every 30 ms (aotrMarkKeys); a fall of 2 MB or more between two readings is a trim.
// Every 30 s one line: MB committed and freed by each caller, calls, the trims, and the faults in the same 30 s.
// It changes nothing the game does: the original stub runs for every call.

#include <psapi.h>
typedef BOOL (WINAPI* tVmEnum)(HANDLE, HMODULE*, DWORD, LPDWORD);
typedef BOOL (WINAPI* tVmModInfo)(HANDLE, HMODULE, LPMODULEINFO, DWORD);
typedef BOOL (WINAPI* tVmMemInfo)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
static tVmEnum p_vmEnum = NULL; static tVmModInfo p_vmModInfo = NULL; static tVmMemInfo p_vmMemInfo = NULL;   // kernel32's K32* (looked up: no psapi.lib)
static HMODULE g_vmSelf = NULL;
struct VmMod { DWORD lo, hi; BYTE cls; };          // cls: 0 skip (system / CRT), 1 game.dat, 2 DXVK d3d9, 3 this DLL, 4 d3dx9, 5 other
static VmMod g_vmMods[96]; static volatile LONG g_vmNMods = 0;
static volatile LONG64 g_vmBytes[2][6]; static volatile LONG g_vmCalls[2][6];   // [0 commit / 1 free][class]
static volatile LONG g_vmOn = 0, g_vmTrims = 0; static volatile LONG64 g_vmTrimBytes = 0;
static BYTE* g_vmTramp = NULL;
typedef LONG (NTAPI* tVmAlloc)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
typedef LONG (NTAPI* tVmFree)(HANDLE, PVOID*, PSIZE_T, ULONG);
static tVmAlloc o_vmAlloc = NULL; static tVmFree o_vmFree = NULL;
static __forceinline int vmClassOf(DWORD a) {
    LONG n = g_vmNMods;
    for (LONG i = 0; i < n; ++i) if (a >= g_vmMods[i].lo && a < g_vmMods[i].hi) return g_vmMods[i].cls;
    return 5;
}
static LONG NTAPI vmAlloc(HANDLE h, PVOID* base, ULONG_PTR zb, PSIZE_T size, ULONG type, ULONG prot);
static LONG NTAPI vmFree(HANDLE h, PVOID* base, PSIZE_T size, ULONG type);
static __forceinline bool vmOwnFrame(DWORD a) {   // a return address inside this file's two hooks
    DWORD x = (DWORD)(ULONG_PTR)&vmAlloc, y = (DWORD)(ULONG_PTR)&vmFree;
    return (a - x) < 0x100 || (a - y) < 0x100;
}
static void vmNote(int kind, SIZE_T bytes) {
    void* fr[16]; USHORT n = RtlCaptureStackBackTrace(0, 16, fr, NULL);
    int cls = 5;
    for (USHORT i = 0; i < n; ++i) {
        DWORD a = (DWORD)(ULONG_PTR)fr[i];
        if (vmOwnFrame(a)) continue;
        int c = vmClassOf(a);
        if (c == 3 && i < 2) continue;              // vmNote's own frame
        if (c) { cls = c; break; }
    }
    InterlockedExchangeAdd64(&g_vmBytes[kind][cls], (LONG64)bytes);
    InterlockedIncrement(&g_vmCalls[kind][cls]);
}
static __forceinline bool vmSelf(HANDLE h) { return h == (HANDLE)(LONG_PTR)-1; }
static LONG NTAPI vmAlloc(HANDLE h, PVOID* base, ULONG_PTR zb, PSIZE_T size, ULONG type, ULONG prot) {
    LONG r = o_vmAlloc(h, base, zb, size, type, prot);
    if (r >= 0 && g_vmOn && (type & MEM_COMMIT) && vmSelf(h) && size) vmNote(0, *size);
    return r;
}
static LONG NTAPI vmFree(HANDLE h, PVOID* base, PSIZE_T size, ULONG type) {
    LONG r = o_vmFree(h, base, size, type);
    if (r >= 0 && g_vmOn && vmSelf(h) && size) vmNote(1, *size);
    return r;
}
static void vmModules() {                          // the report thread: which address belongs to which module
    HMODULE mods[96]; DWORD need = 0;
    if (!p_vmEnum || !p_vmModInfo || !p_vmEnum(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    int n = (int)(need / sizeof(HMODULE)); if (n > 96) n = 96;
    VmMod t[96]; int k = 0;
    for (int i = 0; i < n; ++i) {
        MODULEINFO mi; char nm[MAX_PATH];
        if (!p_vmModInfo(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) || !GetModuleFileNameA(mods[i], nm, MAX_PATH)) continue;
        char* b = nm; for (char* q = nm; *q; ++q) if (*q == '\\' || *q == '/') b = q + 1;
        CharLowerA(b);
        BYTE c = 5;
        if (!lstrcmpA(b, "ntdll.dll") || !lstrcmpA(b, "kernel32.dll") || !lstrcmpA(b, "kernelbase.dll") || !lstrcmpA(b, "msvcr71.dll") ||
            !lstrcmpA(b, "ucrtbase.dll") || !lstrcmpA(b, "msvcrt.dll") || !lstrcmpA(b, "vcruntime140.dll")) c = 0;
        else if ((DWORD)(ULONG_PTR)mods[i] == 0x00400000) c = 1;
        else if (mods[i] == g_vmSelf) c = 3;
        else if (!lstrcmpA(b, "d3d9.dll")) c = 2;
        else if (!lstrcmpA(b, "d3dx9_27.dll")) c = 4;
        t[k].lo = (DWORD)(ULONG_PTR)mi.lpBaseOfDll; t[k].hi = t[k].lo + mi.SizeOfImage; t[k].cls = c; ++k;
    }
    // published while the hooks may be reading: entries are written before the count grows; a shrinking table only
    // makes a caller fall back to "other" for a moment
    InterlockedExchange(&g_vmNMods, 0);
    for (int i = 0; i < k; ++i) g_vmMods[i] = t[i];
    InterlockedExchange(&g_vmNMods, k);
}
static bool vmWrap(const char* name, void* hook, void** orig, BYTE* tramp) {
    BYTE* f = (BYTE*)GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
    if (!f || f[0] != 0xB8) return false;          // the stub must begin with `mov eax, <service number>`
    memcpy(tramp, f, 5); tramp[5] = 0xE9; *(DWORD*)(tramp + 6) = (DWORD)(ULONG_PTR)(f + 5) - (DWORD)(ULONG_PTR)(tramp + 10);
    *orig = tramp;
    DWORD op = 0; if (!VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &op)) return false;
    f[0] = 0xE9; *(DWORD*)(f + 1) = (DWORD)(ULONG_PTR)hook - (DWORD)(ULONG_PTR)(f + 5);
    VirtualProtect(f, 5, op, &op); FlushInstructionCache(GetCurrentProcess(), f, 5);
    return true;
}
static void installVmProbe() {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    p_vmEnum = (tVmEnum)GetProcAddress(k32, "K32EnumProcessModules"); p_vmModInfo = (tVmModInfo)GetProcAddress(k32, "K32GetModuleInformation");
    p_vmMemInfo = (tVmMemInfo)GetProcAddress(k32, "K32GetProcessMemoryInfo");
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&installVmProbe, &g_vmSelf);
    if (!p_vmEnum || !p_vmModInfo || !p_vmMemInfo || !g_vmSelf) { logf("vmprobe: the process functions are missing - not installed."); return; }
    g_vmTramp = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_vmTramp) return;
    vmModules();
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    bool a = vmWrap("NtAllocateVirtualMemory", (void*)&vmAlloc, (void**)&o_vmAlloc, g_vmTramp);
    bool b = a && vmWrap("NtFreeVirtualMemory", (void*)&vmFree, (void**)&o_vmFree, g_vmTramp + 32);
    resumeAll(fz, nf);
    if (a && !b) { logf("vmprobe: NtFreeVirtualMemory is not the expected stub - only commits are counted."); }
    if (!a) { logf("vmprobe: NtAllocateVirtualMemory is not the expected stub (another hook?) - not installed."); return; }
    InterlockedExchange(&g_vmOn, 1);
    logf("vmprobe: memory commits and frees are counted by caller (game.dat / DXVK / this DLL / d3dx9 / other), and working-set trims every 30 ms (diagnostics).");
}
static void vmSampleWs() {                         // aotrMarkKeys, every 30 ms
    static SIZE_T last = 0;
    PROCESS_MEMORY_COUNTERS pm; pm.cb = sizeof(pm);
    if (!p_vmMemInfo || !p_vmMemInfo(GetCurrentProcess(), &pm, sizeof(pm))) return;
    if (last && pm.WorkingSetSize + (2u << 20) <= last) { InterlockedIncrement(&g_vmTrims); g_vmTrimBytes += (LONG64)(last - pm.WorkingSetSize); }
    last = pm.WorkingSetSize;
}
static void vmReport() {                           // the 30 s report thread
    if (!g_vmOn) return;
    static LONG64 lB[2][6]; static LONG lC[2][6], lT = 0; static LONG64 lTB = 0; static DWORD lPf = 0;
    static const char* const kNm[6] = { "system", "game.dat", "DXVK", "this DLL", "d3dx9", "other" };
    char ln[2][400];
    for (int k = 0; k < 2; ++k) {
        int q = 0; ln[k][0] = 0;
        for (int c = 1; c < 6; ++c) {
            LONG64 b = g_vmBytes[k][c] - lB[k][c]; LONG n = g_vmCalls[k][c] - lC[k][c]; lB[k][c] = g_vmBytes[k][c]; lC[k][c] = g_vmCalls[k][c];
            if (n) q += wsprintfA(ln[k] + q, " %s %d MB in %d calls;", kNm[c], (int)(b >> 20), (int)n);
        }
    }
    PROCESS_MEMORY_COUNTERS pm; pm.cb = sizeof(pm); DWORD pf = 0;
    if (p_vmMemInfo(GetCurrentProcess(), &pm, sizeof(pm))) pf = pm.PageFaultCount;
    LONG t = g_vmTrims - lT; LONG64 tb = g_vmTrimBytes - lTB; lT = g_vmTrims; lTB = g_vmTrimBytes;
    logf("vmprobe: last 30 s - %u page faults; committed:%s | freed:%s | working set fell %d times (%d MB in all; a fall is Windows trimming the game)",
         lPf ? pf - lPf : 0, ln[0][0] ? ln[0] : " nothing", ln[1][0] ? ln[1] : " nothing", (int)t, (int)(tb >> 20));
    lPf = pf;
    vmModules();
}
