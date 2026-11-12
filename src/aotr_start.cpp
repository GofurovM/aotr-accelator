// aotr_start.exe (shipped as AOTR_ACCELERATOR.exe) - installs, starts and keeps loading the AOTR accelerator.
//
// One click from the package folder installs it: the files go to %ProgramData%\AOTR Accelerator, a desktop shortcut
// is made, and a Task Scheduler task starts it hidden at every logon (with the rights the game needs, without a
// Windows prompt each time). From then on it sits in the notification area as a small ring:
//   - when the AotR launcher opens, the game's files are opened once ahead (the virus scanner looks at each file the
//     first time - about 6 ms each, 53 000 files);
//   - when AOTR's game.dat starts, bfme2_accel.dll (beside this exe) is loaded into it - a game.dat of any other
//     install is left alone - and Windows' power plan goes to "High performance" until the game closes;
//   - the ring is gold while all is well, yellow when a part of the DLL switched itself off, red when the DLL is not
//     running (blocked or missing, or the render thread could not start) - read from aotr_status.txt that the DLL
//     writes; a balloon says why;
//   - the desktop shortcut (or a second start) only asks the running copy to open the AotR launcher - it never runs twice.
// Started from the source tree's out\ folder (PLAY.bat) it installs nothing and ends 20 s after the game closes.
//
// Nothing is written into the game folder and game.dat on disk is never modified (the AotR launcher checks both).
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <taskschd.h>
#include <oleauto.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_out[MAX_PATH];        // this exe's folder (out\ of the source tree, or wherever the package was unpacked)
static char g_dll[MAX_PATH];        // bfme2_accel.dll beside this exe
static char g_aotrExe[MAX_PATH];    // ...\AotR_Launcher.exe
static char g_aotrRoot[MAX_PATH];   // its folder

static bool exists(const char* p) { return p && *p && GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void folderOf(const char* path, char* out, DWORD n) {
    lstrcpynA(out, path, n);
    char* s = strrchr(out, '\\'); if (s) *s = 0;
}
static void stamp(char* out, size_t n) {
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf(out, n, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond); out[n - 1] = 0;
}
static CRITICAL_SECTION g_sayCs;
static void say(const char* fmt, ...) {                             // aotr_start.log beside the exe (no window to print to)
    char ts[16]; stamp(ts, sizeof(ts));
    char line[1200]; int n = _snprintf(line, sizeof(line) - 3, "[%s] ", ts);
    va_list a; va_start(a, fmt);
    int m = _vsnprintf(line + n, sizeof(line) - 3 - n, fmt, a); va_end(a);
    if (m < 0) m = (int)strlen(line + n);
    n += m; line[n++] = '\r'; line[n++] = '\n'; line[n] = 0;
    char p[MAX_PATH]; _snprintf(p, sizeof(p), "%s\\aotr_start.log", g_out); p[MAX_PATH - 1] = 0;
    EnterCriticalSection(&g_sayCs);
    FILE* f = fopen(p, "ab"); if (f) { fwrite(line, 1, n, f); fclose(f); }
    LeaveCriticalSection(&g_sayCs);
}

// ---------------------------------------------------------------- finding AOTR
static bool tryRoot(const char* root) {
    if (!root || !*root) return false;
    char p[MAX_PATH]; _snprintf(p, sizeof(p), "%s\\AotR_Launcher.exe", root); p[MAX_PATH - 1] = 0;
    if (!exists(p)) return false;
    lstrcpynA(g_aotrExe, p, MAX_PATH);
    return true;
}
static bool regPath(HKEY root, const char* subkey, const char* value, char* out, DWORD n) {
    HKEY k;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) return false;
    char buf[MAX_PATH]; DWORD sz = sizeof(buf) - 1, type = 0;
    bool ok = RegQueryValueExA(k, value, NULL, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
              (type == REG_SZ || type == REG_EXPAND_SZ) && sz > 0;
    RegCloseKey(k);
    if (!ok) return false;
    buf[sz < sizeof(buf) ? sz : sizeof(buf) - 1] = 0;
    for (char* q = buf + strlen(buf); q > buf && q[-1] == '\\'; --q) q[-1] = 0;
    lstrcpynA(out, buf, n);
    return true;
}
static bool findAotr(bool ask = true) {
    char saved[MAX_PATH]; _snprintf(saved, sizeof(saved), "%s\\aotr_path.txt", g_out);
    FILE* f = fopen(saved, "r");
    if (!f) { char old[MAX_PATH]; _snprintf(old, sizeof(old), "%s\\aotr_yol.txt", g_out); f = fopen(old, "r"); }   // (the name older builds used)
    if (f) {
        char line[MAX_PATH] = "";
        if (fgets(line, sizeof(line), f))
            for (char* q = line + strlen(line); q > line && (q[-1] == '\n' || q[-1] == '\r' || q[-1] == ' '); --q) q[-1] = 0;
        fclose(f);
        if (exists(line)) { lstrcpynA(g_aotrExe, line, MAX_PATH); return true; }
    }
    // where the installers put it: the RotWK key points at ...\rotwk (AOTR keeps its launcher one level up) or at the root
    static const char* kKeys[] = {
        "SOFTWARE\\Electronic Arts\\Electronic Arts\\The Lord of the Rings, The Rise of the Witch-king",
        "SOFTWARE\\Electronic Arts\\Electronic Arts\\The Battle for Middle-earth II",
    };
    bool hit = false;
    for (int i = 0; i < 2 && !hit; ++i) {
        char v[MAX_PATH], up[MAX_PATH];
        if (regPath(HKEY_LOCAL_MACHINE, kKeys[i], "InstallPath", v, sizeof(v)) ||
            regPath(HKEY_CURRENT_USER,  kKeys[i], "InstallPath", v, sizeof(v))) {
            folderOf(v, up, sizeof(up));
            hit = tryRoot(v) || tryRoot(up);
        }
    }
    if (!hit) hit = tryRoot("C:\\AgeoftheRing") || tryRoot("D:\\AgeoftheRing") || tryRoot("E:\\AgeoftheRing") ||
                    tryRoot("C:\\Games\\AgeoftheRing") || tryRoot("D:\\Games\\AgeoftheRing");
    if (!hit && !ask) return false;
    if (!hit) {
        say("AotR_Launcher.exe was not found - asking where it is (only once).");
        char pick[MAX_PATH] = "";
        OPENFILENAMEA o; ZeroMemory(&o, sizeof(o));
        o.lStructSize = sizeof(o);
        o.lpstrFilter = "AotR_Launcher.exe\0AotR_Launcher.exe\0Programs (*.exe)\0*.exe\0";
        o.lpstrFile = pick; o.nMaxFile = MAX_PATH;
        o.lpstrTitle = "Pick the Age of the Ring launcher (AotR_Launcher.exe)";
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&o)) return false;
        lstrcpynA(g_aotrExe, pick, MAX_PATH);
    }
    f = fopen(saved, "w");
    if (f) { fprintf(f, "%s\n", g_aotrExe); fclose(f); }
    return true;
}

// ---------------------------------------------------------------- processes
static DWORD findPid(const char* name, DWORD skip) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do { if (pe.th32ProcessID != skip && _stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}
static bool imagePath(DWORD pid, char* out, DWORD n) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;
    DWORD len = n;
    BOOL ok = QueryFullProcessImageNameA(hp, 0, out, &len);
    CloseHandle(hp);
    return ok != 0;
}
// AOTR's game.dat: under the AotR launcher's folder, or in a folder where the AotR launcher has staged its .big files
static bool isAotrGame(const char* gamePath) {
    size_t r = strlen(g_aotrRoot);
    if (r && !_strnicmp(gamePath, g_aotrRoot, r) && (gamePath[r] == '\\' || gamePath[r] == 0)) return true;
    char dir[MAX_PATH], pat[MAX_PATH]; folderOf(gamePath, dir, sizeof(dir));
    _snprintf(pat, sizeof(pat), "%s\\*aotr*.big", dir); pat[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}
// 32-bit loader, 32-bit game: kernel32's LoadLibraryA is at the same address in both
static bool injectInto(DWORD pid) {
    // v85: only the rights the load needs, and no debug privilege: the game runs as the same user (the AotR launcher
    // starts it, this program has the same rights), so nothing more is required - and asking for more is what
    // antivirus heuristics read as an attack on another process.
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
    if (!hp) return false;
    DWORD code = 0;
    if (GetExitCodeProcess(hp, &code) && code != STILL_ACTIVE) { CloseHandle(hp); return false; }   // died during start-up
    SIZE_T len = strlen(g_dll) + 1, wrote = 0;
    void* remote = VirtualAllocEx(hp, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hp); return false; }
    if (!WriteProcessMemory(hp, remote, g_dll, len, &wrote)) { VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return false; }
    FARPROC loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(hp, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) { VirtualFreeEx(hp, remote, 0, MEM_RELEASE); CloseHandle(hp); return false; }
    WaitForSingleObject(th, 15000);
    DWORD mod = 0; GetExitCodeThread(th, &mod);
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(th); CloseHandle(hp);
    return mod != 0;
}

