// aotr_fdbuf.inc - read-ahead for the engine's file reads (v52).
//
// WHAT WAS SLOW (long-frame profile of a save load, 2026-10-07): a quarter of the whole load - 9 s - was the game
// thread inside NtReadFile, called from LocalFile::read (game.dat 0x00A19AF0) under the W3D chunk reader
// (0x00A28E70). The engine's file class sits directly on the C runtime's low-level _read / _lseek, which do no
// buffering at all: every 4-byte count, every 8-byte chunk header, every channel of every animation is its own
// system call (and its own pair of lock operations inside msvcr71).
//
// WHAT THIS DOES: the game's import slots for _read, _lseek, _write, _close, _open, _wopen and _get_osfhandle point
// here. A file opened read-only in binary mode gets read-ahead windows (4 KB, growing to 64 KB while it is read
// forwards; up to four per file so that streams interleaved on one archive handle keep theirs); small reads are
// copies out of a window and seeks inside the file are arithmetic. The position the program sees is tracked here,
// the real handle is moved only when a window is filled. Everything else - text mode, files open for writing,
// devices, pipes, a handle the program asked the OS handle of - goes straight to msvcr71 as before.
//
// SAME BYTES, SAME RETURN VALUES: the offline test (fdbuf_test.cpp) runs random reads, seeks (all three origins,
// before the start and past the end), skips and end-of-file reads against msvcr71's own functions on a second
// handle of the same file and compares every result and every byte, single- and multi-threaded.
//
// The includer supplies FDB_LOG(fmt, ...) or nothing.
#ifndef FDB_LOG
#define FDB_LOG(...) ((void)0)
#endif
#define FDB_MAXFD  2048
#define FDB_WINMAX (64u << 10)
#define FDB_WINMIN (4u << 10)
#define FDB_NWIN   4
#define FDB_DIRECT (32u << 10)          // a read this large goes straight into the caller's buffer

typedef int  (__cdecl* FdbReadF)(int, void*, unsigned);
typedef long (__cdecl* FdbSeekF)(int, long, int);
typedef int  (__cdecl* FdbWriteF)(int, const void*, unsigned);
typedef int  (__cdecl* FdbCloseF)(int);
typedef int  (__cdecl* FdbWOpenF)(const wchar_t*, int, int);
typedef int  (__cdecl* FdbOpenF)(const char*, int, int);
typedef INT_PTR (__cdecl* FdbOsfF)(int);
typedef int* (__cdecl* FdbErrnoF)(void);
static FdbErrnoF fdb_o_errno = NULL;
static FdbReadF fdb_o_read = NULL; static FdbSeekF fdb_o_lseek = NULL; static FdbWriteF fdb_o_write = NULL; static FdbCloseF fdb_o_close = NULL;
static FdbWOpenF fdb_o_wopen = NULL; static FdbOpenF fdb_o_open = NULL; static FdbOsfF fdb_o_osf = NULL;

struct FdbWin { BYTE* mem; LONG pos, len; DWORD used, grow; };          // file offset of the first byte, valid bytes; mem is FDB_WINMAX long
struct FdbFd {
    LONG state;                 // 0 never seen (opened before the hooks were in), 1 buffered, 2 opened through the hooks as a candidate, 3 left to msvcr71
    LONG pos, rpos, size;       // where the program is, where the real handle is, how long the file is
    DWORD clock;
    FdbWin win[FDB_NWIN];
    // v34 (ISH-8) write buffer: wok 0 = not opened through the hooks for writing, 1 = may be buffered (checked at the
    // first write), 2 = buffering, 3 = never (left to msvcr71). wbuf holds wlen bytes the program has written that
    // are not in the file yet; they go out, in order, before anything else touches the handle.
    // (v34 round 2) the buffer is a window on the file: it holds the bytes of [wpos0, wpos0 + wlen); the real handle stands
    // at wpos0; the program stands at wcur (wpos0 <= wcur <= wpos0 + wlen); wsize is the file's size on the disk.
    LONG wok; BYTE* wbuf; DWORD wlen, wt0; HANDLE wh; DWORD wn, wsys; LONG64 wbytes; LONG wpos0, wcur, wsize; DWORD wseek; LONG64 wsinceCheck;
    DWORD wnPub, wseekPub; LONG64 wbytesPub;                           // what of wn / wseek / wbytes is already in the global counters
};
static FdbFd g_fdb[FDB_MAXFD];
static CRITICAL_SECTION g_fdbCs[64];
static volatile LONG g_fdbOn = 0;
static BYTE* volatile g_fdbFree = NULL;                                 // spare window buffers (first pointer-size bytes link them)
static CRITICAL_SECTION g_fdbPoolCs;
static volatile LONG g_fdbNRead = 0, g_fdbNReal = 0, g_fdbNSeek = 0, g_fdbNSeekReal = 0, g_fdbNAdopt = 0, g_fdbNDirect = 0, g_fdbNRaw = 0, g_fdbNWin = 0, g_fdbNLate = 0;
static LONG g_fdbNPass = 0;                                             // reads left to msvcr71 (a plain count, not exact across threads)
// A handle opened before the hooks were in (the archives the engine opens at start) never passed through fdb_wopen, so
// its mode is not known from the open. msvcr71 keeps it in its handle table, which it exports (__pioinfo: 64 blocks of
// 32 entries {OS handle, flags, ...}); the layout assumed here is checked against two files opened for the purpose
// before anything relies on it.
static void** g_fdbPio = NULL;
#define FDB_PIO_SIZE 36
static __forceinline const BYTE* fdbPio(int fd) { const BYTE* blk = g_fdbPio ? (const BYTE*)g_fdbPio[fd >> 5] : NULL; return blk ? blk + (fd & 31) * FDB_PIO_SIZE : NULL; }
static bool fdbLooksBinaryFile(int fd) {                                // open, not text, not append, not a device, not a pipe
    const BYTE* e = fdbPio(fd);
    if (!e) return false;
    BYTE of = e[sizeof(INT_PTR)];
    return (of & 0x01) && !(of & (0x80 | 0x40 | 0x20 | 0x08));
}
static volatile LONG64 g_fdbBytes = 0;

