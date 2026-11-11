// ---------------------------------------------------------------- the GeForce, not the integrated chip (v30, aotr_gpupref.inc)
// This laptop has two graphics chips: the Intel UHD 630 that drives the screen and the GeForce GTX 1050 Ti. DXVK
// skips the Intel chip by itself (its own log: "Skipping Vulkan 1.2 adapter: Intel(R) UHD Graphics 630"), but
// Windows' own Direct3D 9 lists exactly ONE adapter for a program Windows has no preference for - the Intel one
// (work\probe11\adapters.exe on this machine: "adapters: 1 / 0: Intel(R) UHD Graphics 630"). Every session without
// DXVK so far ran on it: the crash dump of 2026-10-20 21:00 has igdumdim32.dll (Intel's Direct3D 9 driver) loaded.
//
// Windows keeps a per-program GPU choice under HKEY_CURRENT_USER\Software\Microsoft\DirectX\UserGpuPreferences -
// the same value Settings > System > Display > Graphics writes. With "GpuPreference=2;" (high performance) for the
// program, the same probe sees "0: NVIDIA GeForce GTX 1050 Ti", and it does even when the value is written after
// d3d9.dll is loaded, as long as Direct3DCreate9 has not run yet (work\probe11\gpulate.exe). So the value is set for
// game.dat here, as early as this DLL runs - from then on it is simply there for every later start too.
//
// It is the player's own setting store, not the game folder: nothing of the game is written and nothing the
// simulation does can see which chip draws the picture. What was there before is kept in gpupref_before.txt beside
// this DLL, and RESTORE.bat puts it back (or removes the value if there was none).
//
// v30 step 2 - the key is named in the log, and so is whether the value can work in THIS start:
//   * the key is game.dat's full long path (GetLongPathName - the form Settings > Graphics and RUN.ps1 write). RUN.ps1
//     leaves the key it wrote in gpupref_run.txt beside this DLL; the log says whether the two are the same.
//   * Windows reads the preference when Direct3D 9 picks its adapter. The loader injects this DLL about 0.8 s after
//     game.dat has started; if the game made its Direct3D object before that, a value written now only counts from the
//     NEXT start. Which chip really draws is measured, not assumed: the overlay's renderer line (the driver that is
//     loaded) and its "gpu: verdict" line, and the panel names this case in words.
static char g_gpuKey[MAX_PATH] = "";
static volatile LONG g_gpuState = 0;      // 0 not set (could not be written), 1 already there when this DLL started, 2 written by this DLL in this start
static volatile LONG g_gpuDrvBefore = 0;  // a Direct3D 9 driver was already loaded when this DLL wrote the value
static bool gpuD3dDriverLoaded() {
    static const char* kDrv[] = { "nvd3dum.dll", "nvldumd.dll", "igdumdim32.dll", "aticfx32.dll" };
    for (int i = 0; i < 4; ++i) if (GetModuleHandleA(kDrv[i])) return true;
    return false;
}
static void gpuRunKey(char* out, int n) {                       // the key RUN.ps1 wrote (gpupref_run.txt), "" when there is none
    out[0] = 0;
    char p[MAX_PATH]; aotrPath(p, "gpupref_run.txt");
    HANDLE fh = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    DWORD got = 0; ReadFile(fh, out, (DWORD)n - 1, &got, NULL); CloseHandle(fh); out[got] = 0;
    char* q = out; if ((BYTE)q[0] == 0xEF && (BYTE)q[1] == 0xBB && (BYTE)q[2] == 0xBF) q += 3;
    char* e = strpbrk(q, "\r\n"); if (e) *e = 0;
    if (q != out) memmove(out, q, (size_t)lstrlenA(q) + 1);
}
// gpupref_before.txt: one "program= / had= / value=" group per key - what was there before anything of ours wrote it.
// RUN.ps1 adds the group for its key, this DLL the group for its own when that one is different; RESTORE.ps1 puts
// every group back. A key that already has a group is left alone (the first record is the player's own value).
static void gpuRecordBefore(const char* exe, bool had, const char* cur) {
    char before[MAX_PATH]; aotrPath(before, "gpupref_before.txt");
    static char old[8192]; old[0] = 0; DWORD got = 0;
    HANDLE fh = CreateFileA(before, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (fh != INVALID_HANDLE_VALUE) { ReadFile(fh, old, sizeof(old) - 1, &got, NULL); old[got] = 0; CloseHandle(fh); }
    char want[MAX_PATH + 16]; int wl = wsprintfA(want, "program=%s", exe);
    for (const char* p = old; *p; ) {
        const char* e = strchr(p, '\n'); int len = e ? (int)(e - p) : lstrlenA(p), l2 = len;
        if (l2 && p[l2 - 1] == '\r') --l2;
        if (l2 == wl && !_strnicmp(p, want, (size_t)wl)) return;
        if (!e) break;
        p = e + 1;
    }
    fh = CreateFileA(before, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    char b[1300]; int m = wsprintfA(b, "%sprogram=%s\r\nhad=%d\r\nvalue=%s\r\n", (got && old[got - 1] != '\n') ? "\r\n" : "", exe, had ? 1 : 0, cur);
    DWORD wr = 0; WriteFile(fh, b, (DWORD)m, &wr, NULL); CloseHandle(fh);
}
static void installGpuPref() {
    char raw[MAX_PATH]; raw[0] = 0;
    if (!GetModuleFileNameA(NULL, raw, MAX_PATH) || !raw[0]) return;
    char exe[MAX_PATH]; DWORD ln = GetLongPathNameA(raw, exe, MAX_PATH);
    if (!ln || ln >= MAX_PATH) lstrcpynA(exe, raw, MAX_PATH);
    lstrcpynA(g_gpuKey, exe, MAX_PATH);
    char runKey[MAX_PATH]; gpuRunKey(runKey, MAX_PATH);
    bool rawDiffers = lstrcmpiA(raw, exe) != 0;
    logf("gpu: key = %s%s%s%s | RUN.ps1 key: %s%s",
         exe, rawDiffers ? " (the process gave its name as " : "", rawDiffers ? raw : "", rawDiffers ? ")" : "",
         !runKey[0] ? "none (gpupref_run.txt is missing - this start did not come from RUN.bat)" : (lstrcmpiA(runKey, exe) ? "DIFFERENT - " : "the same"),
         (runKey[0] && lstrcmpiA(runKey, exe)) ? runKey : "");
    if (runKey[0] && lstrcmpiA(runKey, exe))
        logf("gpu: WARNING - the value RUN.ps1 wrote is for another program path, so it does not apply to this game.dat; this DLL writes the value for the key above.");
    HKEY k = NULL;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        logf("gpu: the per-program GPU preference could not be opened - Windows decides which chip draws (DXVK picks the GeForce itself).");
        return;
    }
    char cur[512]; DWORD type = 0, cb = sizeof(cur) - 1; cur[0] = 0;
    LONG q = RegQueryValueExA(k, exe, NULL, &type, (BYTE*)cur, &cb);
    bool had = (q == ERROR_SUCCESS && type == REG_SZ);
    if (had) cur[cb < sizeof(cur) ? cb : sizeof(cur) - 1] = 0; else cur[0] = 0;
    if (had && strstr(cur, "GpuPreference=2;")) {
        g_gpuState = 1;
        logf("gpu: game.dat already has Windows' high-performance GPU preference (%s) - it was there before this DLL ran (RUN.ps1 writes it before the game starts), so Windows' own Direct3D 9 should use the GeForce in this start; the renderer line says which chip really draws.", cur);
        RegCloseKey(k); return;
    }
    // the new value: the old one with any GpuPreference=...; taken out, and GpuPreference=2; put first
    char nv[600]; int n = wsprintfA(nv, "GpuPreference=2;");
    for (const char* p = cur; *p; ) {
        const char* e = strchr(p, ';'); int len = e ? (int)(e - p) + 1 : lstrlenA(p);
        if (_strnicmp(p, "GpuPreference=", 14) && len > 0 && n + len < (int)sizeof(nv) - 1) { memcpy(nv + n, p, (size_t)len); n += len; nv[n] = 0; }
        p += len; if (!e) break;
    }
    gpuRecordBefore(exe, had, cur);
    g_gpuDrvBefore = gpuD3dDriverLoaded() ? 1 : 0;
    LONG w = RegSetValueExA(k, exe, 0, REG_SZ, (const BYTE*)nv, (DWORD)n + 1);
    RegCloseKey(k);
    if (w == ERROR_SUCCESS) {
        g_gpuState = 2;
        logf("gpu: Windows' high-performance GPU preference written now, during this start, for %s (\"%s\"%s%s). RESTORE.bat puts the old value back. %s",
             exe, nv, had ? ", was " : ", there was none", had ? cur : "",
             g_gpuDrvBefore ? "A Direct3D 9 driver was ALREADY loaded when it was written - Direct3D has chosen its chip, so the value counts from the NEXT start."
                            : "If the game made its Direct3D object before this moment, it counts only from the NEXT start - the renderer line says which chip draws now.");
    } else
        logf("gpu: the GPU preference could not be written (error %ld) - Windows decides which chip draws.", w);
}
