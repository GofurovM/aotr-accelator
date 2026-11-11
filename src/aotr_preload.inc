// aotr_preload.inc - v46: what a match can bring on screen, found from the mod's own data and read ahead (phase 1).
//
// WHY: the hitches left in battle come when something appears for the first time in the session - a hero, a spell, an
// upgrade's effect, a summoned unit (test-28: "files first opened during play" - .w3d models and animations, .dds
// textures of effects, .wav sounds; several of those hitches waited on the disk). Which things can appear depends on
// the factions in the match, and the mod adds its own (Rohan, Woodland Realm, Lothlorien ...), so nothing here names
// a faction: it is all read from the mod's INI files and from the objects actually on the map.
//
// HOW (phase 1 - the files):
//   1. at start, on a background thread: every file under the game's folders is listed (art and sound files by base
//      name), and every INI file of the mod is read and cut into definitions (Object, ChildObject, FXList,
//      FXParticleSystem, ObjectCreationList, Weapon, SpecialPower, Upgrade, CommandSet, CommandButton, AudioEvent ...;
//      #define macros and #include files are definitions of their own) and the words each one uses;
//   2. when a match begins (its first logic step, on the game thread): the template names of the objects on the map
//      are taken - the players' buildings and units, whoever the factions are;
//   3. from those names every definition they mention is followed, and what those mention, and so on (what a
//      fortress can build, what a unit can upgrade to, which weapon, which effect, which particle system, which
//      sound ...). Every word of those definitions that is the name of a model, texture or sound file is collected;
//   4. those files are read into Windows' file cache on a background thread at background priority - models first,
//      then textures, then sounds - up to a budget, and never while the machine is short of memory.
// Nothing in the game is changed: its own loading reads the same files, only without waiting for the disk.
// The log says how many of the files the game later opened for the first time during play were found in advance -
// the measure for phase 2 (making the models themselves ahead of time). Off: PRELOAD_OFF next to the DLL.

#define PL_STR_ARENA   (48u << 20)
#define PL_TAB         (1u << 21)              // intern table slots
#define PL_MAXSTR      (1u << 20)              // distinct words
#define PL_MAXEDGE     (8u << 20)             // (definition, word) pairs while reading
#define PL_MAXASSET    (1u << 18)
struct PlStr { DWORD off; DWORD hash; LONG asset; LONG defFirst; };   // asset: index into g_plAsset or -1; defFirst: CSR start or -1
struct PlAsset { DWORD pathOff; DWORD size; BYTE kind; BYTE queued; };   // kind 0 model, 1 texture, 2 sound
static PlStr* g_plStr = NULL; static LONG g_plNStr = 0;
static DWORD* g_plTab = NULL;
static char* g_plArena = NULL; static DWORD g_plArenaUsed = 0, g_plArenaCommit = 0;
static PlAsset* g_plAsset = NULL; static LONG g_plNAsset = 0;
static DWORD* g_plEdge = NULL; static LONG g_plNEdge = 0; static DWORD g_plEdgeCommit = 0;   // pairs: def id, word id
static LONG* g_plCsr = NULL; static LONG* g_plCsrWords = NULL;                          // built after reading
static BYTE* g_plIsDef = NULL;
static volatile LONG g_plOn = 0, g_plIndexReady = 0, g_plIndexFailed = 0, g_plNIni = 0, g_plNDef = 0, g_plNFiles = 0;
static volatile LONG64 g_plIniBytes = 0;
static DWORD g_plIndexMs = 0;
// the match's seeds, handed from the game thread
static char* g_plSeed = NULL; static volatile LONG g_plSeedLen = 0, g_plSeedN = 0, g_plSeedReady = 0;
static DWORD g_plSeedFrameLast = 0xFFFFFFFF, g_plSeedMap = 0;
static HANDLE g_plEvt = NULL;
// the closure of the current match
static BYTE* g_plInSet = NULL;                    // per asset: 1 found for this match
static volatile LONG g_plSetModels = 0, g_plSetTex = 0, g_plSetSnd = 0, g_plSetDefs = 0;
static volatile LONG64 g_plSetBytes = 0, g_plReadBytes = 0;
static volatile LONG g_plReadFiles = 0, g_plReadMs = 0, g_plStopMem = 0, g_plMatches = 0;
static volatile LONG g_plPlayFirst = 0, g_plPlayHit = 0, g_plPlayMissN = 0;
static char g_plMiss[1400]; static LONG g_plMissLen = 0;

static __forceinline DWORD plHash(const char* p, int n) { DWORD h = 0x811C9DC5u; for (int i = 0; i < n; ++i) h = (h ^ (BYTE)p[i]) * 0x01000193u; return h; }
// a word's id (made if new); words are kept lower case. -1 when full.
static LONG plIntern(const char* p, int n, bool make) {
    if (n <= 0 || n > 200) return -1;
    DWORD h = plHash(p, n);
    for (DWORD i = h & (PL_TAB - 1), k = 0; k < PL_TAB; i = (i + 1) & (PL_TAB - 1), ++k) {
        DWORD v = g_plTab[i];
        if (!v) {
            if (!make || g_plNStr >= (LONG)PL_MAXSTR || g_plArenaUsed + (DWORD)n + 1 > PL_STR_ARENA) return -1;
            if (g_plArenaUsed + (DWORD)n + 1 > g_plArenaCommit) {
                if (!VirtualAlloc(g_plArena + g_plArenaCommit, 1u << 20, MEM_COMMIT, PAGE_READWRITE)) return -1;
                g_plArenaCommit += 1u << 20;
            }
            LONG id = g_plNStr++;
            PlStr& s = g_plStr[id]; s.off = g_plArenaUsed; s.hash = h; s.asset = -1; s.defFirst = -1;
            memcpy(g_plArena + g_plArenaUsed, p, (size_t)n); g_plArena[g_plArenaUsed + n] = 0; g_plArenaUsed += (DWORD)n + 1;
            g_plTab[i] = (DWORD)id + 1;
            return id;
        }
        const PlStr& s = g_plStr[v - 1];
        if (s.hash == h && !memcmp(g_plArena + s.off, p, (size_t)n) && !g_plArena[s.off + n]) return (LONG)(v - 1);
    }
    return -1;
}
static LONG plInternZ(const char* p) { return plIntern(p, lstrlenA(p), true); }
static bool plEdge(LONG def, LONG word) {
    if (def < 0 || word < 0 || def == word) return true;
    if ((DWORD)g_plNEdge >= PL_MAXEDGE) return false;
    if ((DWORD)(g_plNEdge + 1) * 8 > g_plEdgeCommit) {
        if (!VirtualAlloc((BYTE*)g_plEdge + g_plEdgeCommit, 1u << 20, MEM_COMMIT, PAGE_READWRITE)) return false;
        g_plEdgeCommit += 1u << 20;
    }
    if (g_plNEdge && g_plEdge[2 * (g_plNEdge - 1)] == (DWORD)def && g_plEdge[2 * (g_plNEdge - 1) + 1] == (DWORD)word) return true;
    g_plEdge[2 * g_plNEdge] = (DWORD)def; g_plEdge[2 * g_plNEdge + 1] = (DWORD)word; g_plNEdge++;
    return true;
}