typedef LONG (NTAPI* FdbNtQif)(HANDLE, void*, void*, ULONG, int);
static FdbNtQif fdb_NtQif = NULL;

static BYTE* fdbWinAlloc() {
    EnterCriticalSection(&g_fdbPoolCs);
    BYTE* p = g_fdbFree; if (p) g_fdbFree = *(BYTE**)p;
    LeaveCriticalSection(&g_fdbPoolCs);
    if (!p) { p = (BYTE*)VirtualAlloc(NULL, FDB_WINMAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (p) InterlockedIncrement(&g_fdbNWin); }
    return p;
}
static void fdbWinFree(BYTE* p) {
    if (!p) return;
    EnterCriticalSection(&g_fdbPoolCs); *(BYTE**)p = g_fdbFree; g_fdbFree = p; LeaveCriticalSection(&g_fdbPoolCs);
}
static void fdbDrop(FdbFd* f, LONG to) {                                // lock held: forget everything about this handle
    for (int i = 0; i < FDB_NWIN; ++i) { fdbWinFree(f->win[i].mem); f->win[i].mem = NULL; f->win[i].len = 0; f->win[i].pos = -1; f->win[i].grow = 0; }
    f->state = to;
}
// lock held, state 2: is it a plain disk file we may read ahead of? (binary and read-only were seen at the open)
static bool fdbAdopt(int fd, FdbFd* f) {
    f->state = 3;
    INT_PTR h = fdb_o_osf(fd);
    if (h == -1 || h == -2 || GetFileType((HANDLE)h) != FILE_TYPE_DISK) return false;
    if (fdb_NtQif) { struct { ULONG_PTR st; ULONG_PTR info; } iosb = { 0, 0 }; ULONG access = 0;
        if (fdb_NtQif((HANDLE)h, &iosb, &access, sizeof(access), 8) != 0 || (access & 0x6)) return false; }      // FileAccessInformation: no FILE_WRITE_DATA / FILE_APPEND_DATA
    long cur = fdb_o_lseek(fd, 0, 1); if (cur < 0) return false;
    long size = fdb_o_lseek(fd, 0, 2); if (size < 0) { fdb_o_lseek(fd, cur, 0); return false; }
    if (fdb_o_lseek(fd, cur, 0) != cur) return false;
    f->pos = f->rpos = cur; f->size = size; f->clock = 0;
    for (int i = 0; i < FDB_NWIN; ++i) { f->win[i].mem = NULL; f->win[i].len = 0; f->win[i].pos = -1; f->win[i].used = 0; f->win[i].grow = 0; }
    f->state = 1;
    InterlockedIncrement(&g_fdbNAdopt);
    return true;
}
static __forceinline void fdbSync(int fd, FdbFd* f) {                   // lock held: put the real handle where the program believes it is
    if (f->rpos != f->pos) { long r = fdb_o_lseek(fd, f->pos, 0); f->rpos = r < 0 ? -1 : r; InterlockedIncrement(&g_fdbNSeekReal); }
}
static BOOL fdbCopyOut(void* dst, const BYTE* src, DWORD n) { __try { memcpy(dst, src, n); return TRUE; } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; } }

// [OUR FORK] how much of the time went to the disk. A real read or an open that took this long or
// more is counted, with its total and the worst one: the read-out (aotr_overlay.inc) shows them,
// which is how a stutter caused by the drive is told apart from one caused by the processor.
static volatile LONG g_fdbSlowN = 0, g_fdbSlowMs = 0, g_fdbSlowWorst = 0;
#define FDB_SLOW_MS 50
// v33 (ISH-7): which file it was - every slow read or open is named in the log (at most 30 a minute), so a "disk"
// stall says which file kept the game waiting.
static char g_fdbName[FDB_MAXFD][72];
static volatile LONG g_fdbSlowLogged = 0; static volatile DWORD g_fdbSlowMinute = 0;
static void fdbNameOf(char* out, const char* a, const wchar_t* w) {
    char full[MAX_PATH]; full[0] = 0;
    if (a) lstrcpynA(full, a, MAX_PATH); else if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, full, MAX_PATH, NULL, NULL);
    int n = lstrlenA(full); const char* p = n > 70 ? full + n - 70 : full;
    lstrcpynA(out, p, 72);
}
static void fdbNoteSlow(LONG64 ticks, int fd = -1, const char* what = NULL, const char* an = NULL, const wchar_t* wn = NULL, unsigned bytes = 0) {
    LONG64 hz = qpcHz();
    LONG ms = (LONG)((ticks * 1000) / (hz ? hz : 1));
    if (ms >= FDB_SLOW_MS) {
        InterlockedIncrement(&g_fdbSlowN);
        InterlockedExchangeAdd(&g_fdbSlowMs, ms);
        if (ms > g_fdbSlowWorst) InterlockedExchange(&g_fdbSlowWorst, ms);
        DWORD minute = GetTickCount() / 60000;
        if (minute != g_fdbSlowMinute) { g_fdbSlowMinute = minute; g_fdbSlowLogged = 0; }
        if (what && InterlockedIncrement(&g_fdbSlowLogged) <= 30) {
            char nm[72]; nm[0] = 0;
            if (an || wn) fdbNameOf(nm, an, wn); else if ((unsigned)fd < FDB_MAXFD) lstrcpynA(nm, g_fdbName[fd], 72);
            logf("disk: a slow %s - %d ms, %s%s (%u bytes, on thread %u%s)", what, (int)ms, nm[0] ? "..." : "", nm[0] ? nm : "(file name not known)", bytes,
                 GetCurrentThreadId(), GetCurrentThreadId() == g_frTidEarly ? ", the GAME thread" : "");
        }
    }
}
static int fdbRealRead(int fd, void* buf, unsigned n) {
    LONG64 t0 = qpcNow();
    int r = fdb_o_read(fd, buf, n);
    fdbNoteSlow(qpcNow() - t0, fd, "read", NULL, NULL, n);
    return r;
}