// ---------------------------------------------------------------- the game's files shown to the virus scanner early
// After a restart (or a signature update) Windows Defender looks at every file the first time it is opened - about 6 ms
// each (measured in v58, aotr_filewarm.inc) - and AOTR is ~53 000 loose files. The DLL's file warm-up opens them ahead of
// the game, but it can only start when game.dat does. This starts the same work the moment this window opens, while the
// launcher is still on screen: every file is opened for reading (all sharing allowed) and closed at once - nothing is
// read, nothing written. First the files the game opened in earlier sessions (filewarm.lst, the DLL's list), then
// everything under the AOTR folder (sounds and films last). It stops when the game starts (the DLL carries on).
#define WARM_CAP 400000
static wchar_t** g_warm = NULL;
static volatile LONG g_warmN = 0, g_warmNext = 0, g_warmOpened = 0, g_warmStop = 0, g_warmBuilt = 0, g_warmAlive = 0;
static DWORD g_warmT0 = 0;
static void warmAdd(const wchar_t* path, size_t len) {
    LONG n = g_warmN;
    if (n >= WARM_CAP || !len) return;
    wchar_t* c = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(wchar_t));
    if (!c) return;
    memcpy(c, path, len * sizeof(wchar_t)); c[len] = 0;
    g_warm[n] = c;
    InterlockedExchange(&g_warmN, n + 1);                           // published after the entry is written
}
static bool warmIsMedia(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    return !_wcsicmp(dot, L".wav") || !_wcsicmp(dot, L".mp3") || !_wcsicmp(dot, L".ogg") || !_wcsicmp(dot, L".bik") ||
           !_wcsicmp(dot, L".vp6") || !_wcsicmp(dot, L".avi");
}
static void warmWalk(const wchar_t* dir, bool media, int depth) {
    if (depth > 24 || g_warmStop) return;
    wchar_t pat[MAX_PATH * 2]; _snwprintf(pat, MAX_PATH * 2, L"%s\\*", dir); pat[MAX_PATH * 2 - 1] = 0;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        wchar_t full[MAX_PATH * 2]; _snwprintf(full, MAX_PATH * 2, L"%s\\%s", dir, fd.cFileName); full[MAX_PATH * 2 - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) warmWalk(full, media, depth + 1); }
        else if (warmIsMedia(fd.cFileName) == media) warmAdd(full, wcslen(full));
    } while (!g_warmStop && FindNextFileW(h, &fd));
    FindClose(h);
}
static DWORD WINAPI warmBuilder(LPVOID) {
    char lst[MAX_PATH]; _snprintf(lst, sizeof(lst), "%s\\filewarm.lst", g_out);
    HANDLE h = CreateFileA(lst, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(h, NULL), got = 0;
        char* buf = (size && size < (64u << 20)) ? (char*)HeapAlloc(GetProcessHeap(), 0, size + 1) : NULL;
        if (buf && ReadFile(h, buf, size, &got, NULL) && got == size) {
            buf[size] = 0;
            for (char* p = buf; *p && !g_warmStop; ) {
                char* e = p; while (*e && *e != '\n' && *e != '\r') ++e;
                int len = (int)(e - p);
                if (len > 3 && len < 1000) {
                    wchar_t w[1024]; int wl = MultiByteToWideChar(CP_UTF8, 0, p, len, w, 1023);
                    if (wl > 0) warmAdd(w, (size_t)wl);
                }
                p = e; while (*p == '\n' || *p == '\r') ++p;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        CloseHandle(h);
    }
    wchar_t root[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, g_aotrRoot, -1, root, MAX_PATH);
    warmWalk(root, false, 0);
    warmWalk(root, true, 0);
    InterlockedExchange(&g_warmBuilt, 1);
    return 0;
}
static DWORD WINAPI warmWorker(LPVOID) {
    InterlockedIncrement(&g_warmAlive);
    while (!g_warmStop) {
        LONG i = g_warmNext;
        if (i >= g_warmN) { if (g_warmBuilt && i >= g_warmN) break; Sleep(5); continue; }
        if (InterlockedCompareExchange(&g_warmNext, i + 1, i) != i) continue;
        HANDLE h = CreateFileW(g_warm[i], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); InterlockedIncrement(&g_warmOpened); }
    }
    if (InterlockedDecrement(&g_warmAlive) == 0 && !g_warmStop)
        say("Game files opened ahead: %ld in %lu s (the virus scanner looked at them before the game needs them).", g_warmOpened, (GetTickCount() - g_warmT0) / 1000);
    return 0;
}
static void warmStart() {
    g_warm = (wchar_t**)VirtualAlloc(NULL, WARM_CAP * sizeof(wchar_t*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_warm) return;
    g_warmT0 = GetTickCount();
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1; if (n < 2) n = 2; if (n > 8) n = 8;
    HANDLE b = CreateThread(NULL, 0, warmBuilder, NULL, 0, NULL); if (b) CloseHandle(b);
    for (int i = 0; i < n; ++i) { HANDLE t = CreateThread(NULL, 0, warmWorker, NULL, 0, NULL); if (t) CloseHandle(t); }
    say("Opening the game's files ahead in the background (%d threads) - until the game starts.", n);
}
static void warmStopNow() {
    if (!g_warm || g_warmStop) return;
    InterlockedExchange(&g_warmStop, 1);
    say("Opening ahead stopped: %ld files opened (%lu s) - the DLL carries on inside the game.", g_warmOpened, (GetTickCount() - g_warmT0) / 1000);
}

// ---------------------------------------------------------------- main
// v59: the Windows power settings while the game runs. test-40..42: the processor fell to ~20% of its speed for
// seconds at a time and every hitch the player marked in test-41 fell into such a moment. Windows' "High performance"
// plan and the power slider's "Best performance" stop Windows itself from lowering the clock (they cannot stop a
// laptop's own heat or power-supply limit). The settings found are written to aotr_power.txt before anything
// is changed and put back when the game is closed - also at the next start if this window was closed meanwhile.
static const GUID kHighPerf = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
static const GUID kBestPerfOverlay = { 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } };
typedef DWORD (WINAPI* tPGetScheme)(HKEY, GUID**);
typedef DWORD (WINAPI* tPSetScheme)(HKEY, const GUID*);
typedef DWORD (WINAPI* tPGetOvl)(GUID*);
typedef DWORD (WINAPI* tPSetOvl)(GUID);
static tPGetScheme p_getScheme; static tPSetScheme p_setScheme; static tPGetOvl p_getOvl; static tPSetOvl p_setOvl;
static bool g_powChanged = false;
// v60: the processor's Turbo off when the DLL asks (aotr_throttle.txt: it saw the processor fall under 35% of its
// speed 3 times in a minute - a laptop's heat or power-supply limit stepping in). The active plan's "processor
// performance boost mode" (AC) is set to Disabled - documented PowerReadACValueIndex / PowerWriteACValueIndex - and its
// old value goes into aotr_power.txt first; put back with the rest.
static const GUID kSubProc = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID kBoostMode = { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
static const GUID kFreqMax = { 0x75b0ae3f, 0xbce0, 0x45a7, { 0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00 } };   // v63: Maximum processor frequency (MHz, 0 = no cap)
typedef DWORD (WINAPI* tPReadAC)(HKEY, const GUID*, const GUID*, const GUID*, LPDWORD);
typedef DWORD (WINAPI* tPWriteAC)(HKEY, const GUID*, const GUID*, const GUID*, DWORD);
static tPReadAC p_readAC; static tPWriteAC p_writeAC;

// v59.1: the slider's two functions are not in the Windows SDK (PowerGetEffectiveOverlayScheme(GUID*),
// PowerSetActiveOverlayScheme(GUID) as tools call them). Called so that a different signature on some Windows cannot
// take this window down: the stack pointer is put back after the call whatever the callee popped, and a fault is caught.
static DWORD powSetOvlSafe(const GUID* g) {
    if (!p_setOvl) return 1;
    DWORD r = 1; void* f = (void*)p_setOvl; DWORD a0 = ((const DWORD*)g)[0], a1 = ((const DWORD*)g)[1], a2 = ((const DWORD*)g)[2], a3 = ((const DWORD*)g)[3];
    __try {
        __asm {
            mov  esi, esp
            push a3
            push a2
            push a1
            push a0
            call f
            mov  esp, esi
            mov  r, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 2; }
    return r;
}
static DWORD powGetOvlSafe(GUID* g) {
    if (!p_getOvl) return 1;
    DWORD r = 1; void* f = (void*)p_getOvl;
    __try {
        __asm {
            mov  esi, esp
            push g
            call f
            mov  esp, esi
            mov  r, eax
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = 2; }
    return r;
}
static bool powLoad() {
    static int done = 0; if (done) return p_getScheme && p_setScheme;
    done = 1; HMODULE pp = LoadLibraryA("powrprof.dll"); if (!pp) return false;
    p_getScheme = (tPGetScheme)GetProcAddress(pp, "PowerGetActiveScheme"); p_setScheme = (tPSetScheme)GetProcAddress(pp, "PowerSetActiveScheme");
    p_getOvl = (tPGetOvl)GetProcAddress(pp, "PowerGetEffectiveOverlayScheme"); p_setOvl = (tPSetOvl)GetProcAddress(pp, "PowerSetActiveOverlayScheme");
    p_readAC = (tPReadAC)GetProcAddress(pp, "PowerReadACValueIndex"); p_writeAC = (tPWriteAC)GetProcAddress(pp, "PowerWriteACValueIndex");
    return p_getScheme && p_setScheme;
}
static void powFile(char* p) { _snprintf(p, MAX_PATH, "%s\\aotr_power.txt", g_out); p[MAX_PATH - 1] = 0; }
static void guidStr(const GUID& g, char* o) { _snprintf(o, 40, "%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]); o[39] = 0; }
static bool guidParse(const char* s, GUID* g) {
    unsigned long a; unsigned b, c, d[8];
    if (sscanf(s, "%8lX-%4X-%4X-%2X%2X%2X%2X%2X%2X%2X%2X", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return false;
    g->Data1 = a; g->Data2 = (WORD)b; g->Data3 = (WORD)c; for (int i = 0; i < 8; ++i) g->Data4[i] = (BYTE)d[i];
    return true;
}
static void powRestore(bool quiet) {                               // what aotr_power.txt holds goes back; the file goes
    char f[MAX_PATH]; powFile(f);
    if (!exists(f)) return;
    char b[256] = ""; FILE* fp = fopen(f, "rb"); if (fp) { size_t n = fread(b, 1, sizeof(b) - 1, fp); b[n] = 0; fclose(fp); }
    if (!powLoad()) return;
    GUID g; const char* s;
    bool ok = true;
    { const char* bs = strstr(b, "boostscheme="); const char* bv = strstr(b, "boost=");      // v60: Turbo back first (it is a value of that plan)
      const char* fv = strstr(b, "freqmax=");
      GUID bg; if (bs && bv && p_writeAC && guidParse(bs + 12, &bg)) { DWORD v = (DWORD)atoi(bv + 6); ok = p_writeAC(NULL, &bg, &kSubProc, &kBoostMode, v) == 0 && ok;
                                                                       if (fv) ok = p_writeAC(NULL, &bg, &kSubProc, &kFreqMax, (DWORD)atoi(fv + 8)) == 0 && ok;
                                                                       p_setScheme(NULL, &bg); } }
    s = strstr(b, "scheme=");
    if (s && guidParse(s + 7, &g)) ok = p_setScheme(NULL, &g) == 0 && ok;
    s = strstr(b, "overlay=");
    if (s && p_setOvl && guidParse(s + 8, &g)) ok = powSetOvlSafe(&g) == 0 && ok;
    DeleteFileA(f); g_powChanged = false;
    if (!quiet) say(ok ? "Windows power settings put back as they were." : "Putting the Windows power settings back failed - please check Settings > Power.");
}
static BOOL WINAPI powCtrl(DWORD);
// v63: processor levels (the DLL picks them, see aotr_guard.inc): 0 as found, 1..3 the highest clock capped at 150 /
// 130 / 115 % of the base clock ("Maximum processor frequency", MHz), 4 Turbo off. The values found are kept in
// aotr_power.txt before the first change and put back with the rest.
static GUID g_lvSch; static DWORD g_lvBoost0 = 1, g_lvFreq0 = 0, g_lvBase = 0; static int g_lvApplied = 0, g_lvLast = 0;
static DWORD baseMhz() {
    typedef LONG (WINAPI* tCNPI)(int, void*, ULONG, void*, ULONG);
    HMODULE pp = LoadLibraryA("powrprof.dll"); tCNPI f = pp ? (tCNPI)GetProcAddress(pp, "CallNtPowerInformation") : NULL;
    if (!f) return 0;
    struct PPI { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; } info[64];
    SYSTEM_INFO si; GetSystemInfo(&si); ULONG n = si.dwNumberOfProcessors > 64 ? 64 : si.dwNumberOfProcessors;
    if (f(11 /* ProcessorInformation */, NULL, 0, info, sizeof(PPI) * n) != 0) return 0;
    return info[0].MaxMhz;
}
static bool lvApply(int lvl) {
    if (!powLoad() || !p_readAC || !p_writeAC) return false;
    if (!g_lvApplied) {
        GUID* cur = NULL; if (p_getScheme(NULL, &cur) != 0 || !cur) return false;
        g_lvSch = *cur; LocalFree(cur);
        if (p_readAC(NULL, &g_lvSch, &kSubProc, &kBoostMode, &g_lvBoost0) != 0) g_lvBoost0 = 1;
        if (p_readAC(NULL, &g_lvSch, &kSubProc, &kFreqMax, &g_lvFreq0) != 0) g_lvFreq0 = 0;
        g_lvBase = baseMhz();
        char pf[MAX_PATH]; powFile(pf); char gs[40]; guidStr(g_lvSch, gs);
        FILE* fp = fopen(pf, "ab"); if (!fp) return false;
        fprintf(fp, "boostscheme=%s\r\nboost=%lu\r\nfreqmax=%lu\r\n", gs, g_lvBoost0, g_lvFreq0); fclose(fp);
        g_powChanged = true; SetConsoleCtrlHandler(powCtrl, TRUE);
        g_lvApplied = 1;
    }
    static const DWORD kPct[4] = { 0, 150, 130, 115 };
    DWORD boost = g_lvBoost0, freq = g_lvFreq0;
    if (lvl >= 1 && lvl <= 3) { if (g_lvBase) freq = g_lvBase * kPct[lvl] / 100; else boost = 3; if (!boost) boost = 1; }   // (no base clock known: "efficient" boost instead)
    if (lvl >= 4) boost = 0;
    bool ok = p_writeAC(NULL, &g_lvSch, &kSubProc, &kBoostMode, boost) == 0;
    ok = p_writeAC(NULL, &g_lvSch, &kSubProc, &kFreqMax, freq) == 0 && ok;
    ok = p_setScheme(NULL, &g_lvSch) == 0 && ok;
    say("Processor level %d: %s", lvl, lvl == 0 ? "as Windows had it" : lvl == 4 ? "Turbo off" : "highest clock capped");
    if (lvl >= 1 && lvl <= 3 && g_lvBase) say("  (base %lu MHz, cap %lu MHz)", g_lvBase, freq);
    return ok;
}
static void turboPoll() {                                           // the waiting loop, every 0.5 s
    char f[MAX_PATH]; _snprintf(f, sizeof(f), "%s\\aotr_throttle.txt", g_out);
    if (!exists(f)) return;
    char b[64] = ""; FILE* fp = fopen(f, "rb"); if (fp) { size_t n = fread(b, 1, sizeof(b) - 1, fp); b[n] = 0; fclose(fp); }
    DeleteFileA(f);
    const char* q = strstr(b, "level="); int lvl = q ? atoi(q + 6) : 4;
    if (lvl < 0) lvl = 0; if (lvl > 4) lvl = 4;
    char mk[MAX_PATH]; _snprintf(mk, sizeof(mk), "%s\\TURBO_KEEP", g_out);
    if (exists(mk)) { if (!g_lvLast) say("The processor keeps slowing itself down, but TURBO_KEEP is present - left alone."); g_lvLast = -1; return; }
    if (!lvApply(lvl)) { say("The processor level could not be changed - left alone."); return; }
    g_lvLast = lvl;
    char a[MAX_PATH]; _snprintf(a, sizeof(a), "%s\\aotr_throttle_ack.txt", g_out);
    FILE* k = fopen(a, "wb"); if (k) { fprintf(k, "level=%d\r\n", lvl); fclose(k); }
}
static BOOL WINAPI powCtrl(DWORD) { if (g_powChanged) powRestore(true); return FALSE; }   // the window is being closed
static void powUp() {
    char f[MAX_PATH]; powFile(f);
    char mk[MAX_PATH]; _snprintf(mk, sizeof(mk), "%s\\POWER_OFF", g_out);
    if (exists(f)) powRestore(false);                               // a session whose window was closed before it could put them back
    if (exists(mk)) { say("Power settings: POWER_OFF is present - left alone."); return; }
    if (!powLoad()) { say("Power settings: powrprof.dll not found - left alone."); return; }
    GUID* cur = NULL; if (p_getScheme(NULL, &cur) != 0 || !cur) { say("Power settings could not be read - left alone."); return; }
    GUID orig = *cur; LocalFree(cur);
    GUID ovl = { 0 }; bool haveOvl = p_getOvl && p_setOvl && powGetOvlSafe(&ovl) == 0;
    char a[40], o[40]; guidStr(orig, a); guidStr(ovl, o);
    FILE* fp = fopen(f, "wb"); if (!fp) { say("Power settings: aotr_power.txt could not be written - left alone."); return; }
    fprintf(fp, "scheme=%s\r\n", a); fclose(fp);
    g_powChanged = true; SetConsoleCtrlHandler(powCtrl, TRUE);
    bool planOk = memcmp(&orig, &kHighPerf, sizeof(GUID)) == 0 || p_setScheme(NULL, &kHighPerf) == 0;
    bool ovlOk = haveOvl && powSetOvlSafe(&kBestPerfOverlay) == 0;
    // v72: the slider is put back only when it was changed - on the "High performance" plan Windows has no slider and
    // refuses both calls, and putting back what was never changed reported a failure after every game
    if (ovlOk) { fp = fopen(f, "ab"); if (fp) { fprintf(fp, "overlay=%s\r\n", o); fclose(fp); } }
    say("Power: Windows plan %s, slider %s (put back when the game closes; a file named POWER_OFF here leaves them alone).",
        planOk ? "'High performance'" : "unchanged (not on this PC)", ovlOk ? "'Best performance'" : "unchanged");
}

// v59.1: if this window ever fails, it says where (and puts the power settings back) instead of only Windows' box.
static LONG WINAPI startCrash(EXCEPTION_POINTERS* e) {
    DWORD code = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
    DWORD at = e && e->ExceptionRecord ? (DWORD)(ULONG_PTR)e->ExceptionRecord->ExceptionAddress : 0;
    char mod[MAX_PATH] = "?"; HMODULE hm = NULL; DWORD off = at;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(ULONG_PTR)at, &hm) && hm) {
        GetModuleFileNameA(hm, mod, MAX_PATH); off = at - (DWORD)(ULONG_PTR)hm; }
    const char* leaf = strrchr(mod, '\\'); leaf = leaf ? leaf + 1 : mod;
    char path[MAX_PATH]; _snprintf(path, sizeof(path), "%s\\aotr_start_crash.txt", g_out); path[MAX_PATH - 1] = 0;
    FILE* f = fopen(path, "ab"); if (f) { char ts[16]; stamp(ts, sizeof(ts)); fprintf(f, "%s aotr_start.exe failed: code %08lX at %s+%08lX\r\n", ts, code, leaf, off); fclose(f); }
    if (g_powChanged) powRestore(true);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ================================================================ v71: the resident (tray), the install, the autostart task
#define IDI_GOLD   101
#define IDI_YELLOW 102
#define IDI_RED    103
#define WM_AOTR_LAUNCH (WM_APP + 10)                 // another start (the shortcut) asks: open the AotR launcher
#define WM_AOTR_QUIT   (WM_APP + 11)                 // the installer (or a PLAY.bat start) asks this copy to end
#define WM_AOTR_TRAY   (WM_APP + 12)                 // the notification icon's own messages
#define WM_AOTR_STATE  (WM_APP + 13)                 // the monitor thread: the icon, its tip or a balloon changed
static const wchar_t* const kWndClass = L"AotrAcceleratorResident";
static const wchar_t* const kTaskName = L"AOTR Accelerator";
static const char* const kExeName = "AOTR_ACCELERATOR.exe";
static HINSTANCE g_hi;
static HWND g_wnd;
static UINT g_wmTaskbarCreated;
static bool g_dev = false;                           // started from the source tree's out\ folder (PLAY.bat)
static volatile LONG g_quit = 0;
static CRITICAL_SECTION g_uiCs;
static int g_icon = IDI_GOLD;                        // under g_uiCs: the icon, its tip, a balloon waiting to be shown
static wchar_t g_tip[128] = L"AOTR Accelerator";
static wchar_t g_status[160] = L"Waiting for the game  |  O'yin kutilmoqda";
static wchar_t g_bTitle[64], g_bText[256]; static DWORD g_bFlags = 0; static bool g_bPending = false;

static void wfrom(const char* a, wchar_t* w, int n) { if (!MultiByteToWideChar(CP_ACP, 0, a, -1, w, n)) w[0] = 0; }
static bool isElevated() {
    HANDLE t; TOKEN_ELEVATION e = { 0 }; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    BOOL ok = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return ok && e.TokenIsElevated;
}
static void installDir(char* out) {
    char pd[MAX_PATH]; if (!GetEnvironmentVariableA("ProgramData", pd, MAX_PATH)) lstrcpyA(pd, "C:\\ProgramData");
    _snprintf(out, MAX_PATH, "%s\\AOTR Accelerator", pd); out[MAX_PATH - 1] = 0;
}
static bool relaunchElevated(const wchar_t* args) {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    SHELLEXECUTEINFOW se; ZeroMemory(&se, sizeof(se)); se.cbSize = sizeof(se);
    se.lpVerb = L"runas"; se.lpFile = self; se.lpParameters = args; se.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&se) != FALSE;
}
static void msg(const wchar_t* text, UINT flags = MB_ICONINFORMATION) { MessageBoxW(NULL, text, L"AOTR Accelerator", flags | MB_SETFOREGROUND); }
static bool aotrGameRunning() {
    DWORD pid = findPid("game.dat", 0); if (!pid) return false;
    char img[MAX_PATH] = ""; return imagePath(pid, img, MAX_PATH) && isAotrGame(img);
}

// ---- the Task Scheduler task: started hidden at logon with the highest rights this user has (no prompt each time).
// With autostart switched off the task stays, without its logon trigger, so the desktop shortcut can still start the
// resident through it - one Windows prompt at install, none afterwards.
static ITaskFolder* taskRoot() {
    ITaskService* svc = NULL; ITaskFolder* root = NULL;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc)) || !svc) return NULL;
    VARIANT e; VariantInit(&e);
    if (SUCCEEDED(svc->Connect(e, e, e, e))) { BSTR p = SysAllocString(L"\\"); svc->GetFolder(p, &root); SysFreeString(p); }
    svc->Release();
    return root;
}
static void xmlEsc(const wchar_t* in, wchar_t* out, int cap) {
    int n = 0;
    for (; *in && n < cap - 8; ++in) {
        const wchar_t* r = *in == L'&' ? L"&amp;" : *in == L'<' ? L"&lt;" : *in == L'>' ? L"&gt;" : *in == L'"' ? L"&quot;" : NULL;
        if (r) { while (*r) out[n++] = *r++; } else out[n++] = *in;
    }
    out[n] = 0;
}
static bool taskRegister(const char* exe, bool atLogon) {
    ITaskFolder* root = taskRoot(); if (!root) return false;
    wchar_t dom[128] = L"", usr[128] = L"", who[300], whoX[600], exeW[MAX_PATH], exeX[MAX_PATH * 2];
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 128); GetEnvironmentVariableW(L"USERNAME", usr, 128);
    _snwprintf(who, 300, L"%s%s%s", dom, dom[0] ? L"\\" : L"", usr); who[299] = 0;
    xmlEsc(who, whoX, 600); wfrom(exe, exeW, MAX_PATH); xmlEsc(exeW, exeX, MAX_PATH * 2);
    static wchar_t xml[6000], trig[800];
    if (atLogon) { _snwprintf(trig, 800, L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId></LogonTrigger></Triggers>", whoX); trig[799] = 0; }
    else trig[0] = 0;                                   // no trigger: started only on request (the shortcut)
    _snwprintf(xml, 6000,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Loads the AOTR accelerator into Age of the Ring when the game starts.</Description></RegistrationInfo>"
        L"%s"
        L"<Principals><Principal id=\"Author\"><UserId>%s</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><AllowStartOnDemand>true</AllowStartOnDemand>"
        L"<Priority>7</Priority><IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>\"%s\"</Command><Arguments>--background</Arguments></Exec></Actions></Task>",
        trig, whoX, exeX);
    xml[5999] = 0;
    BSTR name = SysAllocString(kTaskName), text = SysAllocString(xml);
    VARIANT e; VariantInit(&e);
    IRegisteredTask* t = NULL;
    HRESULT hr = root->RegisterTask(name, text, TASK_CREATE_OR_UPDATE, e, e, TASK_LOGON_INTERACTIVE_TOKEN, e, &t);
    if (t) t->Release();
    SysFreeString(name); SysFreeString(text); root->Release();
    say("autostart task %s (%s): %08lX", atLogon ? "with the logon trigger" : "without a trigger", SUCCEEDED(hr) ? "registered" : "NOT registered", (unsigned long)hr);
    return SUCCEEDED(hr);
}
static IRegisteredTask* taskGet() {
    ITaskFolder* root = taskRoot(); if (!root) return NULL;
    IRegisteredTask* t = NULL; BSTR name = SysAllocString(kTaskName);
    if (FAILED(root->GetTask(name, &t))) t = NULL;
    SysFreeString(name); root->Release();
    return t;
}
static bool taskExists() { IRegisteredTask* t = taskGet(); if (!t) return false; t->Release(); return true; }
static bool taskRun() {
    IRegisteredTask* t = taskGet(); if (!t) return false;
    VARIANT e; VariantInit(&e); IRunningTask* r = NULL;
    HRESULT hr = t->Run(e, &r);
    if (r) r->Release(); t->Release();
    return SUCCEEDED(hr);
}
static void taskDelete() {
    ITaskFolder* root = taskRoot(); if (!root) return;
    BSTR name = SysAllocString(kTaskName); root->DeleteTask(name, 0); SysFreeString(name); root->Release();
}
static bool autostartOn() { char f[MAX_PATH]; _snprintf(f, MAX_PATH, "%s\\autostart_off", g_out); f[MAX_PATH - 1] = 0; return !exists(f) && taskExists(); }

// ---- the desktop shortcut
static void shortcutPath(wchar_t* out) {
    wchar_t d[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, d))) d[0] = 0;
    _snwprintf(out, MAX_PATH, L"%s\\AOTR Accelerator.lnk", d); out[MAX_PATH - 1] = 0;
}
static bool shortcutMake(const char* exe) {
    IShellLinkW* sl = NULL; IPersistFile* pf = NULL; bool ok = false;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) || !sl) return false;
    wchar_t exeW[MAX_PATH], dirW[MAX_PATH], lnk[MAX_PATH];
    wfrom(exe, exeW, MAX_PATH); char dir[MAX_PATH]; folderOf(exe, dir, MAX_PATH); wfrom(dir, dirW, MAX_PATH);
    sl->SetPath(exeW); sl->SetWorkingDirectory(dirW); sl->SetIconLocation(exeW, 0);
    sl->SetDescription(L"Start Age of the Ring with the accelerator");
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) { shortcutPath(lnk); ok = SUCCEEDED(pf->Save(lnk, TRUE)); pf->Release(); }
    sl->Release();
    return ok;
}