// ---------------------------------------------------------------- 1a: the files (art and sound by base name, ini to read)
static WCHAR** g_plIni = NULL; static LONG g_plIniCap = 0;
static void plAddIni(const WCHAR* path) {
    if (g_plNIni >= g_plIniCap) {
        LONG cap = g_plIniCap ? g_plIniCap * 2 : 4096;
        WCHAR** n = (WCHAR**)HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR*) * cap); if (!n) return;
        if (g_plIni) { memcpy(n, g_plIni, sizeof(WCHAR*) * g_plNIni); HeapFree(GetProcessHeap(), 0, g_plIni); }
        g_plIni = n; g_plIniCap = cap;
    }
    int len = lstrlenW(path); WCHAR* c = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (len + 1) * 2); if (!c) return;
    memcpy(c, path, (len + 1) * 2); g_plIni[g_plNIni++] = c;
}
static void plWalk(WCHAR* dir, int len, int depth) {
    if (depth > 16 || len > 900) return;
    lstrcpyW(dir + len, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(dir, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 2 /* FIND_FIRST_EX_LARGE_FETCH */);
    dir[len] = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const WCHAR* n = fd.cFileName;
        if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
        int nl = lstrlenW(n);
        if (len + 1 + nl > 1000) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dir[len] = L'\\'; lstrcpyW(dir + len + 1, n);
            plWalk(dir, len + 1 + nl, depth + 1);
            dir[len] = 0;
            continue;
        }
        g_plNFiles++;
        const WCHAR* dot = NULL; for (const WCHAR* q = n; *q; ++q) if (*q == L'.') dot = q;
        if (!dot) continue;
        WCHAR ext[8]; int el = 0; for (const WCHAR* q = dot + 1; *q && el < 7; ++q) ext[el++] = (WCHAR)((*q >= L'A' && *q <= L'Z') ? *q + 32 : *q); ext[el] = 0;
        int kind = -1, isIni = 0;
        if (!lstrcmpW(ext, L"w3d")) kind = 0;
        else if (!lstrcmpW(ext, L"dds") || !lstrcmpW(ext, L"tga")) kind = 1;
        else if (!lstrcmpW(ext, L"wav") || !lstrcmpW(ext, L"mp3")) kind = 2;
        else if (!lstrcmpW(ext, L"ini") || !lstrcmpW(ext, L"inc")) isIni = 1;
        if (kind < 0 && !isIni) continue;
        dir[len] = L'\\'; lstrcpyW(dir + len + 1, n);
        if (isIni) {
            WCHAR low[1024]; lstrcpynW(low, dir, 1024); CharLowerW(low);
            if (wcsstr(low, L"\\data\\ini\\")) plAddIni(dir);
        } else if (g_plNAsset < (LONG)PL_MAXASSET) {
            char b[260]; int bl = WideCharToMultiByte(CP_UTF8, 0, n, (int)(dot - n), b, 259, NULL, NULL);
            if (bl > 0) {
                CharLowerBuffA(b, bl);
                LONG id = plIntern(b, bl, true);
                char full[1100]; int fl = WideCharToMultiByte(CP_UTF8, 0, dir, -1, full, 1099, NULL, NULL);
                if (id >= 0 && fl > 0) {
                    LONG pid = plIntern(full, fl - 1, true);                // the path itself is kept in the arena as a word
                    if (pid >= 0) {                                         // the same name again: the later folder (the mod, walked after the game) wins, as in the game
                        LONG ai = g_plStr[id].asset >= 0 ? g_plStr[id].asset : g_plNAsset++;
                        PlAsset& a = g_plAsset[ai]; a.pathOff = g_plStr[pid].off; a.size = fd.nFileSizeLow; a.kind = (BYTE)kind; a.queued = 0; g_plStr[id].asset = ai;
                    }
                }
            }
        }
        dir[len] = 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- 1b: the INI definitions
static const char* const kPlTop[] = { "object", "childobject", "objectreskin", "fxlist", "fxparticlesystem", "particlesystem",
    "objectcreationlist", "weapon", "specialpower", "upgrade", "commandset", "commandbutton", "science", "audioevent", "dialogevent",
    "musictrack", "locomotor", "armor", "damagefx", "experiencelevel", "attributemodifier", "modifierlist", "crate", "cratedata",
    "emotionnugget", "mappedimage", "animation", "multisound", "streamedsound", "largegroupaudiomap", "createaherosystem",
    "spellbook", "linearcampaign", "fire", "firelogicsystem", "textureset", "horde", "formationassistant", "stancetemplate", "playertemplate" };
static LONG g_plTopId[sizeof(kPlTop) / sizeof(kPlTop[0])];
static int plTopIndex(LONG w) { for (int i = 0; i < (int)(sizeof(kPlTop) / sizeof(kPlTop[0])); ++i) if (g_plTopId[i] == w) return i; return -1; }
static __forceinline bool plWordCh(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '\'' || (BYTE)c >= 0x80; }
static LONG plFileDef(const WCHAR* path) {                 // the pseudo-definition "file:<path>" (lower case)
    char b[1100]; int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, b + 5, 1090, NULL, NULL);
    if (n <= 1) return -1;
    memcpy(b, "file:", 5); CharLowerBuffA(b + 5, n - 1);
    return plIntern(b, n - 1 + 5, true);
}
static void plReadIni(const WCHAR* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > (32u << 20)) { CloseHandle(f); return; }
    char* buf = (char*)VirtualAlloc(NULL, size + 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0; BOOL ok = buf && ReadFile(f, buf, size, &got, NULL);
    CloseHandle(f);
    if (!ok || got != size) { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return; }
    buf[size] = '\n'; buf[size + 1] = 0;
    g_plIniBytes += size;
    CharLowerBuffA(buf, size);
    LONG fileDef = plFileDef(path), cur = fileDef;
    WCHAR dir[1024]; lstrcpynW(dir, path, 1024); { WCHAR* s = wcsrchr(dir, L'\\'); if (s) s[1] = 0; }
    char* p = buf; char* end = buf + size;
    while (p < end) {
        char* eol = p; while (*eol != '\n') ++eol;
        *eol = 0;
        char* c1 = strchr(p, ';'); if (c1) *c1 = 0;
        char* c2 = strstr(p, "//"); if (c2) *c2 = 0;
        char* q = p; while (*q == ' ' || *q == '\t' || *q == '\r') ++q;
        if (*q == '#') {
            if (!strncmp(q, "#include", 8)) {                        // #include "relative\path"
                char* a = strchr(q, '"'); char* b2 = a ? strchr(a + 1, '"') : NULL;
                if (a && b2 && b2 - a > 1) {
                    *b2 = 0; WCHAR rel[600]; int rn = MultiByteToWideChar(CP_UTF8, 0, a + 1, -1, rel, 600);
                    if (rn > 1) {
                        for (int i = 0; rel[i]; ++i) if (rel[i] == L'/') rel[i] = L'\\';
                        WCHAR full[1100], fp[1100]; lstrcpynW(full, dir, 1024); if (lstrlenW(full) + lstrlenW(rel) < 1090) lstrcatW(full, rel);
                        if (GetFullPathNameW(full, 1100, fp, NULL)) plEdge(cur, plFileDef(fp));
                    }
                }
            } else if (!strncmp(q, "#define", 7)) {                  // #define NAME value...: NAME is a definition of its words
                char* r = q + 7; while (*r == ' ' || *r == '\t') ++r;
                char* s = r; while (plWordCh(*s)) ++s;
                LONG mac = plIntern(r, (int)(s - r), true);
                if (mac >= 0) { if (!g_plIsDef[mac]) g_plIsDef[mac] = 0xFE; plEdge(cur, mac);
                    for (char* t = s; *t; ) { while (*t && !plWordCh(*t)) ++t; char* u = t; while (plWordCh(*u)) ++u; if (u > t) plEdge(mac, plIntern(t, (int)(u - t), true)); t = u; } }
            }
            p = eol + 1; continue;
        }
        // the words of the line
        LONG w[64]; int nw = 0; bool eqAfterFirst = false;
        for (char* t = q; *t && nw < 64; ) {
            while (*t && !plWordCh(*t)) { if (*t == '=' && nw == 1) eqAfterFirst = true; ++t; }
            char* u = t; while (plWordCh(*u)) ++u;
            if (u > t) w[nw++] = plIntern(t, (int)(u - t), true);
            t = u;
        }
        int top = (nw >= 2 && !eqAfterFirst && w[0] >= 0 && w[1] >= 0) ? plTopIndex(w[0]) : -1;
        if (top >= 0) {
            cur = w[1]; if (!g_plIsDef[cur] || g_plIsDef[cur] == 0xFE) g_plIsDef[cur] = (BYTE)(top + 1); g_plNDef++;   // the kind of definition (1 + its keyword's index)
            plEdge(fileDef, cur);                                       // the file mentions it (an included file's definitions come along)
            for (int i = 2; i < nw; ++i) plEdge(cur, w[i]);             // ChildObject NAME PARENT ...
        } else for (int i = 0; i < nw; ++i) plEdge(cur, w[i]);
        p = eol + 1;
    }
    VirtualFree(buf, 0, MEM_RELEASE);
}
static int __cdecl plEdgeCmp(const void* a, const void* b) { const DWORD* x = (const DWORD*)a; const DWORD* y = (const DWORD*)b; return x[0] < y[0] ? -1 : (x[0] > y[0] ? 1 : (x[1] < y[1] ? -1 : (x[1] > y[1] ? 1 : 0))); }
static void plBuildCsr() {                                   // the (definition, word) pairs sorted, duplicates out: each definition's words in one run
    qsort(g_plEdge, (size_t)g_plNEdge, 8, plEdgeCmp);
    g_plCsrWords = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * ((size_t)g_plNEdge + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plCsr = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * ((size_t)g_plNStr + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_plCsrWords || !g_plCsr) { g_plCsr = NULL; return; }
    for (LONG i = 0; i <= g_plNStr; ++i) g_plCsr[i] = -1;
    LONG m = 0; DWORD lastD = 0xFFFFFFFF, lastW = 0xFFFFFFFF;
    for (LONG i = 0; i < g_plNEdge; ++i) {
        DWORD d = g_plEdge[2 * i], w = g_plEdge[2 * i + 1];
        if (d == lastD && w == lastW) continue;
        if (d != lastD) g_plCsr[d] = m;
        g_plCsrWords[m++] = (LONG)w; lastD = d; lastW = w;
    }
    VirtualFree(g_plEdge, 0, MEM_RELEASE); g_plEdge = NULL;
    g_plNEdge = m;
}
static LONG* g_plDefEnd = NULL;                    // per word id: end of its CSR run
static void plBuildEnds() {
    g_plDefEnd = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * ((size_t)g_plNStr + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_plDefEnd) return;
    // the runs are in id order of the definitions (the pairs were sorted by definition id)
    LONG lastId = -1;
    for (LONG id = 0; id < g_plNStr; ++id) {
        g_plDefEnd[id] = -1;
        if (g_plCsr[id] < 0) continue;
        if (lastId >= 0) g_plDefEnd[lastId] = g_plCsr[id];
        lastId = id;
    }
    if (lastId >= 0) g_plDefEnd[lastId] = g_plNEdge;
}
static void plLearnWarm();
static DWORD WINAPI plIndexThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    DWORD t0 = GetTickCount();
    for (int i = 0; i < (int)(sizeof(kPlTop) / sizeof(kPlTop[0])); ++i) g_plTopId[i] = plInternZ(kPlTop[i]);
    for (int r = 0; r < g_fwNRoot; ++r) {
        if (!g_fwRoot[r].recursive) continue;
        WCHAR d[1024]; int n = MultiByteToWideChar(CP_UTF8, 0, g_fwRoot[r].p, -1, d, 1000);
        if (n <= 1) continue;
        int len = lstrlenW(d); while (len > 3 && d[len - 1] == L'\\') d[--len] = 0;
        plWalk(d, len, 0);
    }
    for (LONG i = 0; i < g_plNIni; ++i) plReadIni(g_plIni[i]);
    plBuildCsr();
    plBuildEnds();
    g_plIndexMs = GetTickCount() - t0;
    if (!g_plCsr || !g_plDefEnd) { g_plIndexFailed = 1; logf("preload: no memory for the index - off."); return 0; }
    LONG nm = 0, nt = 0, ns = 0; for (LONG i = 0; i < g_plNAsset; ++i) { if (g_plAsset[i].kind == 0) nm++; else if (g_plAsset[i].kind == 1) nt++; else ns++; }
    logf("preload: index ready in %u ms (background) - %d files under the game's folders (%d models, %d textures, %d sounds), %d INI files (%d KB), %d definitions, %d words, %d references.",
         g_plIndexMs, (int)g_plNFiles, (int)nm, (int)nt, (int)ns, (int)g_plNIni, (int)(g_plIniBytes >> 10), (int)g_plNDef, (int)g_plNStr, (int)g_plNEdge);
    InterlockedExchange(&g_plIndexReady, 1);
    plLearnWarm();                                                // v66: what appeared in earlier battles, into the file cache while the menu is up
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return 0;
}