// ---------------------------------------------------------------- v34 (ISH-8): write buffering
// WHAT WAS SLOW (test-14 log, 3 stalls of 18.6 / 20.3 / 20.7 s): saving a skirmish (test*.BfME2Skirmish, 25-27 MB,
// written at exactly those times) - LocalFile::write (game.dat 0xA19B2D) is a bare _write(fd, buf, n) and the
// engine's Xfer writes the save a few bytes at a time (0xA20615: a 4-byte length, then the data), so a save was
// millions of NtWriteFile calls on the game thread (96 % of the samples of those frames). It also asks where it is
// and seeks back to fill in block sizes (LocalFile::seek 0xA19B56 -> _lseek) - in the test of 2026-10-23 8.8 million
// writes and about as many seeks for one 27 MB save.
// WHAT THIS DOES: a file opened through the hooks for writing, in binary mode, not append, on a disk, gets a 1 MB
// window: the window holds the bytes of the file from wpos0 on that the program has written and that are not on the
// disk yet. A write at the program's position inside or at the end of the window goes into it; a seek (or "where am
// I") that lands inside the window is answered here. Anything else - a write or seek outside the window, a read, a
// close, a request for the OS handle (the program may use it directly), a window a second old - first writes the
// window out with one _write at wpos0 and puts the real handle where the program is, then does what was asked through
// msvcr71. So every byte reaches the file at the offset the program wrote it to, later writes over earlier ones, and
// the file is the same byte for byte once closed (offline test fdbwrite_test.cpp: SHA-256 equal, every return value
// of every write / seek / read / close equal, also with the save's write-and-patch pattern; in the game a self-test
// against msvcr71 on two temp files runs before it is switched on).
// RETURN VALUES: msvcr71's binary _write on a disk file returns n when it writes everything, and a seek returns the
// new position - so do these. A write the disk would refuse cannot be told in advance, so a file is only buffered
// while its volume has 256 MB or more free (checked at the first write and after every 64 MB written); below that
// every call goes to msvcr71 as before. If writing the window out still fails, the call that caused it (or the close)
// returns -1 with msvcr71's errno and the file is left to msvcr71 from then on. Not used in a multiplayer game's
// logic: writing is file I/O, the simulation never sees it.
#define FDB_WCAP (1u << 20)
#define FDB_WFREE_MIN (256ull << 20)
static volatile LONG g_fdbWOn = 0;
static volatile LONG g_fdbWFiles = 0, g_fdbWErr = 0, g_fdbWLow = 0;
static volatile LONG g_fdbWN = 0, g_fdbWSys = 0, g_fdbWSeekHere = 0, g_fdbWSeekReal = 0; static volatile LONG64 g_fdbWBytes = 0;
static volatile LONG64 g_fdbWTicks = 0;                                 // time spent in the real writes and seeks
typedef LONG (NTAPI* FdbNtQvif)(HANDLE, void*, void*, ULONG, int);
static FdbNtQvif fdb_NtQvif = NULL;
static bool fdbVolumeHasRoom(HANDLE h) {
    if (!fdb_NtQvif) return false;
    struct { LONG64 total, callerAvail, actualAvail; ULONG secPerUnit, bytesPerSec; } fs = { 0, 0, 0, 0, 0 };
    struct { ULONG_PTR st; ULONG_PTR info; } iosb = { 0, 0 };
    if (fdb_NtQvif(h, &iosb, &fs, sizeof(fs), 7) != 0) return false;       // FileFsFullSizeInformation
    unsigned long long freeB = (unsigned long long)fs.callerAvail * fs.secPerUnit * fs.bytesPerSec;
    if (freeB < FDB_WFREE_MIN) { InterlockedIncrement(&g_fdbWLow); return false; }
    return true;
}
static __forceinline LONG fdbWEnd(const FdbFd* f) { LONG e = f->wpos0 + (LONG)f->wlen; return e > f->wsize ? e : f->wsize; }   // the file's size as the program sees it
// lock held, wok 2: the window goes out at wpos0 with msvcr71's own _write and the real handle is put where the program
// is; after it the window is empty and starts there. false if msvcr71 did not take it all (the file is then left to it).
static __forceinline void fdbWPublish(FdbFd* f) {                     // the per-file counts into the global ones (the hot path keeps no locked counters)
    if (f->wn != f->wnPub) { InterlockedExchangeAdd(&g_fdbWN, (LONG)(f->wn - f->wnPub)); f->wnPub = f->wn; }
    if (f->wseek != f->wseekPub) { InterlockedExchangeAdd(&g_fdbWSeekHere, (LONG)(f->wseek - f->wseekPub)); f->wseekPub = f->wseek; }
    if (f->wbytes != f->wbytesPub) { InterlockedExchangeAdd64(&g_fdbWBytes, f->wbytes - f->wbytesPub); f->wbytesPub = f->wbytes; }
}
static bool fdbWFlush(int fd, FdbFd* f) {
    if (f->wok != 2) return true;
    fdbWPublish(f);
    LONG64 t0 = qpcNow();
    if (f->wlen) {
        DWORD n = f->wlen; f->wlen = 0;
        int r = fdb_o_write(fd, f->wbuf, n);
        InterlockedIncrement(&g_fdbWSys); f->wsys++;
        if (r != (int)n) {
            InterlockedExchangeAdd64(&g_fdbWTicks, qpcNow() - t0); InterlockedIncrement(&g_fdbWErr);
            FDB_LOG("files: a buffered write did NOT go out whole (%d of %u bytes, errno %d) - this file goes back to msvcr71 unbuffered; the call that caused it returns -1", r, n, fdb_o_errno ? *fdb_o_errno() : -1);
            f->wok = 3;
            return false;
        }
        LONG e = f->wpos0 + (LONG)n; if (e > f->wsize) f->wsize = e;
        if (f->wcur != e) {                                             // the program stands inside what was written
            long q = fdb_o_lseek(fd, f->wcur, 0); InterlockedIncrement(&g_fdbWSeekReal);
            if (q != f->wcur) { InterlockedIncrement(&g_fdbWErr); f->wok = 3; FDB_LOG("files: putting a written file's handle back to %d gave %d - this file goes back to msvcr71 unbuffered", (int)f->wcur, (int)q); return false; }
        }
        f->wsinceCheck += n;
        if (f->wsinceCheck >= (64ll << 20)) { f->wsinceCheck = 0; if (!fdbVolumeHasRoom(f->wh)) f->wok = 3; }   // the disk is getting full: no more buffering for this file
    }
    f->wpos0 = f->wcur;
    LONG64 dt = qpcNow() - t0;
    InterlockedExchangeAdd64(&g_fdbWTicks, dt);
    fdbNoteSlow(dt, fd, "write", NULL, NULL, 0);
    return true;
}
// lock held: the window goes out and the file stays unbuffered for good (the program wants the OS handle)
static bool fdbWStop(int fd, FdbFd* f) {
    bool ok = fdbWFlush(fd, f);
    if (f->wok) f->wok = 3;
    return ok;
}
// no lock, last resort (the process is ending or crashing): straight to the OS handle, which stands at wpos0 - exactly
// where msvcr71's _write would have put these bytes (binary, not append: _write is WriteFile at the current position)
static void fdbWFlushAllNoLock(bool tryLock) {
    for (int fd = 0; fd < FDB_MAXFD; ++fd) {
        FdbFd* f = &g_fdb[fd];
        if (f->wok != 2 || !f->wlen || !f->wbuf || !f->wh) continue;
        CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
        bool locked = tryLock && TryEnterCriticalSection(cs);
        if (tryLock && !locked) continue;
        DWORD n = f->wlen, done = 0; f->wlen = 0;
        while (done < n) { DWORD w = 0; if (!WriteFile(f->wh, f->wbuf + done, n - done, &w, NULL) || !w) break; done += w; }
        if (done == n) { LONG e = f->wpos0 + (LONG)n; if (e > f->wsize) f->wsize = e; LARGE_INTEGER li; li.QuadPart = f->wcur; SetFilePointerEx(f->wh, li, NULL, FILE_BEGIN); f->wpos0 = f->wcur; }
        if (locked) LeaveCriticalSection(cs);
    }
}
// the reporting thread: windows older than a second go out (so a crash or a kill loses nothing older than that)
static void fdbWFlushOld() {
    DWORD now = GetTickCount();
    for (int fd = 0; fd < FDB_MAXFD; ++fd) {
        FdbFd* f = &g_fdb[fd];
        if (f->wok != 2 || (!f->wlen && f->wn == f->wnPub && f->wseek == f->wseekPub) || (f->wlen && now - f->wt0 < 1000)) continue;
        CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
        if (!TryEnterCriticalSection(cs)) continue;
        if (f->wok == 2) { if (f->wlen && now - f->wt0 >= 1000) fdbWFlush(fd, f); else if (!f->wlen) fdbWPublish(f); }
        LeaveCriticalSection(cs);
    }
}
static BOOL fdbCopyIn(BYTE* dst, const void* src, DWORD n) { __try { memcpy(dst, src, n); return TRUE; } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; } }
static __forceinline bool fdbWIs(int fd) { return (unsigned)fd < FDB_MAXFD && g_fdb[fd].wok == 2; }
// v34: a read of a file whose writes are collected (opened read-write): the window first, then msvcr71's read from where
// the program is, and the window starts again where the read ended
static bool fdbWRead(int fd, void* buf, unsigned n, int* res) {
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->wok != 2) { LeaveCriticalSection(cs); return false; }
    if (!fdbWFlush(fd, f)) { LeaveCriticalSection(cs); *res = -1; return true; }
    int r = fdbRealRead(fd, buf, n);
    if (r > 0) { f->wcur += r; f->wpos0 = f->wcur; }
    else if (r < 0) { long c = fdb_o_lseek(fd, 0, 1); if (c >= 0) { f->wcur = f->wpos0 = c; } }
    LeaveCriticalSection(cs);
    *res = r; return true;
}
// v34: a seek of a file whose writes are collected: inside the window it is answered here; otherwise the window goes out
// and msvcr71 seeks with the same arguments (its "current" and "end" are then the program's, so its answer is the one
// it would have given without the window)
static bool fdbWSeek(int fd, long off, int whence, long* res) {
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->wok != 2) { LeaveCriticalSection(cs); return false; }
    if ((unsigned)whence <= 2) {
        LONGLONG t = (LONGLONG)off + (whence == 0 ? 0 : whence == 1 ? f->wcur : fdbWEnd(f));
        if (t >= f->wpos0 && t <= (LONGLONG)f->wpos0 + f->wlen) { f->wcur = (LONG)t; f->wseek++; LeaveCriticalSection(cs); *res = (long)t; return true; }
    }
    if (!fdbWFlush(fd, f)) { LeaveCriticalSection(cs); *res = -1L; return true; }
    long r = fdb_o_lseek(fd, off, whence); InterlockedIncrement(&g_fdbWSeekReal);
    if (r >= 0) { f->wcur = f->wpos0 = r; if (r > f->wsize) { /* past the end: the size changes only when written */ } }
    LeaveCriticalSection(cs);
    *res = r; return true;
}

