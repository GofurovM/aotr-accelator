// ---------------------------------------------------------------- the read-out (aotr_overlay.inc)
// A small panel in the corner of the game window: pictures a second, the longest frame of the last
// second, how many frames were long enough to be felt, the processor and memory the game is using,
// and how much of the last second went to the hard disk (a real read or open that took 50 ms or
// more, with the worst one).
//
// [Engine] Overlay = 0 turns the whole thing off. OverlayShow = 0 starts it hidden.
// OverlayKey selects the key (0x76 = F7 by default). OverlayX / OverlayY move it.
//
// It draws with GDI on its own layered, click-through, never-focused window: not one Direct3D call
// is added to the frame and no game state is touched. The only thing it hooks is
// IDirect3DDevice9::Present itself, and only to count it.

#include <tlhelp32.h>                        // CreateToolhelp32Snapshot: the process's thread count

static volatile LONG g_ovOn = 0;             // armed (the device is not made yet)
static volatile LONG g_ovReady = 0;          // the device is hooked, the panel may come up
static volatile LONG g_ovShow = 1;
static volatile LONG g_ovBadge = 0;           // v71: release build - only the status light is painted (aotr_badge.inc)
static volatile LONG g_ovBBW = 0;             // the back buffer's width (the light sits in its top right corner)
static volatile LONG g_ovKey = VK_F7;
static volatile LONG g_ovPx = 8, g_ovPy = 8;
static HWND g_ovGame = NULL;                 // the window the device was made for
static HWND g_ovWnd = NULL;
static HFONT g_ovFont = NULL;
static char g_ovText[1024] = "";
static void* g_ovPresentOrig = NULL;
static DWORD WINAPI ovThread(void*);         // the panel thread (below)
static DWORD WINAPI ovWatchThread(void*);    // it watches the panel thread (below)

// written by Present (game or render thread), read by the panel thread
static volatile LONG g_ovFrames = 0;         // Present calls in the window
static volatile LONG g_ovGapSum = 0;         // the sum of the gaps between them, ms
static volatile LONG g_ovWorst = 0;          // the longest gap in the window, ms
static volatile LONG g_ovLongs = 0;          // gaps of 100 ms or more in the window
static LONG64 g_ovPrev = 0;
static LONG g_ovKeyDown = 0;

// ---------------------------------------------------------------- this run, kept and remembered
// The panel thread takes one reading a second, so a session leaves a record even when nothing is ever drawn:
// what the pictures were, how long the worst frame was, what the machine was doing at that moment, and which
// switches were on. It writes aotr_report.txt every minute - with the worst moments, each one attributed to
// the disk, to waiting on the renderer, or to the game's own single thread - and remembers the best sustained
// reading in aotr_best.txt so the next run can be judged against it. This is the measuring instrument the
// escalation is built on: one switch at a time, and the report says whether it helped.
#define OV_STALLS 12
struct OvStall { LONG ms; LONG at; int cpu; LONG slowMs; LONG syf; LONG qpf; char top[24]; LONG busyMs; LONG tpMs; LONG pf; LONG hpf; LONG files; };
static OvStall g_ovStall[OV_STALLS]; static int g_ovNStall = 0;
static LONG g_ovRepSec = 0; static LONG64 g_ovRepFrames = 0;
static LONG g_ovRepCpuSum = 0, g_ovRepCpuN = 0;
// v31: the processor over the seconds that had 10 pictures or more. test-11's "cpu 0.76 cores on average" was taken over
// every second of the run, four minutes without a single picture (0.07 cores) included; while the game was being played
// it was 1.11. Both are given now, and how much of the playing time the game's own thread ran.
static LONG g_ovPlayCpuSum = 0, g_ovPlayCpuN = 0; static LONG64 g_ovPlayGameMs = 0, g_ovPlaySpan = 0;
static LONG g_ovSt100 = 0, g_ovSt250 = 0, g_ovSt500 = 0, g_ovBurst = 0, g_ovPeak10 = 0;
static LONG g_ovStCls[4] = {0, 0, 0, 0};     // v30 step 4: 0 the game's thread busy, 1 the disk, 2 the game's thread waiting, 3 mixed
static LONG g_ovRing[5] = {0, 0, 0, 0, 0}; static int g_ovRingI = 0, g_ovRingN = 0;
static LONG g_ovBest5 = 0, g_ovBest5At = 0, g_ovRunWorst = 0, g_ovRepTick = 0;
static LONG g_ovRepWant = 1;                 // [Engine] Report
static LONG g_ovWantWin = 1;                 // [Engine] Overlay
static LONG g_ovBestStored = 0, g_ovBestStoredSecs = 0;
static char g_ovBestStoredWhen[48] = "", g_ovBestSw[256] = "";
// v30 step 3: every second that had pictures, by its whole fps (0..300, higher counts as 300) - the session summary
// line gives the median and the slowest tenth from it, so two sessions (DXVK / Windows D3D9, GeForce / Intel) can be
// put side by side with the same numbers.
#define OV_FPSH 301
static LONG g_ovFpsHist[OV_FPSH]; static LONG g_ovFpsSecs = 0; static LONG64 g_ovFpsFrames = 0;

// ---------------------------------------------------------------- the instrument's own health
// The panel thread IS the measuring instrument: if it wedges, or a counter comes back impossible, that
// has to be visible in the log instead of a frozen screen and numbers that cannot be true. In the 00:10
// run the panel stopped after 23 seconds and its readings were garbage - worst frame 1848557232 ms,
// disk 1257554917 ms - none of which its own clamps can produce, so somebody else's write had reached
// this memory. These are the guard rails: every buffer the report builds lives here instead of on the
// thread's stack, every line goes through ovAppend which only writes into what is left, every counter
// is range-checked with a tripwire that names the addresses, the loop body runs under a __try, a
// heartbeat says every ten seconds that the thread is alive, and a second thread watches the first.
static char g_ovB[4096];                          // the report is built here, never on the stack
static char g_ovSw[320], g_ovSecBuf[800], g_ovRp[MAX_PATH], g_ovIniBuf[MAX_PATH];
static char g_ovCat[1200];                        // one scratch line (wsprintf caps at 1024 characters)
static volatile LONG g_ovPh = 0;                  // the phase the panel thread is inside right now
static volatile DWORD g_ovPhAt = 0;               // the tick it entered that phase
static volatile DWORD g_ovIter = 0;               // finished iterations (the heartbeat)
static volatile LONG g_ovCorruptN = 0;            // a reading came back impossible this many times
static volatile LONG g_ovExc = 0;                 // exceptions caught inside the loop
static volatile LONG g_ovRepWrites = 0, g_ovRepBytes = 0;

static const char* ovPhaseName(LONG p) {
    switch (p) {
    case 0:  return "between iterations (the loop itself)";
    case 1:  return "waiting for the device";
    case 2:  return "Sleep(1000)";
    case 3:  return "hooking Present";
    case 4:  return "reading the counters";
    case 5:  return "the processor reading";
    case 6:  return "the memory reading";
    case 7:  return "the thread snapshot";
    case 8:  return "the renderer counters";
    case 9:  return "the stall stamp";
    case 10: return "the panel text";
    case 11: return "the session report";
    case 12: return "sizing the panel";
    case 13: return "finding the game window";
    case 14: return "placing the panel";
    case 15: return "showing the panel";
    case 16: return "the message pump";
    }
    return "?";
}

static bool ovNameHas(const char* s, const char* w) {
    for (; *s; ++s) { const char* a = s; const char* b = w; while (*a && *b && *a == *b) { ++a; ++b; } if (!*b) return true; }
    return false;
}
static LONG ovReadInt(const char* s) {
    LONG v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '-') { neg = 1; ++s; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return neg ? -v : v;
}
static void ovCopyLine(char* d, int cap, const char* s) {
    int i = 0;
    while (s[i] && s[i] != '\r' && s[i] != '\n' && i < cap - 1) { d[i] = s[i]; ++i; }
    d[i] = 0;
}

// Every report line goes through here. wsprintfA itself caps at 1024 characters, and this copies only
// into what is actually left of the buffer - so a line that ever grew too long would cost the rest of
// the report, never the thread that is writing it. (The report block used to build into a 4096-byte
// buffer on the stack with unguarded n += wsprintfA, which in a worst case could pass the end of the
// array and smash the loop's own variables - exactly the kind of garbage the 00:10 run showed.)
static char g_ovFmt[1100];
static int ovAppend(char* b, int cap, int n, const char* fmt, ...) {
    if (n < 0) n = 0;
    if (n > cap - 1) return cap - 1;
    va_list ap; va_start(ap, fmt);
    int k = wvsprintfA(g_ovFmt, fmt, ap);            // never more than 1024 characters
    va_end(ap);
    if (k < 0) k = 0;
    if (k > 1024) k = 1024;
    int room = cap - 1 - n;
    if (k > room) k = room;
    memcpy(b + n, g_ovFmt, (size_t)k);
    b[n + k] = 0;
    return n + k;
}

// ---------------------------------------------------------------- the in-frame read-out (OverlayInFrame)
// The panel above is a window, and no window of ours can be drawn over a fullscreen game - the display
// owns the screen there. This read-out is painted into the frame itself with IDirect3DDevice9::Clear:
// rectangles only, no shader, no texture, no vertex buffer, no stream source, no render state, not one
// thing the game's own drawing depends on. A digit is seven rectangles, and every rectangle of one
// colour goes in a single Clear, so the whole read-out is three Clear calls a frame.
//
// It is drawn from Present, on the game thread, before the frame's Present is handed to the render
// thread - so it is queued behind that frame's own work and is executed before the picture is shown.
static volatile LONG g_ovIfWant = 0;      // [Engine] OverlayInFrame
static volatile LONG g_ovIfDrawing = 0;   // it has painted a frame (the window read-out steps aside)
static volatile LONG g_ovIfLast = 0;      // tick of the last frame it painted: the window comes back by itself if it stops
static volatile LONG g_ovIfLogged = 0;
static volatile LONG g_ovIfDead = 0;      // it stopped itself after a fault
static LONG g_ovIfOk = 0;                 // 0 not checked yet, 1 the target is the back buffer, -1 it is not
static LONG g_ovIfBadLogged = 0;
static LONG g_ovIfRTLogged = 0;
static LONG g_ovIfSecs = 0;
static LONG g_ovIfNextChk = 0;
static void* g_ovVt = NULL;               // the device's vtable once a real device has been seen
static DWORD g_ovIfRT = 0, g_ovIfBB = 0;
static LONG g_ovIfFps10 = 0, g_ovIfWorst = 0, g_ovIfLongs = 0;
static LONG64 g_ovIfT0 = 0; static LONG g_ovIfN = 0, g_ovIfWorstMs = 0, g_ovIfLongMs = 0;
static void ovInFrameDraw(void* dev);
static volatile LONG g_ovIfFirst = 0;           // tick of the first picture painted on (the banner's 30 s count from here)
static volatile LONG g_ovIfBannerUntil = 0;     // F7 brought the panel back: the banner again until this tick
static volatile LONG g_ovIfMsgUntil = 0, g_ovIfMsgKind = 0;   // 1 "SHOWN", 2 "HIDDEN" until this tick
static volatile LONG g_ovKeyPresses = 0;        // F7 presses (the panel thread logs them - never a file write in Present)
static void* g_ovDev = NULL;                 // the device whose Present is counted