// ---- the AotR launcher, opened on request
static void uiBalloon(const wchar_t* title, const wchar_t* text, DWORD flags);
struct LaunchWin { DWORD pid; HWND w; };
static BOOL CALLBACK launchFindWin(HWND h, LPARAM l) {   // the launcher's own top window
    LaunchWin* x = (LaunchWin*)l; DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p == x->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { x->w = h; return FALSE; }
    return TRUE;
}
static void launchAotr() {
    if (!g_aotrExe[0] && !findAotr(true)) {
        say("no AotR launcher picked - nothing opened");
        msg(L"AotR_Launcher.exe was not found, so nothing was opened. Start again and pick it.\nAotR_Launcher.exe topilmadi - hech narsa ochilmadi. Qayta ishga tushirib, uni tanlang.", MB_ICONWARNING);
        return;
    }
    folderOf(g_aotrExe, g_aotrRoot, sizeof(g_aotrRoot));
    if (findPid("game.dat", 0)) {                        // v82: never silent - the player sees why nothing new opened
        say("the game is already running - nothing opened");
        uiBalloon(L"AOTR is already running", L"The game is already open. / O'yin allaqachon ochiq.", NIIF_INFO);
        return;
    }
    if (DWORD lp = findPid("AotR_Launcher.exe", 0)) {
        say("the AotR launcher is already open (pid %lu) - brought to the front", lp);
        LaunchWin c = { lp, NULL };
        EnumWindows(launchFindWin, (LPARAM)&c);
        if (c.w) { ShowWindow(c.w, IsIconic(c.w) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(c.w); }
        else uiBalloon(L"The AOTR launcher is already running", L"It is open in the background (look near the clock). / Launcher fonda ochiq (soat yonidagi belgilarga qarang).", NIIF_INFO);
        return;
    }
    char cmd[MAX_PATH + 2]; _snprintf(cmd, sizeof(cmd), "\"%s\"", g_aotrExe); cmd[sizeof(cmd) - 1] = 0;
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_aotrRoot, &si, &pi)) {
        say("the AotR launcher could not be started (error %lu)", GetLastError());
        msg(L"The AOTR launcher could not be started.\nAOTR launcherini ochib bo'lmadi.", MB_ICONWARNING);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    say("AotR launcher opened: %s", g_aotrExe);
}