static int __cdecl fdb_read(int fd, void* buf, unsigned n) {
    { int wr; if (fdbWIs(fd) && fdbWRead(fd, buf, n, &wr)) return wr; }    // v34: a file whose writes are collected
    if ((unsigned)fd >= FDB_MAXFD || g_fdb[fd].state == 3) { g_fdbNPass++; return fdbRealRead(fd, buf, n); }
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->state == 0) { if (g_fdbOn && fdbLooksBinaryFile(fd) && fdbAdopt(fd, f)) InterlockedIncrement(&g_fdbNLate); else f->state = 3; }
    else if (f->state == 2) fdbAdopt(fd, f);
    if (f->state != 1) { LeaveCriticalSection(cs); g_fdbNPass++; return fdbRealRead(fd, buf, n); }
    if (!n || !buf || f->pos < 0 || n >= FDB_DIRECT || f->pos >= f->size) {
        // nothing to copy, a bad buffer, a large read or the end of the file: msvcr71 does it, from the right place
        fdbSync(fd, f);
        int r = fdbRealRead(fd, buf, n);
        if (r > 0) { f->pos += r; f->rpos = f->pos; if (f->pos > f->size) f->size = f->pos; InterlockedIncrement(&g_fdbNDirect); }
        else if (r < 0) { long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; if (c >= 0) f->pos = c; }
        InterlockedIncrement(&g_fdbNReal);
        LeaveCriticalSection(cs);
        return r;
    }
    unsigned done = 0; BYTE* out = (BYTE*)buf;
    while (done < n) {
        LONG p = f->pos; FdbWin* w = NULL;
        for (int i = 0; i < FDB_NWIN; ++i) { FdbWin* c = &f->win[i]; if (c->mem && p >= c->pos && p < c->pos + c->len) { w = c; break; } }
        if (!w) {
            if (p >= f->size) break;                                    // the end of the file
            // a window for this place: the one that ended exactly here grows (read forwards), otherwise the least recently used starts small
            FdbWin* v = NULL; DWORD want = FDB_WINMIN;
            for (int i = 0; i < FDB_NWIN; ++i) { FdbWin* c = &f->win[i]; if (c->mem && c->len > 0 && c->pos + c->len == p) { v = c; want = c->grow >= FDB_WINMAX / 4 ? FDB_WINMAX : (c->grow ? c->grow * 4 : FDB_WINMIN * 4); break; } }
            if (!v) { for (int i = 0; i < FDB_NWIN; ++i) { FdbWin* c = &f->win[i]; if (!v || (!c->mem && v->mem) || (!!c->mem == !!v->mem && c->used < v->used)) v = c; } }
            if (!v->mem) { v->mem = fdbWinAlloc(); if (!v->mem) goto rest_direct; }
            if (want > FDB_WINMAX) want = FDB_WINMAX;
            if ((LONGLONG)p + want > f->size) want = (DWORD)(f->size - p);
            if (f->rpos != p) { long r = fdb_o_lseek(fd, p, 0); f->rpos = r < 0 ? -1 : r; InterlockedIncrement(&g_fdbNSeekReal); if (r != p) { v->len = 0; v->pos = -1; goto rest_direct; } }
            int got = fdbRealRead(fd, v->mem, want);
            InterlockedIncrement(&g_fdbNReal);
            if (got < 0) { v->len = 0; v->pos = -1; long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; LeaveCriticalSection(cs); return done ? (int)done : -1; }
            if (got == 0) { v->len = 0; v->pos = -1; f->rpos = p; f->size = p; break; }
            f->rpos = p + got; v->pos = p; v->len = got; v->grow = want;
            if ((DWORD)got < want) f->size = p + got;                   // shorter than it said: that is the end
            w = v;
        }
        w->used = ++f->clock;
        DWORD off = (DWORD)(p - w->pos), take = (DWORD)w->len - off;
        if (take > n - done) take = n - done;
        if (!fdbCopyOut(out + done, w->mem + off, take)) { LeaveCriticalSection(cs); return done ? (int)done : -1; }
        done += take; f->pos += (LONG)take;
    }
    InterlockedIncrement(&g_fdbNRead); InterlockedExchangeAdd64(&g_fdbBytes, done);
    LeaveCriticalSection(cs);
    return (int)done;
