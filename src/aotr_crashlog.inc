// ================================================================ crash log (diagnostic only)
// The Windows event log shows the game dying with an access violation inside its own heap code (game.dat+0x3330A, the
// free-chunk unlink of its dlmalloc) at the end of nearly every session since 2026-09-09 - always after "Exit" was
// pressed. The event log has no call chain, so it cannot say whose teardown frees or overruns what. This handler
// writes that down: exception code and address, the registers, the address touched, and every return address on the
// stack that points behind a call in game.dat or into this DLL. It changes nothing: it always returns
// EXCEPTION_CONTINUE_SEARCH, so whoever handled (or did not handle) the exception before still does.
// First-chance exceptions that are then handled would be logged too, so: access violations, illegal instructions,
// stack overflows and integer divides only, outside this DLL's own code, and at most 12 entries per process. AOTR_CRASHLOG=0 turns it off.
static volatile LONG g_clN = 0, g_clBusy = 0;
// v38: every exception the process raises is counted (cheap: this runs only when one is raised) - test-19 had a few
// per cent of the game thread in what looked like exception dispatch; the 30 s line says how many, from where.
static volatile LONG g_clAll = 0, g_clSelfN = 0, g_clGame = 0;
#define CL_IPN 32
static volatile DWORD g_clIp[CL_IPN]; static volatile LONG g_clIpN[CL_IPN];
static void clCount(DWORD code, DWORD ip) {
    InterlockedIncrement(&g_clAll);
    if (g_selfLo && ip >= g_selfLo && ip < g_selfHi) InterlockedIncrement(&g_clSelfN);
    else if (g_textLo && ip >= g_textLo && ip < g_textHi) InterlockedIncrement(&g_clGame);
    DWORD key = (ip & ~0xFu) | 1u;                     // (bit 0: never 0, so an empty slot stays 0)
    int k = (int)(((ip >> 4) * 2654435761u) >> 27);
    for (int t = 0; t < 4; ++t, k = (k + 1) & (CL_IPN - 1)) {
        if (g_clIp[k] == key) { InterlockedIncrement(&g_clIpN[k]); return; }
        if (!g_clIp[k]) { if (InterlockedCompareExchange((volatile LONG*)&g_clIp[k], (LONG)key, 0) == 0 || g_clIp[k] == key) { InterlockedIncrement(&g_clIpN[k]); return; } }
    }
}
static void clReport() {                                   // from the 30 s product report thread
    static LONG lA = 0, lS = 0, lG = 0; static LONG lN[CL_IPN];
    LONG a = g_clAll, sN = g_clSelfN, g = g_clGame;
    if (a == lA) return;
    char ln[900]; int q = wsprintfA(ln, "exceptions: last 30 s - %d raised (%d in this DLL, %d in game.dat, %d elsewhere) | by place:", (int)(a - lA), (int)(sN - lS), (int)(g - lG), (int)((a - lA) - (sN - lS) - (g - lG)));
    for (int i = 0; i < CL_IPN && q < 800; ++i) { LONG d = g_clIpN[i] - lN[i]; lN[i] = g_clIpN[i]; if (d <= 0) continue;
        char sym[160]; symName(g_clIp[i] & ~0xFu, sym); q += wsprintfA(ln + q, " %s=%d", sym, (int)d); }
    logf("%s", ln);
    lA = a; lS = sN; lG = g;
}
static DWORD g_clProbe[16]; static int g_clProbeN = 0;   // the entry points of Windows' IsBad*Ptr functions
static LONG CALLBACK clHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    clCount(code, (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress);
    if (code != 0xC0000005u && code != 0xC000001Du && code != 0xC00000FDu && code != 0xC0000094u) return EXCEPTION_CONTINUE_SEARCH;
    { DWORD ip = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;                         // this DLL's own guarded probes fault on purpose and handle it themselves (v25: they used up the entries)
      if (g_selfLo && ip >= g_selfLo && ip < g_selfHi) return EXCEPTION_CONTINUE_SEARCH; }
    { DWORD ip = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;                         // v85: Windows' IsBad*Ptr probe memory by faulting on purpose and
      for (int i = 0; i < g_clProbeN; ++i) if (ip - g_clProbe[i] < 0x100) return EXCEPTION_CONTINUE_SEARCH; }   // handle it themselves (v84 test: a "crash" at IsBadStringPtrW+2A that was not one)
    if (InterlockedIncrement(&g_clN) > 12) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_clBusy, 1)) return EXCEPTION_CONTINUE_SEARCH;               // an exception raised while this handler is writing: not its business
    fdbWFlushAllNoLock(true);                                                                 // v34: writes still collected go to their files first (nothing the game wrote is lost in a crash)
    const CONTEXT* c = ep->ContextRecord;
    char sym[160]; symName((DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress, sym);
    DWORD rw = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[0] : 9, at = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    logf("crash: exception %08X at %s (thread %u, %s %08X) | eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X | phase %d, logic sub-frame %d, frame %d",
         code, sym, GetCurrentThreadId(), rw == 0 ? "reading" : (rw == 1 ? "writing" : (rw == 8 ? "executing" : "touching")), at,
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, (int)g_phase, (int)g_lgCurN, (int)g_mkFrames);
    // ---- v29 guilt guard (defect 2). The one question that matters when the game dies while this DLL is live is
    // "is this ours?". Three facts settle it without needing a debugger, and they are written here for every fatal
    // fault: (a) exactly where in game.dat, or in this DLL, the instruction pointer is; (b) the bytes at that
    // address, so a fault reported mid-instruction (a corrupted return or indirect target) is visible as such; and
    // (c) whether the faulting thread is one of ours - the game/render thread this DLL adopted, or one of the
    // worker threads it started. If (c) is a thread we own AND (a) lands in our .text, it is ours, with proof.
    {
        DWORD ip = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
        const char* where = "(not in game.dat or this DLL)";
        DWORD off = 0;
        if (g_gameBase && ip >= g_gameBase && ip < g_gameEnd) { where = "game.dat"; off = ip - g_gameBase; }
        else if (g_selfLo && ip >= g_selfLo && ip < g_selfHi) { where = "THIS DLL"; off = ip - g_selfLo; }
        const char* whose = "not a thread this DLL started (the game's own)";
        { DWORD t = GetCurrentThreadId();
          if (t == g_rtMainTid) whose = "the game thread this DLL is hooked into";
          else if (t == g_rtWorkerTid) whose = "THIS DLL's render worker (FULL mode)";
          else if (g_mkTid && t == g_mkTid) whose = "the engine render thread (the game's own, named by an engine hook)"; }
        logf("crash:   guilt guard: eip in %s + %X | faulting thread: %s (tid %u) | our main=%u worker=%u | module base %08X",
             where, off, whose, GetCurrentThreadId(), g_rtMainTid, g_rtWorkerTid, g_gameBase);
        BYTE* p = (BYTE*)(ip & ~3u) - 8;
        char bx[64]; int bq = 0;
        __try { for (int i = 0; i < 24 && bq < 60; ++i) bq += wsprintfA(bx + bq, "%02X ", p[i]); }
        __except (EXCEPTION_EXECUTE_HANDLER) { bq = wsprintfA(bx, "(unreadable) "); }
        bx[bq] = 0;
        logf("crash:   guilt guard: bytes around the faulting address (from ip-8, 4-byte aligned): %s", bx);
        // (d) was this DLL doing anything at all, and how recently? These are the counters the report also prints: a
        // DLL that has handled no call, served no frame and queued nothing cannot have corrupted the heap. If they are
        // large, the DLL was busy and its own self-checks (the tripwire, the preshader MISMATCH, forwarded-to-CRT) are
        // the next thing to read - they are in this same log, just above.
        logf("crash:   guilt guard: this DLL so far - calls served on the game thread %d (kept) + %d (de-duped away), sync waits %d, Presents seen %d, records queued %d, PRESENT-COUNT frame %d",
             (int)g_rtNCallKept, (int)g_rtNCallSkip, (int)g_rtNSync, (int)g_rtNPresent, (int)g_rtNAsync, (int)g_mkFrames);
    }
    if (code == 0xC00000FDu) { g_clBusy = 0; return EXCEPTION_CONTINUE_SEARCH; }                  // no stack left to walk with
    char ln[1400]; int q = 0, found = 0;
    __try {
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        DWORD lo = c->Esp & ~3u, hi = (DWORD)(ULONG_PTR)tib->StackBase;
        if (hi > lo + 0x6000) hi = lo + 0x6000;                                                  // the nearest 24 KB of stack
        for (DWORD p = lo; p + 4 <= hi && found < 40; p += 4) {
            DWORD v = *(DWORD*)(ULONG_PTR)p;
            bool game = g_textLo && gsIsRet(v), self = g_selfLo && v >= g_selfLo && v < g_selfHi;
            if (!game && !self) continue;
            char s2[160]; symName(v, s2);
            q += wsprintfA(ln + q, " %s", s2); ++found;
            if (q > 1200) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { q += wsprintfA(ln + q, " (stack unreadable)"); }
    ln[q] = 0;
    logf("crash:   return addresses on the stack (nearest first, may include stale ones):%s", ln);
    g_clBusy = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}
static void installCrashLog() {
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_CRASHLOG", env, sizeof(env)) && env[0] == '0') return;
    if (!g_selfLo) { HMODULE hs = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&clHandler, &hs) && hs) {
            IMAGE_NT_HEADERS* nh = (IMAGE_NT_HEADERS*)((BYTE*)hs + ((IMAGE_DOS_HEADER*)hs)->e_lfanew); g_selfLo = (DWORD)(ULONG_PTR)hs; g_selfHi = g_selfLo + nh->OptionalHeader.SizeOfImage; } }
    { static const char* const kDll[] = { "kernel32.dll", "kernelbase.dll" };
      static const char* const kFn[] = { "IsBadReadPtr", "IsBadWritePtr", "IsBadStringPtrA", "IsBadStringPtrW", "IsBadCodePtr", "IsBadHugeReadPtr", "IsBadHugeWritePtr" };
      for (int d = 0; d < 2; ++d) { HMODULE h = GetModuleHandleA(kDll[d]); if (!h) continue;
          for (int f = 0; f < 7 && g_clProbeN < 16; ++f) { FARPROC a = GetProcAddress(h, kFn[f]); if (a) g_clProbe[g_clProbeN++] = (DWORD)(ULONG_PTR)a; } } }
    if (AddVectoredExceptionHandler(1, clHandler)) logf("crashlog: fatal exceptions will be written here with their call chain (nothing is handled or changed).");
}