typedef DWORD (__stdcall* OvPresentF)(void*, DWORD, DWORD, DWORD, DWORD);
// v29 (defect 1): the game presents through IDirect3DDevice9::Present on some sessions and through
// IDirect3DSwapChain9::Present on others - with DXVK it is the swap chain. This overlay hooked the device's Present
// only, so in a session that presents through the swap chain not one frame was ever counted: the panel, the report
// and every stall number read zero for a whole two-hour run. Both entries now call this same counter, and a frame
// is counted once: the two are never both a frame boundary for the same picture (the engine calls one of them), but a
// device Present that forwards to its swap chain would arrive twice, so a present within the same millisecond as the
// last one is not counted again.
static volatile LONG g_ovLastCountMs = -1;
static volatile LONG g_ovPresentsSeen = 0;   // every counted picture of the session (the panel thread watches it move)
// v30 step 4: how long the game's own thread really ran inside the longest frame of each second (100 ms and more),
// from its cycle count at the two Presents around that frame (QueryThreadCycleTime: one cheap call per picture, only
// read). A long frame the thread ran through was its own work; one it mostly did not run in was a wait (the driver or
// the GPU, memory being paged in, a lock). The panel thread turns cycles into ms with the clock rate it measures.
static volatile DWORD g_ovGameTid = 0;        // the thread that presents - the game's own thread
static ULONG64 g_ovPrevCyc = 0;               // its cycle count at the last counted picture (0: not known)
static volatile LONG g_ovLongMs = 0;          // the longest frame of 100 ms+ since the panel last looked (0: none)
static volatile LONG64 g_ovLongCyc = -1;      // the game thread's cycles in that frame (-1: not known)
static __forceinline void ovCountFrame(void) {
    LONG64 now = qpcMs();
    // One picture, one frame. The game presents through the device's Present on some sessions and through the swap
    // chain's Present on others (DXVK uses the chain); when the device's Present forwards into its chain, both of our
    // entries run for the same picture. A real frame gap is never below 1 ms - the engine cannot present two pictures
    // inside a millisecond, and its fastest is ~7 ms at 140 fps - while a forwarding pair is microseconds apart, so
    // a present within the same millisecond as the last one is the same picture (counted by whichever hook ran first).
    if ((LONG)now == InterlockedExchange(&g_ovLastCountMs, (LONG)now)) return;
    ULONG64 cyc = 0; DWORD tid = GetCurrentThreadId();
    if (!g_ovGameTid) g_ovGameTid = tid;
    bool cycOk = tid == g_ovGameTid && QueryThreadCycleTime(GetCurrentThread(), &cyc);
    if (g_ovPrev) {
        LONG gap = (LONG)(now - g_ovPrev);
        if (gap >= 100 && gap < 60000 && gap > g_ovLongMs) { g_ovLongCyc = (cycOk && g_ovPrevCyc) ? (LONG64)(cyc - g_ovPrevCyc) : -1; g_ovLongMs = gap; }
        if (gap > 0 && gap < 60000) {
            InterlockedIncrement(&g_ovFrames);
            InterlockedExchangeAdd(&g_ovGapSum, gap);
            if (gap > g_ovWorst) g_ovWorst = gap;
            if (gap >= 100) InterlockedIncrement(&g_ovLongs);
            ++g_ovIfN;                                        // the same window the read-out shows, but kept always:
            if (gap > g_ovIfWorstMs) g_ovIfWorstMs = gap;      // the numbers go to the log whether or not anything
            if (gap >= 100) ++g_ovIfLongMs;                    // is ever drawn on a screen
        }
    }
    g_ovPrev = now;
    g_ovPrevCyc = cycOk ? cyc : 0;
    InterlockedIncrement(&g_ovPresentsSeen);
    if (!g_ovIfT0) g_ovIfT0 = now;
    {
        LONG span = (LONG)(now - g_ovIfT0);
        if (span >= 1000) {                                    // (v30: the log line for these numbers is written by the
            g_ovIfFps10 = (LONG)((LONG64)g_ovIfN * 10000 / span);   //  panel thread - never a file write inside Present)
            g_ovIfWorst = g_ovIfWorstMs; g_ovIfLongs = g_ovIfLongMs;
            g_ovIfT0 = now; g_ovIfN = 0; g_ovIfWorstMs = 0; g_ovIfLongMs = 0;
        }
    }
}
// v31: F7 always gets an answer on the screen - "SHOWN" or "HIDDEN" for 3 s - and showing the panel again brings
// the banner back for 5 s. The press is counted here and written to the log by the panel thread.
static __forceinline void ovKeyAndDraw(void* d) {
    LONG k = g_ovKey;
    if (k && (GetAsyncKeyState(k) & 0x8000)) {
        if (!g_ovKeyDown) {
            g_ovKeyDown = 1; g_ovShow = g_ovShow ? 0 : 1;
            LONG now = (LONG)GetTickCount();
            if (!g_ovBadge) { g_ovIfMsgKind = g_ovShow ? 1 : 2; g_ovIfMsgUntil = now + 3000;
                              if (g_ovShow) g_ovIfBannerUntil = now + 5000; }
            InterlockedIncrement(&g_ovKeyPresses);
        }
    }
    else g_ovKeyDown = 0;
    if (g_ovIfWant && d) ovInFrameDraw(d);
}
static __forceinline void ovAtPresent(void* d) {
    ovCountFrame();
    ovKeyAndDraw(d);
}
// A device Present that hands the picture on to its swap chain's Present (DXVK does) reaches our swap chain hook too;
// while the device's Present is running on a thread, that thread's swap chain Present is the same picture.
static volatile DWORD g_ovInDevPresent = 0;
static DWORD __stdcall ovPresent(void* d, DWORD a0, DWORD a1, DWORD a2, DWORD a3) {
    ovAtPresent(d);
    g_ovInDevPresent = GetCurrentThreadId();
    DWORD r = ((OvPresentF)g_ovPresentOrig)(d, a0, a1, a2, a3);
    g_ovInDevPresent = 0;
    return r;
}
// ---- v30 (test-10b defect: the panel counted nothing): the device's Present taken in its CODE, not in its table.
// Windows' own d3d9.dll (Windows 10/11) gives every device a private copy of its method table inside the device object
// (+0x2F5C here) and writes that copy back from its originals when the game records a state block - measured with
// work\probe11\vtprobe.exe on this machine: a Present hook placed in the table counted 7 of 40 Presents, and the slot
// was back to d3d9's own after the first BeginStateBlock. The engine records state blocks all the time, so in every
// session without DXVK (test-9 20:56/20:59/21:02, test-10 22:02, test-10b) the device hook died within the first
// frames and not one picture was counted. d3d9.dll's functions start with the hot-patch prologue (mov edi,edi with
// five padding bytes before it) precisely so that they can be taken this way: a jump in the padding, a two-byte
// jump over the prologue, and the original continues at +2. A table copy can be rewritten as often as d3d9 likes -
// the code it points to is the same. DXVK's d3d9.dll has one static table per class and no such prologue: there the
// table hook stays in place (test-8 and test-9 counted every frame through it) and is used as before.
static void* g_ovHotFn = NULL;               // the Present code taken this way, or NULL
static void* g_ovHotCont = NULL;             // where the original continues (the function + 2)
static DWORD __stdcall ovHotPresent(void* d, DWORD a0, DWORD a1, DWORD a2, DWORD a3) {
    ovAtPresent(d);
    g_ovInDevPresent = GetCurrentThreadId();
    DWORD r = ((OvPresentF)g_ovHotCont)(d, a0, a1, a2, a3);
    g_ovInDevPresent = 0;
    return r;
}
static bool ovHotPatch(void* fn, void* hook, void** cont) {
    BYTE* p = (BYTE*)fn;
    if (!p || ((ULONG_PTR)p & 1)) return false;
    __try {
        if (p[0] != 0x8B || p[1] != 0xFF) return false;                       // mov edi, edi
        for (int i = 1; i <= 5; ++i) if (p[-i] != 0xCC && p[-i] != 0x90) return false;
        DWORD op = 0;
        if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &op)) return false;
        *cont = p + 2;
        p[-5] = 0xE9; *(DWORD*)(p - 4) = (DWORD)((BYTE*)hook - p);          // jmp hook (relative to the end of the jump, = p)
        InterlockedExchange16((volatile SHORT*)p, (SHORT)0xF9EB);             // EB F9: jmp to p - 5, one atomic store
        VirtualProtect(p - 5, 7, op, &op);
        FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
// IDirect3DSwapChain9::Present (slot 3): the same boundary, when the game presents through the swap chain.
static void* g_ovScPresentOrig = NULL;
static DWORD __stdcall ovScPresent(void* d, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4) {
    if (g_ovInDevPresent != GetCurrentThreadId()) { ovCountFrame(); ovKeyAndDraw(g_ovDev); }   // v31: this path paints too (DXVK may present here)
    return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD))g_ovScPresentOrig)(d, a0, a1, a2, a3, a4);
}