rest_direct:                                                            // no window to be had: msvcr71 reads the rest itself
    {
        fdbSync(fd, f);
        int r = fdbRealRead(fd, out + done, n - done);
        InterlockedIncrement(&g_fdbNReal);
        if (r > 0) { f->pos += r; f->rpos = f->pos; done += (unsigned)r; }
        else { long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; if (r < 0 && !done) { LeaveCriticalSection(cs); return -1; } }
        LeaveCriticalSection(cs);
        return (int)done;
    }
}
static long __cdecl fdb_lseek(int fd, long off, int whence) {
    { long wr; if (fdbWIs(fd) && fdbWSeek(fd, off, whence, &wr)) return wr; }   // v34: a file whose writes are collected
    if ((unsigned)fd >= FDB_MAXFD || g_fdb[fd].state != 1) return fdb_o_lseek(fd, off, whence);      // not read ahead of (yet): the real handle is the truth
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->state != 1) { LeaveCriticalSection(cs); return fdb_o_lseek(fd, off, whence); }
    if ((unsigned)whence <= 2 && f->pos >= 0) {
        LONGLONG t = (LONGLONG)off + (whence == 0 ? 0 : whence == 1 ? f->pos : f->size);
        if (t >= 0 && t <= f->size) { f->pos = (LONG)t; InterlockedIncrement(&g_fdbNSeek); LeaveCriticalSection(cs); return (long)t; }   // inside the file: nothing to ask the system
    }
    fdbSync(fd, f);                                                     // outside it (or an origin msvcr71 will reject): its answer, from the right place
    long r = fdb_o_lseek(fd, off, whence);
    InterlockedIncrement(&g_fdbNSeekReal);
    if (r >= 0) { f->pos = f->rpos = r; }
    else { long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; if (c >= 0) f->pos = c; }
    LeaveCriticalSection(cs);
    return r;
}
static void fdbGoRaw(int fd) {                                          // from here on msvcr71 alone: the real handle is where the program is
    if ((unsigned)fd >= FDB_MAXFD || (g_fdb[fd].state == 3 && (g_fdb[fd].wok == 0 || g_fdb[fd].wok == 3))) return;
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->wok == 2) fdbWStop(fd, f);                                    // v34: buffered writes first, and no more buffering
    if (f->state == 1) { fdbSync(fd, f); InterlockedIncrement(&g_fdbNRaw); }
    fdbDrop(f, 3);
    LeaveCriticalSection(cs);
}
// v34: lock held, wok 1: is this a file whose writes may be collected? Binary, not append, not a device or a pipe (from
// msvcr71's own handle table), a disk file, and room on its volume. The window starts where the handle stands.
static bool fdbWAdopt(int fd, FdbFd* f) {
    f->wok = 3;
    if (!g_fdbWOn || !fdbLooksBinaryFile(fd)) return false;
    INT_PTR h = fdb_o_osf(fd);
    if (h == -1 || h == -2 || GetFileType((HANDLE)h) != FILE_TYPE_DISK || !fdbVolumeHasRoom((HANDLE)h)) return false;
    if (f->state == 1) { fdbSync(fd, f); fdbDrop(f, 3); }                 // (a read-write file read ahead of: the real handle where the program is)
    long cur = fdb_o_lseek(fd, 0, 1); LARGE_INTEGER sz;
    if (cur < 0 || !GetFileSizeEx((HANDLE)h, &sz) || sz.QuadPart > 0x7FF00000) return false;
    if (!f->wbuf) f->wbuf = (BYTE*)VirtualAlloc(NULL, FDB_WCAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!f->wbuf) return false;
    f->state = 3;
    f->wh = (HANDLE)h; f->wlen = 0; f->wn = 0; f->wsys = 0; f->wbytes = 0; f->wseek = 0; f->wsinceCheck = 0; f->wnPub = 0; f->wseekPub = 0; f->wbytesPub = 0;
    f->wpos0 = f->wcur = (LONG)cur; f->wsize = (LONG)sz.QuadPart;
    f->wok = 2;
    InterlockedIncrement(&g_fdbWFiles);
    return true;
}
static int __cdecl fdb_write(int fd, const void* buf, unsigned n) {
    if ((unsigned)fd >= FDB_MAXFD || (g_fdb[fd].wok != 1 && g_fdb[fd].wok != 2)) { fdbGoRaw(fd); return fdb_o_write(fd, buf, n); }
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->wok == 1) fdbWAdopt(fd, f);
    if (f->wok != 2) {                                                  // not to be buffered after all: as before
        if (f->state == 1) { fdbSync(fd, f); InterlockedIncrement(&g_fdbNRaw); }
        fdbDrop(f, 3);
        LeaveCriticalSection(cs);
        return fdb_o_write(fd, buf, n);
    }
    f->wn++;
    DWORD at = (DWORD)(f->wcur - f->wpos0);                              // where in the window the program stands
    if (!n || !buf || n >= FDB_WCAP / 2) {                              // nothing, no buffer, or big: in order, straight through msvcr71
        if (!fdbWFlush(fd, f)) { LeaveCriticalSection(cs); return -1; }
        LONG64 t0 = qpcNow();
        int r = fdb_o_write(fd, buf, n);
        InterlockedExchangeAdd64(&g_fdbWTicks, qpcNow() - t0); InterlockedIncrement(&g_fdbWSys); f->wsys++;
        if (r > 0) { f->wbytes += r; f->wcur += r; f->wpos0 = f->wcur; if (f->wcur > f->wsize) f->wsize = f->wcur; }
        else if (r < 0) { long c = fdb_o_lseek(fd, 0, 1); if (c >= 0) f->wcur = f->wpos0 = c; }
        LeaveCriticalSection(cs);
        return r;
    }
    if (at + n > FDB_WCAP) {                                            // does not fit: the window goes out and starts here
        if (!fdbWFlush(fd, f)) { LeaveCriticalSection(cs); return -1; }
        if (f->wok != 2) { LeaveCriticalSection(cs); return fdb_o_write(fd, buf, n); }   // (the disk is getting full: this one through msvcr71)
        at = 0;
    }
    if (!fdbCopyIn(f->wbuf + at, buf, n)) {                             // a buffer the program cannot read: msvcr71's answer, in order
        bool ok = fdbWFlush(fd, f);
        int r = ok ? fdb_o_write(fd, buf, n) : -1;
        if (ok) { long c = fdb_o_lseek(fd, 0, 1); if (c >= 0) { f->wcur = f->wpos0 = c; if (c > f->wsize) f->wsize = c; } }
        LeaveCriticalSection(cs);
        return r;
    }
    if (!f->wlen) f->wt0 = GetTickCount();
    f->wcur += (LONG)n;
    if (at + n > f->wlen) f->wlen = at + n;
    f->wbytes += n;
    LeaveCriticalSection(cs);
    return (int)n;
}
static INT_PTR __cdecl fdb_get_osfhandle(int fd) { fdbGoRaw(fd); return fdb_o_osf(fd); }
static int __cdecl fdb_close(int fd) {
    bool wfail = false;
    if ((unsigned)fd < FDB_MAXFD && (g_fdb[fd].state || g_fdb[fd].wok)) {
        CRITICAL_SECTION* cs = &g_fdbCs[fd & 63]; EnterCriticalSection(cs);
        FdbFd* f = &g_fdb[fd];
        if (f->wok == 2) {                                              // v34: everything written goes out before the close
            wfail = !fdbWFlush(fd, f);
            if (f->wn >= 1000) FDB_LOG("files: closed ...%s - %u writes (%d KB) and %u seeks answered from the write window; %u system writes%s", g_fdbName[fd], f->wn, (int)(f->wbytes >> 10), f->wseek, f->wsys, wfail ? " - the LAST ONE FAILED" : "");
        }
        if (f->wbuf) { VirtualFree(f->wbuf, 0, MEM_RELEASE); f->wbuf = NULL; }
        f->wok = 0; f->wlen = 0; f->wh = NULL;
        fdbDrop(f, 3);                                                  // the number means nothing until the next open
        LeaveCriticalSection(cs);
    }
    int r = fdb_o_close(fd);
    return wfail ? -1 : r;
}
// _O_RDONLY 0, _O_WRONLY 1, _O_RDWR 2, _O_APPEND 8, _O_TEMPORARY 0x40, _O_CREAT 0x100, _O_TRUNC 0x200, _O_EXCL 0x400, _O_TEXT 0x4000, _O_BINARY 0x8000
static void fdbOpened(int fd, int oflag) {
    if ((unsigned)fd >= FDB_MAXFD) return;
    CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    fdbDrop(&g_fdb[fd], 3);                                             // whatever this number meant before
    if (g_fdbOn && (oflag & 0x8000) && !(oflag & (0x1 | 0x2 | 0x8 | 0x40 | 0x100 | 0x200 | 0x4000))) g_fdb[fd].state = 2;
    // v34: open for writing, binary, not append, not text, not temporary: its writes may be collected (checked at the first write)
    { FdbFd* f = &g_fdb[fd];
      if (f->wlen) FDB_LOG("files: fd %d was opened again with %u written bytes still buffered (it was closed past the hooks) - they are dropped, not written into the new file", fd, f->wlen);
      f->wlen = 0; f->wh = NULL;
      f->wok = (g_fdbWOn && (oflag & 0x8000) && (oflag & (0x1 | 0x2)) && !(oflag & (0x8 | 0x40 | 0x4000))) ? 1 : 0; }
    LeaveCriticalSection(cs);
}
// (v58) every open is timed and told to whoever wants to know (aotr_filewarm.inc: which files the game opens, and how
// long the first open of each took - the virus scanner's look)
static void (*g_fdbNoteOpen)(const char* a, const wchar_t* w, int oflag, LONG64 ticks) = NULL;
static int __cdecl fdb_wopen(const wchar_t* name, int oflag, int pmode) {
    void (*note)(const char*, const wchar_t*, int, LONG64) = g_fdbNoteOpen;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    int fd = fdb_o_wopen(name, oflag, pmode);
    QueryPerformanceCounter(&t1);
    fdbNoteSlow(t1.QuadPart - t0.QuadPart, fd, "open", NULL, name, 0);   // [OUR FORK] a slow open is a stutter too
    if ((unsigned)fd < FDB_MAXFD) fdbNameOf(g_fdbName[fd], NULL, name);
    if (note && fd >= 0 && name) note(NULL, name, oflag, t1.QuadPart - t0.QuadPart);
    if (fd >= 0) fdbOpened(fd, oflag);
    return fd;
}
static int __cdecl fdb_open(const char* name, int oflag, int pmode) {
    void (*note)(const char*, const wchar_t*, int, LONG64) = g_fdbNoteOpen;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    int fd = fdb_o_open(name, oflag, pmode);
    QueryPerformanceCounter(&t1);
    fdbNoteSlow(t1.QuadPart - t0.QuadPart, fd, "open", name, NULL, 0);   // [OUR FORK] ditto
    if ((unsigned)fd < FDB_MAXFD) fdbNameOf(g_fdbName[fd], name, NULL);
    if (note && fd >= 0 && name) note(name, NULL, oflag, t1.QuadPart - t0.QuadPart);
    if (fd >= 0) fdbOpened(fd, oflag);
    return fd;
}