// ---- what the tray shows (the monitor thread sets it, the window thread draws it)
static void uiSet(int icon, const wchar_t* status, const wchar_t* bTitle = NULL, const wchar_t* bText = NULL, DWORD bFlags = NIIF_INFO) {
    EnterCriticalSection(&g_uiCs);
    bool changed = icon != g_icon || lstrcmpW(status, g_status) != 0;
    g_icon = icon; lstrcpynW(g_status, status, 160);
    _snwprintf(g_tip, 128, L"AOTR Accelerator - %s", status); g_tip[127] = 0;
    if (bTitle && bText) { lstrcpynW(g_bTitle, bTitle, 64); lstrcpynW(g_bText, bText, 256); g_bFlags = bFlags; g_bPending = true; changed = true; }
    LeaveCriticalSection(&g_uiCs);
    if (changed && g_wnd) PostMessageW(g_wnd, WM_AOTR_STATE, 0, 0);
}
static void uiBalloon(const wchar_t* title, const wchar_t* text, DWORD flags) {   // the icon and the status stay as they are
    EnterCriticalSection(&g_uiCs);
    lstrcpynW(g_bTitle, title, 64); lstrcpynW(g_bText, text, 256); g_bFlags = flags; g_bPending = true;
    LeaveCriticalSection(&g_uiCs);
    if (g_wnd) PostMessageW(g_wnd, WM_AOTR_STATE, 0, 0);
}
static void trayUpdate(DWORD op) {
    NOTIFYICONDATAW n; ZeroMemory(&n, sizeof(n)); n.cbSize = sizeof(n);
    n.hWnd = g_wnd; n.uID = 1; n.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE; n.uCallbackMessage = WM_AOTR_TRAY;
    EnterCriticalSection(&g_uiCs);
    n.hIcon = (HICON)LoadImageW(g_hi, MAKEINTRESOURCEW(g_icon), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpynW(n.szTip, g_tip, 128);
    if (g_bPending && op != NIM_DELETE) { n.uFlags |= NIF_INFO; lstrcpynW(n.szInfoTitle, g_bTitle, 64); lstrcpynW(n.szInfo, g_bText, 256); n.dwInfoFlags = g_bFlags; g_bPending = false; }
    LeaveCriticalSection(&g_uiCs);
    if (!Shell_NotifyIconW(op, &n) && op == NIM_MODIFY) Shell_NotifyIconW(NIM_ADD, &n);
    if (n.hIcon) DestroyIcon(n.hIcon);
}

// ---- the monitor: the launcher, the game, the DLL's status file, the power settings
struct DllStatus { char state[16]; char why[96]; int menu; DWORD pid; };
static bool readStatus(DllStatus* s) {
    char p[MAX_PATH]; _snprintf(p, MAX_PATH, "%s\\aotr_status.txt", g_out); p[MAX_PATH - 1] = 0;
    FILE* f = fopen(p, "rb"); if (!f) return false;
    char b[512]; size_t n = fread(b, 1, sizeof(b) - 1, f); b[n] = 0; fclose(f);
    ZeroMemory(s, sizeof(*s));
    for (char* line = strtok(b, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "state=", 6)) lstrcpynA(s->state, line + 6, sizeof(s->state));
        else if (!strncmp(line, "why=", 4)) lstrcpynA(s->why, line + 4, sizeof(s->why));
        else if (!strncmp(line, "menu=", 5)) s->menu = atoi(line + 5);
        else if (!strncmp(line, "pid=", 4)) s->pid = (DWORD)strtoul(line + 4, NULL, 10);
    }
    return s->state[0] != 0;
}
static ULONGLONG procAgeMs(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) return 0;
    FILETIME c, e, k, u, now; ULONGLONG age = 0;
    if (GetProcessTimes(h, &c, &e, &k, &u)) { GetSystemTimeAsFileTime(&now);
        age = ((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - (((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime)) / 10000; }
    CloseHandle(h);
    return age;
}
static DWORD WINAPI monitor(LPVOID) {
    DWORD gamePid = 0, injectedPid = 0, injectAt = 0, goneSince = 0, lastLauncher = 0, lastStatusRead = 0;
    bool warmed = false, powered = false, longNoted = false, yellowNoted = false, redNoted = false;
    char mk[MAX_PATH]; _snprintf(mk, MAX_PATH, "%s\\WARM_OFF", g_out); mk[MAX_PATH - 1] = 0;
    bool warmOff = exists(mk);
    while (!g_quit) {
        DWORD now = GetTickCount();
        DWORD launcher = findPid("AotR_Launcher.exe", 0);
        if (launcher && launcher != lastLauncher) {          // the AOTR folder, learned from the running launcher
            lastLauncher = launcher;
            char img[MAX_PATH] = "";
            if (imagePath(launcher, img, MAX_PATH)) {
                lstrcpynA(g_aotrExe, img, MAX_PATH); folderOf(img, g_aotrRoot, sizeof(g_aotrRoot));
                char saved[MAX_PATH]; _snprintf(saved, MAX_PATH, "%s\\aotr_path.txt", g_out); saved[MAX_PATH - 1] = 0;
                FILE* f = fopen(saved, "w"); if (f) { fprintf(f, "%s\n", img); fclose(f); }
            }
            if (!warmed && !warmOff && g_aotrRoot[0]) { warmed = true; warmStart(); }
        }
        DWORD pid = findPid("game.dat", 0);
        if (pid && pid != gamePid) {                            // a new game.dat: one attempt each
            gamePid = pid; goneSince = 0; longNoted = yellowNoted = redNoted = false;
            warmStopNow();
            Sleep(800);                                         // let the game finish loading its own modules
            char img[MAX_PATH] = "";
            if (!imagePath(pid, img, MAX_PATH)) say("game.dat (pid %lu): its path could not be read - left alone", pid);
            else if (!isAotrGame(img)) say("game.dat (pid %lu) is not AOTR's (%s) - left alone", pid, img);
            else {
                if (!powered) { powUp(); powered = true; }
                if (!exists(g_dll)) {
                    say("bfme2_accel.dll is missing (%s)", g_dll);
                    uiSet(IDI_RED, L"DLL missing  |  DLL topilmadi", L"Accelerator missing / Tezlatgich yo'q",
                          L"bfme2_accel.dll is missing - an antivirus may have removed it. Reinstall and add the folder to its exceptions.\n"
                          L"bfme2_accel.dll topilmadi - antivirus o'chirgan bo'lishi mumkin. Qayta o'rnating.", NIIF_ERROR);
                    redNoted = true;
                } else if (injectInto(pid)) {
                    injectedPid = pid; injectAt = GetTickCount();
                    now = injectAt; lastStatusRead = injectAt;      // (v71.1: the 60 s count starts here - "now" was read before the 0.8 s wait and the injection)
                    say("game found - the accelerator is loaded (pid %lu): %s", pid, img);
                    uiSet(IDI_GOLD, L"Starting in the game...  |  O'yinda ishga tushmoqda...");
                } else {
                    say("the accelerator could not be loaded (pid %lu)", pid);
                    uiSet(IDI_RED, L"Not loaded  |  Yuklanmadi", L"Accelerator not loaded / Tezlatgich yuklanmadi",
                          L"It could not be loaded into the game (blocked?). The game runs as usual.\n"
                          L"O'yinga yuklab bo'lmadi (bloklangan bo'lishi mumkin). O'yin odatdagidek ishlaydi.", NIIF_ERROR);
                    redNoted = true;
                }
            }
        }
        if (pid) {
            goneSince = 0;
            turboPoll();
            if (injectedPid == pid && now - lastStatusRead >= 2000) {
                lastStatusRead = now;
                DllStatus st; bool have = readStatus(&st) && st.pid == pid;
                if (!have) {
                    if ((LONG)(now - injectAt) > 60000 && !redNoted) {
                        redNoted = true;
                        uiSet(IDI_RED, L"Not running in the game  |  O'yinda ishlamayapti", L"Accelerator not running / Tezlatgich ishlamayapti",
                              L"It did not start inside the game - an antivirus may block it. The game runs as usual.\n"
                              L"O'yin ichida ishga tushmadi - antivirus bloklagan bo'lishi mumkin. O'yin odatdagidek ishlaydi.", NIIF_ERROR);
                    }
                } else {
                    wchar_t why[96]; wfrom(st.why, why, 96);
                    wchar_t line[160];
                    if (!strcmp(st.state, "red")) {
                        _snwprintf(line, 160, L"Main part off: %s  |  Asosiy qism ishlamayapti", why); line[159] = 0;
                        if (!redNoted) { redNoted = true; uiSet(IDI_RED, line, L"Main part off / Asosiy qism o'chiq", line, NIIF_ERROR); }
                        else uiSet(IDI_RED, line);
                    } else if (!strcmp(st.state, "yellow")) {
                        _snwprintf(line, 160, L"A part switched itself off: %s  |  Bir qism o'zini o'chirdi", why); line[159] = 0;
                        if (!yellowNoted) { yellowNoted = true;
                            uiSet(IDI_YELLOW, line, L"A part is off / Bir qism o'chiq",
                                  L"A self-check switched one part off; the game goes on normally with the original code.\n"
                                  L"Tekshiruv bir qismni o'chirdi; o'yin asl kod bilan odatdagidek davom etadi.", NIIF_WARNING); }
                        else uiSet(IDI_YELLOW, line);
                    } else uiSet(IDI_GOLD, L"Running - all parts on  |  Ishlayapti - hammasi yoqiq");
                    if (st.menu && !longNoted && procAgeMs(pid) >= 150ull * 60 * 1000) {
                        longNoted = true;
                        uiBalloon(L"Long session / Uzoq o'yin",
                              L"Over 2.5 hours in one game session. Restarting the game keeps the graphics clean.\n"
                              L"2.5 soatdan ko'p o'ynadingiz. O'yinni qayta ochsangiz, grafika toza qoladi.", NIIF_INFO);
                    }
                }
            }
        } else if (gamePid) {
            if (!goneSince) goneSince = now ? now : 1;
            else if (now - goneSince > 20000) {                // the game has been gone for 20 s: the session is over
                say("the game is closed");
                if (powered) { powRestore(false); powered = false; }
                char a[MAX_PATH]; _snprintf(a, MAX_PATH, "%s\\aotr_throttle_ack.txt", g_out); DeleteFileA(a);
                _snprintf(a, MAX_PATH, "%s\\aotr_throttle.txt", g_out); DeleteFileA(a);
                gamePid = injectedPid = 0; goneSince = 0;
                uiSet(IDI_GOLD, L"Waiting for the game  |  O'yin kutilmoqda");
                if (g_dev) { PostMessageW(g_wnd, WM_AOTR_QUIT, 0, 0); break; }
            }
        }
        Sleep(pid ? 1000 : (launcher ? 500 : 2000));          // while the game runs: once a second is enough (status, throttle)
    }
    if (powered) powRestore(true);
    return 0;
}

// ---- the window: tray icon, its menu, messages from other starts
enum { CMD_START = 1, CMD_AUTOSTART, CMD_LOG, CMD_UNINSTALL, CMD_EXIT };
static void uninstallNow() {
    taskDelete();
    wchar_t lnk[MAX_PATH]; shortcutPath(lnk); DeleteFileW(lnk);
    powRestore(true);
    char dir[MAX_PATH]; installDir(dir);                 // the files go at the next restart (this exe is running)
    WIN32_FIND_DATAA fd; char pat[MAX_PATH]; _snprintf(pat, MAX_PATH, "%s\\*", dir); pat[MAX_PATH - 1] = 0;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) { do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char f[MAX_PATH]; _snprintf(f, MAX_PATH, "%s\\%s", dir, fd.cFileName); f[MAX_PATH - 1] = 0;
        if (!DeleteFileA(f)) MoveFileExA(f, NULL, MOVEFILE_DELAY_UNTIL_REBOOT); } while (FindNextFileA(h, &fd)); FindClose(h); }
    if (!RemoveDirectoryA(dir)) MoveFileExA(dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    msg(L"AOTR Accelerator is uninstalled (the last files go at the next restart).\nAOTR Accelerator o'chirildi (qolgan fayllar keyingi qayta yoqishda o'chadi).");
}
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_wmTaskbarCreated && m) { trayUpdate(NIM_ADD); return 0; }
    switch (m) {
    case WM_AOTR_LAUNCH: launchAotr(); return 0;
    case WM_AOTR_QUIT: DestroyWindow(h); return 0;
    case WM_AOTR_STATE: trayUpdate(NIM_MODIFY); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (w) { InterlockedExchange(&g_quit, 1); if (g_powChanged) powRestore(true); } return 0;
    case WM_AOTR_TRAY:
        if (LOWORD(l) == WM_LBUTTONDBLCLK) { launchAotr(); return 0; }
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            HMENU mn = CreatePopupMenu();
            wchar_t st[200]; EnterCriticalSection(&g_uiCs); lstrcpynW(st, g_status, 200); LeaveCriticalSection(&g_uiCs);
            AppendMenuW(mn, MF_STRING, CMD_START, L"Start Age of the Ring  |  AOTR'ni ochish");
            AppendMenuW(mn, MF_STRING | MF_GRAYED, 0, st);
            AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
            if (!g_dev) AppendMenuW(mn, MF_STRING | (autostartOn() ? MF_CHECKED : 0), CMD_AUTOSTART, L"Start with Windows  |  Windows bilan birga");
            AppendMenuW(mn, MF_STRING, CMD_LOG, L"Open the folder (log)  |  Papkani ochish (log)");
            if (!g_dev) AppendMenuW(mn, MF_STRING, CMD_UNINSTALL, L"Uninstall  |  O'chirib tashlash");
            AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
            AppendMenuW(mn, MF_STRING, CMD_EXIT, L"Exit  |  Chiqish");
            POINT p; GetCursorPos(&p); SetForegroundWindow(h);
            int c = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
            DestroyMenu(mn); PostMessageW(h, WM_NULL, 0, 0);
            if (c == CMD_START) launchAotr();
            else if (c == CMD_AUTOSTART) {
                char self[MAX_PATH], f[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
                _snprintf(f, MAX_PATH, "%s\\autostart_off", g_out); f[MAX_PATH - 1] = 0;
                if (autostartOn()) { FILE* x = fopen(f, "wb"); if (x) fclose(x); taskRegister(self, false); }
                else { DeleteFileA(f); taskRegister(self, true); }
            } else if (c == CMD_LOG) ShellExecuteA(NULL, "open", g_out, NULL, NULL, SW_SHOWNORMAL);
            else if (c == CMD_UNINSTALL) {
                if (MessageBoxW(NULL, L"Remove AOTR Accelerator (autostart, shortcut and files)?\nAOTR Accelerator o'chirilsinmi (avto-ishga tushish, yorliq va fayllar)?",
                                L"AOTR Accelerator", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES) { uninstallNow(); DestroyWindow(h); }
            } else if (c == CMD_EXIT) DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_quit, 1);
        trayUpdate(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static int resident(bool launch) {
    HANDLE mx = CreateMutexW(NULL, TRUE, L"Local\\AotrAcceleratorResident");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {        // one copy only
        HWND other = FindWindowW(kWndClass, NULL);
        if (g_dev && other) {                            // a PLAY.bat start takes over from an installed copy: one DLL only
            DWORD op = 0; GetWindowThreadProcessId(other, &op);
            PostMessageW(other, WM_AOTR_QUIT, 0, 0);
            HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, op); if (p) { WaitForSingleObject(p, 8000); CloseHandle(p); }
            WaitForSingleObject(mx, 5000);
        } else {
            if (other && launch) PostMessageW(other, WM_AOTR_LAUNCH, 0, 0);
            return 0;
        }
    }
    if (!g_aotrExe[0] && findAotr(false)) folderOf(g_aotrExe, g_aotrRoot, sizeof(g_aotrRoot));
    { char f[MAX_PATH]; powFile(f); if (exists(f)) powRestore(false); }   // a session that ended without putting them back
    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc)); wc.lpfnWndProc = wndProc; wc.hInstance = g_hi; wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(g_hi, MAKEINTRESOURCEW(IDI_GOLD));
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(kWndClass, L"AOTR Accelerator", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, g_hi, NULL);
    if (!g_wnd) { say("the window could not be made (%lu)", GetLastError()); return 1; }
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_wnd, WM_AOTR_LAUNCH, MSGFLT_ALLOW, NULL);   // the shortcut's start runs without admin rights
    ChangeWindowMessageFilterEx(g_wnd, WM_AOTR_QUIT, MSGFLT_ALLOW, NULL);
    if (g_wmTaskbarCreated) ChangeWindowMessageFilterEx(g_wnd, g_wmTaskbarCreated, MSGFLT_ALLOW, NULL);
    trayUpdate(NIM_ADD);
    say("resident started (%s%s)", g_dev ? "from the source tree" : "installed", launch ? ", opening the AotR launcher" : "");
    HANDLE th = CreateThread(NULL, 0, monitor, NULL, 0, NULL);
    if (launch) PostMessageW(g_wnd, WM_AOTR_LAUNCH, 0, 0);
    MSG q;
    while (GetMessageW(&q, NULL, 0, 0) > 0) { TranslateMessage(&q); DispatchMessageW(&q); }
    InterlockedExchange(&g_quit, 1);
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    if (g_powChanged) powRestore(true);
    say("resident ended");
    if (mx) { ReleaseMutex(mx); CloseHandle(mx); }
    return 0;
}