// ---------------------------------------------------------------- 2: the objects on the map (game thread, first logic step of a match)
static void plFeedReport(); static volatile LONG g_plPostCheck = 0, g_plFirstStep = 0; static void plFirstStepFeed();
static void plSeedOnGameThread() {
    gdOnLogicStep();
    if (!g_plOn || !g_plSeed) return;
    plFeedReport();                                               // (only when a feed is still open: the wait loop ended without our last check)
    if (g_plFirstStep) { g_plFirstStep = 0; plFirstStepFeed(); }   // v50: a saved game - its objects exist now
    if (g_plPostCheck) {                                          // the first logic step after a feed: is anything of it still loading? (it should not be)
        g_plPostCheck = 0;
        DWORD r = ((DWORD (__cdecl*)())(ULONG_PTR)0x00A32D40)();
        logf("preload: first logic step after the engine-side preload - the asset manager reports %u%% loaded%s.", r, r < 100 ? " (NOT complete: the rest loads during play)" : " (nothing left to load)");
    }
    DWORD gl = *(DWORD*)(ULONG_PTR)0x00DE412C;
    if (!gl) return;
    DWORD fr = *(DWORD*)(ULONG_PTR)(gl + 0x40), mode = *(DWORD*)(ULONG_PTR)(gl + 0x110);
    static DWORD s_mode = 0xFFFFFFFF;
    bool newMatch = fr < g_plSeedFrameLast || (mode != s_mode && mode != 9);   // the frame counter started over, or the menu's map gave way (a saved game keeps its frame count)
    g_plSeedFrameLast = fr; s_mode = mode;
    if (!newMatch || mode == 9 || g_plSeedReady) return;          // 9: the menu's shell map
    LONG len = 0, n = 0;
    __try {
        DWORD o = *(DWORD*)(ULONG_PTR)(gl + 0xAC);
        for (int guard = 0; o && guard < 200000; ++guard) {
            DWORD t = *(DWORD*)(ULONG_PTR)(o + 4);
            DWORD s = t ? *(DWORD*)(ULONG_PTR)(t + 0x64) : 0;
            if (s) {
                const char* nm = (const char*)(ULONG_PTR)(s + 8);
                int k = 0; while (k < 120 && nm[k]) ++k;
                if (k > 0 && k < 120 && len + k + 2 < (2 << 20)) { memcpy(g_plSeed + len, nm, (size_t)k); len += k; g_plSeed[len++] = '\n'; n++; }
            }
            o = *(DWORD*)(ULONG_PTR)(o + 0x8C);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_plSeed[len] = 0; g_plSeedLen = len; g_plSeedN = n;
    if (n) { InterlockedExchange(&g_plSeedReady, 1); SetEvent(g_plEvt); }
}

// ---------------------------------------------------------------- 3 + 4: the closure and the read-ahead
// has the game itself opened this file already this session? (aotr_filewarm.inc's table: then it is loaded and cached)
static bool plGameOpened(const char* path) {
    char low[1100]; int n = lstrlenA(path); if (n <= 0 || n >= 1099) return false;
    memcpy(low, path, (size_t)n + 1); CharLowerBuffA(low, n);
    DWORD h = fwHash(low, n); bool r = false;
    EnterCriticalSection(&g_fwCs);
    for (DWORD i = h & (FW_TABLE - 1), k = 0; k < FW_TABLE; i = (i + 1) & (FW_TABLE - 1), ++k) {
        DWORD v = g_fwTab[i]; if (!v) break;
        const FwEnt& x = g_fwEnt[v - 1];
        if (x.hash == h && !strcmp(g_fwArena + x.off, low)) { r = (x.flags & 1) != 0; break; }
    }
    LeaveCriticalSection(&g_fwCs);
    return r;
}
static volatile LONG g_plSkipOpened = 0;
// v49: the files in the order the walk reached them (nearest to the map's objects first), not models, then textures,
// then sounds; sounds over 2 MB are left to the game (it streams them); and the reading stands back while the game
// itself opens files (test-31: 22 hitches waiting for the disk in the first minute of play while 1.2 GB were read
// alongside the game's own loading - 117 s for what took 67 s in test-30). Budget 600 MB.
static LONG* g_plAOrd = NULL; static LONG g_plNAOrd = 0; static volatile LONG g_plYields = 0, g_plW3dTex = 0;
// v50: a model names its textures inside the .w3d (test-31: mubalrog.dds, mubalrogwings.dds were not on the list - the
// INI names the model, the model names the texture). Each texture name in a model that is read ahead goes on the list
// too, after what is already there.
static void plW3dTextures(const BYTE* b, DWORD n) {
    for (DWORD i = 4; i + 4 <= n; ++i) {
        if (b[i] != '.') continue;
        char e0 = (char)(b[i + 1] | 32), e1 = (char)(b[i + 2] | 32), e2 = (char)(b[i + 3] | 32);
        if (!((e0 == 'd' && e1 == 'd' && e2 == 's') || (e0 == 't' && e1 == 'g' && e2 == 'a'))) continue;
        DWORD st = i; while (st > 0 && i - st < 100) { BYTE c = b[st - 1]; if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') --st; else break; }
        int k = (int)(i - st); if (k <= 0 || k >= 100) continue;
        char nm[104]; for (int j = 0; j < k; ++j) { char c = (char)b[st + j]; nm[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } nm[k] = 0;
        LONG id = plIntern(nm, k, false);
        if (id < 0) continue;
        LONG a = g_plStr[id].asset;
        if (a < 0 || a >= g_plNAsset || g_plInSet[a]) continue;
        g_plInSet[a] = 1; if (g_plNAOrd < (LONG)PL_MAXASSET) g_plAOrd[g_plNAOrd++] = a; g_plW3dTex++;
    }
}
static volatile LONG g_plEarly = 0;
// v65: the faction's files kept in Windows' file cache - outside the game's 4 GB, so what the engine-side preload has no
// room for still loads from memory, not from the disk, when it first appears: up to 2 GB, never leaving under 2 GB free.
static LONG64 plFileCacheBudget() {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 600ll << 20;
    LONG64 freeB = (LONG64)ms.ullAvailPhys - (2048ll << 20);
    if (freeB < (600ll << 20)) freeB = 600ll << 20;
    if (freeB > (2048ll << 20)) freeB = 2048ll << 20;
    return freeB;
}
static volatile LONG g_plPlanState = 0;       // 0 none, 1 wanted (the scripts' AddKeys ran), 2 planned, 3 fed (reported)
static void plReadAhead(LONG64 budget, DWORD tStop, bool early = false) {
    static BYTE buf[1 << 20];
    for (LONG k = 0; k < g_plNAOrd; ++k) {
        LONG i = g_plAOrd[k];
        PlAsset& a = g_plAsset[i];
        if (!g_plInSet[i] || a.queued) continue;
        if (g_plReadBytes >= budget || GetTickCount() > tStop) return;
        if (early && g_plPlanState != 2) return;                    // the loading screen's wait is over: the rest at background priority from the first logic step
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys < (1200ull << 20)) { g_plStopMem = 1; return; }
        a.queued = 1;
        if (a.size > (a.kind == 2 ? (2u << 20) : (96u << 20))) continue;
        if (plGameOpened(g_plArena + a.pathOff)) { g_plSkipOpened++; continue; }
        while (!early && GetTickCount() - g_fwLastGameOpen < 400 && GetTickCount() < tStop) { g_plYields++; Sleep(50); }   // the game is reading: it goes first (not on the loading screen: there it is waiting for exactly these)
        WCHAR w[1100]; if (MultiByteToWideChar(CP_UTF8, 0, g_plArena + a.pathOff, -1, w, 1100) <= 1) continue;
        HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got) { g_plReadBytes += got; if (a.kind == 0) plW3dTextures(buf, got); }
        CloseHandle(f);
        g_plReadFiles++;
    }
}
// v66: the models and textures of the learned list (out\aotr_learned.txt: first opened in earlier battles) are read
// into Windows' file cache as soon as the INI index is ready - while the menu is up, before any match is chosen - at
// background priority, standing back while the game reads, up to 512 MB. Files only: nothing is loaded into the game.
static volatile LONG g_plLwFiles = 0, g_plLwMB = 0;
static void plLearnWarm() {
    if (gdNewOff()) return;
    char path[MAX_PATH]; aotrPath(path, "aotr_learned.txt");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char* list = (char*)VirtualAlloc(NULL, 512 << 10, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!list || !buf) { CloseHandle(f); return; }
    DWORD got = 0; ReadFile(f, list, (512 << 10) - 1, &got, NULL); CloseHandle(f); list[got] = 0;
    DWORD t0 = GetTickCount(); LONG64 bytes = 0; LONG files = 0;
    // v79: no more than a third of the memory free now (512 MB at most), and it stops when under 1.2 GB is free - the
    // file cache must never push the game out of memory on a small PC.
    LONG64 cap = 512ll << 20;
    { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); if (GlobalMemoryStatusEx(&ms)) { LONG64 b = (LONG64)ms.ullAvailPhys / 3; if (b < cap) cap = b; } }
    if (cap < (64ll << 20)) { logf("preload: menu - the learned list is not read ahead (too little free memory)."); VirtualFree(list, 0, MEM_RELEASE); VirtualFree(buf, 0, MEM_RELEASE); return; }
    // v76: newest first. The file grows at its end, and the 512 MB below used to be spent on its oldest names - the
    // v74 test read 2719 files (exactly 512 MB) and never reached what the last battles had added.
    for (char* end = list + got; end > list && bytes < cap; ) {
        { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); if ((files & 31) == 0 && GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys < (1200ull << 20)) break; }
        while (end > list && (end[-1] == '\n' || end[-1] == '\r')) --end;
        char* p = end; while (p > list && p[-1] != '\n' && p[-1] != '\r') --p;
        int n = (int)(end - p);
        LONG id = (n > 2 && n <= 60) ? plIntern(p, n, false) : -1;
        end = p;
        if (id < 0) continue;
        LONG a = g_plStr[id].asset; if (a < 0 || a >= g_plNAsset) continue;
        PlAsset& as = g_plAsset[a];
        if (as.queued || as.kind == 2 || as.size > (96u << 20)) continue;
        as.queued = 1;
        if (plGameOpened(g_plArena + as.pathOff)) continue;
        while (GetTickCount() - g_fwLastGameOpen < 400) Sleep(50);       // the game is reading: it goes first
        WCHAR w[1100]; if (MultiByteToWideChar(CP_UTF8, 0, g_plArena + as.pathOff, -1, w, 1100) <= 1) continue;
        HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD r = 0; while (ReadFile(h, buf, 1 << 20, &r, NULL) && r) bytes += r;
        CloseHandle(h); files++;
    }
    g_plLwFiles = files; g_plLwMB = (LONG)(bytes >> 20);
    logf("preload: menu - %d files of the learned list (%d MB: what appeared in earlier battles) read into the file cache in %u ms, before any match.", (int)files, (int)(bytes >> 20), GetTickCount() - t0);
    VirtualFree(list, 0, MEM_RELEASE); VirtualFree(buf, 0, MEM_RELEASE);
}
static LONG plPlayerSeeds(LONG* q, BYTE* seen, LONG qt);
// v57: the walk in three tiers. test-41: Isengard's 25-point spell (its FreezingRainCaster's FireWeaponUpdate) cost a
// logic step 161-164 ms both times it was cast - its files read from the disk and its textures made during play: the
// v55 walk was breadth-first from the map's objects AND the factions together, so a spell (faction -> spell book ->
// object creation list -> caster object -> weapon -> effects: four or five steps deep) came after every building and
// unit near the map's objects, and the budget ended long before it (97 of 5002 templates). Now, for every faction in
// the match and nothing named:
//   tier 1: from the players' factions (PlayerTemplate: spell book, starting buildings and units, hero system) - what
//           those create THEMSELVES: spells and summons (spell book -> its object creation lists -> the summoned and
//           caster objects, their weapons, effects, particles, sounds), crossing into other objects but not through a
//           command set, command button, science or another faction (those lead to the whole tech tree);
//   tier 2: the same from the map's objects (their abilities, summons, projectiles, debris);
//   tier 3: everything else, breadth-first as before (what can be built, upgraded, bought).
// v58 (test-42: that tier 1 held 4720 of the 5002 templates - a faction's heroes, its buildings' #include files and
// macros lead to nearly everything, so the spells were again in the middle of the whole tree). Tier 1 is now only
// what APPEARS SUDDENLY, decided by the kind of each step and not by any name:
//   * an object named by a faction (spell book, starting units and buildings, its heroes) or by the map is a ROOT: its
//     weapons, effects, object creation lists, special powers, upgrades are followed, but its own models, textures and
//     sounds and the objects it names directly are held back (they are built or already on the map - tier 3);
//   * an object reached through an object creation list or a weapon is CREATED (a summon, a spell's caster, a
//     projectile, debris): all of it is followed, and the objects it names directly too (a summoned horde's members);
//   * objects named by an #include file or a #define macro are held back; command sets, buttons, sciences and other
//     factions as before.
// A word held back and later reached the right way is taken then. q gets every reached word in that order
// (definitions; with allWords every word - the asset-name look-up needs them); tierEnd[k] = the queue length at the
// end of tier k+1; seen: 1 queued, 2 held back (the seeds are 1); defer and fl (per word: 1 root, 2 created) are each
// caller's own - the game thread's plan and the read-ahead thread can walk at the same time.
static LONG plKindIdx(const char* name) { for (int i = 0; i < (int)(sizeof(kPlTop) / sizeof(kPlTop[0])); ++i) if (!lstrcmpA(kPlTop[i], name)) return i + 1; return -1; }
static volatile LONG g_plT1Roots = 0, g_plT1Created = 0, g_plT1Held = 0;
static LONG plTierWalk(LONG* q, BYTE* seen, LONG* defer, BYTE* fl, const LONG* sPt, LONG nPt, const LONG* sMap, LONG nMap, bool allWords, LONG tierEnd[3]) {
    const LONG kCs = plKindIdx("commandset"), kCb = plKindIdx("commandbutton"), kSci = plKindIdx("science"), kPt = plKindIdx("playertemplate");
    const LONG kObj = plKindIdx("object"), kChild = plKindIdx("childobject"), kReskin = plKindIdx("objectreskin");
    const LONG kOcl = plKindIdx("objectcreationlist"), kWpn = plKindIdx("weapon");
    LONG qh = 0, qt = 0, nd = 0, roots = 0, created = 0, held = 0;
    for (int tier = 0; tier < 3; ++tier) {
        if (tier == 0) { for (LONG i = 0; i < nPt; ++i) { q[qt++] = sPt[i]; fl[sPt[i]] = 2; } }
        else if (tier == 1) { for (LONG i = 0; i < nMap; ++i) { q[qt++] = sMap[i]; if (!fl[sMap[i]]) { fl[sMap[i]] = 1; roots++; } } }
        else { for (LONG i = 0; i < nd; ++i) { LONG w = defer[i]; if (seen[w] != 2) continue; seen[w] = 1; q[qt++] = w; } nd = 0; }
        while (qh < qt) {
            LONG d = q[qh++];
            LONG s0 = g_plCsr[d]; if (s0 < 0) continue;
            LONG kd = g_plIsDef[d];
            bool dObj = kd == kObj || kd == kChild || kd == kReskin;
            bool dCreator = kd == kOcl || kd == kWpn;
            BYTE fd = fl[d] ? fl[d] : 2;
            for (LONG i = s0, e = g_plDefEnd[d]; i < e && qt < (LONG)PL_MAXSTR - 1 && nd < (LONG)PL_MAXSTR - 1; ++i) {
                LONG w = g_plCsrWords[i]; if (w < 0 || w >= g_plNStr) continue;
                bool def = g_plCsr[w] >= 0;
                if (!def && !allWords && g_plStr[w].asset < 0) { seen[w] = seen[w] ? seen[w] : 1; continue; }
                if (tier == 2) { if (seen[w]) continue; seen[w] = 1; q[qt++] = w; continue; }
                LONG k = g_plIsDef[w];
                bool wObj = def && (k == kObj || k == kChild || k == kReskin);
                BYTE fw = 0; bool hold = false;
                if (def && (k == kCs || k == kCb || k == kSci || k == kPt)) hold = true;
                else if (wObj) {
                    if (dCreator) fw = 2;                                  // made by an object creation list or a weapon
                    else if (dObj && fd == 2) fw = 2;                      // named by a created object (a summoned horde's members)
                    else if (kd == kPt) fw = 1;                            // named by a faction: a root
                    else hold = true;                                      // named by a root object, a macro, an #include file ...
                } else if (dObj && fd == 1 && !def) hold = true;          // a root object's own art and sounds: tier 3
                else fw = fd;
                if (hold) { if (!seen[w]) { seen[w] = 2; defer[nd++] = w; held++; } continue; }
                if (seen[w] == 1) {                                        // already taken: a root that turns out to be created goes again
                    if (wObj && fw == 2 && fl[w] == 1) { fl[w] = 2; q[qt++] = w; created++; }
                    continue;
                }
                seen[w] = 1; fl[w] = fw; q[qt++] = w;
                if (wObj) { if (fw == 1) roots++; else created++; }
            }
        }
        tierEnd[tier] = qt;
    }
    g_plT1Roots = roots; g_plT1Created = created; g_plT1Held = held;
    return qt;
}
static DWORD WINAPI plMatchThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_plEvt, INFINITE);
        while (!g_plIndexReady && !g_plIndexFailed) Sleep(200);
        bool early = InterlockedExchange(&g_plEarly, 0) != 0, normal = g_plSeedReady != 0;
        if (g_plIndexFailed || (!early && !normal)) continue;
        early = early && !normal;                                     // (both: the first logic step's run, which has everything)
        if (!early) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        DWORD t0 = GetTickCount();
        if (!early) g_plMatches++;
        memset(g_plInSet, 0, (size_t)g_plNAsset);
        static BYTE* seen = NULL; static LONG* q = NULL;
        if (!seen) seen = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!q) q = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!seen || !q) { if (!early) g_plSeedReady = 0; continue; }
        memset(seen, 0, (size_t)PL_MAXSTR);
        LONG seeds = 0, found = 0;
        static LONG sMap[65536], sPt[64]; LONG nMap = 0, nPt = 0;
        for (char* p = g_plSeed; !early && *p; ) {
            char* e = strchr(p, '\n'); if (!e) break;
            char tmp[128]; int k = (int)(e - p); if (k > 127) k = 127; memcpy(tmp, p, (size_t)k); CharLowerBuffA(tmp, k);
            LONG id = plIntern(tmp, k, false);
            if (id >= 0 && !seen[id] && nMap < 65536) { seen[id] = 1; sMap[nMap++] = id; seeds++; if (g_plCsr[id] >= 0) found++; }
            p = e + 1;
        }
        { LONG n = plPlayerSeeds(q, seen, 0); for (LONG i = 0; i < n && nPt < 64; ++i) sPt[nPt++] = q[i]; seeds += n; found += n; }   // v55: the players' factions too
        LONG defs = 0; LONG64 bytes = 0; LONG nm = 0, nt = 0, ns = 0;
        if (!g_plAOrd) g_plAOrd = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXASSET, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_plNAOrd = 0;
        if (!g_plAOrd) { if (!early) g_plSeedReady = 0; continue; }
        LONG tierEnd[3] = { 0, 0, 0 };
        static LONG* defer = NULL; if (!defer) defer = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        static BYTE* fl = NULL; if (!fl) fl = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!defer || !fl) { if (!early) g_plSeedReady = 0; continue; }
        memset(fl, 0, (size_t)PL_MAXSTR);
        LONG qt = plTierWalk(q, seen, defer, fl, sPt, nPt, sMap, nMap, false, tierEnd);     // v57: spells and summons of the factions first
        LONG tierAsset[3] = { 0, 0, 0 };
        for (LONG qh = 0; qh < qt; ++qh) {
            LONG d = q[qh];
            if (g_plStr[d].asset >= 0) { LONG a = g_plStr[d].asset; if (!g_plInSet[a]) { g_plInSet[a] = 1; if (g_plNAOrd < (LONG)PL_MAXASSET) g_plAOrd[g_plNAOrd++] = a; bytes += g_plAsset[a].size; if (g_plAsset[a].kind == 0) nm++; else if (g_plAsset[a].kind == 1) nt++; else ns++;
                                                                       tierAsset[qh < tierEnd[0] ? 0 : (qh < tierEnd[1] ? 1 : 2)]++; } }
            if (g_plCsr[d] >= 0) defs++;
        }
        if (early) {                                                  // v59: the loading screen - the factions' files read now, ahead of the engine's own reads
            DWORD r0 = GetTickCount(); LONG64 b0 = g_plReadBytes; LONG f0 = g_plReadFiles;
            plReadAhead(g_plReadBytes + plFileCacheBudget(), GetTickCount() + 60000, true);
            logf("preload: loading screen - %d of the factions' files (%d MB) read into the file cache in %u ms alongside the engine's loading (its own reads then come from memory); tier 1 had %d files.",
                 (int)(g_plReadFiles - f0), (int)((g_plReadBytes - b0) >> 20), GetTickCount() - r0, (int)tierAsset[0]);
            continue;
        }
        logf("preload: match %d - the file list in three tiers: %d files from the factions' own spells and summons first, %d from the map's objects' own abilities, then %d of everything else.", (int)g_plMatches + 0, (int)tierAsset[0], (int)tierAsset[1], (int)tierAsset[2]);
        g_plSetDefs = defs; g_plSetModels = nm; g_plSetTex = nt; g_plSetSnd = ns; g_plSetBytes = bytes;
        logf("preload: match %d - %d objects on the map (%d different templates, %d of them found in the mod's INI) lead to %d definitions and %d files: %d models, %d textures, %d sounds, %d MB (found in %u ms). Reading them into the file cache now (background priority).",
             (int)g_plMatches, (int)g_plSeedN, (int)seeds, (int)found, (int)defs, (int)(nm + nt + ns), (int)nm, (int)nt, (int)ns, (int)(bytes >> 20), GetTickCount() - t0);
        DWORD r0 = GetTickCount(); LONG64 b0 = g_plReadBytes; LONG f0 = g_plReadFiles;
        LONG64 budget = g_plReadBytes + plFileCacheBudget();
        DWORD tStop = GetTickCount() + 600000; LONG y0 = g_plYields;   // v65: up to 10 min (the budget is up to 2 GB now)
        plReadAhead(budget, tStop);
        logf("preload: match %d - %d files (%d MB) read into the file cache in %u ms, the factions' spells and summons first, standing back %d times while the game read; %d textures named inside the models added (%d already opened by the game this session were left alone)%s.", (int)g_plMatches, (int)(g_plReadFiles - f0), (int)((g_plReadBytes - b0) >> 20), GetTickCount() - r0, (int)(g_plYields - y0), (int)g_plW3dTex, (int)g_plSkipOpened,
             g_plStopMem ? " - stopped: the machine has little free memory" : "");
        g_plSeedReady = 0;
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
}
// v59: the LEARNED list. Every model and texture the game opened for the first time during play (a first appearance
// the preload had not prepared) is written to out\aotr_learned.txt; the next match's plan puts those of them that its
// own walk reaches in front of everything (they are what really appeared in a battle). g_fwCs held (filewarm's hook).
static char g_plLearn[32768]; static LONG g_plLearnLen = 0; static DWORD g_plLearnSet[4096];
static void plLearnNote(const char* b, int n) {
    if (n <= 2 || n > 60) return;
    DWORD h = plHash(b, n) | 1; DWORD i = h & 4095;
    for (int k = 0; k < 4096; ++k, i = (i + 1) & 4095) { if (g_plLearnSet[i] == h) return; if (!g_plLearnSet[i]) { g_plLearnSet[i] = h; break; } }
    if (g_plLearnLen + n + 2 >= (LONG)sizeof(g_plLearn)) return;
    memcpy(g_plLearn + g_plLearnLen, b, (size_t)n); g_plLearnLen += n; g_plLearn[g_plLearnLen++] = '\n';
}
static volatile LONG g_plLearnSaved = 0, g_plLearnUsed = 0, g_plLearnRead = 0;
// v76: the list stopped learning for good once it reached 512 KB. Near that size its oldest part is dropped: the
// newest 256 KB (whole lines) are kept - what the latest battles needed.
static void plLearnTrim(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char* b = (char*)VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD got = 0; if (b) ReadFile(f, b, (1 << 20) - 1, &got, NULL); CloseHandle(f);
    if (!b) return;
    if (got > (256u << 10)) {
        DWORD from = got - (256u << 10); while (from < got && b[from] != '\n') ++from; while (from < got && (b[from] == '\n' || b[from] == '\r')) ++from;
        HANDLE w = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (w != INVALID_HANDLE_VALUE) { DWORD wr = 0; WriteFile(w, b + from, got - from, &wr, NULL); CloseHandle(w); }
        logf("preload: the learned list reached %u KB - its oldest names were dropped, the newest %u KB kept.", got >> 10, (got - from) >> 10);
    }
    VirtualFree(b, 0, MEM_RELEASE);
}
static void plLearnSave() {                                       // 30 s report thread
    char buf[32768]; LONG n = 0;
    EnterCriticalSection(&g_fwCs); n = g_plLearnLen; if (n) memcpy(buf, g_plLearn, (size_t)n); g_plLearnLen = 0; LeaveCriticalSection(&g_fwCs);
    if (!n) return;
    char path[MAX_PATH]; aotrPath(path, "aotr_learned.txt");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; bool full = false;
    if (GetFileSizeEx(f, &sz)) { full = sz.QuadPart + n >= (448 << 10); if (sz.QuadPart < (512 << 10)) { DWORD w = 0; WriteFile(f, buf, (DWORD)n, &w, NULL); for (LONG i = 0; i < n; ++i) if (buf[i] == '\n') g_plLearnSaved++; } }
    CloseHandle(f);
    if (full) plLearnTrim(path);
}
// from aotr_filewarm.inc's open hook: a file the game opened for the first time during play - was it found in advance?
static void plNotePlayOpen(const char* fullLower, int n) {
    if (!g_plOn || !g_plIndexReady || !g_plMatches || !g_plInSet) return;
    const char* b = fullLower; for (int i = 0; i < n; ++i) if (fullLower[i] == '\\' || fullLower[i] == '/') b = fullLower + i + 1;
    const char* dot = NULL; for (const char* q = b; *q; ++q) if (*q == '.') dot = q;
    if (!dot) return;
    char ext[6]; int el = 0; for (const char* q = dot + 1; *q && el < 5; ++q) ext[el++] = *q; ext[el] = 0;
    if (strcmp(ext, "w3d") && strcmp(ext, "dds") && strcmp(ext, "tga") && strcmp(ext, "wav") && strcmp(ext, "mp3")) return;
    g_plPlayFirst++;
    if (ext[0] != 'w' || ext[1] != 'a') plLearnNote(b, (int)(dot - b));   // v59: models and textures - the next match prepares them first
    LONG id = plIntern(b, (int)(dot - b), false);
    if (id >= 0 && g_plStr[id].asset >= 0 && g_plInSet[g_plStr[id].asset]) { g_plPlayHit++; return; }
    g_plPlayMissN++;
    int k = (int)(dot - b) + 4; if (k > 40) k = 40;
    if (g_plMissLen + k + 2 < (LONG)sizeof(g_plMiss) - 1) { if (g_plMissLen) g_plMiss[g_plMissLen++] = ' '; memcpy(g_plMiss + g_plMissLen, b, (size_t)k); g_plMissLen += k; g_plMiss[g_plMissLen] = 0; }
}
static void plReport() {                                   // 30 s product report
    static LONG lf = 0, lh = 0;
    if (!g_plOn || !g_plMatches) return;
    LONG f = g_plPlayFirst, h = g_plPlayHit;
    if (f == lf) return;
    char miss[1000]; EnterCriticalSection(&g_fwCs); lstrcpynA(miss, g_plMiss, 900); g_plMissLen = 0; g_plMiss[0] = 0; LeaveCriticalSection(&g_fwCs);
    logf("preload: last 30 s - %d model/texture/sound files first opened during play, %d of them were in this match's list | not in it: %s", (int)(f - lf), (int)(h - lh), miss[0] ? miss : "-");
    lf = f; lh = h;
}
// ---------------------------------------------------------------- phase 2 (v48): the engine loads them itself, on the loading screen
// BFME2's own way to have a thing's art in memory before it is needed: ThingTemplate::GetAssetList (0x73E909) gathers
// every asset a template can show (all its model states, textures, and - through its modules and weapons - what it
// creates), and AssetManager::AddKeys (0xA32CC0 -> 0xA371B0) puts them into the asset manager's wanted set (under its
// lock; the streamer loads them afterwards). The engine does this on the loading screen for the scripts' objects
// (startNewGame's tail, 0x631472..0x6314AB, skipped when GlobalData+0x1110 is set), and startNewGame then waits in
// 0x6282F0 until the manager reports 100% (0x628635: call 0xA32D40, cmp eax,100, jb - the loop pumps the loading
// screen and sleeps 1 ms).
// v48.1 - what was learned since v48: AddKeys returns at once, so v48's memory check between batches could see
// nothing, and test-30's list (4768 models, 1705 textures, 16285 sounds, 4460 MB of files) would all have been
// handed over, to be loaded behind the loading screen without any bound. Now:
//   1. (v49) test-31: GlobalData+0x1110 is set in every BFME2 (its constructor, 0x643905) - the engine's own preloading
//      is off, so the scripts' AddKeys never runs and v48.1 never planned. With the flag clear, GetAssetList would also
//      walk the template's command set (+0x70 -> 0xDE7744, 0x80C86A: up to 33 buttons, each built thing's own list) -
//      everything buildable, the reason it is off - so the flag is left alone. startNewGame's call of its tail
//      (0x6315A7) now marks each load, and at the end of the engine's own load the walk is made and the plan kept: the object templates the walk reached,
//      nearest to the map's objects first, each with the estimated memory of all files the walk had reached by then
//      (models x2, textures x1.4 for the mip levels made on load, sounds up to 2 MB - longer ones are streamed);
//      only those within a budget from the free address space are planned (60% of what is left below 2.3 GB, at
//      most 800 MB, half of it on a machine with under 1.5 GB of free memory);
//   2. nothing more happens until the engine's own loading is complete: when its wait loop sees 100%, 20 more
//      templates are handed over and the loop goes on waiting - the loading screen stays up, the game thread is
//      doing nothing else then - until the plan is done, 8 s have passed since the engine's own 100%, or the
//      address space reaches 2.3 GB. So the engine's load is never slower, and the extra wait has a hard cap.
//   3. nothing is ever handed over during play (v48 did that for saved games: the streamer would then have finished
//      the art on the game thread in the battle). A save that reaches the wait loop with its objects is planned there.
#define PL_AL_CTOR   0x0046C1DA      // AssetList::AssetList (thiscall)
#define PL_AL_DTOR   0x0046B735      // AssetList::~AssetList (thiscall)
#define PL_GETASSETS 0x0073E909      // ThingTemplate::GetAssetList(AssetList*, AssetLoadMode*) (thiscall, ret 8)
#define PL_ADDKEYS   0x00A32CC0      // AssetManager AddKeys(AssetList*) (cdecl)
#define PL_PROGRESS  0x00A32D40      // AssetManager progress in percent (cdecl, 100 without a manager)
#define PL_TAILSITE  0x006315A7      // startNewGame: call 0x6311ED (its tail - the scripts' AddKeys inside it is skipped while GlobalData+0x1110 is set)
#define PL_WAITSITE  0x00628635      // 0x6282F0's wait loop: call progress / cmp eax,64h / jb
#define PL_TF        0x00DE4A40      // TheThingFactory (templates in a hash map at +0x14)
#define PL_FEED_MS   15000                                       // v55: 8 -> 15 s (the factions' whole trees; the load screen only)
static volatile LONG g_pl2On = 0, g_pl2Runs = 0;
static LONG* g_plOrder = NULL;       // per word id: its place in the last breadth-first walk (+1), 0 = not reached
static LONG* g_plCumKB = NULL;       // per definition: estimated memory (KB) of all files the walk had reached once it was expanded
struct PlTmpl { DWORD t; LONG order; LONG cumKB; };
// v55: assets by name. The walk's words that the engine's asset registry knows (0xA332B0 FindPrototype: the name's key
// when it is a model, texture ... of asset.dat, in the .big archives or loose; 0 otherwise) are handed to the asset
// manager after the templates - effects' particle textures and models, debris, projectiles, spell objects: what a
// template's own asset list does not name. In walk order, inside the same memory budget.
struct PlAName { char nm[60]; LONG cumKB; LONG order; };   // order (v57): the walk position of the word
static LONG g_plANOrderNow = 0;
static PlAName* g_plAN = NULL; static LONG g_plANN = 0, g_plANAt = 0, g_plANIn = 0, g_plANFed = 0, g_plANTried = 0;
#define PL_AN_MAX 65536
// NOT the engine's FindPrototype (0xA332B0): on its way it turns the name into a key with the shared NameKeyGenerator
// (0xA34110), which MAKES a new key for a name it has not seen - that would renumber every key made after it, and keys
// are used by the simulation. This is the same look-up, read only: the generator's table (manager+0x1F8: 45007
// buckets at +4, node { next, key, AsciiString name }, hash h*33+tolower(c), names compared case-blind), then the
// prototype table (manager+0x50..+0x54 buckets, node { next, key }), under the manager's own lock (+0x34) as 0xA34FB0
// takes it. Nothing is made; a name that is not there gives 0.
static int plStrCmpI(const char* a, const char* b) {             // msvcrt _strcmpi in the C locale: only A-Z fold
    for (;; ++a, ++b) { BYTE x = (BYTE)*a, y = (BYTE)*b; if (x >= 'A' && x <= 'Z') x = (BYTE)(x + 32); if (y >= 'A' && y <= 'Z') y = (BYTE)(y + 32); if (x != y) return (int)x - (int)y; if (!x) return 0; }
}
static DWORD plFindProto(const char* nm) {
    DWORD mgr = *(volatile DWORD*)0x00DEF75C, found = 0;
    if (!mgr) return 0;
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)(ULONG_PTR)(mgr + 0x34);
    EnterCriticalSection(cs);
    __try {
        DWORD gen = *(DWORD*)(ULONG_PTR)(mgr + 0x1F8), key = 0;
        if (gen) {
            DWORD h = 0; for (const char* c = nm; *c; ++c) { BYTE ch = (BYTE)*c; if (ch >= 'A' && ch <= 'Z') ch = (BYTE)(ch + 32); h = h * 33 + ch; }
            for (DWORD n = *(DWORD*)(ULONG_PTR)(gen + 4 + (h % 45007u) * 4); n; n = *(DWORD*)(ULONG_PTR)n) {
                DWORD d = *(DWORD*)(ULONG_PTR)(n + 8);
                if (d && !plStrCmpI((const char*)(ULONG_PTR)(d + 8), nm)) { key = *(DWORD*)(ULONG_PTR)(n + 4); break; }
            }
        }
        if (key) {
            DWORD vb = *(DWORD*)(ULONG_PTR)(mgr + 0x50), ve = *(DWORD*)(ULONG_PTR)(mgr + 0x54), cnt = (ve - vb) / 4;
            if (cnt) for (DWORD p = *(DWORD*)(ULONG_PTR)(vb + (key % cnt) * 4); p; p = *(DWORD*)(ULONG_PTR)p) if (*(DWORD*)(ULONG_PTR)(p + 4) == key) { found = key; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    LeaveCriticalSection(cs);
    return found;
}
static bool plTryName(const char* w, LONG64* cum, LONG assetIdx) { // the word itself, or with the texture endings
    if (gdNewOff()) return false;                                 // GUARD safe mode: v55's by-name preload off
    static const char* const ext[] = { "", ".tga", ".dds", ".w3d" };
    char b[64]; int n = lstrlenA(w); if (n <= 2 || n > 50) return false;
    for (int k = 0; k < 4; ++k) {
        memcpy(b, w, (size_t)n); lstrcpyA(b + n, ext[k]);
        g_plANTried++;
        if (!plFindProto(b)) continue;
        LONG64 sz = 256 << 10;                                      // an archived asset: an estimate
        if (assetIdx >= 0 && assetIdx < g_plNAsset) { sz = g_plAsset[assetIdx].size; if (g_plAsset[assetIdx].kind == 0) sz *= 2; else if (g_plAsset[assetIdx].kind == 1) sz = sz * 14 / 10; else return false; }
        *cum += sz;
        if (g_plANN < PL_AN_MAX) { lstrcpynA(g_plAN[g_plANN].nm, b, 60); g_plAN[g_plANN].cumKB = (LONG)(*cum >> 10); g_plAN[g_plANN].order = g_plANOrderNow; g_plANN++; }
        return true;
    }
    return false;
}
// the plan (game thread): the learned names its walk reached go first (order 1); returns how many
static LONG plLearnFront(LONG64* cum, const BYTE* seen) {
    char path[MAX_PATH]; aotrPath(path, "aotr_learned.txt");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static char buf[512 << 10]; DWORD got = 0; ReadFile(f, buf, sizeof(buf) - 1, &got, NULL); CloseHandle(f); buf[got] = 0;
    LONG used = 0, read = 0;
    for (char* p = buf; *p; ) {
        char* e = p; while (*e && *e != '\n' && *e != '\r') ++e;
        int n = (int)(e - p);
        if (n > 2 && n <= 60) {
            read++;
            LONG id = plIntern(p, n, false);
            if (id >= 0 && seen[id] && !g_plOrder[id]) {                 // reached by this match's walk (and not taken yet)
                char nm[64]; memcpy(nm, p, (size_t)n); nm[n] = 0;
                g_plANOrderNow = 1;
                LONG a = g_plStr[id].asset;
                if (plTryName(nm, cum, a >= 0 && g_plAsset[a].kind != 2 ? a : -1)) { used++; g_plOrder[id] = 1; }
            }
        }
        p = e; while (*p == '\n' || *p == '\r') ++p;
    }
    g_plLearnUsed = used; g_plLearnRead = read;
    return used;
}
static PlTmpl* g_plTl = NULL;
static LONG g_plPlanN = 0, g_plPlanAt = 0, g_plPlanAll = 0, g_plPlanKB = 0, g_plPlanBudgetKB = 0, g_plPlanSeeds = 0, g_plPlanTmpl = 0;
static DWORD g_plFeedT0 = 0, g_plFeedCallMs = 0, g_plPlanT = 0; static LONG g_plFeedBatches = 0; static ULONG64 g_plFeedVa0 = 0;
static const char* g_plFeedStop = "";
static int __cdecl plTmplCmp(const void* a, const void* b) { LONG x = ((const PlTmpl*)a)->order, y = ((const PlTmpl*)b)->order; return x < y ? -1 : (x > y ? 1 : 0); }
static ULONG64 plVaUsedMB() { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); return GlobalMemoryStatusEx(&ms) ? (ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20 : 0; }
static const ULONG64 kPlVaStop = 2300;                             // MB of the 4 GB address space: nothing more is handed over above this
static ULONG64 plVaLine();                                         // v59: the measured line between batches - from the process's limit (v57 2250, v58 2500 MB fixed)
static LONG g_plTierEnd[3] = { 0, 0, 0 };                          // v57: the plan walk's tier ends (queue positions)
static LONG g_plFedTier[3] = { 0, 0, 0 }, g_plFedNameTier[3] = { 0, 0, 0 };
static volatile LONG g_plRepT1Fed = 0, g_plRepT1All = 0, g_plRepT1Names = 0;   // the last load's tier 1: handed over / planned, names (report)
static LONG plTierCount(int t);
static int plTierOf(LONG order) { return order <= g_plTierEnd[0] ? 0 : (order <= g_plTierEnd[1] ? 1 : 2); }   // order: queue position + 1
// the names of the templates of the objects on the map; 0 when there are none yet
// v55: the players' factions as seeds. A new skirmish or multiplayer match has NO objects on the map when the engine's
// own load completes (test-37: the plan stayed empty, so nothing of the factions was prepared and every first unit,
// building, upgrade and spell of the match loaded during play). The players exist by then: ThePlayerList (0xDE4928:
// count at +0x14, Player* at +0x18..), each Player holding its side as an AsciiString. The side is found among the
// Player's AsciiString fields as the one that is a word of the mod's PlayerTemplate definitions (no offset or faction
// name is assumed), and every PlayerTemplate definition with that side becomes a seed: its starting buildings and
// units, spell book, heroes, science and command sets lead the walk to the faction's whole tech tree.
static BYTE* g_plPtWord = NULL;                                  // per word: 1 = used by some PlayerTemplate definition
static LONG g_plPtKind = 0;                                      // g_plIsDef value of a PlayerTemplate
static char g_plSideLog[600]; static LONG g_plSideOff = -1;
static LONG plPlayerSeeds(LONG* q, BYTE* seen, LONG qt) {
    if (gdNewOff()) return qt;                                    // GUARD safe mode: v55's faction seeds off
    if (!g_plIndexReady || !g_plCsr || !g_plDefEnd) return qt;
    if (!g_plPtKind) for (int i = 0; i < (int)(sizeof(kPlTop) / sizeof(kPlTop[0])); ++i) if (!lstrcmpA(kPlTop[i], "playertemplate")) g_plPtKind = i + 1;
    if (!g_plPtWord) {
        g_plPtWord = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_plPtWord) return qt;
        for (LONG id = 0; id < g_plNStr; ++id) if (g_plIsDef[id] == g_plPtKind && g_plCsr[id] >= 0)
            for (LONG i = g_plCsr[id], e = g_plDefEnd[id]; i < e; ++i) { LONG w = g_plCsrWords[i]; if (w >= 0 && w < (LONG)PL_MAXSTR && g_plIsDef[w] != g_plPtKind) g_plPtWord[w] = 1; }
    }
    LONG sides[24]; int ns = 0; int lp = 0; g_plSideLog[0] = 0;
    __try {
        DWORD pl = *(DWORD*)(ULONG_PTR)0x00DE4928;
        int n = pl ? *(int*)(ULONG_PTR)(pl + 0x14) : 0; if (n > 20) n = 20;
        for (int i = 0; i < n; ++i) {
            DWORD p = *(DWORD*)(ULONG_PTR)(pl + 0x18 + 4 * i); if (!p) continue;
            for (LONG off = 0x38; off <= 0x80; off += 4) {
                if (g_plSideOff >= 0 && off != g_plSideOff) continue;
                DWORD d = *(DWORD*)(ULONG_PTR)(p + off);
                if (d < 0x10000 || d >= 0x7FFF0000) continue;
                char tmp[48]; int k = 0; bool ok = true;
                __try { const char* c = (const char*)(ULONG_PTR)(d + 8); while (k < 47 && c[k]) { char ch = c[k]; if (ch < 32 || ch > 126) { ok = false; break; } tmp[k++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; } if (k >= 47) ok = false; }
                __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
                if (!ok || k < 2) continue;
                tmp[k] = 0;
                LONG id = plIntern(tmp, k, false);
                if (id < 0 || !g_plPtWord[id]) continue;
                if (g_plSideOff < 0) g_plSideOff = off;                 // the first match fixes the field for the rest
                bool dup = false; for (int j = 0; j < ns; ++j) if (sides[j] == id) dup = true;
                if (!dup && ns < 24) { sides[ns++] = id; if (lp < 520) lp += wsprintfA(g_plSideLog + lp, "%s%s", lp ? ", " : "", tmp); }
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (!ns) return qt;
    for (LONG id = 0; id < g_plNStr && qt < (LONG)PL_MAXSTR - 1; ++id) {
        if (g_plIsDef[id] != g_plPtKind || g_plCsr[id] < 0 || seen[id]) continue;
        bool hit = false;
        for (LONG i = g_plCsr[id], e = g_plDefEnd[id]; i < e && !hit; ++i) for (int j = 0; j < ns; ++j) if (g_plCsrWords[i] == sides[j]) { hit = true; break; }
        if (!hit) continue;
        seen[id] = 1; q[qt++] = id;
        if (lp < 560) lp += wsprintfA(g_plSideLog + lp, " [%s]", g_plArena + g_plStr[id].off);
    }
    return qt;
}
static LONG plSeedsNow(LONG* q, BYTE* seen, LONG* nMap) {
    DWORD gl = *(DWORD*)(ULONG_PTR)0x00DE412C;
    if (!gl) return 0;
    LONG qt = 0;
    __try {
        DWORD o = *(DWORD*)(ULONG_PTR)(gl + 0xAC);
        for (int guard = 0; o && guard < 200000; ++guard) {
            DWORD t = *(DWORD*)(ULONG_PTR)(o + 4), s = t ? *(DWORD*)(ULONG_PTR)(t + 0x64) : 0;
            if (s) {
                char tmp[128]; const char* nm = (const char*)(ULONG_PTR)(s + 8); int k = 0;
                while (k < 127 && nm[k]) { char c = nm[k]; tmp[k++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
                LONG id = (k > 0 && k < 127) ? plIntern(tmp, k, false) : -1;
                if (id >= 0 && !seen[id]) { seen[id] = 1; q[qt++] = id; }
            }
            o = *(DWORD*)(ULONG_PTR)(o + 0x8C);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    *nMap = qt;
    return plPlayerSeeds(q, seen, qt);                            // v55: and the players' factions (a new match has no objects yet)
}
// the plan (game thread, on the loading screen). true: planned (possibly empty, with the reason logged); false: not yet (no objects)
static bool plPlan(const char* when) {
    g_plPlanN = g_plPlanAt = g_plPlanAll = g_plPlanKB = g_plPlanBudgetKB = g_plPlanSeeds = g_plPlanTmpl = 0;
    { DWORD gl = *(DWORD*)(ULONG_PTR)0x00DE412C; if (gl && *(DWORD*)(ULONG_PTR)(gl + 0x110) == 9) return true; }   // the menu's shell map: nothing to do
    if (!g_plIndexReady) { logf("preload: %s - the INI index is not ready yet; the engine-side preload is skipped for this match.", when); return true; }
    DWORD t0 = GetTickCount();
    static BYTE* seen = NULL; static LONG* q = NULL;
    if (!seen) seen = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!q) q = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_plTl) g_plTl = (PlTmpl*)VirtualAlloc(NULL, sizeof(PlTmpl) * 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_plOrder) g_plOrder = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_plCumKB) g_plCumKB = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_plAN) g_plAN = (PlAName*)VirtualAlloc(NULL, sizeof(PlAName) * PL_AN_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plANN = g_plANAt = g_plANIn = g_plANFed = g_plANTried = 0;
    if (!seen || !q || !g_plTl || !g_plOrder || !g_plCumKB) return true;
    memset(seen, 0, (size_t)PL_MAXSTR); memset(g_plOrder, 0, sizeof(LONG) * (size_t)PL_MAXSTR); memset(g_plCumKB, 0, sizeof(LONG) * (size_t)PL_MAXSTR);
    LONG nMapSeeds = 0;
    LONG ns0 = plSeedsNow(q, seen, &nMapSeeds), seeds = ns0;
    if (!seeds) return false;
    static LONG sSeed[65536 + 64]; static LONG* defer = NULL; static BYTE* fl = NULL;
    if (!defer) defer = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!fl) fl = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!defer || !fl) return true;
    memset(fl, 0, (size_t)PL_MAXSTR);
    if (ns0 > 65536 + 64) ns0 = 65536 + 64;
    memcpy(sSeed, q, sizeof(LONG) * (size_t)ns0);
    // v57: tier 1 the factions' own spells and summons, tier 2 the map's objects' own, tier 3 the rest (plTierWalk)
    LONG tierEnd[3] = { 0, 0, 0 };
    LONG qt = plTierWalk(q, seen, defer, fl, sSeed + nMapSeeds, ns0 - nMapSeeds, sSeed, nMapSeeds, true, tierEnd);
    g_plTierEnd[0] = tierEnd[0]; g_plTierEnd[1] = tierEnd[1]; g_plTierEnd[2] = tierEnd[2];
    LONG64 cum = 0;
    if (g_plAN) plLearnFront(&cum, seen);                              // v59: what really appeared in earlier battles, first
    for (LONG qh = 0; qh < qt; ++qh) {
        LONG w = q[qh]; if (g_plOrder[w]) continue;                     // (a root found again as created: walked twice, counted once)
        g_plOrder[w] = qh + 1; g_plANOrderNow = qh + 1;
        if (g_plCsr[w] < 0) {                                          // a plain word: an asset's name?
            if (g_plAN && g_plStr[w].asset < 0 && !g_plIsDef[w]) plTryName(g_plArena + g_plStr[w].off, &cum, -1);   // v55: an archived asset's name
        }
        LONG a = g_plStr[w].asset;
        if (a >= 0 && g_plAN && g_plAsset[a].kind != 2) { LONG64 c0 = cum; if (plTryName(g_plArena + g_plStr[w].off, &cum, a)) cum = c0; }   // (its size is added just below)
        if (a >= 0 && a < g_plNAsset) {
            LONG64 sz = g_plAsset[a].size;
            if (g_plAsset[a].kind == 0) sz *= 2; else if (g_plAsset[a].kind == 1) sz = sz * 14 / 10; else if (sz > (2ll << 20)) sz = 2ll << 20;
            cum += sz;
        }
        g_plCumKB[w] = (LONG)(cum >> 10);
    }
    // the templates the engine has, by name, that the walk reached (TheThingFactory's hash map: buckets at +4..+8 of
    // the map at +0x14, each node { next, AsciiString name, ThingTemplate* })
    LONG nt = 0, nTmpl = 0;
    PlTmpl* tl = g_plTl;
    __try {
        DWORD tf = *(DWORD*)(ULONG_PTR)PL_TF, map = tf + 0x14;
        DWORD b = tf ? *(DWORD*)(ULONG_PTR)(map + 4) : 0, e = tf ? *(DWORD*)(ULONG_PTR)(map + 8) : 0;
        for (DWORD bk = b; bk && bk < e && nt < 65536; bk += 4)
            for (DWORD n = *(DWORD*)(ULONG_PTR)bk; n && nt < 65536; n = *(DWORD*)(ULONG_PTR)n) {
                nTmpl++;
                DWORD sd = *(DWORD*)(ULONG_PTR)(n + 4), t = *(DWORD*)(ULONG_PTR)(n + 8);
                if (!sd || !t) continue;
                char tmp[128]; const char* nm = (const char*)(ULONG_PTR)(sd + 8); int k = 0;
                while (k < 127 && nm[k]) { char c = nm[k]; tmp[k++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
                LONG id = (k > 0 && k < 127) ? plIntern(tmp, k, false) : -1;
                if (id < 0 || !g_plOrder[id]) continue;
                BYTE kind = g_plIsDef[id];
                if (kind < 1 || kind > 3) continue;                // Object, ChildObject, ObjectReskin
                tl[nt].t = t; tl[nt].order = fl[id] == 1 ? tierEnd[1] + g_plOrder[id] : g_plOrder[id]; tl[nt].cumKB = g_plCumKB[id]; nt++;   // v58: a root's own art (built, or on the map) is tier 3
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("preload: %s - the template table could not be read - engine-side preload skipped.", when); return true; }
    qsort(tl, (size_t)nt, sizeof(PlTmpl), plTmplCmp);
    ULONG64 va = plVaUsedMB();
    // v57: no estimate cuts the plan any more. test-41: the estimate (every file of the walk, models x2) said 179 MB for
    // 97 templates; the address space grew by 29 MB when they were loaded (most of their art was shared or already
    // in). The whole tier-ordered list is planned and handed over batch by batch while the address space MEASURED after
    // each batch's art is loaded stays under kPlVaFeedStop (and the 15 s cap, and enough free memory) - the same 2.3 GB
    // line as before, now reached for real.
    ULONG64 vaLine = plVaLine();
    LONG budgetKB = va < vaLine ? (LONG)((vaLine - va) << 10) : 0;
    LONG in = budgetKB > 0 ? nt : 0;
    if (gdNewOff() && in > 100) in = 100;                           // GUARD safe mode: as small as v56's plan (it handed over 97)
    g_plANIn = budgetKB > 0 ? g_plANN : 0;
    LONG tt[3] = { 0, 0, 0 }; for (LONG i = 0; i < nt; ++i) tt[plTierOf(tl[i].order)]++;
    LONG tn[3] = { 0, 0, 0 }; for (LONG i = 0; i < g_plANN; ++i) tn[plTierOf(g_plAN[i].order)]++;
    g_plPlanN = in; g_plPlanAll = nt; g_plPlanKB = nt ? tl[nt - 1].cumKB : 0; g_plPlanBudgetKB = budgetKB; g_plPlanSeeds = seeds; g_plPlanTmpl = nTmpl;
    logf("preload: %s - asset names: %d of the walk's names are assets the engine knows (%d looks), %d of them inside the budget - handed over after the templates.", when, (int)g_plANN, (int)g_plANTried, (int)g_plANIn);
    logf("preload: %s - seeds: the map's objects and the players' factions (side field at Player+0x%X: %s).", when, (unsigned)(g_plSideOff < 0 ? 0 : g_plSideOff), g_plSideLog[0] ? g_plSideLog : "none found");
    logf("preload: %s - engine-side plan: %d seeds (objects' templates and faction templates) lead to %d templates (of %d the engine has) and %d asset names, in three tiers: the factions' own spells and summons %d templates + %d names, the map's objects' own abilities %d + %d, the rest %d + %d; planned in %u ms. Handed to the engine's asset manager in that order once its own loading reports 100%%, while the address space measured after each batch stays under %u MB (now %u MB: room %d MB), for at most %d s (15 s in a network game); the rest during play.",
         when, (int)seeds, (int)nt, (int)nTmpl, (int)g_plANN, (int)tt[0], (int)tn[0], (int)tt[1], (int)tn[1], (int)tt[2], (int)tn[2], GetTickCount() - t0, (unsigned)vaLine, (unsigned)va, (int)(budgetKB >> 10), 45);
    logf("preload: %s - learned list (out\\aotr_learned.txt: models and textures first opened during earlier battles): %d names read, %d of them reached by this match's walk - handed over first.", when, (int)g_plLearnRead, (int)g_plLearnUsed);
    logf("preload: %s - tier 1 (v58: only what appears suddenly) - %d objects created by object creation lists and weapons (summons, spell casters, projectiles, debris), %d root objects followed only for what they create, %d words held back to tier 3 (roots' own art, objects named by #include files and macros, command sets, sciences).",
         when, (int)g_plT1Created, (int)g_plT1Roots, (int)g_plT1Held);
    return true;
}
// the asset list of the next nT templates and the names up to them (at most nN) to the asset manager, one AddKeys.
// false: an engine call failed (the plan is closed). *tier1 gets how many of them were tier 1.
static bool plFeedOne(LONG nT, LONG nN, LONG* tier1) {
    BYTE al[0x40]; memset(al, 0, sizeof(al)); BYTE mode = 0; LONG t1 = 0;
    __try {
        ((void (__fastcall*)(void*, void*))(ULONG_PTR)PL_AL_CTOR)(al, NULL);
        *(DWORD*)(al + 0x0C) = 0; al[0x10] = 1;                     // as the engine sets them after the constructor
        LONG j = g_plPlanAt;
        for (; j < g_plPlanN && j < g_plPlanAt + nT; ++j) {
            ((void (__fastcall*)(DWORD, void*, void*, void*))(ULONG_PTR)PL_GETASSETS)(g_plTl[j].t, NULL, al, &mode);
            int tr = plTierOf(g_plTl[j].order); g_plFedTier[tr]++; if (!tr) t1++;
        }
        LONG upTo = (j > g_plPlanAt && j < g_plPlanN) ? g_plTl[j - 1].order : 0x7FFFFFFF;   // names up to the last template of this batch (all once the templates are done)
        LONG k = g_plANAt;
        for (int cnt = 0; k < g_plANIn && cnt < nN && g_plAN[k].order <= upTo; ++k, ++cnt) {
            if (!plFindProto(g_plAN[k].nm)) continue;                 // only a name the engine already has a key and a prototype for
            struct { DWORD rc, cap; char c[64]; } data; memset(&data, 0, sizeof(data)); lstrcpynA(data.c, g_plAN[k].nm, 64);
            DWORD str = (DWORD)(ULONG_PTR)&data;                         // an AsciiString: a pointer to its data, the characters at +8
            ((void (__fastcall*)(void*, void*, DWORD*))(ULONG_PTR)0x0046C15B)(al, NULL, &str);   // AssetList::operator<<(const AsciiString&)
            g_plANFed++; { int tr = plTierOf(g_plAN[k].order); g_plFedNameTier[tr]++; if (!tr) t1++; }
        }
        ((void (__cdecl*)(void*))(ULONG_PTR)PL_ADDKEYS)(al);
        ((void (__fastcall*)(void*, void*))(ULONG_PTR)PL_AL_DTOR)(al, NULL);
        g_plPlanAt = j; g_plANAt = k;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_plPlanN = g_plPlanAt; g_plANIn = g_plANAt; return false; }
    if (tier1) *tier1 = t1;
    return true;
}
// v59: the address space line from the process's own limit: 1300 MB is kept for play and for the address space's
// fragments (test-42: play added 309 MB on top of the load, the peak 2693 of 4095 MB), never above 2800 MB. A 2 GB
// process (no large-address flag) gets 747 - nothing is handed over there.
// v65: test-47 (v64, a 29 min 4v4 started fresh): the load went to 2798 MB and the battle added 836 MB on top - 3634
// of 4095 MB, and the player saw stretched trees and odd shapes on soldiers in spells: the classic sign of a 32-bit
// BFME2 running out of address space (Direct3D objects that cannot be made). Now 1700 MB stay free for the battle and
// the line is never above 2400 MB (4 GB: 2395). What does not fit stays in Windows' file cache (plFileCacheBudget).
static ULONG64 plVaLine() {
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    ULONG64 tot = GlobalMemoryStatusEx(&ms) ? (ms.ullTotalVirtual >> 20) : 2048;
    ULONG64 l = tot > 1700 ? tot - 1700 : 0; if (l > 2400) l = 2400;
    return l;
}
// v59: how long the loading screen waits for the plan. test-42: 15 s - and on a saved game the screen showed no
// picture meanwhile (the guard's "no picture for 20 s"). Now 10 s while the loading screen draws, 5 s when it does
// not; what is left goes on during play in small pieces (plTrickleOnFrame).
static LONG g_plFeedFrames0 = 0;
// v64: test-46 - the loading screen's pictures are not frames of the game (the wait loop draws them), so v59's "no
// picture" rule always chose 5 s: 20-80 of 3455 sudden things were ready. The player wants the factions ready before
// the match starts. A game of one human (skirmish, campaign, a saved game): up to 45 s; a network game (every player
// waits for the slowest): 15 s. What is left goes on during play as before.
static bool plNetGame() { DWORD gl = *(volatile DWORD*)0x00DE412C; DWORD m = gl ? *(DWORD*)(ULONG_PTR)(gl + 0x110) : 0; return m == 1 || m == 5; }
static DWORD plFeedCapMs() { return plNetGame() ? 15000u : 45000u; }
static bool plFeedBatch() {
    if (g_plPlanAt >= g_plPlanN && g_plANAt >= g_plANIn) { g_plFeedStop = "all handed over"; return false; }
    if (GetTickCount() - g_plFeedT0 > plFeedCapMs()) { g_plFeedStop = plNetGame() ? "time cap (15 s, network game) - the rest during play" : "time cap (45 s) - the rest during play"; return false; }
    if (plVaUsedMB() >= plVaLine()) { g_plFeedStop = "the address space measured reached its line (kept for play)"; return false; }
    { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys < (1024ull << 20)) { g_plFeedStop = "the machine has under 1 GB of free memory"; return false; } }
    DWORD c0 = GetTickCount();
    if (!plFeedOne(20, 400, NULL)) { g_plFeedStop = "an engine call failed (stopped)"; return false; }
    g_plFeedCallMs += GetTickCount() - c0; g_plFeedBatches++;
    return true;
}
// ---------------------------------------------------------------- v59: the rest of the plan during play
// What the loading screen did not get to is handed over while the match runs: two templates and the names up to them
// at a time, on the game thread at the start of a frame, only when the asset manager has finished the previous piece
// (its reader thread loads; the game thread finishes what it delivered, as it does for anything the game itself asks
// for in a battle), the last frame took under 45 ms, and the address space is under its line. At first one piece every
// 0.4 s; a frame over 60 ms within 2 s after a piece doubles the gap (up to 6.4 s), twenty calm pieces halve it again.
// Asset loading only: the asset manager's own name keys (manager+0x1F8, used by nothing but the asset code: 0xA34110 is
// called from five places, all in it) - nothing of the simulation is touched (multiplayer-safe).
static volatile LONG g_plTrOn = 0, g_plTrBatches = 0, g_plTrT1 = 0, g_plTrSlowAfter = 0, g_plTrSkipSlow = 0, g_plTrSkipBusy = 0;
static DWORD g_plTrLast = 0, g_plTrGap = 400; static LONG g_plTrCalm = 0; static LONG64 g_plTrTicks = 0;
static const char* g_plTrStop = "";
static void plTrickleOnFrame(LONG64 periodTicks) {
    if (!g_plTrOn || g_gdState != 2) return;
    LONG fms = g_pqpf.QuadPart ? (LONG)(periodTicks * 1000 / g_pqpf.QuadPart) : 0;
    DWORD now = GetTickCount();
    if (g_plTrLast && now - g_plTrLast < 2000 && fms > 60) {          // a slow frame right after a piece: slower
        g_plTrSlowAfter++; g_plTrCalm = 0; if (g_plTrGap < 6400) g_plTrGap *= 2; g_plTrLast = now; return;
    }
    if (now - g_plTrLast < g_plTrGap) return;
    if (fms > 45) { g_plTrSkipSlow++; return; }
    if (g_plPlanAt >= g_plPlanN && g_plANAt >= g_plANIn) { g_plTrOn = 0; g_plTrStop = "all handed over"; logf("preload: during play - the whole plan is handed over (%d pieces, tier 1 items %d).", (int)g_plTrBatches, (int)g_plTrT1); return; }
    if (((DWORD (__cdecl*)())(ULONG_PTR)PL_PROGRESS)() < 100) { g_plTrSkipBusy++; return; }   // the previous piece (or the game's own) still loading
    if (plVaUsedMB() >= plVaLine()) { g_plTrOn = 0; g_plTrStop = "the address space reached its line"; logf("preload: during play - stopped: the address space reached %u MB (its line %u MB).", (unsigned)plVaUsedMB(), (unsigned)plVaLine()); return; }
    { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys < (768ull << 20)) { g_plTrLast = now; g_plTrGap = 6400; return; } }
    LONG64 t0 = qpcNow(); LONG t1 = 0;
    if (!plFeedOne(2, 40, &t1)) { g_plTrOn = 0; g_plTrStop = "an engine call failed"; logf("preload: during play - an engine call failed: stopped."); return; }
    g_plTrTicks += qpcNow() - t0; g_plTrBatches++; g_plTrT1 += t1; g_plTrLast = now;
    if (++g_plTrCalm >= 20 && g_plTrGap > 400) { g_plTrGap /= 2; g_plTrCalm = 0; }
}
static void plTrickleReport() {                                   // 30 s report thread
    static LONG lB = 0; static LONG64 lT = 0;
    LONG b = g_plTrBatches; LONG64 t = g_plTrTicks;
    if (b == lB) return;
    logf("preload: during play - last 30 s %d pieces handed over (%d us each on the game thread), plan at %d of %d templates and %d of %d names (tier 1 items so far %d) | gap now %u ms, slow frames after a piece %d, skipped for a slow frame %d, for the manager still loading %d%s",
         (int)(b - lB), g_pqpf.QuadPart ? (int)((t - lT) * 1000000 / g_pqpf.QuadPart / (b - lB)) : 0, (int)g_plPlanAt, (int)g_plPlanN, (int)g_plANAt, (int)g_plANIn, (int)g_plTrT1,
         g_plTrGap, (int)g_plTrSlowAfter, (int)g_plTrSkipSlow, (int)g_plTrSkipBusy, g_plTrOn ? "" : " | done");
    lB = b; lT = t;
}

static LONG plTierCount(int t) { LONG n = 0; for (LONG i = 0; i < g_plPlanAll; ++i) if (plTierOf(g_plTl[i].order) == t) n++; return n; }
static void plFeedReport() {
    if (g_plPlanState != 2) return;
    g_plPlanState = 3; g_pl2Runs++; g_plPostCheck = 1;
    if ((g_plPlanAt < g_plPlanN || g_plANAt < g_plANIn) && !gdNewOff()) { g_plTrOn = 1; g_plTrLast = 0; g_plTrGap = 400; g_plTrCalm = 0; g_plTrStop = ""; }   // v59: the rest during play
    logf("preload: loading screen - engine-side: asset names handed over %d (of %d planned) | by tier - the factions' own spells and summons: %d of %d templates, %d names; the map's objects' own: %d templates, %d names; the rest: %d templates, %d names.",
         (int)g_plANFed, (int)g_plANIn, (int)g_plFedTier[0], (int)plTierCount(0), (int)g_plFedNameTier[0], (int)g_plFedTier[1], (int)g_plFedNameTier[1], (int)g_plFedTier[2], (int)g_plFedNameTier[2]);
    g_plRepT1Fed = g_plFedTier[0]; g_plRepT1All = plTierCount(0); g_plRepT1Names = g_plFedNameTier[0];
    g_plFedTier[0] = g_plFedTier[1] = g_plFedTier[2] = 0; g_plFedNameTier[0] = g_plFedNameTier[1] = g_plFedNameTier[2] = 0;
    logf("preload: loading screen - engine-side: %d of the %d planned templates handed over in %d batches (%u ms in the engine's calls); the loading screen waited %u ms longer for their art; address space %u -> %u MB; stopped: %s.",
         (int)g_plPlanAt, (int)g_plPlanN, (int)g_plFeedBatches, g_plFeedCallMs, g_plFeedT0 ? GetTickCount() - g_plFeedT0 : 0, (unsigned)g_plFeedVa0, (unsigned)plVaUsedMB(), g_plFeedStop);
}
static void __cdecl plOnTail() {                              // after startNewGame's tail (0x6311ED) - every load, whatever GlobalData+0x1110 says
    gdOnLoading(); g_plTrOn = 0;                                // (a new load: the last plan's rest is not this match's)
    g_plPlanState = 1; g_plFeedT0 = 0; g_plFeedCallMs = 0; g_plFeedBatches = 0; g_plFeedStop = "";
    g_plPlanT = GetTickCount();
}
static __declspec(naked) void plTailStub() {                  // replaces 0x6315A7: call 0x6311ED (thiscall, one argument, ret 4)
    __asm {
        push dword ptr [esp + 4]
        mov  eax, 0x006311ED
        call eax
        call plOnTail
        ret  4
    }
}
static DWORD __cdecl plProgressHook() {                      // replaces 0x6282F0's call of the manager's progress
    DWORD r = ((DWORD (__cdecl*)())(ULONG_PTR)PL_PROGRESS)();
    if (r < 100 || !g_pl2On) return r;
    if (g_plPlanState == 1) {                                     // the engine's own load is complete: plan now (the map's objects exist, a saved game's too)
        if (plPlan("loading screen (the engine's own load complete)")) g_plPlanState = (g_plPlanN || g_plANIn) ? 2 : 0;
        if (g_plPlanState == 2 && !gdNewOff()) { InterlockedExchange(&g_plEarly, 1); SetEvent(g_plEvt); }   // v59: the plan's files read ahead of the engine's own reads, now
        else { g_plPlanState = 0; logf("preload: loading screen - no objects on the map yet at the end of the engine's load (a saved game brings them after it): nothing handed over (v52: no wait at the first logic step - in test-33 the engine's own asset manager was at 2%% there, so waiting only froze the screen)."); }
    }
    if (g_plPlanState != 2) return r;
    if (!g_plFeedT0) { g_plFeedT0 = GetTickCount(); g_plFeedVa0 = plVaUsedMB(); g_plFeedFrames0 = g_gdFrames; }
    for (;;) {                                                    // batches until one brings art that is not loaded yet
        if (!plFeedBatch()) { plFeedReport(); return r; }
        r = ((DWORD (__cdecl*)())(ULONG_PTR)PL_PROGRESS)();
        if (r < 100) return r;                                    // below 100 again while the new art loads: the loop goes on
    }
}
// v50: a saved game's objects are made after the engine's wait loop (test-32: "no objects at the end of the engine's
// load"). At its first logic step - before anything of the match has been drawn or simulated - the plan is made and
// fed here the same way, the game thread waiting for the asset manager between batches and finishing what its reader
// thread delivered (0x532D6F, as the engine's own loading loops do), for at most 4 s (under Windows' 5 s "not
// responding" mark: no window message is handled meanwhile).
static void plFirstStepFeed() {
    if (!g_pl2On) return;
    g_plFeedT0 = 0; g_plFeedCallMs = 0; g_plFeedBatches = 0; g_plFeedStop = "";
    if (!plPlan("first logic step of a saved game") || (!g_plPlanN && !g_plANIn)) return;
    g_plPlanState = 2;
    DWORD t0 = GetTickCount(); g_plFeedT0 = t0; g_plFeedVa0 = plVaUsedMB(); g_plFeedFrames0 = g_gdFrames;
    LONG waits = 0; bool done = false;
    for (;;) {
        DWORD r = ((DWORD (__cdecl*)())(ULONG_PTR)PL_PROGRESS)();
        if (r >= 100) { if (done || GetTickCount() - t0 > 2000) break; if (!plFeedBatch()) done = true; continue; }
        if (GetTickCount() - t0 > 2000) { g_plFeedStop = "2 s cap at the first logic step (v59: the rest during play, in small pieces)"; break; }
        __try { ((void (__cdecl*)())(ULONG_PTR)0x00532D6F)(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_plFeedStop = "the engine's finishing call failed"; break; }
        Sleep(2); waits++;
    }
    if (!g_plFeedStop[0]) g_plFeedStop = "all handed over";
    plFeedReport();
    logf("preload: first logic step of a saved game - the game waited %u ms for the art (%d short waits).", GetTickCount() - t0, (int)waits);
}
static bool plPatchCall(DWORD site, DWORD target, void* fn) {
    BYTE* q = (BYTE*)(ULONG_PTR)site;
    if (q[0] != 0xE8 || site + 5 + *(LONG*)(q + 1) != target) return false;
    DWORD old = 0;
    if (!VirtualProtect(q, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(LONG*)(q + 1) = (LONG)((DWORD)(ULONG_PTR)fn - (site + 5));
    VirtualProtect(q, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), q, 5);
    return true;
}
static void installPreload2() {
    char mk[MAX_PATH];
    // v67: OFF unless PRELOAD2_ON. Since the engine-side hand-over grew from 97-580 templates (v55-v58) to 1000-3400 (v63+),
    // the player saw broken pictures exactly in what it prepares - the Army of the Dead, the spider forest's strange lines,
    // the Balrog's missing rocks - while the address space was fine (test-49: 2827 MB). This path (GetAssetList + AddKeys on
    // the loading screen) is one the game itself never takes in BFME2 (GlobalData+0x1110 keeps the engine's own preload
    // off), so it is the prime suspect. The files stay in Windows' file cache (that cannot change a picture).
    if (GetFileAttributesA(aotrPath(mk, "PRELOAD2_ON")) == INVALID_FILE_ATTRIBUTES) { logf("preload: the engine-side preload is OFF (v67: suspected of the broken pictures in spells; PRELOAD2_ON turns it on) - the factions' files are still read into the file cache."); return; }
    static const BYTE kSite[] = { 0x57, 0x8B, 0xCE, 0xE8, 0x41, 0xFC, 0xFF, 0xFF };   // 0x6315A4 push edi / mov ecx,esi / call 0x6311ED (startNewGame's tail)
    static const BYTE kWait[] = { 0xE8, 0x06, 0xA7, 0x40, 0x00, 0x83, 0xF8, 0x64, 0x72, 0xDE };   // 0x628635 call progress / cmp eax,64h / jb 0x62861D
    static const BYTE kCtor[] = { 0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8D, 0x45, 0xFF };
    static const BYTE kGet[] = { 0x55, 0x8B, 0xEC, 0x33, 0xC0, 0x53, 0x8B, 0x5D, 0x0C, 0x38, 0x03 };
    static const BYTE kAdd[] = { 0x8B, 0x0D, 0x5C, 0xF7, 0xDE, 0x00, 0x85, 0xC9, 0x74, 0x0A };
    static const BYTE kProg[] = { 0x8B, 0x0D, 0x5C, 0xF7, 0xDE, 0x00, 0x85, 0xC9, 0x74, 0x05, 0xE9 };
    __try {
        if (memcmp((void*)0x006315A4, kSite, sizeof(kSite)) || *(const BYTE*)0x006314CA != 0xC2 || *(const WORD*)0x006314CB != 4 ||
            memcmp((void*)(ULONG_PTR)PL_WAITSITE, kWait, sizeof(kWait)) || memcmp((void*)(ULONG_PTR)PL_PROGRESS, kProg, sizeof(kProg)) ||
            memcmp((void*)(ULONG_PTR)PL_AL_CTOR, kCtor, sizeof(kCtor)) || memcmp((void*)(ULONG_PTR)PL_GETASSETS, kGet, sizeof(kGet)) ||
            memcmp((void*)(ULONG_PTR)PL_ADDKEYS, kAdd, sizeof(kAdd)) || *(BYTE*)(ULONG_PTR)PL_AL_DTOR != 0xB8) {
            logf("preload: the engine's asset calls differ from the analysed build - engine-side preload off."); return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HANDLE fz[256]; int nf = 0; suspendOthers(fz, &nf, 256);
    bool ok = plPatchCall(PL_WAITSITE, PL_PROGRESS, (void*)&plProgressHook);
    if (ok && !plPatchCall(PL_TAILSITE, 0x006311ED, (void*)&plTailStub)) {             // both or neither
        plPatchCall(PL_WAITSITE, (DWORD)(ULONG_PTR)&plProgressHook, (void*)(ULONG_PTR)PL_PROGRESS); ok = false; }
    resumeAll(fz, nf);
    if (!ok) { logf("preload: the engine-side preload could not patch its two calls - off."); return; }
    g_pl2On = 1;
    DWORD gd = *(DWORD*)(ULONG_PTR)0x00DE4364;
    logf("preload: engine-side preload on - planned on the loading screen once the engine's own load is complete; the templates and asset names of this match's list (v57: the factions' own spells and summons first, then the map's objects' own, then the rest; up to the measured 2.25 GB line) are handed over once the engine's own load reports 100%%, for at most %d s (5 s when the screen draws no picture; ThingTemplate::GetAssetList + AddKeys); v59: the rest during play in small pieces when frames are calm. PRELOAD2_OFF = off.%s",
         10, (gd && *(BYTE*)(ULONG_PTR)(gd + 0x1110)) ? " (GlobalData+0x1110 is set, as in every BFME2: the engine's own preloading is off; it stays off - with it the engine would also preload everything every command set can build.)" : "");
}

static void installPreload() {
    char mk[MAX_PATH];
    if (GetFileAttributesA(aotrPath(mk, "PRELOAD_OFF")) != INVALID_FILE_ATTRIBUTES) { logf("preload: PRELOAD_OFF is present - off."); return; }
    if (!g_fwOn || !g_fwNRoot) { logf("preload: the game's folders are not known (file read-ahead off) - off."); return; }
    g_plStr = (PlStr*)VirtualAlloc(NULL, sizeof(PlStr) * (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plTab = (DWORD*)VirtualAlloc(NULL, sizeof(DWORD) * (size_t)PL_TAB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plArena = (char*)VirtualAlloc(NULL, PL_STR_ARENA, MEM_RESERVE, PAGE_READWRITE);
    g_plAsset = (PlAsset*)VirtualAlloc(NULL, sizeof(PlAsset) * (size_t)PL_MAXASSET, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plEdge = (DWORD*)VirtualAlloc(NULL, (size_t)PL_MAXEDGE * 8, MEM_RESERVE, PAGE_READWRITE);
    g_plIsDef = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXSTR, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plInSet = (BYTE*)VirtualAlloc(NULL, (size_t)PL_MAXASSET, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plSeed = (char*)VirtualAlloc(NULL, 2u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_plEvt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_plStr || !g_plTab || !g_plArena || !g_plAsset || !g_plEdge || !g_plIsDef || !g_plInSet || !g_plSeed || !g_plEvt) { logf("preload: no memory - off."); return; }
    g_plOn = 1;
    installPreload2();
    HANDLE t = CreateThread(NULL, 256 * 1024, plIndexThread, NULL, 0, NULL); if (t) CloseHandle(t);
    t = CreateThread(NULL, 256 * 1024, plMatchThread, NULL, 0, NULL); if (t) CloseHandle(t);
    logf("preload: on - the mod's INI files are read in the background; at each match the objects on the map lead to the models, textures and sounds that can appear, which are read into the file cache ahead of time (no faction is named anywhere). PRELOAD_OFF = off.");
}