static bool fdbInit(HMODULE crt) {
    static LONG done = 0;
    if (InterlockedExchange(&done, 1)) return fdb_o_read != NULL;
    for (int i = 0; i < 64; ++i) InitializeCriticalSection(&g_fdbCs[i]);
    InitializeCriticalSection(&g_fdbPoolCs);
    for (int i = 0; i < FDB_MAXFD; ++i) for (int k = 0; k < FDB_NWIN; ++k) g_fdb[i].win[k].pos = -1;
    FdbReadF r = (FdbReadF)GetProcAddress(crt, "_read"); FdbSeekF s = (FdbSeekF)GetProcAddress(crt, "_lseek"); FdbWriteF w = (FdbWriteF)GetProcAddress(crt, "_write");
    FdbCloseF c = (FdbCloseF)GetProcAddress(crt, "_close"); FdbWOpenF wo = (FdbWOpenF)GetProcAddress(crt, "_wopen"); FdbOpenF o = (FdbOpenF)GetProcAddress(crt, "_open");
    FdbOsfF h = (FdbOsfF)GetProcAddress(crt, "_get_osfhandle");
    if (!r || !s || !w || !c || !wo || !o || !h) return false;
    fdb_o_lseek = s; fdb_o_write = w; fdb_o_close = c; fdb_o_wopen = wo; fdb_o_open = o; fdb_o_osf = h;
    fdb_NtQif = (FdbNtQif)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationFile");
    fdb_NtQvif = (FdbNtQvif)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryVolumeInformationFile");
    fdb_o_errno = (FdbErrnoF)GetProcAddress(crt, "_errno");
    fdb_o_read = r;
    // msvcr71's handle table, as far as it can be trusted: this DLL's own file opened in binary and in text mode
    void** pio = (void**)GetProcAddress(crt, "__pioinfo");
    wchar_t self[MAX_PATH];
    if (pio && GetModuleFileNameW(crt, self, MAX_PATH)) {
        int fb = wo(self, 0x8000, 0), ft = wo(self, 0x4000, 0);
        if (fb >= 0 && ft >= 0 && fb < FDB_MAXFD && ft < FDB_MAXFD) {
            g_fdbPio = pio;
            const BYTE* eb = fdbPio(fb); const BYTE* et = fdbPio(ft);
            bool ok = eb && et && *(const INT_PTR*)eb == h(fb) && *(const INT_PTR*)et == h(ft) && h(fb) != -1 && h(ft) != -1
                      && (eb[sizeof(INT_PTR)] & 0x81) == 0x01 && (et[sizeof(INT_PTR)] & 0x81) == 0x81 && fdbLooksBinaryFile(fb) && !fdbLooksBinaryFile(ft);
            if (!ok) g_fdbPio = NULL;
        }
        if (fb >= 0) c(fb);
        if (ft >= 0) c(ft);
    }
    return true;
}