// ---- the install (from the package folder): one Windows prompt, then everything is in place
static bool copyInto(const char* dir, const char* from, const char* leaf, bool onlyIfMissing) {
    char d[MAX_PATH]; _snprintf(d, MAX_PATH, "%s\\%s", dir, leaf); d[MAX_PATH - 1] = 0;
    if (onlyIfMissing && exists(d)) return true;
    if (!exists(from)) return !onlyIfMissing ? false : true;
    return CopyFileA(from, d, FALSE) != FALSE;
}
static HWND waitResident(DWORD ms) { for (DWORD t = 0; t < ms; t += 100) { HWND w = FindWindowW(kWndClass, NULL); if (w) return w; Sleep(100); } return NULL; }
static int doInstall() {
    char dir[MAX_PATH]; installDir(dir);
    if (aotrGameRunning()) { msg(L"Please close the game first, then run this again.\nAvval o'yinni yoping, keyin buni qayta ishga tushiring.", MB_ICONWARNING); return 1; }
    HWND old = FindWindowW(kWndClass, NULL);             // an older copy in the tray: it ends first (its files are replaced)
    if (old) { DWORD op = 0; GetWindowThreadProcessId(old, &op); PostMessageW(old, WM_AOTR_QUIT, 0, 0);
               HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, op);
               if (p) { if (WaitForSingleObject(p, 8000) != WAIT_OBJECT_0) TerminateProcess(p, 0); CloseHandle(p); } }
    CreateDirectoryA(dir, NULL);
    char self[MAX_PATH], src[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    bool ok = copyInto(dir, self, kExeName, false) && copyInto(dir, g_dll, "bfme2_accel.dll", false);
    static const char* const kDocs[] = { "README_EN.txt", "QOLLANMA_UZ.txt" };
    for (int i = 0; i < 2; ++i) { _snprintf(src, MAX_PATH, "%s\\%s", g_out, kDocs[i]); src[MAX_PATH - 1] = 0; if (exists(src)) copyInto(dir, src, kDocs[i], false); }
    _snprintf(src, MAX_PATH, "%s\\aotr_learned.txt", g_out); src[MAX_PATH - 1] = 0; copyInto(dir, src, "aotr_learned.txt", true);
    _snprintf(src, MAX_PATH, "%s\\aotr_path.txt", g_out); src[MAX_PATH - 1] = 0; copyInto(dir, src, "aotr_path.txt", true);
    if (!ok) {
        say("install: copying into %s failed (%lu)", dir, GetLastError());
        msg(L"The files could not be copied (an antivirus may block them).\nFayllarni nusxalab bo'lmadi (antivirus to'sgan bo'lishi mumkin).", MB_ICONERROR);
        return 2;
    }
    char exe[MAX_PATH]; _snprintf(exe, MAX_PATH, "%s\\%s", dir, kExeName); exe[MAX_PATH - 1] = 0;
    char off[MAX_PATH]; _snprintf(off, MAX_PATH, "%s\\autostart_off", dir); off[MAX_PATH - 1] = 0;
    bool task = taskRegister(exe, !exists(off));
    bool lnk = shortcutMake(exe);
    say("install: %s - task %s, shortcut %s", dir, task ? "ok" : "FAILED", lnk ? "ok" : "FAILED");
    if (!(task && taskRun())) {                          // no task: started directly (it then runs with these rights)
        char cmd[MAX_PATH + 32]; _snprintf(cmd, sizeof(cmd), "\"%s\" --background", exe); cmd[sizeof(cmd) - 1] = 0;
        STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    HWND w = waitResident(10000);
    if (MessageBoxW(NULL,
            L"AOTR Accelerator is installed.\n"
            L"From now on just open the AOTR launcher as usual (or the desktop shortcut) - the accelerator loads by itself.\n"
            L"In the game a small light in the top right corner shows it works; F7 hides it.\n\n"
            L"AOTR Accelerator o'rnatildi.\n"
            L"Endi AOTR launcherini odatdagidek oching (yoki ish stolidagi yorliqni) - tezlatgich o'zi yuklanadi.\n"
            L"O'yinda yuqori o'ng burchakdagi kichik belgi ishlayotganini ko'rsatadi; F7 uni yashiradi.\n\n"
            L"Start Age of the Ring now?  /  AOTR hozir ochilsinmi?",
            L"AOTR Accelerator", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES && w)
        PostMessageW(w, WM_AOTR_LAUNCH, 0, 0);
    return 0;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR cmd, int) {
    g_hi = hi;
    InitializeCriticalSection(&g_sayCs); InitializeCriticalSection(&g_uiCs);
    SetUnhandledExceptionFilter(startCrash);
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, MAX_PATH);
    folderOf(self, g_out, sizeof(g_out));
    _snprintf(g_dll, sizeof(g_dll), "%s\\bfme2_accel.dll", g_out); g_dll[sizeof(g_dll) - 1] = 0;
    { char src[MAX_PATH], up[MAX_PATH]; folderOf(g_out, up, MAX_PATH); _snprintf(src, MAX_PATH, "%s\\src\\aotr_accel.cpp", up); src[MAX_PATH - 1] = 0; g_dev = exists(src); }
    { char lg[MAX_PATH]; _snprintf(lg, MAX_PATH, "%s\\aotr_start.log", g_out); lg[MAX_PATH - 1] = 0;
      WIN32_FILE_ATTRIBUTE_DATA fa; if (GetFileAttributesExA(lg, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (1u << 20))) DeleteFileA(lg); }
    bool bg = strstr(cmd, "--background") != NULL, wantInstall = strstr(cmd, "--install") != NULL;
    char dir[MAX_PATH]; installDir(dir);
    bool installedCopy = _stricmp(g_out, dir) == 0;
    bool elevated = isElevated();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int rc = 0;
    if (!g_dev && !installedCopy) {                      // the package folder: install (or update) it
        if (!elevated) { if (!relaunchElevated(L"--install")) msg(L"The install needs administrator rights (answer Yes).\nO'rnatish uchun administrator ruxsati kerak (\"Ha\" ni bosing).", MB_ICONWARNING); }
        else rc = doInstall();
        (void)wantInstall;
    } else if (!elevated && g_dev) {                     // v82: PLAY.bat / PLAY_DIAG.bat - this build must be the one that loads, so it
        say("started from the source tree without administrator rights - asking for them");   // takes over even from a running installed copy
        if (!relaunchElevated(L"--launch")) msg(L"Starting the game with the accelerator needs administrator rights (answer Yes).\nAkseleratorni ishga tushirish uchun administrator ruxsati kerak (\"Ha\" ni bosing).", MB_ICONWARNING);
    } else if (!elevated) {                              // the shortcut: the running copy opens the launcher
        HWND w = FindWindowW(kWndClass, NULL);
        if (w) PostMessageW(w, WM_AOTR_LAUNCH, 0, 0);
        else if (!g_dev && taskExists() && taskRun()) { w = waitResident(10000); if (w) PostMessageW(w, WM_AOTR_LAUNCH, 0, 0); }
        else relaunchElevated(L"--launch");
    } else rc = resident(!bg);
    CoUninitialize();
    return rc;
}
