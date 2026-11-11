// aotr_w3dlock.inc - v54: the W3D render lock without a system call per lock.
//
// WHAT WAS SLOW (test-35 framediag, big battle): ~9% of the client step's samples were inside ntdll, and the paths name
// the engine's render lock: lock 0x51EEC0 (WaitForSingleObject(mutex, 20000), then a critical section, owner thread
// and count) and unlock 0x5208D0 (... ReleaseMutex). The mutex (0xDD1FD8) is made unnamed and not owned by W3D init
// (0x52506B CreateMutexA(0,0,0)) and locked once by the game thread right after; the game thread then takes it again
// (recursively) around render work 123 places in the code - two kernel calls per pair, hundreds of pairs a frame.
//
// WHAT THIS DOES: the handle is read in exactly five places (checked in the binary: 0x51EEC0, 0x51EF54, 0x520919,
// 0x525071, 0x52528B). The calls there - CreateMutexA, the two waits, the two releases - go to the functions below,
// which keep the same rules in user mode: owned by one thread, recursive for its owner, a wait with a time limit
// returns WAIT_TIMEOUT (0x102) when it runs out, a thread that ends while owning it leaves it "abandoned" (the next
// waiter gets it with WAIT_ABANDONED, as from the kernel), a release by a non-owner fails with ERROR_NOT_OWNER. The
// kernel mutex is still created (the handle stays what the game stored) and is simply never waited on. Only taken when
// the game has not made the mutex yet when the hooks go in (the DLL arrives at process start), so one lock never mixes
// the two. A contended wait sleeps on an event (woken by the release), so it does not poll. Off: W3DLOCK_OFF.
static volatile LONG g_wlOwner = 0, g_wlCount = 0, g_wlWaiters = 0, g_wlOn = 0;
static HANDLE g_wlH = NULL, g_wlEvt = NULL;
static volatile LONG g_wlLocks = 0, g_wlContended = 0, g_wlTimeouts = 0, g_wlAbandoned = 0, g_wlBadRelease = 0, g_wlForeign = 0;
static bool wlThreadGone(DWORD tid) {
    HANDLE t = OpenThread(SYNCHRONIZE, FALSE, tid);
    if (!t) return true;                                          // no such thread any more
    DWORD r = WaitForSingleObject(t, 0); CloseHandle(t);
    return r == WAIT_OBJECT_0;
}
static HANDLE WINAPI wlCreate(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name) {
    HANDLE h = CreateMutexA(sa, initialOwner, name);
    if (h && !initialOwner && !name && !g_wlH && g_wlEvt) { g_wlH = h; g_wlOn = 1; }
    return h;
}
static DWORD WINAPI wlWait(HANDLE h, DWORD ms) {
    if (!g_wlOn || h != g_wlH) { if (h == g_wlH) g_wlForeign++; return WaitForSingleObject(h, ms); }
    LONG me = (LONG)GetCurrentThreadId();
    g_wlLocks++;
    if (g_wlOwner == me) { g_wlCount++; return WAIT_OBJECT_0; }
    if (InterlockedCompareExchange(&g_wlOwner, me, 0) == 0) { g_wlCount = 1; return WAIT_OBJECT_0; }
    InterlockedIncrement(&g_wlContended);
    for (int i = 0; i < 400; ++i) {                               // a short spin: the owner may be about to let go
        _mm_pause();
        if (g_wlOwner == 0 && InterlockedCompareExchange(&g_wlOwner, me, 0) == 0) { g_wlCount = 1; return WAIT_OBJECT_0; }
    }
    DWORD t0 = GetTickCount();
    for (;;) {
        InterlockedIncrement(&g_wlWaiters);
        if (InterlockedCompareExchange(&g_wlOwner, me, 0) == 0) { InterlockedDecrement(&g_wlWaiters); g_wlCount = 1; return WAIT_OBJECT_0; }
        DWORD el = GetTickCount() - t0;
        DWORD left = (ms == INFINITE) ? 50 : (el >= ms ? 0 : ms - el);
        if (left > 50) left = 50;
        DWORD r = left ? WaitForSingleObject(g_wlEvt, left) : WAIT_TIMEOUT;
        InterlockedDecrement(&g_wlWaiters);
        if (InterlockedCompareExchange(&g_wlOwner, me, 0) == 0) { g_wlCount = 1; return WAIT_OBJECT_0; }
        if (r == WAIT_TIMEOUT) {
            LONG o = g_wlOwner;
            if (o && wlThreadGone((DWORD)o) && InterlockedCompareExchange(&g_wlOwner, me, o) == o) {   // the owner ended holding it
                g_wlCount = 1; InterlockedIncrement(&g_wlAbandoned); return WAIT_ABANDONED;
            }
            if (ms != INFINITE && GetTickCount() - t0 >= ms) { InterlockedIncrement(&g_wlTimeouts); return WAIT_TIMEOUT; }
        }
    }
}
static BOOL WINAPI wlRelease(HANDLE h) {
    if (!g_wlOn || h != g_wlH) { if (h == g_wlH) g_wlForeign++; return ReleaseMutex(h); }
    if (g_wlOwner != (LONG)GetCurrentThreadId()) { g_wlBadRelease++; SetLastError(ERROR_NOT_OWNER); return FALSE; }
    if (--g_wlCount == 0) {
        InterlockedExchange(&g_wlOwner, 0);
        if (g_wlWaiters > 0) SetEvent(g_wlEvt);
    }
    return TRUE;
}
static void installW3dLock() {
    struct Site { DWORD at; DWORD iat; void* fn; };
    const Site s[5] = { { 0x0052506B, 0x00BD021C, (void*)&wlCreate }, { 0x0051EECB, 0x00BD0234, (void*)&wlWait }, { 0x0051EF5C, 0x00BD0234, (void*)&wlWait },
                        { 0x00520920, 0x00BD0224, (void*)&wlRelease }, { 0x00525291, 0x00BD0224, (void*)&wlRelease } };
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "W3DLOCK_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("w3dlock: W3DLOCK_OFF is present - the render lock stays a kernel mutex."); return; }
    __try {
        for (int i = 0; i < 5; ++i) {
            const BYTE* q = (const BYTE*)(ULONG_PTR)s[i].at;
            if (q[0] != 0xFF || q[1] != 0x15 || *(const DWORD*)(q + 2) != s[i].iat) { logf("w3dlock: call site %08X differs from the analysed build - off.", s[i].at); return; }
        }
        if (memcmp((void*)0x0051EEC0, "\xA1\xD8\x1F\xDD\x00\x68\x20\x4E\x00\x00\x50", 11) || memcmp((void*)0x0051EF54, "\x8B\x0D\xD8\x1F\xDD\x00", 6) ||
            memcmp((void*)0x00520919, "\x8B\x15\xD8\x1F\xDD\x00\x52", 7) || memcmp((void*)0x00525071, "\xA3\xD8\x1F\xDD\x00", 5) ||
            memcmp((void*)0x0052528B, "\xA1\xD8\x1F\xDD\x00\x50", 6)) { logf("w3dlock: the render lock code differs - off."); return; }
        if (*(volatile DWORD*)0x00DD1FD8) { logf("w3dlock: the render lock mutex already exists (made before the hooks) - it stays a kernel mutex."); return; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    g_wlEvt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_wlEvt) { logf("w3dlock: no event - off."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    if (*(volatile DWORD*)0x00DD1FD8) { resumeAll(fz, nf); logf("w3dlock: the render lock mutex was made while installing - it stays a kernel mutex."); return; }
    for (int i = 0; i < 5; ++i) {                                 // ff 15 <iat>  ->  e8 <rel32> 90
        BYTE* q = (BYTE*)(ULONG_PTR)s[i].at; DWORD old = 0;
        if (VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &old)) {
            q[0] = 0xE8; *(LONG*)(q + 1) = (LONG)((DWORD)(ULONG_PTR)s[i].fn - (s[i].at + 5)); q[5] = 0x90;
            VirtualProtect(q, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), q, 6);
        }
    }
    resumeAll(fz, nf);
    logf("w3dlock: the W3D render lock (mutex 0xDD1FD8) is kept in user mode - owner, recursion, time limit and abandonment as the kernel's; no system call per lock (W3DLOCK_OFF).");
}
static void wlReport() {                                          // 30 s report thread
    static LONG lL = 0, lC = 0;
    if (!g_wlEvt) return;
    LONG l = g_wlLocks, c = g_wlContended;
    if (l == lL) return;
    logf("w3dlock: %s | last 30 s: %d locks (%d waited for another thread) | in all: timeouts %d, abandoned %d, refused releases %d, kernel-path calls %d",
         g_wlOn ? "on" : "not used (mutex not made through the hook)", (int)(l - lL), (int)(c - lC), (int)g_wlTimeouts, (int)g_wlAbandoned, (int)g_wlBadRelease, (int)g_wlForeign);
    lL = l; lC = c;
}