// A short walk over one file through the layer and through msvcr71 side by side, where the layer runs (the game
// process): reads of 1..600 bytes and seeks from all three origins, every result and byte compared.
static bool fdbSelfTest(const wchar_t* path) {
    int a = fdb_wopen(path, 0x8000, 0), b = fdb_o_wopen(path, 0x8000, 0);
    bool ok = a >= 0 && b >= 0;
    static BYTE x[640], y[640]; unsigned seed = 12345;
    for (int i = 0; i < 600 && ok; ++i) {
        seed = seed * 1103515245u + 12345u; unsigned k = (seed >> 16) & 0x7FFF;
        if (k % 5 == 0) {
            int wh = (int)((k >> 3) % 3); long off = wh == 2 ? -(long)(k % 9000) : wh == 1 ? (long)(k % 4000) - 1500 : (long)((k * 37u) % 300000u) - 40;
            ok = fdb_lseek(a, off, wh) == fdb_o_lseek(b, off, wh);
        } else {
            unsigned n = 1 + k % 600; int ra = fdb_read(a, x, n), rb = fdb_o_read(b, y, n);
            ok = ra == rb && (ra <= 0 || memcmp(x, y, (size_t)ra) == 0);
        }
    }
    ok = ok && fdb_lseek(a, 0, 1) == fdb_o_lseek(b, 0, 1) && g_fdb[a].state == 1;
    if (a >= 0) fdb_close(a);
    if (b >= 0) fdb_o_close(b);
    return ok;
}
static void fdbAllRaw() { for (int fd = 0; fd < FDB_MAXFD; ++fd) fdbGoRaw(fd); }