static volatile LONG g_ovDevSeen = 0;         // a real device has been seen (the panel thread may come up)
static volatile LONG g_ovPatchLock = 0;
static void ovTryPatchPresent();
static void ovHookPresent(void* dev) {
    if (!dev || g_ovPresentOrig) return;
    if (g_ovDev) { ovTryPatchPresent(); return; }                     // a device is already known: it may be time now
    __try {
        void** vt = *(void***)dev;
        // The texture workers hand D3DX their own stand-in device for their format questions (a plain struct, not a
        // COM object), and since we now sit in front of the render thread those calls reach us too. A real device's
        // first vtable entry and its Present are both code inside an executable image; the stand-in's are whatever
        // its first fields happen to hold. Note it is the ENTRIES that must be code: the vtable itself is a table
        // of pointers in plain data, so testing the table was always false and quietly threw every device away -
        // which is why the read-out never came up.
        if ((ULONG_PTR)vt < 0x10000) return;
        if (!rtIsCode((DWORD)(ULONG_PTR)vt[0]) || !rtIsCode((DWORD)(ULONG_PTR)vt[17])) {
            if (!g_ovIfBadLogged) { InterlockedExchange(&g_ovIfBadLogged, 1);
                logf("overlay: device %08X is not a COM object (vtable %08X, entry 0 %08X, Present %08X) - the texture workers' stand-in. Nothing read through it.",
                     (DWORD)(ULONG_PTR)dev, (DWORD)(ULONG_PTR)vt, (DWORD)(ULONG_PTR)vt[0], (DWORD)(ULONG_PTR)vt[17]); }
            return;
        }
        g_ovVt = vt; g_ovDev = dev;
        logf("overlay: device %08X seen (vtable %08X) - reading the frame counter from its Present.", (DWORD)(ULONG_PTR)dev, (DWORD)(ULONG_PTR)vt);
        InterlockedExchange(&g_ovDevSeen, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("overlay: the device could not be read - no read-out."); return; }
    ovTryPatchPresent();
}
// The accelerator's render thread hooks this very table from inside its first effect call, and it keeps the
// genuine Present as its original. It runs AFTER this overlay - the overlay is reached first, and only then does
// the render thread install - so taking the device's Present here and now would be wrong: the render thread would
// adopt OUR hook as the device's own Present and then call it from its worker thread. Our counting and our drawing
// would run on the wrong thread, inside the worker's own call executor, and the game dies there (that is exactly
// the crash of the 23:39 run). Waiting for the render thread first puts this hook where it was designed to be:
// the outermost one, called by the game thread, with the frame's own work already queued behind it.
// If the render thread gave up (tried and not installed) or is switched off, its hooks pass straight through and
// the table still holds the genuine Present, so hooking then is just as correct.
static void ovTryPatchPresent() {
    if (g_ovPresentOrig || !g_ovVt) return;
    if (!g_rtInstalled && !g_rtTried && g_rtWantOn != 0) return;      // the render thread has not decided yet
    if (InterlockedCompareExchange(&g_ovPatchLock, 1, 0) != 0) return; // one writer only: a second one could take
    __try {                                                            // our own hook as the "original" and recurse
        void** vt = (void**)g_ovVt;
        DWORD f = (DWORD)(ULONG_PTR)vt[17];
        if (f == (DWORD)(ULONG_PTR)&ovPresent) { InterlockedExchange(&g_ovReady, 1); return; }
        if (!rtIsCode(f)) { if (!g_ovIfBadLogged) { InterlockedExchange(&g_ovIfBadLogged, 1);
            logf("overlay: Present %08X is not executable code - no read-out.", f); } return; }
        { char own[64]; rtOwner(f, own, sizeof(own));
          bool tableInObject = (ULONG_PTR)vt > (ULONG_PTR)g_ovDev && (ULONG_PTR)vt < (ULONG_PTR)g_ovDev + 0x10000;
          if (ovHotPatch((void*)(ULONG_PTR)f, (void*)&ovHotPresent, &g_ovHotCont)) {
              g_ovHotFn = (void*)(ULONG_PTR)f; g_ovPresentOrig = (void*)(ULONG_PTR)f;
              InterlockedExchange(&g_ovReady, 1);
              logf("overlay: counting at Present in its code (%08X in %s, hot-patch entry) - the device's method table is %s, so a table hook would not last; this one does whatever d3d9 does to its tables.",
                   f, own, tableInObject ? "a private copy inside the device object (Windows' own d3d9 rewrites it)" : "where it is");
          } else {
              DWORD op = 0;
              if (!VirtualProtect(&vt[17], sizeof(void*), PAGE_READWRITE, &op)) { logf("overlay: VirtualProtect failed - no read-out."); InterlockedExchange(&g_ovPatchLock, 0); return; }
              g_ovPresentOrig = vt[17];
              vt[17] = (void*)(ULONG_PTR)&ovPresent;
              VirtualProtect(&vt[17], sizeof(void*), op, &op);
              InterlockedExchange(&g_ovReady, 1);
              logf("overlay: counting at Present on device %08X through its method table (vtable %08X, Present %08X in %s)%s.", (DWORD)(ULONG_PTR)g_ovDev, (DWORD)(ULONG_PTR)vt, f, own,
                   tableInObject ? " - WARNING: this table lives inside the device object and its code has no hot-patch entry; if the counter stays at 0 the panel says so" : "");
          } }
        // v29 (defect 1): the swap chain's own Present, because a session that presents through it (DXVK does) left
        // every frame uncounted. GetSwapChain(0) is the direct3D device's implicit chain, slot 3 is its Present.
        if (!g_ovScPresentOrig) {
            void* sc = NULL;
            __try { ((DWORD(__stdcall*)(void*, DWORD, void**))vt[14])(g_ovDev, 0, &sc); }   // IDirect3DDevice9::GetSwapChain
            __except (EXCEPTION_EXECUTE_HANDLER) { sc = NULL; }
            if (sc) {
                __try {
                    void** svt = *(void***)sc;
                    DWORD sf = (DWORD)(ULONG_PTR)svt[3];
                    if (rtIsCode(sf) && sf != (DWORD)(ULONG_PTR)&ovScPresent) {
                        DWORD op2 = 0;
                        if (VirtualProtect(&svt[3], sizeof(void*), PAGE_READWRITE, &op2)) {
                            g_ovScPresentOrig = svt[3];
                            svt[3] = (void*)(ULONG_PTR)&ovScPresent;
                            VirtualProtect(&svt[3], sizeof(void*), op2, &op2);
                            logf("overlay: counting at SwapChain::Present too (chain %08X, vtable %08X) - a frame is counted once whichever way the game presents.",
                                 (DWORD)(ULONG_PTR)sc, (DWORD)(ULONG_PTR)svt);
                        }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { logf("overlay: the swap chain could not be read - device Present is still counted."); }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("overlay: the Present hook faulted - no read-out."); }
    InterlockedExchange(&g_ovPatchLock, 0);
}

// ---------------------------------------------------------------- which Direct3D 9 and which graphics chip (v30)
// The question "is DXVK needed?" is decided per session from what is actually loaded, not from what a launcher was
// told: d3d9.dll from the Windows folder is Windows' own Direct3D 9, any other d3d9.dll is DXVK (the AOTR launcher
// copies it into the game folder). The graphics chip is told by its driver's user-mode part: NVIDIA's Direct3D 9
// driver (nvd3dum / nvldumd) or Vulkan driver (nvoglv32), Intel's (igdumdim32 / igvk32), AMD's (aticfx32 / amdvlk32).
// On this laptop Windows' own Direct3D 9 lists ONE adapter - the Intel UHD 630 that drives the screen - unless the
// game has a "high performance" GPU preference (measured with work\probe11\adapters.exe; aotr_gpupref.inc sets it).
static char g_ovRenderer[96] = "?";             // short form for the panel: "DXVK / NVIDIA", "Windows D3D9 / Intel"
static volatile LONG g_ovRendererKnown = 0;
static volatile LONG g_ovGpu = 0;               // 1 NVIDIA, 2 Intel, 3 AMD
static volatile LONG g_ovDxvk = -1;             // 1 DXVK, 0 Windows' own d3d9, -1 not known yet
// v30 step 2: did the GeForce preference take effect in THIS start? Told from what draws (the driver that is loaded) and
// from aotr_gpupref's state - the value was there before this DLL ran (1), was written by it during this start (2), or
// could not be set (0). A value written during the start may come after the game's Direct3D object: then this start
// stays on the Intel chip and the next one uses the GeForce - said so, in the panel too, instead of a bare "Intel".
static char g_ovGpuNote[96] = "GPU: not known yet";
static void ovGpuVerdict() {
    const char* note; const char* line;
    LONG st = g_gpuState;
    if (g_ovDxvk == 1) {
        if (g_ovGpu == 1) { note = "GeForce: chosen by DXVK"; line = "DXVK draws on the GeForce (it skips the Intel chip by itself); the Windows GPU preference does not matter for it."; }
        else { note = "DXVK GeForce'da EMAS - kutilmagan"; line = "DXVK draws on a chip that is not the NVIDIA one - not expected (DXVK skips the Intel chip); its own log rotwk\\game.dat_d3d9.log says why."; }
    } else if (g_ovGpu == 1) {
        if (st == 2) { note = "GeForce: sozlama shu safar kuchga kirdi"; line = "Windows' own Direct3D 9 draws on the GeForce - the preference written during this start was in time."; }
        else if (st == 1) { note = "GeForce: GPU sozlamasi kuchda"; line = "Windows' own Direct3D 9 draws on the GeForce - the GPU preference that was there before the start is in effect."; }
        else { note = "GeForce (chosen by Windows)"; line = "Windows' own Direct3D 9 draws on the GeForce without our preference."; }
    } else {
        if (st == 2) { note = "Intel! GeForce from the NEXT start - restart the game"; line = "Windows' own Direct3D 9 draws on the Intel chip: the GPU preference was written during this start, after Direct3D had chosen its chip - it takes effect from the NEXT start. Close the game and start it again."; }
        else if (st == 1) { note = "Intel! sozlama bor, Windows bermadi"; line = "Windows' own Direct3D 9 draws on the Intel chip although game.dat had the high-performance preference before the start - Windows did not apply it (check Settings > System > Display > Graphics for game.dat, or turn DXVK on)."; }
        else { note = "Intel! GPU sozlamasi yozilmadi"; line = "Windows' own Direct3D 9 draws on the Intel chip and the GPU preference could not be written - turn DXVK on, or set game.dat to High performance in Settings > Graphics."; }
    }
    lstrcpynA(g_ovGpuNote, note, sizeof(g_ovGpuNote));
    logf("gpu: verdict - %s (key %s, preference state %d: %s%s)", line, g_gpuKey[0] ? g_gpuKey : "?", (int)st,
         st == 2 ? "written by this DLL during this start" : (st == 1 ? "there before this DLL ran" : "not set"),
         (st == 2 && g_gpuDrvBefore) ? ", a Direct3D 9 driver was already loaded at that moment" : "");
}
static void ovDetectRenderer() {
    if (g_ovRendererKnown) return;
    HMODULE d9 = GetModuleHandleA("d3d9.dll");
    if (!d9) return;
    char path[MAX_PATH]; path[0] = 0; GetModuleFileNameA(d9, path, MAX_PATH);
    char sys[MAX_PATH]; sys[0] = 0;
    UINT ns = GetSystemWow64DirectoryA(sys, MAX_PATH); if (!ns) ns = GetSystemDirectoryA(sys, MAX_PATH);
    char sys2[MAX_PATH]; GetSystemDirectoryA(sys2, MAX_PATH);
    bool winOwn = (ns && !_strnicmp(path, sys, ns) && path[ns] == '\\') || (!_strnicmp(path, sys2, lstrlenA(sys2)) && path[lstrlenA(sys2)] == '\\');
    int gpu = 0; const char* drv = "";
    if (GetModuleHandleA("nvd3dum.dll"))      { gpu = 1; drv = "nvd3dum.dll"; }
    else if (GetModuleHandleA("nvldumd.dll")) { gpu = 1; drv = "nvldumd.dll"; }
    else if (GetModuleHandleA("nvoglv32.dll")){ gpu = 1; drv = "nvoglv32.dll"; }
    else if (GetModuleHandleA("igdumdim32.dll")) { gpu = 2; drv = "igdumdim32.dll"; }
    else if (GetModuleHandleA("igvk32.dll"))     { gpu = 2; drv = "igvk32.dll"; }
    else if (GetModuleHandleA("aticfx32.dll"))   { gpu = 3; drv = "aticfx32.dll"; }
    else if (GetModuleHandleA("amdvlk32.dll"))   { gpu = 3; drv = "amdvlk32.dll"; }
    if (!gpu) return;                                                         // the driver is not in yet: ask again next second
    g_ovDxvk = winOwn ? 0 : 1; g_ovGpu = gpu;
    static const char* kGpu[4] = { "?", "NVIDIA", "Intel", "AMD" };
    wsprintfA(g_ovRenderer, "%s / %s", winOwn ? "Windows D3D9" : "DXVK", kGpu[gpu]);
    InterlockedExchange(&g_ovRendererKnown, 1);
    logf("renderer: %s Direct3D 9 (%s) drawing on the %s graphics chip (its driver %s is loaded).%s",
         winOwn ? "Windows' own" : "DXVK", path, kGpu[gpu], drv,
         gpu == 2 ? " WARNING: this is the integrated Intel chip, not the GeForce - see the gpu: verdict line." : "");
    ovGpuVerdict();
}

// ---------------------------------------------------------------- the session summary (v30 step 3)
// One line that answers "which path drew, and how well": the d3d9 (DXVK / Windows D3D9) and the chip, the GPU
// preference state (aotr_gpupref), the fps over the seconds that had pictures (average, median, the slowest tenth,
// the best five seconds), the long frames and their causes, the processor. "session: so far" is written every minute
// by the panel thread; "session: END" once more when the game closes (DllMain, process exit). Read-only.
static int ovFpsAt(int pct) {                               // the fps that pct % of the seconds with pictures were at or under
    LONG n = g_ovFpsSecs; if (n <= 0) return 0;
    LONG want = (LONG)(((LONG64)n * pct + 99) / 100); if (want < 1) want = 1;
    LONG c = 0;
    for (int i = 0; i < OV_FPSH; ++i) { c += g_ovFpsHist[i]; if (c >= want) return i; }
    return OV_FPSH - 1;
}
static void ovSessionLine(char* out, const char* tag) {     // out: at least 1100 bytes (logf-sized)
    LONG secs = g_ovFpsSecs; LONG64 fr = g_ovFpsFrames;
    LONG avg10 = secs ? (LONG)(fr * 10 / secs) : 0;
    int cpu = g_ovRepCpuN ? (int)(g_ovRepCpuSum / g_ovRepCpuN) : 0;
    int pcpu = g_ovPlayCpuN ? (int)(g_ovPlayCpuSum / g_ovPlayCpuN) : 0;
    LONG st = g_gpuState;
    wsprintfA(out, "session: %s - %s, preference state %d (%s) | %d:%02d measured, %d s with pictures | fps average %d.%d, median %d, slowest tenth of the seconds at or under %d, best 5 s %d.%d | frames of 100 ms+ %d (250+ %d, 500+ %d), longest %d ms, causes: disk %d, game thread busy %d, waiting %d, mixed %d | cpu %d.%02d cores on average, %d.%02d while playing (seconds with 10+ pictures; the game's thread ran %d%% of that time) | ground patch textures %d on %d threads | files read ahead %d | pictures counted %d (%s)",
              tag, g_ovRendererKnown ? g_ovRenderer : "renderer not known yet", (int)st,
              st == 2 ? "written during this start" : (st == 1 ? "there before the start" : "not set"),
              (int)(g_ovRepSec / 60), (int)(g_ovRepSec % 60), (int)secs,
              (int)(avg10 / 10), (int)(avg10 % 10), ovFpsAt(50), ovFpsAt(10), (int)(g_ovBest5 / 10), (int)(g_ovBest5 % 10),
              (int)g_ovSt100, (int)g_ovSt250, (int)g_ovSt500, (int)g_ovRunWorst, (int)g_ovStCls[1], (int)g_ovStCls[0], (int)g_ovStCls[2], (int)g_ovStCls[3],
              cpu / 100, cpu % 100, pcpu / 100, pcpu % 100, g_ovPlaySpan ? (int)(g_ovPlayGameMs * 100 / g_ovPlaySpan) : -1, (int)g_tpNTex, g_tpOn ? (int)g_tpHelpers + 1 : 1, (int)g_fwNWarm,
              (int)g_ovPresentsSeen, !g_ovReady ? "Present not hooked" : (g_ovHotFn ? "Present hooked in its code" : "Present hooked in its method table"));
}
// At process exit every other thread is already gone, and one of them may have died holding the log's lock: so the
// lock is only tried, never waited for, and the line is written either way (nobody else is left to write).
static void ovSessionEnd() {
    if (g_ovRepSec <= 0) return;                            // the panel never ran: nothing was measured
    char line[1200];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = wsprintfA(line, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    ovSessionLine(line + n, "END (the game is closing)");
    n += lstrlenA(line + n); line[n++] = '\r'; line[n++] = '\n';
    BOOL locked = TryEnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileA(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, line, (DWORD)n, &w, NULL); CloseHandle(h); }
    if (locked) LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------- the in-frame read-out (v31: text, painted into the picture)
// test-11: the player could not tell whether the DLL was running. The read-out was a window, and the game runs in
// exclusive fullscreen (Options.ini "Resolution = 1920 1080", DXVK's log "Windowed: false"): no window of ours is ever
// drawn over that. So v31 paints the read-out into the game's own picture, right before it is shown:
//   - for the first 30 s of pictures (and 5 s after F7 brings it back) a banner: "AOTR V84 BUILD 134 RUNNING" and
//     which Direct3D 9 draws on which chip ("DXVK / NVIDIA", "WINDOWS D3D9 / NVIDIA");
//   - always, while shown, one small line: fps, the longest frame of the last second, the long frames so far;
//   - on F7 for 3 s: "AOTR PANEL SHOWN (F7)" or "AOTR PANEL HIDDEN (F7)", so a press always has an answer.
// It is drawn with IDirect3DDevice9::Clear and nothing else: rectangles of one colour (a 5x7 pixel font, each glyph a
// few rectangles), no shader, texture, buffer or render state - not one thing the game's own drawing depends on is set.
// Clear writes only inside its rectangles, so the rest of the picture is untouched (work\ish4\iftest.exe compares every
// pixel outside the panel's box before and after, on Windows' d3d9 and on DXVK). Clear is clipped by the viewport the
// game left set; it is never changed. The rectangles are rebuilt only when the text changes (once a second).
#define OV_MAXR 1600
struct OvRect { LONG x1, y1, x2, y2; };                  // laid out exactly like D3DRECT
struct OvRects { OvRect r[OV_MAXR]; DWORD n; };
static void ovAdd(OvRects* b, LONG x1, LONG y1, LONG x2, LONG y2) {
    if (b->n >= OV_MAXR || x2 <= x1 || y2 <= y1) return;
    OvRect* p = &b->r[b->n++]; p->x1 = x1; p->y1 = y1; p->x2 = x2; p->y2 = y2;
}
// 5x7 glyphs: the character, then seven rows, bit 4 = the leftmost column
static const BYTE kOvFont[][8] = {
    {'0',0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {'1',0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {'2',0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {'3',0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, {'4',0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {'5',0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {'6',0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {'7',0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {'8',0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {'9',0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {'A',0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {'B',0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {'C',0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {'D',0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {'E',0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {'F',0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {'G',0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {'H',0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {'I',0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {'J',0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {'K',0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {'L',0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {'M',0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {'N',0x11,0x11,0x19,0x15,0x13,0x11,0x11}, {'O',0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {'P',0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {'Q',0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {'R',0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {'S',0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {'T',0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {'U',0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {'V',0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {'W',0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {'X',0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {'Y',0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {'Z',0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {' ',0,0,0,0,0,0,0},                      {'.',0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {',',0x00,0x00,0x00,0x00,0x0C,0x04,0x08},
    {':',0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {'/',0x01,0x01,0x02,0x04,0x08,0x10,0x10}, {'-',0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {'+',0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, {'%',0x18,0x19,0x02,0x04,0x08,0x13,0x03}, {'(',0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    {')',0x08,0x04,0x02,0x02,0x02,0x04,0x08}, {'=',0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, {'\'',0x04,0x04,0x08,0x00,0x00,0x00,0x00},
    {'!',0x04,0x04,0x04,0x04,0x04,0x00,0x04}, {'?',0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
};
static const BYTE* ovGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (int i = 0; i < (int)(sizeof(kOvFont) / sizeof(kOvFont[0])); ++i) if ((char)kOvFont[i][0] == c) return &kOvFont[i][1];
    return c == '?' ? NULL : ovGlyph('?');
}
// is columns a..b of this row one run (set, with clear neighbours)?
static __forceinline bool ovRun(BYTE row, int a, int b) {
    for (int i = a; i <= b; ++i) if (!(row & (0x10 >> i))) return false;
    if (a > 0 && (row & (0x10 >> (a - 1)))) return false;
    if (b < 4 && (row & (0x10 >> (b + 1)))) return false;
    return true;
}
// one line of text at (x, y), sc pixels per font pixel; the width it took
static LONG ovText(OvRects* out, LONG x, LONG y, int sc, const char* s) {
    LONG cx = x;
    for (; *s; ++s, cx += 6 * sc) {
        const BYTE* g = ovGlyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; ++r) {
            for (int a = 0; a < 5; ) {
                if (!(g[r] & (0x10 >> a))) { ++a; continue; }
                int b = a; while (b < 4 && (g[r] & (0x10 >> (b + 1)))) ++b;
                if (!(r > 0 && ovRun(g[r - 1], a, b))) {               // a run the row above has too was emitted with it
                    int h = 1; while (r + h < 7 && ovRun(g[r + h], a, b)) ++h;
                    ovAdd(out, cx + a * sc, y + r * sc, cx + (b + 1) * sc, y + (r + h) * sc);
                }
                a = b + 1;
            }
        }
    }
    return cx - x - sc;
}
// the text, written by the panel thread once a second into the buffer the game thread is not reading
struct OvIfTxt { char banner[64]; char renderer[96]; char line[128]; LONG fps10; };
static OvIfTxt g_ovIfTxt[2];
static volatile LONG g_ovMpChk = 0, g_ovMpBad = 0;
static volatile LONG g_ovLogic10 = 50, g_ovMpSecs = 0, g_ovMpWaitSecs = 0, g_ovMpNetSecs = 0; static LONG64 g_ovMpLogicSum = 0, g_ovMpFpsSum = 0;   // v68: logic steps a second x10; network-game seconds   // v67: the multiplayer check shown on the screen
static volatile LONG g_ovIfPub = -1;            // which of the two is published (-1: none yet)
static volatile LONG g_ovIfSeq = 0;             // bumped with every publication
static void ovIfPublish(const char* banner, const char* renderer, const char* line, LONG fps10) {
    LONG w = (g_ovIfPub == 0) ? 1 : 0;
    lstrcpynA(g_ovIfTxt[w].banner, banner, sizeof(g_ovIfTxt[w].banner));
    lstrcpynA(g_ovIfTxt[w].renderer, renderer, sizeof(g_ovIfTxt[w].renderer));
    lstrcpynA(g_ovIfTxt[w].line, line, sizeof(g_ovIfTxt[w].line));
    g_ovIfTxt[w].fps10 = fps10;
    InterlockedExchange(&g_ovIfPub, w);
    InterlockedIncrement(&g_ovIfSeq);
}
// Is what is about to be shown the thing we would be painting on? If a render target of the game's is
// still bound at Present, clearing would write into that texture instead of the picture. Its own __try:
// a bad moment here (a device reset, say) must not be fatal, it only means "not now". Both references
// are always handed back, or the game's next Reset would be refused while they are still alive.
static bool ovIsBackBuffer(void** vt, void* dev) {
    void* rt = NULL; void* bb = NULL;
    __try {
        if (((HRESULT(__stdcall*)(void*, DWORD, void**))vt[38])(dev, 0, &rt) < 0) rt = NULL;
        if (rt && ((HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, void**))vt[18])(dev, 0, 0, 0, &bb) < 0) bb = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_ovIfRT = (DWORD)(ULONG_PTR)rt; g_ovIfBB = (DWORD)(ULONG_PTR)bb;
    if (bb) {                                                    // v71: its width - IDirect3DSurface9::GetDesc (slot 12), Width at +24
        DWORD desc[8] = { 0 };
        __try { if (((HRESULT(__stdcall*)(void*, DWORD*))(*(void***)bb)[12])(bb, desc) >= 0 && desc[6] >= 64 && desc[6] <= 16384) g_ovBBW = (LONG)desc[6]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    // IUnknown::Release is slot 2 of the surface's METHOD TABLE (v31 fix: v30 read slot 2 of the object itself and
    // called whatever number was there - never run in a game, found by work\ish4\iftest.exe on its first run)
    if (bb) ((ULONG(__stdcall*)(void*))(*(void***)bb)[2])(bb);
    if (rt) ((ULONG(__stdcall*)(void*))(*(void***)rt)[2])(rt);
    return bb != NULL && bb == rt;
}

typedef HRESULT (__stdcall* OvClearF)(void*, DWORD, const OvRect*, DWORD, DWORD, float, DWORD);
// the rectangles, by colour; rebuilt on the game thread only when what is shown changes
static OvRects g_ovIfBg, g_ovIfBan, g_ovIfRen, g_ovIfLn, g_ovIfMsg;
static LONG g_ovIfSig = -1; static DWORD g_ovIfLnCol = 0xFF40FF40u;
static OvRect g_ovIfBox = { 0, 0, 0, 0 };       // the box last painted (the offline test reads it)
static void ovIfBuild(bool banner, int msg, bool show) {
    g_ovIfBg.n = g_ovIfBan.n = g_ovIfRen.n = g_ovIfLn.n = g_ovIfMsg.n = 0;
    LONG pub = g_ovIfPub;
    const OvIfTxt* t = pub >= 0 ? &g_ovIfTxt[pub] : NULL;
    LONG x = g_ovPx + 8, y = g_ovPy + 6, w = 0;
    if (show && banner && t) {
        LONG a = ovText(&g_ovIfBan, x, y, 3, t->banner); if (a > w) w = a; y += 7 * 3 + 7;
        LONG b = ovText(&g_ovIfRen, x, y, 2, t->renderer); if (b > w) w = b; y += 7 * 2 + 6;
    }
    if (show) {
        LONG c = ovText(&g_ovIfLn, x, y, 2, t ? t->line : "AOTR V84 B134"); if (c > w) w = c; y += 7 * 2 + 6;
        LONG f = t ? t->fps10 : 0;
        g_ovIfLnCol = f >= 280 ? 0xFF40FF40u : (f >= 200 ? 0xFFFFFF40u : 0xFFFF6040u);
    }
    if (msg) {
        LONG d = ovText(&g_ovIfMsg, x, y, 3, msg == 1 ? "AOTR PANEL SHOWN (F7)" : "AOTR PANEL HIDDEN (F7)"); if (d > w) w = d; y += 7 * 3 + 7;
    }
    if (w > 0) { ovAdd(&g_ovIfBg, g_ovPx, g_ovPy, x + w + 8, y); g_ovIfBox = g_ovIfBg.r[0]; }
}
static void ovInFrameDraw(void* dev) {
    if (!g_ovIfWant || g_ovIfDead || !dev) return;
    LONG now = (LONG)GetTickCount();
    if (!g_ovIfFirst) g_ovIfFirst = now ? now : 1;
    bool banner = (now - g_ovIfFirst) < 30000 || (g_ovIfBannerUntil - now) > 0;
    int msg = (g_ovIfMsgUntil - now) > 0 ? (int)g_ovIfMsgKind : 0;
    bool show = g_ovShow != 0;
    if (!show && !msg) return;
    void** vt = NULL;
    __try { vt = *(void***)dev; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = NULL; }
    if (!vt) return;
    if ((LONG)(g_ovIfNextChk - GetTickCount()) <= 0) {         // asked again twice a second: had the game left a
        g_ovIfNextChk = (LONG)GetTickCount() + (g_ovBadge ? 2000 : 500);   // render target bound, we would paint into it
        bool ok = ovIsBackBuffer(vt, dev);
        g_ovIfOk = ok ? 1 : -1;
        if (!ok && !InterlockedExchange(&g_ovIfRTLogged, 1))
            logf("overlay: the in-frame read-out is waiting - the render target at Present (%08X) is not the back buffer (%08X). Nothing is being painted over.",
                 g_ovIfRT, g_ovIfBB);
    }
    if (g_ovIfOk != 1) return;
    if (g_ovBadge) {                                             // v71: the status light only (aotr_badge.inc)
        if (!show || g_ovBBW <= 0) return;
        __try {
            static OvRects ring, dot; static LONG builtW = -1;
            if (builtW != g_ovBBW) {                             // a filled circle as one rectangle per row, a dark rim around it
                builtW = g_ovBBW; ring.n = dot.n = 0;
                const LONG R = 8, cx = g_ovBBW - 22, cy = 22;
                for (LONG dy = -(R + 2); dy <= R + 2; ++dy) {
                    LONG h2 = (R + 2) * (R + 2) - dy * dy; LONG hw = 0; while ((hw + 1) * (hw + 1) <= h2) ++hw;
                    ovAdd(&ring, cx - hw, cy + dy, cx + hw + 1, cy + dy + 1);
                    LONG i2 = R * R - dy * dy; if (i2 < 0) continue;
                    LONG iw = 0; while ((iw + 1) * (iw + 1) <= i2) ++iw;
                    ovAdd(&dot, cx - iw, cy + dy, cx + iw + 1, cy + dy + 1);
                }
            }
            OvClearF cl = (OvClearF)vt[43];                      // IDirect3DDevice9::Clear
            cl(dev, ring.n, ring.r, 1u /*D3DCLEAR_TARGET*/, 0xFF101010u, 0.0f, 0u);
            cl(dev, dot.n, dot.r, 1u, bdgColour(), 0.0f, 0u);
            if (!InterlockedExchange(&g_ovIfLogged, 1))
                logf("badge: the status light is painting on device %08X (back buffer %d wide); F7 hides and shows it.", (DWORD)(ULONG_PTR)dev, (int)g_ovBBW);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!InterlockedExchange(&g_ovIfDead, 1))
                logf("badge: the status light faulted and has switched itself off. The game itself is unaffected.");
        }
        return;
    }
    __try {
        LONG sig = (g_ovIfSeq << 4) | (banner ? 1 : 0) | (msg << 1) | (show ? 8 : 0);
        if (sig != g_ovIfSig) { g_ovIfSig = sig; ovIfBuild(banner, msg, show); }
        if (!g_ovIfBg.n) return;
        OvClearF cl = (OvClearF)vt[43];                        // IDirect3DDevice9::Clear
        cl(dev, g_ovIfBg.n, g_ovIfBg.r, 1u /*D3DCLEAR_TARGET*/, 0xFF0A0A0Au, 0.0f, 0u);
        if (g_ovIfBan.n) cl(dev, g_ovIfBan.n, g_ovIfBan.r, 1u, 0xFF40FF40u, 0.0f, 0u);
        if (g_ovIfRen.n) cl(dev, g_ovIfRen.n, g_ovIfRen.r, 1u, 0xFFFFFFFFu, 0.0f, 0u);
        if (g_ovIfLn.n)  cl(dev, g_ovIfLn.n,  g_ovIfLn.r,  1u, g_ovIfLnCol, 0.0f, 0u);
        if (g_ovIfMsg.n) cl(dev, g_ovIfMsg.n, g_ovIfMsg.r, 1u, 0xFFFFD020u, 0.0f, 0u);
        if (!InterlockedExchange(&g_ovIfLogged, 1))
            logf("overlay: the in-frame read-out is painting on device %08X (target %08X) - box %d,%d-%d,%d, %d rectangles.", (DWORD)(ULONG_PTR)dev, g_ovIfRT,
                 (int)g_ovIfBox.x1, (int)g_ovIfBox.y1, (int)g_ovIfBox.x2, (int)g_ovIfBox.y2,
                 (int)(g_ovIfBg.n + g_ovIfBan.n + g_ovIfRen.n + g_ovIfLn.n + g_ovIfMsg.n));
        InterlockedExchange(&g_ovIfDrawing, 1);
        InterlockedExchange(&g_ovIfLast, (LONG)GetTickCount());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!InterlockedExchange(&g_ovIfDead, 1))
            logf("overlay: the in-frame read-out faulted and has switched itself off. The game itself is unaffected.");
    }
}

// The game imports only d3dx9_27.dll - it takes Direct3DCreate9 at run time, so there is no d3d9.dll
// import slot to take. Every D3DX entry point below takes the device as its FIRST argument though, and
// those slots are the game's own imports, so one of them always hands the device over before the first
// frame. The call is forwarded untouched.
static void* g_ovDxOrig[3] = { NULL, NULL, NULL };
static DWORD __stdcall ovDxEffect(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h, DWORD i) {
    if (a) ovHookPresent((void*)(ULONG_PTR)a);
    return ((DWORD(__stdcall*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))g_ovDxOrig[0])(a, b, c, d, e, f, g, h, i);
}
static DWORD __stdcall ovDxEffectFile(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h) {
    if (a) ovHookPresent((void*)(ULONG_PTR)a);
    return ((DWORD(__stdcall*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))g_ovDxOrig[1])(a, b, c, d, e, f, g, h);
}
static DWORD __stdcall ovDxTexture(DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g, DWORD h) {
    if (a) ovHookPresent((void*)(ULONG_PTR)a);
    return ((DWORD(__stdcall*)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))g_ovDxOrig[2])(a, b, c, d, e, f, g, h);
}
static const struct { const char* name; void* hook; } kOvDx[3] = {
    { "D3DXCreateEffect",          (void*)&ovDxEffect },
    { "D3DXCreateEffectFromFileA", (void*)&ovDxEffectFile },
    { "D3DXCreateTexture",         (void*)&ovDxTexture },
};

// v71: badge mode (the release build): the Present hook and the status light only - no panel window, no report, no
// panel thread. A small thread arms the Present hook once the render thread has decided (the panel thread did that).
static DWORD WINAPI ovBadgeArm(void*) {
    for (int i = 0; i < 1200 && !g_ovReady; ++i) { Sleep(500); if (g_ovDev) ovTryPatchPresent(); }
    return 0;
}
static void installOverlay(bool badge = false) {
    int wantWin = badge ? 0 : (iniWant("Overlay", 1) != 0);
    int wantIf  = badge ? 1 : (iniWant("OverlayInFrame", 1) != 0);   // v31: on - the window read-out cannot be seen over a fullscreen game
    if (badge) InterlockedExchange(&g_ovBadge, 1);
    if (wantIf) InterlockedExchange(&g_ovIfWant, 1);
    g_ovWantWin = wantWin ? 1 : 0;
    g_ovRepWant = badge ? 0 : (iniWant("Report", 1) ? 1 : 0);
    if (!wantWin && !wantIf && !g_ovRepWant) { logf("overlay: off ([Engine] Overlay = 0, OverlayInFrame = 0, Report = 0)."); return; }
    { int k = iniWant("OverlayKey", 0x76); if (k > 0) g_ovKey = k; }
    g_ovPx = iniWant("OverlayX", 8); g_ovPy = iniWant("OverlayY", 8);
    g_ovShow = iniWant("OverlayShow", 1) ? 1 : 0;
    HMODULE hGame = GetModuleHandleA("game.dat");
    if (!hGame) { logf("overlay: this is not the game process - no read-out."); return; }
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        // The render thread takes these same three imports before we run, and it finds its slot by matching the value
        // in it against the D3DX export - so once it has hooked the slot the slot no longer answers to iatSlot and we
        // are told there is nothing to take (the read-out never came up because of exactly this). The render thread
        // calls whatever it found through g_rtXO[i], so becoming the original it calls puts us on the same path with
        // the same result: every call still reaches D3DX, once, and comes past us first.
        FARPROC* s = iatSlot((BYTE*)hGame, "d3dx9_27.dll", kOvDx[i].name);
        if (s && *s && hookSlot(s, kOvDx[i].hook, &g_ovDxOrig[i])) { ++n; logf("overlay: %s is ours.", kOvDx[i].name); continue; }
        if (g_rtXO[i]) {
            g_ovDxOrig[i] = g_rtXO[i];
            g_rtXO[i] = (void*)kOvDx[i].hook;
            ++n;
            logf("overlay: %s is ours (in front of the render thread).", kOvDx[i].name);
        }
    }
    if (!n) { logf("overlay: none of the D3DX entry points could be taken - no read-out."); return; }
    InterlockedExchange(&g_ovOn, 1);
    if (badge) { CreateThread(NULL, 0, ovBadgeArm, NULL, 0, NULL); g_bdgShowSaved = g_ovShow; }
    else if (wantWin || g_ovRepWant) {
        if (CreateThread(NULL, 0, ovThread, NULL, 0, NULL)) CreateThread(NULL, 0, ovWatchThread, NULL, 0, NULL);
    }
    logf("overlay: armed on %d D3DX entry point(s). in-frame read-out %s, window read-out %s, session report %s; key 0x%02X shows and hides them.",
         n, wantIf ? "on" : "off", wantWin ? "on" : "off", g_ovRepWant ? "on" : "off", (unsigned)g_ovKey);
}

// ---------------------------------------------------------------- the panel
static LRESULT CALLBACK ovProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(10, 10, 10));
        FillRect(dc, &r, b); DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(160, 255, 160));
        HGDIOBJ of = SelectObject(dc, g_ovFont ? (HGDIOBJ)g_ovFont : GetStockObject(ANSI_FIXED_FONT));
        RECT t = r; t.left += 6; t.top += 4; t.right -= 4;
        DrawTextA(dc, g_ovText, -1, &t, DT_LEFT | DT_TOP | DT_NOCLIP | DT_EXPANDTABS);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(h, m, w, l);
}

// The panel is only drawn while the game is actually in front. Getting this wrong is what kept the read-out off
// the screen: the game window cannot always be found by shape and size (and a fullscreen display mode can put a
// different window of the same game in front), so "any window of this process is in front" also counts, and while
// the game window is still unknown the panel is shown rather than hidden.
static bool ovGameIsForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return true;                   // a window of this game, whatever it is called
    if (!g_ovGame || !IsWindow(g_ovGame)) return true;               // the game window is not known yet: do not hide
    if (fg == g_ovGame) return true;
    if (GetAncestor(g_ovGame, GA_ROOT) == fg) return true;
    return GetAncestor(fg, GA_ROOT) == GetAncestor(g_ovGame, GA_ROOT);
}

static BOOL CALLBACK ovFindWnd(HWND h, LPARAM l) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != NULL) return TRUE;                 // a dialog or a tool window
    RECT r; GetClientRect(h, &r);
    if (r.right - r.left < 240 || r.bottom - r.top < 180) return TRUE;
    *(HWND*)l = h; return FALSE;
}

// frames-per-second tenths for one A/B segment: frames * 100 / ms gives tenths directly, no rounding traps.
static int ovAbFps10(const long* ab) { return ab[1] > 0 ? (int)((LONG64)ab[2] * 100 / ab[1]) : 0; }

// The exact switch set that produced a reading, so "the best run" can be compared with this one. The names
// are the INI's own, shortened, and the numbers are read out of the INI itself - never invented.
static void ovSwitches(char* out, int cap) {                // v30: one fixed set - the build says what it is
    lstrcpynA(out, "v84 build 134 (one fixed set)", cap);
}
static void ovLoadBest() {
    char p[MAX_PATH]; aotrPath(p, "aotr_best.txt");
    HANDLE fh = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    char b[600]; DWORD rd = 0; memset(b, 0, sizeof(b));
    if (ReadFile(fh, b, sizeof(b) - 1, &rd, NULL) && rd) {
        for (char* q = b; *q; ) {
            if (!memcmp(q, "best=", 5)) g_ovBestStored = ovReadInt(q + 5);
            else if (!memcmp(q, "secs=", 5)) g_ovBestStoredSecs = ovReadInt(q + 5);
            else if (!memcmp(q, "when=", 5)) ovCopyLine(g_ovBestStoredWhen, 40, q + 5);
            else if (!memcmp(q, "switches=", 9)) ovCopyLine(g_ovBestSw, 250, q + 9);
            while (*q && *q != '\n') ++q;
            if (*q) ++q;
        }
    }
    CloseHandle(fh);
    if (g_ovBestStored)
        logf("overlay: remembered best - %d.%d fps sustained (5 s average), %d s into that run, %s | %s",
             (int)(g_ovBestStored / 10), (int)(g_ovBestStored % 10), (int)g_ovBestStoredSecs, g_ovBestStoredWhen, g_ovBestSw);
}
static void ovWriteBest(LONG fps10, LONG secs, const char* sw) {
    char p[MAX_PATH]; aotrPath(p, "aotr_best.txt");
    HANDLE fh = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char b[600]; int n = 0;
    n += wsprintfA(b + n, "best=%d\n", (int)fps10);
    n += wsprintfA(b + n, "secs=%d\n", (int)secs);
    n += wsprintfA(b + n, "when=%04d-%02d-%02d %02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    n += wsprintfA(b + n, "switches=%s\n", sw);
    DWORD wr = 0; WriteFile(fh, b, (DWORD)n, &wr, NULL); CloseHandle(fh);
}

// v31: how many of this process's page faults were read from the DISK (pagefile or a mapped file) - "hard" faults.
// test-11 logged 7000-26000 page faults a second with "disk waits 0 ms"; the plain counter cannot say whether those were
// soft faults (memory that is already in RAM: new pages zero-filled, pages on the standby list) or reads from the hard
// disk. The system's process list has the hard ones per process (SYSTEM_PROCESS_INFORMATION.HardFaultCount, offset
// 0x10 in the 32-bit layout; UniqueProcessId at 0x44). Read once a second on the panel thread; -1: not available.
static BYTE* g_ovSpi = NULL; static ULONG g_ovSpiCap = 0;
static LONG ovHardFaults() {
    typedef LONG (WINAPI* NtQsiF)(ULONG, void*, ULONG, ULONG*);
    static NtQsiF qsi = NULL; static LONG tried = 0;
    if (!tried) { tried = 1; qsi = (NtQsiF)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"); }
    if (!qsi) return -1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!g_ovSpi) { g_ovSpiCap = 1u << 20; g_ovSpi = (BYTE*)VirtualAlloc(NULL, g_ovSpiCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (!g_ovSpi) return -1; }
        ULONG need = 0;
        LONG st = qsi(5 /*SystemProcessInformation*/, g_ovSpi, g_ovSpiCap, &need);
        if (st == (LONG)0xC0000004 /*STATUS_INFO_LENGTH_MISMATCH*/) {
            VirtualFree(g_ovSpi, 0, MEM_RELEASE); g_ovSpi = NULL;
            if (need > (64u << 20)) return -1;
            g_ovSpiCap = (need + 0x10000) & ~0xFFFFu;
            g_ovSpi = (BYTE*)VirtualAlloc(NULL, g_ovSpiCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!g_ovSpi) return -1;
            continue;
        }
        if (st < 0) return -1;
        DWORD pid = GetCurrentProcessId(); ULONG off = 0;
        for (int guard = 0; guard < 100000; ++guard) {
            if (off + 0x48 > g_ovSpiCap) return -1;
            BYTE* e = g_ovSpi + off;
            if (*(DWORD*)(e + 0x44) == pid) return (LONG)*(DWORD*)(e + 0x10);
            ULONG nx = *(ULONG*)e; if (!nx) return -1; off += nx;
        }
        return -1;
    }
    return -1;
}

static DWORD WINAPI ovThread(void*) {
    for (int i = 0; i < 7200 && !g_ovDevSeen; ++i) Sleep(50);       // up to six minutes for the game to make its device
    if (!g_ovDevSeen) return 0;
    WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ovProc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "aotrReadOut"; wc.hbrBackground = NULL; wc.hCursor = NULL;
    RegisterClassA(&wc);
    g_ovWnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              "aotrReadOut", "AOTR", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (!g_ovWnd) { logf("overlay: the panel window could not be made - no read-out."); return 0; }
    SetLayeredWindowAttributes(g_ovWnd, 0, 215, LWA_ALPHA);
    g_ovFont = CreateFontA(-12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!g_ovFont) g_ovFont = CreateFontA(-12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
    logf("overlay: the read-out is up on window %08X (game window %08X).", (DWORD)(ULONG_PTR)g_ovWnd, (DWORD)(ULONG_PTR)g_ovGame);

    LONG lastSlowN = g_fdbSlowN, lastSlowMs = g_fdbSlowMs;
    ULONG64 lastCpu = 0; DWORD lastTick = GetTickCount();
    // v30 step 4 (read only): the game thread's time this second, the clock rate for its cycle counts, page faults,
    // and the game-thread time spent building ground patches.
    HANDLE gameTh = NULL; ULONG64 lastGameCpu = 0; LONG64 lastTpTicks = rd64(&g_tpTicks); DWORD lastPf = 0;
    ULONG64 lastTsc = __rdtsc(); LONG64 lastQpc = qpcNow(); double cycPerMs = 0;
    LONG lastMk = g_mkFrames, lastAsync = g_rtNAsync, lastSync = g_rtNSync;
    static LONG lastHist[512];
    for (int i = 0; i < 512; ++i) lastHist[i] = g_rtSyncHist[i];
    ovLoadBest();
    DWORD excCode = 0;
    for (;;) {
        // A thread that owns a window has to run a message loop, or anything outside that ever sends this
        // window a message waits for an answer that can never come. One pump a second is far more than
        // this panel needs - it takes no input at all - and it removes a whole class of wedged threads.
        g_ovPh = 16; g_ovPhAt = GetTickCount();
        { MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); } }
        g_ovPh = 2; g_ovPhAt = GetTickCount();                                  // Sleep(1000)
        Sleep(1000);
        DWORD workStart = GetTickCount();
      __try {
        g_ovPh = 3; g_ovPhAt = GetTickCount();                                 // hooking Present
        ovTryPatchPresent();                                         // arms as soon as the render thread has hooked the table
        // ---- one second of frames. The span is MEASURED, not assumed: a wake-up that the game's own freeze
        //      delayed must not turn the frames it missed into an inflated rate. That is where a reading like
        //      "500 fps" came from - the pictures were counted, but over the wrong second.
        g_ovPh = 4; g_ovPhAt = GetTickCount();                                 // reading the counters
        DWORD nowTick = GetTickCount();
        DWORD span = nowTick - lastTick; lastTick = nowTick;
        if (!span) span = 1;
        LONG frames = InterlockedExchange(&g_ovFrames, 0);
        LONG gaps   = InterlockedExchange(&g_ovGapSum, 0);
        LONG worst  = InterlockedExchange(&g_ovWorst, 0);
        LONG longs  = InterlockedExchange(&g_ovLongs, 0);
        // ---- the disk, this second
        LONG sn = g_fdbSlowN, sm = g_fdbSlowMs;
        LONG slowN = sn - lastSlowN, slowMs = sm - lastSlowMs;
        lastSlowN = sn; lastSlowMs = sm;
        // ---- processor and memory
        g_ovPh = 5; g_ovPhAt = GetTickCount();                                 // the processor reading
        ULONG64 cpu = 0;
        { FILETIME c, e, k, u; if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
              cpu = (((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime); }
        // A percent of the whole machine: the kernel times are in 100 ns units, so one second of one core
        // is 10^7 units, and percent of the span is d / (span * 100). The old divisor (span * 10000) gave
        // a HUNDREDTH of a percent, so every reading was a hundred times too small and the verdicts that
        // lean on it were decided on nonsense. 6400 = 64 cores flat out, a ceiling no reading can pass.
        int cpuPct = 0;
        if (lastCpu) { ULONG64 d = cpu - lastCpu; cpuPct = (int)(d / ((ULONG64)span * 100)); }
        lastCpu = cpu;
        g_ovPh = 6; g_ovPhAt = GetTickCount();                                 // the memory reading
        DWORD ramMB = 0, threads = 0, pfNow = 0;
        { typedef BOOL (WINAPI* GpmF)(HANDLE, DWORD*, DWORD, DWORD*);
          static GpmF gpm = NULL; static LONG tried = 0;
          if (!tried) { InterlockedExchange(&tried, 1); HMODULE p = LoadLibraryA("psapi.dll");
                        gpm = p ? (GpmF)GetProcAddress(p, "GetProcessMemoryInfo") : NULL; }
          struct { DWORD cb; DWORD PageFaultCount; SIZE_T PeakWS, WorkingSetSize, QuotaPeakPaged, QuotaPaged,
                   QuotaPeakNonPaged, QuotaNonPaged, PagefileUsage, PeakPagefileUsage; } pm;
          memset(&pm, 0, sizeof(pm)); pm.cb = sizeof(pm);
          if (gpm && gpm(GetCurrentProcess(), (DWORD*)&pm, sizeof(pm), NULL)) { ramMB = (DWORD)(pm.WorkingSetSize >> 20); pfNow = pm.PageFaultCount; } }
        // ---- v30 step 4: the game's own thread this second (kernel + user time; scheduler granularity, ~16 ms), the
        //      cycle clock (thread cycle counts are in the processor's time-stamp ticks), page faults, ground patches.
        LONG gameMs = -1;
        { DWORD tid = g_ovGameTid;
          if (tid && !gameTh) gameTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
          FILETIME c, e, k, u;
          if (gameTh && GetThreadTimes(gameTh, &c, &e, &k, &u)) {
              ULONG64 t = (((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime);
              if (lastGameCpu) gameMs = (LONG)((t - lastGameCpu) / 10000);
              lastGameCpu = t; } }
        { ULONG64 ts = __rdtsc(); LONG64 q = qpcNow();
          if (g_pqpf.QuadPart && q > lastQpc && ts > lastTsc) cycPerMs = (double)(ts - lastTsc) * (double)g_pqpf.QuadPart / ((double)(q - lastQpc) * 1000.0);
          lastTsc = ts; lastQpc = q; }
        LONG tpMs = 0;
        { LONG64 tk = rd64(&g_tpTicks); if (g_pqpf.QuadPart) tpMs = (LONG)((tk - lastTpTicks) * 1000 / g_pqpf.QuadPart); lastTpTicks = tk; }
        LONG pfSec = (lastPf && pfNow >= lastPf) ? (LONG)(pfNow - lastPf) : 0; if (pfNow) lastPf = pfNow;
        // v31: of them read from the disk (hard faults), and the files the game opened this second
        LONG hpfSec = -1;
        { static LONG lastHard = -1; LONG hNow = ovHardFaults();
          if (hNow >= 0 && lastHard >= 0 && hNow >= lastHard) hpfSec = hNow - lastHard;
          if (hNow >= 0) lastHard = hNow; }
        LONG filesSec = 0;
        { static LONG lastOpened = 0; LONG o = g_fwNGame; filesSec = o - lastOpened; if (filesSec < 0) filesSec = 0; lastOpened = o; }
        LONG longMs = InterlockedExchange(&g_ovLongMs, 0); LONG64 longCyc = g_ovLongCyc;
        LONG busyMs = (longMs && longCyc >= 0 && cycPerMs > 0) ? (LONG)((double)longCyc / cycPerMs) : -1;
        if (busyMs > longMs) busyMs = longMs;                                // the clock is measured over a second, the frame is shorter
        g_ovPh = 7; g_ovPhAt = GetTickCount();                                 // the thread snapshot
        { HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
          if (snap != INVALID_HANDLE_VALUE) {
              THREADENTRY32 te; te.dwSize = sizeof(te);
              DWORD pid = GetCurrentProcessId();
              if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid) ++threads; } while (Thread32Next(snap, &te));
              CloseHandle(snap); } }
        // ---- what the renderer itself was doing: the counters it logs every five seconds, live here. Both are
        //      per-frame numbers, exactly as in its own report - "navbat/call queue" is every call the game
        //      hands to the render thread, "sync" is every call it has to wait for the renderer to finish.
        g_ovPh = 8; g_ovPhAt = GetTickCount();                                 // the renderer counters
        LONG dF = 0, qpf = 0, syf = 0, topN = 0; char topOp[24]; topOp[0] = 0;
        {
            LONG mk = g_mkFrames, as = g_rtNAsync, sy = g_rtNSync;
            dF = mk - lastMk; if (dF < 0) dF = 0;
            if (dF) { qpf = (as - lastAsync) / dF; syf = (sy - lastSync) / dF; if (qpf < 0) qpf = 0; if (syf < 0) syf = 0; }
            lastMk = mk; lastAsync = as; lastSync = sy;
            int bi = -1; LONG bc = 0;
            for (int i = 0; i < 512; ++i) { LONG v = g_rtSyncHist[i]; LONG d = v - lastHist[i]; lastHist[i] = v; if (d > bc) { bc = d; bi = i; } }
            if (bi >= 0 && bc > 0 && dF) { topN = bc / dF; const char* nm = rtOpName(bi); if (nm && nm[0]) lstrcpynA(topOp, nm, sizeof(topOp)); }
        }
        int hit = 0;
        { LONG h = g_txcNHit, m = g_txcNMiss; if (h + m > 0) hit = (int)((LONG64)h * 100 / (h + m)); }
        // ---- everything read above has a range it cannot leave: Present clamps every gap to under a
        //      minute, the disk waits are real reads of a real second, the processor share is a percent.
        //      A value outside its range is not a measurement at all - some other writer has reached this
        //      memory - so clamp it so the instrument stays upright, and NAME the addresses, because the
        //      writer is out there and this is how it gets found.
        if (worst < 0 || worst > 60000 || longs < 0 || longs > 60000 || frames < 0 || frames > 60000 ||
            slowN < 0 || slowN > 10000 || slowMs < 0 || slowMs > 3600000 || cpuPct < 0 || cpuPct > 6400) {
            LONG occ = InterlockedIncrement(&g_ovCorruptN);
            if (occ <= 3 || (occ % 60) == 0)
                logf("overlay: TRIPWIRE - a reading came back impossible (worst %d ms, long %d, frames %d, disk %d ms, cpu %d%%) at %d:%02d into the run; clamped, occurrence %d. &g_ovWorst=%08X &g_ovFrames=%08X &g_ovGapSum=%08X &g_fdbSlowMs=%08X &g_ovRepCpuSum=%08X &g_ovRepTick=%08X",
                     (int)worst, (int)longs, (int)frames, (int)slowMs, cpuPct,
                     (int)(g_ovRepSec / 60), (int)(g_ovRepSec % 60), (int)occ,
                     (DWORD)(ULONG_PTR)&g_ovWorst, (DWORD)(ULONG_PTR)&g_ovFrames, (DWORD)(ULONG_PTR)&g_ovGapSum,
                     (DWORD)(ULONG_PTR)&g_fdbSlowMs, (DWORD)(ULONG_PTR)&g_ovRepCpuSum, (DWORD)(ULONG_PTR)&g_ovRepTick);
            if (worst < 0 || worst > 60000) worst = 0;
            if (longs < 0 || longs > 60000) longs = 0;
            if (frames < 0 || frames > 60000) frames = 0;
            if (slowN < 0 || slowN > 10000) slowN = 0;
            if (slowMs < 0 || slowMs > 3600000) slowMs = 0;
            if (cpuPct < 0 || cpuPct > 6400) cpuPct = 0;
        }
        LONG fps10  = (LONG)((LONG64)frames * 10000 / span);          // pictures a second, x10
        LONG avg    = frames ? gaps / frames : 0;
        // ---- one second of this run, kept
        ++g_ovRepSec;
        g_ovRepFrames += frames;
        if (frames > 0) { LONG b = fps10 / 10; if (b < 0) b = 0; if (b >= OV_FPSH) b = OV_FPSH - 1; ++g_ovFpsHist[b]; ++g_ovFpsSecs; g_ovFpsFrames += frames; }
        g_ovRepCpuSum += cpuPct; ++g_ovRepCpuN;
        if (fps10 >= 100) { g_ovPlayCpuSum += cpuPct; ++g_ovPlayCpuN; if (gameMs >= 0) { g_ovPlayGameMs += gameMs; g_ovPlaySpan += (LONG)span; } }   // v31: seconds with 10+ pictures only
        if (fps10 > g_ovPeak10) g_ovPeak10 = fps10;
        if (fps10 > 1200) ++g_ovBurst;                                // a second over 120 fps: the engine ran uncapped
        if (worst > g_ovRunWorst) g_ovRunWorst = worst;
        g_ovRing[g_ovRingI] = fps10; g_ovRingI = (g_ovRingI + 1) % 5; if (g_ovRingN < 5) ++g_ovRingN;
        LONG a5 = 0, pk5 = 0;
        { LONG64 s = 0; for (int i = 0; i < g_ovRingN; ++i) { s += g_ovRing[i]; if (g_ovRing[i] > pk5) pk5 = g_ovRing[i]; }
          a5 = g_ovRingN ? (LONG)(s / g_ovRingN) : 0; }
        if (g_ovRingN >= 5 && a5 > g_ovBest5) { g_ovBest5 = a5; g_ovBest5At = g_ovRepSec; }
        // ---- a frame long enough to be felt, and what the machine was doing at that moment
        g_ovPh = 9; g_ovPhAt = GetTickCount();                                 // the stall stamp
        ovDetectRenderer();                                                     // once the driver is loaded: which d3d9, which chip
        // v30: there is no render queue any more, so a long frame is either the disk (a real read or open of 50 ms or
        // more inside it) or the game's own thread (its logic, its drawing, the driver it calls).
        // v30 step 4: the game's own thread is told apart by how much of that very frame it really ran (busyMs, from its
        // cycle count): 75 % or more - busy, its own work; 25 % or less - waiting (driver / GPU, paging, a lock); between -
        // mixed. Not known (the frame was presented from another thread, or the clock not measured yet): busy, as before,
        // and the line says "?".
        if (worst >= 100) {
            ++g_ovSt100; if (worst >= 250) ++g_ovSt250; if (worst >= 500) ++g_ovSt500;
            LONG frameMs = longMs ? longMs : worst;
            LONG share = (busyMs >= 0 && frameMs > 0) ? (LONG)((LONG64)busyMs * 100 / frameMs) : -1;
            int cls = (slowMs * 2 >= worst) ? 1 : (share < 0 || share >= 75) ? 0 : (share <= 25 ? 2 : 3);
            ++g_ovStCls[cls];
            int slot = -1;
            if (g_ovNStall < OV_STALLS) slot = g_ovNStall++;
            else for (int i = 0; i < OV_STALLS; ++i) if (g_ovStall[i].ms < worst) { slot = i; break; }
            if (slot >= 0) {
                OvStall st; st.ms = worst; st.at = g_ovRepSec; st.cpu = cpuPct; st.slowMs = slowMs; st.syf = (LONG)g_tpNTex; st.qpf = cls;
                st.busyMs = busyMs; st.tpMs = tpMs; st.pf = pfSec; st.hpf = hpfSec; st.files = filesSec;
                static const char* kCls[4] = { "game thread busy", "disk", "game thread waited", "mixed" };
                lstrcpynA(st.top, kCls[cls], sizeof(st.top));
                g_ovStall[slot] = st;
                for (int i = slot; i > 0 && g_ovStall[i].ms > g_ovStall[i - 1].ms; --i) { OvStall t = g_ovStall[i]; g_ovStall[i] = g_ovStall[i - 1]; g_ovStall[i - 1] = t; }
            }
            { static const char* kWhy[4] = { "the game's own thread was BUSY (its own work: logic, drawing calls)", "the disk",
                                             "the game's own thread was WAITING (driver / GPU, memory paged in, or a lock)",
                                             "the game's own thread, part busy, part waiting" };
              char bz[32]; if (busyMs >= 0) wsprintfA(bz, "%d ms (%d%%)", (int)busyMs, (int)share); else lstrcpynA(bz, "? ms", sizeof(bz));
              char wk[96] = "";                                  // v43: what the render worker did longest around this frame
              if (g_rtActive) {
                  LONG a = g_rtWCurMax, b = g_rtWPrevMax; LONG k = a > b ? a : b; int op = a > b ? (int)g_rtWCurOp : (int)g_rtWPrevOp;
                  double kms = (g_tscPerQpc > 0 && g_pqpf.QuadPart) ? 1024.0 * 1000.0 / (g_tscPerQpc * (double)g_pqpf.QuadPart) : 0;
                  if (kms > 0) { LONG us = (LONG)(k * kms * 1000.0); wsprintfA(wk, "; render worker's longest call %d.%d ms (%s)", (int)(us / 1000), (int)((us % 1000) / 100), rtOpName(op & 511)); }
              }
              logf("perf: stall - a frame of %d ms at %d:%02d into the run (cpu %d.%02d cores, disk waits %d ms in that second; the game's thread ran %s of that frame; %d page faults that second, %d of them read from the disk; %d files opened that second; ground patches %d ms on the game's thread that second) - %s.",
                   (int)worst, (int)(g_ovRepSec / 60), (int)(g_ovRepSec % 60), cpuPct / 100, cpuPct % 100, (int)slowMs,
                   bz, (int)pfSec, (int)hpfSec, (int)filesSec, (int)tpMs, kWhy[cls]);
              if (wk[0]) logf("perf: stall   ...%s", wk); }
        }
        // ---- the five-second line: the numbers the panel shows, in the log, whether or not a screen ever shows them
        { static LONG w5 = 0, l5 = 0, s5 = 0, f5 = 0, c5 = 0, n5 = 0, lastTp = 0, lastFw = 0, g5 = 0, sp5 = 0, pf5 = 0, h5 = 0, hn5 = 0; static LONG64 lastFwB = 0; static DWORD t5 = 0;
          if (!t5) t5 = nowTick;
          if (worst > w5) w5 = worst; l5 += longs; s5 += slowMs; f5 += frames; c5 += cpuPct; ++n5;
          if (gameMs >= 0) { g5 += gameMs; sp5 += (LONG)span; } pf5 += pfSec; if (hpfSec >= 0) { h5 += hpfSec; ++hn5; }
          if (n5 >= 5) {
              LONG tp = g_tpNTex, fw = g_fwNWarm; LONG64 fwb = g_fwReadBytes;
              DWORD ms5 = nowTick - t5; if (!ms5) ms5 = 1; t5 = nowTick;
              LONG fps5 = (LONG)((LONG64)f5 * 10000 / ms5);                    // x10, over the time these five readings really took
              logf("perf: last 5 s - %d.%d fps, longest frame %d ms, %d frames of 100 ms or more, cpu %d.%02d cores (the game's thread ran %d%% of the time), disk waits %d ms, %d page faults a second (%d a second read from the disk) | ground patch textures built %d (on %d threads) | files read ahead %d (%d MB) | %s",
                   (int)(fps5 / 10), (int)(fps5 % 10), (int)w5, (int)l5, (int)(c5 / n5 / 100), (int)((c5 / n5) % 100), sp5 ? (int)((LONG64)g5 * 100 / sp5) : -1, (int)s5, (int)(pf5 / n5), hn5 ? (int)(h5 / hn5) : -1,
                   (int)(tp - lastTp), g_tpOn ? (int)g_tpHelpers + 1 : 1, (int)(fw - lastFw), (int)((fwb - lastFwB) >> 20), g_ovRenderer);
              lastTp = tp; lastFw = fw; lastFwB = fwb;
              w5 = l5 = s5 = f5 = c5 = n5 = g5 = sp5 = pf5 = h5 = hn5 = 0;
          } }
        // ---- the counter itself must be seen to work: a device that is drawn with but whose pictures are not counted
        //      is said once, with what the method table holds now, instead of a panel that silently reads 0.
        { static LONG lastSeen = 0, still = 0, said = 0;
          LONG seen = g_ovPresentsSeen;
          if (g_ovReady && seen == lastSeen && g_ovRepSec > 20) { if (++still == 15 && !said) { said = 1;
              void* vt = NULL; DWORD s17 = 0; __try { vt = *(void**)g_ovDev; s17 = (DWORD)(ULONG_PTR)((void**)vt)[17]; } __except (EXCEPTION_EXECUTE_HANDLER) { }
              char own[64]; rtOwner(s17, own, sizeof(own));
              logf("overlay: WARNING - no picture counted for 15 s (%d so far). Device %08X now has vtable %08X with Present %08X (%s); hot-patch %s. If the game is in a menu or minimised this is expected.",
                   (int)seen, (DWORD)(ULONG_PTR)g_ovDev, (DWORD)(ULONG_PTR)vt, s17, own, g_ovHotFn ? "in place" : "not used"); } }
          else still = 0;
          lastSeen = seen; }
        g_ovPh = 10; g_ovPhAt = GetTickCount();                                // the panel text
        { static LONG lastTp = 0; LONG tp = g_tpNTex; LONG tpS = tp - lastTp; lastTp = tp;
        wsprintfA(g_ovCat,
                  "AOTR accel v84 build 134   F7 = hide   %d min\r\n"
                  "fps %d.%d   average(5s) %d.%d   frame %d ms   longest %d ms\r\n"
                  "hitches(100ms+) %d   longest this session %d ms   disk waits %d ms\r\n"
                  "cpu %d.%02d cores   ram %u MB   threads %u   ground patches %d/s\r\n"
                  "%s   files read ahead %d\r\n"
                  "%s",
                  (int)(g_ovRepSec / 60), fps10 / 10, fps10 % 10, a5 / 10, a5 % 10, avg, worst,
                  (int)g_ovSt100, (int)g_ovRunWorst, slowMs,
                  cpuPct / 100, cpuPct % 100, (unsigned)ramMB, (unsigned)threads, (int)tpS,
                  g_ovRenderer, (int)g_fwNWarm, g_ovGpuNote); }
        lstrcpynA(g_ovText, g_ovCat, sizeof(g_ovText));                        // bounded, whatever wsprintf did
        // v31: the in-frame read-out's text (painted by the game thread at its next Present)
        { char ren[96], ln[128];
          wsprintfA(ren, "%s - F7 HIDES", g_ovRendererKnown ? g_ovRenderer : "RENDERER NOT KNOWN YET");
          // v68: logic steps a second (the game's own logic frame counter, TheGameLogic+0x40). BFME2's logic runs 5 steps a
          // second; in a network game a step only runs when every player's orders for it have arrived, so fewer than 5
          // with fast pictures means the game waited for the network or for a slower player - time no change on this PC
          // can win back. Measured every second, summed over network-game time for the report.
          { static DWORD lgFr = 0, lgT = 0; DWORD glp = *(volatile DWORD*)0x00DE412C; DWORD fr = glp ? *(DWORD*)(ULONG_PTR)(glp + 0x40) : 0; DWORD tn = GetTickCount();
            if (lgT && tn > lgT && fr >= lgFr && fr - lgFr < 100) {
                g_ovLogic10 = (LONG)((fr - lgFr) * 10000 / (tn - lgT));
                DWORD md = glp ? *(DWORD*)(ULONG_PTR)(glp + 0x110) : 0;
                if ((md == 1 || md == 5) && g_frLastLogicTick && tn - g_frLastLogicTick < 3000) {
                    g_ovMpSecs++; g_ovMpLogicSum += g_ovLogic10; g_ovMpFpsSum += fps10;
                    if (g_ovLogic10 < 45) { g_ovMpWaitSecs++; if (fps10 >= 250) g_ovMpNetSecs++; }
                    if (g_ovMpSecs % 30 == 0) logf("mp: network game %d s - %d.%d fps, %d.%d logic steps a second on average; %d s with fewer than 4.5 steps (%d of them with fast pictures: waiting for the network or a slower player) | self-checks %d, different %d",
                        (int)g_ovMpSecs, (int)(g_ovMpFpsSum / g_ovMpSecs / 10), (int)(g_ovMpFpsSum / g_ovMpSecs % 10), (int)(g_ovMpLogicSum / g_ovMpSecs / 10), (int)(g_ovMpLogicSum / g_ovMpSecs % 10), (int)g_ovMpWaitSecs, (int)g_ovMpNetSecs, (int)g_ovMpChk, (int)g_ovMpBad);
                }
            }
            lgFr = fr; lgT = tn; }
          // v67: the multiplayer check on the screen - every part that touches the game's logic proves itself against the
          // engine's own code from the first logic step and all through the game; one difference turns that part off
          { LONG chk = g_pfChecked + g_amfChecked + g_eqChecked + g_fltChecked + g_lsSeqChecked + g_cbfChecked + g_fmfChecked + g_mskChecked + g_hfChecked;
            LONG bad = g_pfBad + g_amfBad + g_eqBad + g_fltBad + g_lsSeqBad + g_cbfBad + g_fmfBad + g_mskBad + g_hfBad;
            DWORD gl = *(volatile DWORD*)0x00DE412C; DWORD md = gl ? *(DWORD*)(ULONG_PTR)(gl + 0x110) : 0;
            bool net = md == 1 || md == 5;
            if (!bad) wsprintfA(ren, "MP CHECK OK: 9 PARTS, %d CHECKS, 0 DIFFERENT%s", (int)chk, net ? " (NETWORK GAME)" : "");
            else wsprintfA(ren, "MP CHECK: %d DIFFERENT! THAT PART TURNED ITSELF OFF - SEND THE LOG", (int)bad);
            g_ovMpChk = chk; g_ovMpBad = bad; }
          wsprintfA(ln, "AOTR B%d  LOGIC %d.%d/S  FPS %d.%d  LONGEST %d MS  HITCHES %d  %s", 134, (int)(g_ovLogic10 / 10), (int)(g_ovLogic10 % 10), (int)(fps10 / 10), (int)(fps10 % 10), (int)worst, (int)g_ovSt100, g_ovMpBad ? "MP: DIFFERENT!" : "MP: OK");
          ovIfPublish("AOTR V84 BUILD 134 RUNNING", ren, ln, fps10); }
        { static LONG lastKeys = 0; LONG kp = g_ovKeyPresses;
          if (kp != lastKeys) { lastKeys = kp; logf("overlay: F7 pressed (%d so far) - the panel is now %s.", (int)kp, g_ovShow ? "SHOWN" : "HIDDEN"); } }
        { static LONG paintSaid = 0;
          if (!paintSaid && g_ovIfWant && g_ovRepSec > 40 && g_ovPresentsSeen > 100 && !g_ovIfDrawing) { paintSaid = 1;
              logf("overlay: WARNING - the in-frame read-out has not painted yet (%s). The log and aotr_report.txt are written either way.",
                   g_ovIfDead ? "it faulted and switched itself off" : (g_ovIfOk == -1 ? "the render target at Present is not the back buffer" : "no reason known")); } }
        // ---------------------------------------------------------------- the session report (every minute)
        g_ovPh = 11; g_ovPhAt = GetTickCount();                                // the session report
        { static DWORD lastSum = 0;
          if (!lastSum) lastSum = nowTick;
          if ((DWORD)(nowTick - lastSum) >= 60000) { lastSum = nowTick; char sl[1100]; ovSessionLine(sl, "so far"); logf("%s", sl); } }
        if (g_ovRepWant && (!g_ovRepTick || (DWORD)(nowTick - (DWORD)g_ovRepTick) >= 60000)) {
            int firstRep = !g_ovRepTick;
            g_ovRepTick = (LONG)nowTick;
            aotrPath(g_ovRp, "aotr_report.txt");
            HANDLE fh = CreateFileA(g_ovRp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (fh != INVALID_HANDLE_VALUE) {
                int n = 0; g_ovB[0] = 0;
                ovSwitches(g_ovSw, sizeof(g_ovSw));
                LONG avgFps10 = g_ovRepSec ? (LONG)(g_ovRepFrames * 10 / g_ovRepSec) : 0;   // x10, like everywhere else
                int cpuAvg = g_ovRepCpuN ? (int)(g_ovRepCpuSum / g_ovRepCpuN) : 0;
                int c0 = (int)g_ovStCls[0], c1 = (int)g_ovStCls[1], c2 = (int)g_ovStCls[2], c3 = (int)g_ovStCls[3];
                n = ovAppend(g_ovB, sizeof(g_ovB), n, g_vanilla ? "AOTR accel v84 (diagnostics, build 134) - VANILLA: THE GAME AS IT IS (measurement only) - REPORT\r\n\r\n" : "AOTR accel v84 (diagnostics, build 134) - REPORT   (rewritten every 60 seconds while the game runs)\r\n\r\n");
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "play time %d min %d s | average %d.%d fps, best 5 s %d.%d fps (at %d s), peak %d.%d\r\n",
                             (int)(g_ovRepSec / 60), (int)(g_ovRepSec % 60), (int)(avgFps10 / 10), (int)(avgFps10 % 10),
                             (int)(g_ovBest5 / 10), (int)(g_ovBest5 % 10), (int)g_ovBest5At, (int)(g_ovPeak10 / 10), (int)(g_ovPeak10 % 10));
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "seconds with pictures %d: fps average %d.%d, median %d, slowest 10%% of seconds <= %d\r\n",
                             (int)g_ovFpsSecs, g_ovFpsSecs ? (int)(g_ovFpsFrames / g_ovFpsSecs) : 0, g_ovFpsSecs ? (int)((g_ovFpsFrames * 10 / g_ovFpsSecs) % 10) : 0,
                             ovFpsAt(50), ovFpsAt(10));
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "100 ms+ hitches %d (250+ %d, 500+ %d), longest frame %d ms\r\n",
                             (int)g_ovSt100, (int)g_ovSt250, (int)g_ovSt500, (int)g_ovRunWorst);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "hitch cause: disk %d | game thread busy (logic, drawing) %d | game thread waited (driver/GPU, memory, lock) %d | mixed %d\r\n", c1, c0, c2, c3);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "cpu average %d.%02d cores (menus included), in play %d.%02d cores (the game thread ran %d%% of the time), ram %u MB, threads %u\r\n",
                             cpuAvg / 100, cpuAvg % 100, (g_ovPlayCpuN ? (int)(g_ovPlayCpuSum / g_ovPlayCpuN) : 0) / 100, (g_ovPlayCpuN ? (int)(g_ovPlayCpuSum / g_ovPlayCpuN) : 0) % 100,
                             g_ovPlaySpan ? (int)(g_ovPlayGameMs * 100 / g_ovPlaySpan) : -1, (unsigned)ramMB, (unsigned)threads);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "machine: processor speed now %d%% (of the fastest this session; lowest %d%%, %d s under 70%%) | power: %s%s%s\r\n",
                             (int)g_gdSpeed, (int)g_gdSpeedMin, (int)g_gdSlowSecs, g_gdAc == 1 ? "mains" : (g_gdAc == 0 ? "BATTERY" : "unknown"), g_gdSaver ? ", BATTERY SAVER ON" : "",
                             g_gdSlowSecs > 20 ? " - THE MACHINE SLOWED ITSELF DOWN (neither the game nor this DLL is the cause)" : "");
                { static const char* const kLv[5] = { "Turbo (Windows)", "clock 150%", "clock 130%", "clock 115%", "Turbo off" };
                  n = ovAppend(g_ovB, sizeof(g_ovB), n, "processor levels (fps and falls under 35%% at each level; the best one is kept):\r\n");
                  for (int i = 0; i < 5; ++i) if (g_gdLvlSecs[i]) n = ovAppend(g_ovB, sizeof(g_ovB), n, "  %d. %s: %d s, %d.%d fps, %d falls%s\r\n", i, kLv[i], (int)g_gdLvlSecs[i],
                      (int)(g_gdLvlFrames[i] * 10 / g_gdLvlSecs[i] / 10), (int)(g_gdLvlFrames[i] * 10 / g_gdLvlSecs[i] % 10), (int)g_gdLvlDeep[i], i == g_gdLvlBest ? "  <- best" : ""); }
                if (g_gdMarks) n = ovAppend(g_ovB, sizeof(g_ovB), n, "your markers (right Ctrl): %d, %d of them within 5 s of the processor slowing itself down\r\n", (int)g_gdMarks, (int)g_gdMarksSlow);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "memory (the 32-bit game's address space): now %d MB, peak %d MB (%d MB when play began), limit %d MB%s\r\n",
                             (int)g_gdVaMB, (int)g_gdVaPeak, (int)g_gdVaPlayStart, (int)g_gdVaTotal, g_gdVaPeak >= 3300 ? " - CLOSE TO THE LIMIT" : " - enough");
                if (g_pl2Runs) n = ovAppend(g_ovB, sizeof(g_ovB), n, "prepared on the loading screen: sudden ones (spells, summons, projectiles, effects) %d of %d + %d names; all %d of %d templates, %d names; stopped: %s\r\n",
                             (int)g_plRepT1All, (int)g_plRepT1Fed, (int)g_plRepT1Names, (int)g_plPlanAll, (int)g_plPlanAt, (int)g_plANFed, g_plFeedStop[0] ? g_plFeedStop : "-");
                if (g_plTrBatches || g_plTrOn) n = ovAppend(g_ovB, sizeof(g_ovB), n, "prepared in the background during play: %d batches (%d of them sudden ones), plan %d/%d templates, %d/%d names%s\r\n",
                             (int)g_plTrBatches, (int)g_plTrT1, (int)g_plPlanAt, (int)g_plPlanN, (int)g_plANAt, (int)g_plANIn, g_plTrOn ? " - still going" : "");
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "learned list (what first appeared in earlier battles): %d files (%d MB) read into the file cache in the menu, newest first; %d new names written this session\r\n",
                             (int)g_plLwFiles, (int)g_plLwMB, (int)g_plLearnSaved);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "graphics: %s | %s\r\n", g_ovRendererKnown ? g_ovRenderer : "not known yet", g_ovGpuNote);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "ground patch textures: %d built on %d cores (check: %s)\r\n",
                             (int)g_tpNTex, g_tpOn ? (int)g_tpHelpers + 1 : 1,
                             !g_tpChecked ? "not yet" : (g_tpCheckBad ? "DIFFERENT - switched off, the game's own code builds them" : "every byte equal to the game's"));
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "files read ahead: %d, %d MB (from the list of earlier sessions)\r\n",
                             (int)g_fwNWarm, (int)(g_fwReadBytes >> 20));
                if (g_ovMpSecs) n = ovAppend(g_ovB, sizeof(g_ovB), n, "MULTIPLAYER: %d s in network games - average %d.%d fps, logic %d.%d steps/s (should be 5); %d s with fewer than 4.5 steps, %d s of them with fast pictures (25+ fps) - the game was waiting for the NETWORK or a SLOWER PLAYER\r\n",
                             (int)g_ovMpSecs, (int)(g_ovMpFpsSum / g_ovMpSecs / 10), (int)(g_ovMpFpsSum / g_ovMpSecs % 10), (int)(g_ovMpLogicSum / g_ovMpSecs / 10), (int)(g_ovMpLogicSum / g_ovMpSecs % 10), (int)g_ovMpWaitSecs, (int)g_ovMpNetSecs);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "MP CHECK: the 9 parts that touch the logic (whole-map query, modifier index, type equivalence, filter memo, logic order, command button names, object modules, flag masks, id map buckets) checked themselves against the original code %d times, %d different%s\r\n",
                             (int)g_ovMpChk, (int)g_ovMpBad, g_ovMpBad ? " - DIFFERENT: that part turned itself off, send the log" : " - all the same");
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "multiplayer: logic speed unchanged (FpsLimit=0, LogicHz=0, SmoothAnimation=0), logic results bit-exact (the speedups check themselves against the original code)\r\n");
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "frame limit: the game itself caps the frame rate at 30 fps (left alone for multiplayer) - the average cannot go above 30, the difference is in the hitches\r\n");
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "\r\nLONGEST FRAMES (time since the start)\r\n");
                for (int i = 0; i < g_ovNStall; ++i) {
                    LONG t = g_ovStall[i].at;                       // seconds into the run
                    n = ovAppend(g_ovB, sizeof(g_ovB), n, "  %5d ms  %d:%02d:%02d  cpu %d.%02d cores  disk %d ms  game thread ran %d ms  page faults %d (%d from disk)  files opened %d  ground patches %d ms  cause: %s\r\n",
                                 (int)g_ovStall[i].ms, (int)(t / 3600), (int)((t / 60) % 60), (int)(t % 60),
                                 g_ovStall[i].cpu / 100, g_ovStall[i].cpu % 100, (int)g_ovStall[i].slowMs, (int)g_ovStall[i].busyMs,
                                 (int)g_ovStall[i].pf, (int)g_ovStall[i].hpf, (int)g_ovStall[i].files, (int)g_ovStall[i].tpMs, g_ovStall[i].top);
                }
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "\r\nSUMMARY\r\n");
                if (!g_ovRepFrames)
                    n = ovAppend(g_ovB, sizeof(g_ovB), n, "  no frame counted yet (the game is in a menu or loading). See the 'overlay:' lines in bfme2_accel.log.\r\n");
                else if (g_ovSt100 == 0)
                    n = ovAppend(g_ovB, sizeof(g_ovB), n, "  no hitches: no frame longer than 100 ms.\r\n");
                else if (c1 * 2 >= (int)g_ovSt100)
                    n = ovAppend(g_ovB, sizeof(g_ovB), n, "  most hitches came FROM THE DISK (%d/%d): files read for the first time. Next game these files are read ahead.\r\n", c1, (int)g_ovSt100);
                else if (c2 * 2 >= (int)g_ovSt100)
                    n = ovAppend(g_ovB, sizeof(g_ovB), n, "  in most hitches the game thread WAITED (%d/%d): driver/GPU, memory (page faults) or a lock. The log's 'perf: stall' lines give the times.\r\n", c2, (int)g_ovSt100);
                else
                    n = ovAppend(g_ovB, sizeof(g_ovB), n, "  most hitches were on the game's own thread (busy %d, waited %d, mixed %d / %d). The log's 'perf: stall' lines give the times.\r\n", c0, c2, c3, (int)g_ovSt100);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "\r\nREMEMBERED BEST: %d.%d fps (5 s average), %s\r\n",
                             (int)(g_ovBestStored / 10), (int)(g_ovBestStored % 10), g_ovBestStoredWhen);
                n = ovAppend(g_ovB, sizeof(g_ovB), n, "panel: %u rounds, %d corruption marks, %d exceptions | frames counted %d (%s)\r\n",
                             (unsigned)g_ovIter, (int)g_ovCorruptN, (int)g_ovExc, (int)g_ovPresentsSeen, g_ovHotFn ? "in Present's code" : "in Present's table");
                if (g_ovRingN >= 5 && g_ovBest5 > g_ovBestStored) {
                    SYSTEMTIME stx; GetLocalTime(&stx);
                    g_ovBestStored = g_ovBest5; g_ovBestStoredSecs = g_ovBest5At;
                    wsprintfA(g_ovBestStoredWhen, "%04d-%02d-%02d %02d:%02d", stx.wYear, stx.wMonth, stx.wDay, stx.wHour, stx.wMinute);
                    ovWriteBest(g_ovBest5, g_ovBest5At, g_ovSw);
                }
                DWORD wr = 0; WriteFile(fh, g_ovB, (DWORD)n, &wr, NULL);
                CloseHandle(fh);
                InterlockedIncrement(&g_ovRepWrites);
                g_ovRepBytes = (LONG)wr;
                if (firstRep) logf("overlay: the session report is %s (rewritten every minute).", g_ovRp);
            } else {
                logf("overlay: the report could not be opened for writing (error %u) - the panel keeps measuring.",
                     (unsigned)GetLastError());
            }
        }
        // ---- size and place
        g_ovPh = 12; g_ovPhAt = GetTickCount();                                // sizing the panel
        int w = 240, h = 74;
        { HDC dc = GetDC(g_ovWnd);
          if (dc) { HGDIOBJ of = SelectObject(dc, g_ovFont ? (HGDIOBJ)g_ovFont : GetStockObject(ANSI_FIXED_FONT));
                    RECT rr = { 0, 0, 0, 0 };
                    DrawTextA(dc, g_ovText, -1, &rr, DT_CALCRECT | DT_LEFT | DT_NOCLIP | DT_EXPANDTABS);
                    w = rr.right + 16; h = rr.bottom + 10;
                    SelectObject(dc, of); ReleaseDC(g_ovWnd, dc); } }
        g_ovPh = 13; g_ovPhAt = GetTickCount();                                // finding the game window
        int x = 8, y = 8;
        if (!g_ovGame || !IsWindow(g_ovGame)) { HWND hw = NULL; EnumWindows(ovFindWnd, (LPARAM)&hw); g_ovGame = hw; }
        g_ovPh = 14; g_ovPhAt = GetTickCount();                                // placing the panel
        if (g_ovGame && IsWindow(g_ovGame)) {
            POINT p; p.x = g_ovPx; p.y = g_ovPy; ClientToScreen(g_ovGame, &p);
            x = p.x; y = p.y;
        }
        SetWindowPos(g_ovWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        // The in-frame read-out is painted into the picture itself, so it cannot be lost by the display mode. While it
        // is actually painting (it says so every frame) this window would only be a second copy of the same numbers
        // on top of it - and if it ever stops, this window is back within a second.
        g_ovPh = 15; g_ovPhAt = GetTickCount();                                // showing the panel
        LONG ifLast = g_ovIfLast;
        int ifAlive = (ifLast != 0 && (LONG)(GetTickCount() - ifLast) < 1500);
        if (g_ovWantWin && g_ovShow && !ifAlive && ovGameIsForeground()) { ShowWindow(g_ovWnd, SW_SHOWNOACTIVATE); RedrawWindow(g_ovWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE); }
        else ShowWindow(g_ovWnd, SW_HIDE);
      } __except (((excCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)) {
            // A fault inside the panel must never take the game with it. Say what happened and which
            // phase it happened in - that line is the whole point of the exercise.
            InterlockedIncrement(&g_ovExc);
            logf("overlay: the panel caught exception %08X in phase %s (%d) at %d:%02d into the run - the run continues.",
                 excCode, ovPhaseName(g_ovPh), (int)g_ovPh, (int)(g_ovRepSec / 60), (int)(g_ovRepSec % 60));
        }
        DWORD workMs = GetTickCount() - workStart;
        if (workMs > 2500)
            logf("overlay: slow panel iteration - %u ms of work at %d:%02d into the run (the last phase was %s).",
                 (unsigned)workMs, (int)(g_ovRepSec / 60), (int)(g_ovRepSec % 60), ovPhaseName(g_ovPh));
        g_ovPh = 0; g_ovPhAt = GetTickCount();
        ++g_ovIter;                                                            // (the perf: line every 5 s is the heartbeat)
    }
}

// The second thread of the instrument. A thread can stop inside a system call without a word anywhere,
// and in the 00:10 run the panel thread stopped for good at the moment the match started - silently,
// taking the whole instrument with it. This one only WATCHES: if the panel's iteration counter stops
// moving while its phase has been running for a long time, it says which phase, so the next log always
// names the call that ate the thread instead of leaving a frozen read-out and nothing else.
static DWORD WINAPI ovWatchThread(void*) {
    DWORD lastIter = 0xFFFFFFFF, lastLog = 0;
    for (;;) {
        Sleep(5000);
        if (!g_ovDevSeen || !g_ovWnd) continue;                                // nothing to watch yet
        DWORD it = g_ovIter;
        if (it != lastIter) { lastIter = it; continue; }                       // it finished an iteration: alive
        DWORD now = GetTickCount(), at = g_ovPhAt, ph = (DWORD)g_ovPh;
        if (ph != 0 && (DWORD)(now - at) < 20000) continue;                    // inside an iteration that is still young
        if ((DWORD)(now - at) < 60000) continue;                               // between iterations, but recently
        if ((DWORD)(now - lastLog) < 60000) continue;                          // at most one line a minute
        lastLog = now;
        logf("overlay: WATCH - the panel thread has finished no iteration for %u s; it sits in phase %u (%s) since %u s.",
             (unsigned)((now - at) / 1000), ph, ovPhaseName((LONG)ph), (unsigned)((now - at) / 1000));
    }
    return 0;
}

#ifdef AOTR_SELFTEST
// ---------------------------------------------------------------- offline check of the Present counter (test DLL only)
// work\probe11\presenttest.exe makes a real device with Windows' own d3d9, hands it here, then presents through the
// device and the swap chain with a state block recorded in between (which rewrites the device's table) and a Reset.
extern "C" __declspec(dllexport) int __cdecl AotrPresentHookTest(void* dev) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    g_rtTried = true; InterlockedExchange(&g_rtWantOn, 0);
    ovHookPresent(dev);
    return g_ovReady ? (g_ovHotFn ? 2 : 1) : 0;
}
extern "C" __declspec(dllexport) int __cdecl AotrPresentCount() { return (int)g_ovPresentsSeen; }
// work\ish4\iftest.exe: paint the in-frame read-out on a device it made (no Present hook needed); mode 0 banner + line,
// 1 the line only, 2 hidden with the "HIDDEN" message, 3 hidden (nothing). box: x1,y1,x2,y2 of what was painted.
extern "C" __declspec(dllexport) int __cdecl AotrInFrameTest(void* dev, int mode, int* box) {
    InterlockedExchange(&g_ovIfWant, 1); g_ovIfDead = 0; g_ovIfOk = 0; g_ovIfNextChk = 0; g_ovIfSig = -1;
    ovIfPublish("AOTR V84 BUILD 134 RUNNING", "WINDOWS D3D9 / NVIDIA - F7 HIDES", "AOTR B134 FPS 29.5  LONGEST 45 MS  HITCHES 3", 295);
    LONG now = (LONG)GetTickCount();
    g_ovIfFirst = (mode == 0) ? now : now - 60000; g_ovIfBannerUntil = 0;
    g_ovShow = (mode <= 1) ? 1 : 0;
    g_ovIfMsgKind = (mode == 2) ? 2 : 0; g_ovIfMsgUntil = (mode == 2) ? now + 3000 : 0;
    g_ovIfBox.x1 = g_ovIfBox.y1 = g_ovIfBox.x2 = g_ovIfBox.y2 = 0; g_ovIfBg.n = 0;
    ovInFrameDraw(dev);
    if (box) { box[0] = g_ovIfBox.x1; box[1] = g_ovIfBox.y1; box[2] = g_ovIfBox.x2; box[3] = g_ovIfBox.y2; }
    return (int)(g_ovIfBg.n + g_ovIfBan.n + g_ovIfRen.n + g_ovIfLn.n + g_ovIfMsg.n);
}
extern "C" __declspec(dllexport) int __cdecl AotrHardFaults() { return (int)ovHardFaults(); }
static void installLoadProf();
static void profRefreshModules();
// work\ish4\stalltest2.exe: the v31 long-frame profiler on the Present counter (after AotrPresentHookTest + the panel)
extern "C" __declspec(dllexport) void __cdecl AotrLoadProfStartTest() {
    if (!p_NtQIT) p_NtQIT = (tNtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    profRefreshModules();
    installLoadProf();
}
extern "C" __declspec(dllexport) void __cdecl AotrGpuPrefTest() { installGpuPref(); }
extern "C" __declspec(dllexport) void __cdecl AotrFileWarmReadTest() { if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf); installFileWarm(true); }
extern "C" __declspec(dllexport) void __cdecl AotrFileWarmReadStat(int* n, int* kb, int* done) { *n = (int)g_fwNWarm; *kb = (int)(g_fwReadBytes >> 10); *done = (g_fwWalkDone && g_fwNext >= g_fwWorkN) ? 1 : 0; }
extern "C" __declspec(dllexport) int __cdecl AotrEngineInstallTest(char* out, int n) {   // game.dat mapped at its base (gamemap_test)
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    installRlSort(); installTerrain();
    if (out && n > 100) wsprintfA(out, "rlsort live %d push %d | terrain on %d helpers %d", (int)g_rlLive, (int)g_rlPushLive, (int)g_tpOn, (int)g_tpHelpers);
    return (g_rlLive && g_rlPushLive && g_tpOn) ? 0 : 1;
}
extern "C" __declspec(dllexport) const char* __cdecl AotrRendererLine() { g_ovRendererKnown = 0; ovDetectRenderer(); return g_ovRenderer; }
extern "C" __declspec(dllexport) const char* __cdecl AotrGpuNote() { return g_ovGpuNote; }
extern "C" __declspec(dllexport) void __cdecl AotrPanelStartTest() {   // after AotrPresentHookTest: the panel thread as in the game
    g_ovWantWin = 1; g_ovRepWant = 1; InterlockedExchange(&g_ovOn, 1); InterlockedExchange(&g_ovIfWant, 1);   // v31: as in the game
    if (CreateThread(NULL, 0, ovThread, NULL, 0, NULL)) CreateThread(NULL, 0, ovWatchThread, NULL, 0, NULL);
}
extern "C" __declspec(dllexport) int __cdecl AotrSessionTest(char* out, int n) {   // seconds 1..100 fps once each, 20 empty seconds
    memset(g_ovFpsHist, 0, sizeof(g_ovFpsHist)); g_ovFpsSecs = 0; g_ovFpsFrames = 0;
    for (int f = 1; f <= 100; ++f) { ++g_ovFpsHist[f]; ++g_ovFpsSecs; g_ovFpsFrames += f; }
    g_ovRepSec = 120; g_ovRepCpuSum = 150 * 120; g_ovRepCpuN = 120; g_ovBest5 = 987; g_ovSt100 = 3; g_ovRunWorst = 260;
    ++g_ovFpsHist[OV_FPSH - 1]; --g_ovFpsHist[100];          // one second over 300 fps is kept in the top bucket
    char line[1200]; ovSessionLine(line, "test");
    if (out && n > 0) lstrcpynA(out, line, n);
    return (ovFpsAt(50) == 50 && ovFpsAt(10) == 10 && ovFpsAt(100) == OV_FPSH - 1) ? 0 : 1;
}
#endif