// v34: the write buffer against msvcr71 itself, in this process: two files in %TEMP%, the same random writes (1 byte to
// 200 KB), seeks, reads (read-write file), a request for the OS handle half way; every return value and, after the
// close, every byte compared. Both files are deleted afterwards.
static bool fdbWSelfTest(int* nOps) {
    wchar_t dir[MAX_PATH], pa[MAX_PATH], pb[MAX_PATH];
    DWORD dl = GetTempPathW(MAX_PATH - 40, dir); if (!dl || dl >= MAX_PATH - 40) return false;
    wsprintfW(pa, L"%saotr_wt_%u_a.tmp", dir, GetCurrentProcessId()); wsprintfW(pb, L"%saotr_wt_%u_b.tmp", dir, GetCurrentProcessId());
    bool ok = true; int ops = 0;
    for (int round = 0; round < 2 && ok; ++round) {
        int fl = 0x8000 | 0x100 | 0x200 | (round == 0 ? 0x1 : 0x2);     // binary, create, truncate; write-only, then read-write
        int a = fdb_wopen(pa, fl, 0x180), b = fdb_o_wopen(pb, fl, 0x180);
        if (a < 0 || b < 0) { if (a >= 0) fdb_close(a); if (b >= 0) fdb_o_close(b); ok = false; break; }
        BYTE* src = (BYTE*)VirtualAlloc(NULL, 256 << 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        BYTE* ra = (BYTE*)VirtualAlloc(NULL, 64 << 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        BYTE* rb = (BYTE*)VirtualAlloc(NULL, 64 << 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!src || !ra || !rb) ok = false;
        unsigned seed = 0x9E3779B9u + round;
        for (int i = 0; ok && i < (256 << 10); ++i) { seed = seed * 1103515245u + 12345u; src[i] = (BYTE)(seed >> 16); }
        for (int i = 0; ok && i < 3000; ++i, ++ops) {
            seed = seed * 1103515245u + 12345u; unsigned k = (seed >> 8) % 100, v = seed >> 12;
            if (k < 70) { unsigned n = k < 50 ? 1 + v % 8 : k < 66 ? 1 + v % 4000 : (v % 200000); ok = fdb_write(a, src + (v % 1000), n) == fdb_o_write(b, src + (v % 1000), n); }
            else if (k < 80) { long off = (long)(v % 300000); int wh = (int)(v % 3); if (wh == 2) off = -(long)(v % 5000); else if (wh == 1) off = (long)(v % 6000) - 3000;
                               ok = fdb_lseek(a, off, wh) == fdb_o_lseek(b, off, wh); }
            else if (k < 88 && round == 1) { unsigned n = 1 + v % 5000; int x = fdb_read(a, ra, n), y = fdb_o_read(b, rb, n); ok = x == y && (x <= 0 || !memcmp(ra, rb, x)); }
            else if (k < 89 && i > 2000) { fdb_get_osfhandle(a); }
            else { ok = fdb_lseek(a, 0, 1) == fdb_o_lseek(b, 0, 1); }
        }
        int ca = fdb_close(a), cb = fdb_o_close(b);
        ok = ok && ca == cb;
        if (src) VirtualFree(src, 0, MEM_RELEASE); if (ra) VirtualFree(ra, 0, MEM_RELEASE); if (rb) VirtualFree(rb, 0, MEM_RELEASE);
        // the two files, byte for byte
        HANDLE ha = CreateFileW(pa, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL), hb = CreateFileW(pb, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (ha == INVALID_HANDLE_VALUE || hb == INVALID_HANDLE_VALUE) ok = false;
        else if (ok) {
            LARGE_INTEGER sa, sb; ok = GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart && sa.QuadPart > 0;
            static BYTE xa[65536], xb[65536];
            while (ok) { DWORD na = 0, nb = 0; if (!ReadFile(ha, xa, sizeof(xa), &na, NULL) || !ReadFile(hb, xb, sizeof(xb), &nb, NULL) || na != nb || memcmp(xa, xb, na)) { ok = false; break; } if (!na) break; }
        }
        if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha); if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    }
    DeleteFileW(pa); DeleteFileW(pb);
    if (nOps) *nOps = ops;
    return ok;
}
