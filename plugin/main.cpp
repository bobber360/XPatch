// XPatch360 v1.2
// Runtime game patcher Dashlaunch plugin (Xbox 360 RGH/JTAG)
// Authors: bobber360 & Claude
//
// Reads Xenia Canary patch files directly from the console:
//   Hdd:\Patches\<TITLEID> - <Game>.patch.toml
// When a title starts (TU already applied by the loader), it computes the same
// module hash as Xenia Canary and applies every [[patch]] with
// is_enabled = true, but only from files whose "hash" matches the running game. Toggle them with the Aurora script
// (Utilities -> XPatch360) or by editing the TOML.
//
// Build: build.bat (Xbox 360 SDK) -> XPatch360.xex
// Load:  launch.ini ->  pluginN = Hdd:\XPatch360.xex
// Own files: Hdd:\Patches\XPatch360\ (XPatch360.log, .ini, .cache, .dump, dumps *.bin)
// Settings: Hdd:\Patches\XPatch360\XPatch360.ini (created with defaults if missing)
//
// Uses xxHash (BSD 2-Clause, https://github.com/Cyan4973/xxHash) for the
// XXH3-64 module hash that Xenia Canary writes into the "hash" field.

#define XPATCH_VERSION "1.2"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The Xbox 360 compiler defines _WIN32, and xxhash takes _WIN32 to mean a
// little-endian CPU. The Xenon is big endian: say so, or every hash is wrong.
#if defined(_XBOX)
#  define XXH_CPU_LITTLE_ENDIAN 0
#endif
#define XXH_INLINE_ALL
#define XXH_NO_INLINE_HINTS 1 // smaller code and stack frames
#define XXH_VECTOR 0          // XXH_SCALAR: portable code, same result on big endian
#include "xxhash.h"

// --------------------------------------------------------------------------
// Kernel / XAM imports
// --------------------------------------------------------------------------
extern "C" {
    typedef struct _XP_STRING {
        USHORT Length;
        USHORT MaximumLength;
        PCHAR  Buffer;
    } XP_STRING, *PXP_STRING;

    VOID  RtlInitAnsiString(PXP_STRING Dest, const char* Src);
    LONG  ObCreateSymbolicLink(PXP_STRING Link, PXP_STRING Device);
    BOOL  MmIsAddressValid(PVOID Address);
    DWORD XexGetModuleHandle(PCSTR ModuleName, PHANDLE Handle);
    DWORD XexGetProcedureAddress(HANDLE Module, DWORD Ordinal, PVOID* Address);
    DWORD XamGetCurrentTitleId(VOID);
    DWORD XamGetExecutionId(PVOID* ExecutionId);

    DWORD ExCreateThread(PHANDLE Handle, DWORD StackSize, LPDWORD ThreadId,
                         PVOID ApiThreadStartup, LPTHREAD_START_ROUTINE Start,
                         LPVOID Param, DWORD CreationFlags);
    VOID  XapiThreadStartup(VOID (__cdecl* Start)(VOID*), VOID* Param);
}

#pragma pack(push, 1)
typedef struct _XP_EXEC_ID {
    DWORD MediaId;
    DWORD Version;        // after TU: the TU's version
    DWORD BaseVersion;
    DWORD TitleId;
    BYTE  Platform;
    BYTE  ExecutableType;
    BYTE  DiscNum;
    BYTE  DiscsInSet;
    DWORD SaveGameId;
} XP_EXEC_ID;
#pragma pack(pop)

#define XP_DIR       "Hdd:\\Patches\\"            // the .toml patch files
#define XP_DATA      XP_DIR "XPatch360\\"             // XPatch360's own files (log, ini, cache, dumps)
#define XP_LOG       XP_DATA "XPatch360.log"
#define XP_MAX_FILE  (128 * 1024)   // biggest .toml file read (Xenia files are 2-10 KB)
#ifndef INVALID_FILE_ATTRIBUTES          // not in the Xbox 360 SDK headers
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

static BOOL g_Running = TRUE;
static char g_FileBuf[XP_MAX_FILE + 1];

// --------------------------------------------------------------------------
// Settings: Hdd:\Patches\XPatch360\XPatch360.ini - read at boot and every time a game
// starts, so a change (by hand or from the Aurora script) needs no reboot.
// --------------------------------------------------------------------------
#define XP_INI XP_DATA "XPatch360.ini"

typedef struct {
    BOOL  Notify;      // notification when a game starts
    BOOL  FullLog;     // TRUE: everything, FALSE: errors only
    DWORD LogMaxKb;    // log bigger than this at boot -> deleted, started again
    BOOL  EarlyPatch;  // reserved: patch before the title's entry point (not implemented)
    BOOL  Debug;       // memory dumps (XPatch360.dump), "DEBUG MODE" in the log
} XP_CFG;
static XP_CFG g_Cfg = { TRUE, TRUE, 64, FALSE, FALSE };

static const char g_IniDefault[] =
    "; XPatch360 settings - read at boot and every time a game starts\r\n"
    "; 1 = on, 0 = off. Can also be changed in Aurora: Utilities -> XPatch360 -> Settings\r\n"
    "[XPatch360]\r\n"
    "notify      = 1     ; notification when a game starts\r\n"
    "log         = 1     ; 1 = full log, 0 = errors only\r\n"
    "log_max_kb  = 64    ; a bigger log is deleted at boot and started again\r\n"
    "early_patch = 0     ; patch before the game starts - for patches marked (early patching required)\r\n"
    "debug       = 0     ; debug mode: memory dumps (XPatch360.dump), debug menu in Aurora\r\n";

static void LoadSettings()
{
    FILE* f = fopen(XP_INI, "rb");
    if (!f) {
        f = fopen(XP_INI, "wb");                    // first run: write the defaults
        if (f) { fwrite(g_IniDefault, 1, sizeof(g_IniDefault) - 1, f); fclose(f); }
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char* c = strchr(line, ';'); if (c) *c = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = line; while (*k == ' ' || *k == '\t') k++;
        char* ke = eq; while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        char* v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        BOOL on = (*v == '1' || *v == 't' || *v == 'T' || ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N')));
        if      (!_stricmp(k, "notify"))      g_Cfg.Notify     = on;
        else if (!_stricmp(k, "log"))         g_Cfg.FullLog    = on;
        else if (!_stricmp(k, "early_patch")) g_Cfg.EarlyPatch = on;
        else if (!_stricmp(k, "debug"))       g_Cfg.Debug      = on;
        else if (!_stricmp(k, "log_max_kb")) {
            DWORD n = strtoul(v, NULL, 10);
            g_Cfg.LogMaxKb = (n < 8) ? 8 : (n > 4096 ? 4096 : n);
        }
    }
    fclose(f);
}

// --------------------------------------------------------------------------
// Logging
// --------------------------------------------------------------------------
// While a game is being patched every hard-disk write costs time the game is
// already running, so log lines are collected in memory and written in one go
// afterwards (LogDefer / LogFlush).
static BOOL  g_LogDefer = FALSE;
static char  g_LogBuf[8192];
static DWORD g_LogLen = 0;

static void LogFlush()
{
    if (g_LogLen) {
        FILE* f = fopen(XP_LOG, "a");
        if (f) { fwrite(g_LogBuf, 1, g_LogLen, f); fclose(f); }
        g_LogLen = 0;
    }
    g_LogDefer = FALSE;
}

static BOOL g_LogError = FALSE;      // set by LogErr: written even with log = 0

static void Log(const char* fmt, ...)
{
    if (!g_Cfg.FullLog && !g_LogError) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    strcat(line, "\r\n");

    if (g_LogDefer) {
        DWORD n = (DWORD)strlen(line);
        if (g_LogLen + n < sizeof(g_LogBuf)) {
            memcpy(g_LogBuf + g_LogLen, line, n);
            g_LogLen += n;
            return;
        }
        LogFlush();                       // buffer full: write it out
    }
    FILE* f = fopen(XP_LOG, "a");
    if (f) { fputs(line, f); fclose(f); }
}

// errors and warnings: always in the log, also with "log = 0"
#define LogErr(...) do { g_LogError = TRUE; Log(__VA_ARGS__); g_LogError = FALSE; } while (0)

// at boot: a log bigger than log_max_kb is deleted and started again
static void TrimLog()
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesEx(XP_LOG, GetFileExInfoStandard, &a) &&
        a.nFileSizeLow > g_Cfg.LogMaxKb * 1024)
        DeleteFile(XP_LOG);
}

static void MountHdd()
{
    XP_STRING link, dev;
    RtlInitAnsiString(&dev, "\\Device\\Harddisk0\\Partition1");
    // Create "Hdd:" in both namespaces: system threads use "\System??\",
    // the rest "\??\". Don't rely on another plugin (JRPC etc.) having done it.
    RtlInitAnsiString(&link, "\\System??\\Hdd:");
    ObCreateSymbolicLink(&link, &dev);
    RtlInitAnsiString(&link, "\\??\\Hdd:");
    ObCreateSymbolicLink(&link, &dev);
}

// Plugins load very early at boot; the hard disk may not be ready yet.
// Retry until Hdd:\Patches can be created or opened (up to ~60 s).
static BOOL WaitForHdd()
{
    for (int i = 0; i < 120; i++) {
        MountHdd();
        if (CreateDirectory("Hdd:\\Patches", NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            CreateDirectory("Hdd:\\Patches\\XPatch360", NULL);
            // v1.1 kept its files in Hdd:\Patches\ itself: move them over once
            static const char* old[] = { "XPatch360.log", "XPatch360.cache", "XPatch360.ini", "XPatch360.dump" };
            for (int k = 0; k < 4; k++) {
                char from[64], to[64];
                _snprintf(from, sizeof(from), XP_DIR "%s", old[k]);
                _snprintf(to, sizeof(to), XP_DATA "%s", old[k]);
                if (GetFileAttributes(from) != INVALID_FILE_ATTRIBUTES &&
                    GetFileAttributes(to) == INVALID_FILE_ATTRIBUTES)
                    MoveFile(from, to);
            }
            return TRUE;
        }
        Sleep(500);
    }
    return FALSE;
}

// --------------------------------------------------------------------------
// Cache flush: make the CPU see the new instructions
// --------------------------------------------------------------------------
static void __declspec(naked) Icbi(void* addr)
{
    __emit(0x7C001FAC);   // icbi 0, r3
    __emit(0x4E800020);   // blr
}

// isync ; blr   (no __isync intrinsic in the XDK either)
static void __declspec(naked) Isync()
{
    __emit(0x4C00012C);   // isync
    __emit(0x4E800020);   // blr
}

static void FlushRange(BYTE* p, DWORD len)
{
    DWORD start = (DWORD)p & ~127u;           // 128-byte cache lines on Xenon
    DWORD end   = ((DWORD)p + len + 127) & ~127u;
    for (DWORD a = start; a < end; a += 128)
        __dcbst(0, (void*)a);
    __sync();
    for (DWORD a = start; a < end; a += 128)
        Icbi((void*)a);
    Isync();
}

// --------------------------------------------------------------------------
// Module hash, the way Xenia Canary computes the "hash" field
// (src/xenia/kernel/user_module.cc, UserModule::CalculateHash):
//   - take the XEX security info of the running executable (after the TU),
//   - find the first and the last page descriptor marked as CODE,
//   - Xenia uses the descriptor INDEX as a page number (index * page size),
//     this is copied exactly so the result matches,
//   - XXH3-64 over [base + first*page, base + (last+1)*page).
// Xenia hashes AFTER it has resolved the imports in its own way
// (src/xenia/cpu/xex_module.cc, SetupLibraryImports):
//   - kernel/xam function thunks (16 bytes) -> sc 2; blr; nop; nop
//   - function import descriptors (4 bytes) -> 0xDEADC0DE
//   - variable imports -> Xenia's own addresses (cannot be reproduced)
// The console writes different values there, so the import table of the XEX
// is read and those places are replaced with Xenia's bytes while hashing
// (memory itself is not changed).
// --------------------------------------------------------------------------
#define XEX_SECTION_CODE            1
#define XEX_HEADER_IMPORT_LIBRARIES 0x000103FF
#define XEX_IMAGE_PAGE_SIZE_4KB     0x10000000

typedef struct {
    BOOL   Ok;
    DWORD  Start, End, PageSize, Descriptors, FirstCode, LastCode;
    DWORD  Thunks, FuncSlots;      // replaced with Xenia's bytes (inside the range)
    DWORD  VarSlots, UserThunks;   // inside the range but NOT reproducible
    UINT64 Raw, XeniaStyle;
} XP_HASH;

// Places to replace while hashing (address, length, bytes to use instead).
// Kept small on purpose: system plugins share a small memory pool with xbdm.
typedef struct { DWORD Addr; DWORD Len; const BYTE* Data; } XP_SUBST;
#define XP_MAX_SUBST 2048
static XP_SUBST g_Subst[XP_MAX_SUBST];
static int      g_SubstCount;

static const BYTE g_XThunk[16] = { 0x44,0x00,0x00,0x42, 0x4E,0x80,0x00,0x20,
                                   0x60,0x00,0x00,0x00, 0x60,0x00,0x00,0x00 };
static const BYTE g_XFunc[4]   = { 0xDE,0xAD,0xC0,0xDE };

// big-endian 32-bit read (the console is big endian; explicit so it is portable)
static DWORD Be32(const BYTE* p)
{
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | (DWORD)p[3];
}

static void AddSubst(DWORD addr, const BYTE* data, DWORD len)
{
    if (g_SubstCount >= XP_MAX_SUBST) return;
    g_Subst[g_SubstCount].Addr = addr;
    g_Subst[g_SubstCount].Len  = len;
    g_Subst[g_SubstCount].Data = data;
    g_SubstCount++;
}

// ---- undo log: original bytes of everything XPatch360 writes ----
// Used to check a cached hash after patching (hash the memory as it was) and
// to undo the writes if that check fails.
typedef struct { DWORD Addr; DWORD Len; DWORD Off; } XP_UNDO;
#define XP_MAX_UNDO 512
static XP_UNDO g_Undo[XP_MAX_UNDO];
static int     g_UndoCount;
static BYTE    g_UndoData[16384];
static DWORD   g_UndoUsed;
static BOOL    g_UndoFull;

// ---- conflict check: two enabled patches writing the same bytes ----
// The later patch wins, which is rarely what the user wants (e.g. RDR
// "Disable V-Sync" and "60 FPS V-Sync" both write 82C1F5BF): logged as an
// error and counted in the notification.
#define XP_MAX_TRACK  512
#define XP_MAX_TNAMES 128
#define XP_MAX_PAIRS  32
typedef struct { DWORD Addr, Len; int Name; } XP_TRACK;
static XP_TRACK g_Track[XP_MAX_TRACK];
static int      g_TrackCount;
static char     g_TrackNames[XP_MAX_TNAMES][64];
static int      g_TrackNameCount;
static int      g_Pairs[XP_MAX_PAIRS][2];
static int      g_Conflicts;               // conflicting patch pairs in this game start

static void TrackReset() { g_TrackCount = 0; g_TrackNameCount = 0; g_Conflicts = 0; }

static void TrackWrite(DWORD titleId, const char* name, DWORD addr, DWORD len)
{
    int ni = -1;
    for (int i = g_TrackNameCount - 1; i >= 0; i--)          // usually the last one
        if (!strcmp(g_TrackNames[i], name)) { ni = i; break; }
    if (ni < 0) {
        if (g_TrackNameCount >= XP_MAX_TNAMES) return;
        ni = g_TrackNameCount++;
        strncpy(g_TrackNames[ni], name, 63);
        g_TrackNames[ni][63] = 0;
    }
    for (int i = 0; i < g_TrackCount; i++) {
        XP_TRACK* t = &g_Track[i];
        if (t->Name == ni || addr >= t->Addr + t->Len || t->Addr >= addr + len) continue;
        BOOL seen = FALSE;
        int pairs = g_Conflicts < XP_MAX_PAIRS ? g_Conflicts : XP_MAX_PAIRS;
        for (int k = 0; k < pairs; k++)
            if (g_Pairs[k][0] == t->Name && g_Pairs[k][1] == ni) { seen = TRUE; break; }
        if (seen) continue;
        if (g_Conflicts < XP_MAX_PAIRS) { g_Pairs[g_Conflicts][0] = t->Name; g_Pairs[g_Conflicts][1] = ni; }
        g_Conflicts++;
        LogErr("[%08X] CONFLICT: '%s' and '%s' both write %08X ('%s' is applied last and wins)",
               titleId, g_TrackNames[t->Name], name, addr > t->Addr ? addr : t->Addr, name);
    }
    if (g_TrackCount < XP_MAX_TRACK) {
        g_Track[g_TrackCount].Addr = addr; g_Track[g_TrackCount].Len = len; g_Track[g_TrackCount].Name = ni;
        g_TrackCount++;
    }
}

static void UndoReset() { g_UndoCount = 0; g_UndoUsed = 0; g_UndoFull = FALSE; TrackReset(); }

static void UndoSave(const BYTE* addr, DWORD len)
{
    if (g_UndoCount >= XP_MAX_UNDO || g_UndoUsed + len > sizeof(g_UndoData)) { g_UndoFull = TRUE; return; }
    memcpy(g_UndoData + g_UndoUsed, addr, len);
    g_Undo[g_UndoCount].Addr = (DWORD)addr;
    g_Undo[g_UndoCount].Len  = len;
    g_Undo[g_UndoCount].Off  = g_UndoUsed;
    g_UndoCount++;
    g_UndoUsed += len;
}

static int CmpSubst(const void* a, const void* b)
{
    DWORD x = ((const XP_SUBST*)a)->Addr, y = ((const XP_SUBST*)b)->Addr;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// portable part: raw XXH3 and Xenia-style XXH3 of [start, start+len) at p,
// using the (sorted) substitutions in g_Subst
static void HashRange(const BYTE* p, DWORD start, DWORD len, XP_HASH* h)
{

    static XXH3_state_t st;   // static: keep it off the thread stack
    XXH3_64bits_reset(&st);
    DWORD done = 0;
    for (int k = 0; k < g_SubstCount; k++) {
        const XP_SUBST* s = &g_Subst[k];
        const BYTE* data = s->Data;
        DWORD       dlen = s->Len;
        if (s->Addr < start || s->Addr - start + dlen > len) continue;
        DWORD off = s->Addr - start;
        if (off < done) continue;                 // overlapping entry, ignore
        if (off > done) XXH3_64bits_update(&st, p + done, off - done);
        XXH3_64bits_update(&st, data, dlen);
        done = off + dlen;
    }
    if (len > done) XXH3_64bits_update(&st, p + done, len - done);
    h->XeniaStyle = XXH3_64bits_digest(&st);
}

// Known-answer test: the same inputs hashed on a PC give these values.
// If the console disagrees, the hash function itself is miscompiled.
static BOOL g_HashWorks = FALSE;   // set by the self-test at start

static BOOL HashSelfTest()
{
    static const char a[] = "XPatch self-test: XXH3-64 must match on PC and console";
    static BYTE b[1000];
    for (int i = 0; i < 1000; i++) b[i] = (BYTE)(i * 31 + 7);
    UINT64 ha = XXH3_64bits(a, sizeof(a) - 1);
    UINT64 hb = XXH3_64bits(b, sizeof(b));
    BOOL ok = (ha == 0x58C80386253F6DE5ULL) && (hb == 0x989765D0EA7A5ECDULL);
    g_LogError = !ok;
    Log("Hash self-test: %s (short %08X%08X, long %08X%08X)", ok ? "OK" : "FAILED",
        (DWORD)(ha >> 32), (DWORD)ha, (DWORD)(hb >> 32), (DWORD)hb);
    g_LogError = FALSE;
    return ok;
}

// MmIsAddressValid for every 4 KB page of [p, p+len)
static BOOL Readable(const void* p, DWORD len)
{
    DWORD a = (DWORD)p & ~0xFFFu, end = (DWORD)p + len;
    for (; a < end; a += 0x1000)
        if (!MmIsAddressValid((PVOID)a)) return FALSE;
    return TRUE;
}

// optional XEX header by key; the value is an offset in the file and an
// absolute address on the console (like the security info field)
static BYTE* FindOptHeader(BYTE* xex, DWORD hdrSize, DWORD key)
{
    DWORD n = Be32(xex + 0x14);
    if (n == 0 || n > 256 || !Readable(xex + 0x18, n * 8)) return NULL;
    for (DWORD i = 0; i < n; i++) {
        if (Be32(xex + 0x18 + i * 8) != key) continue;
        DWORD v   = Be32(xex + 0x18 + i * 8 + 4);
        DWORD off = (v >= (DWORD)xex) ? v - (DWORD)xex : v;
        if (off < 0x18 || (hdrSize && off >= hdrSize)) return NULL;
        return xex + off;
    }
    return NULL;
}

// is the address in a CODE section? (real page layout: descriptors with page counts)
static BOOL IsCodeAddr(DWORD a, DWORD base, const BYTE* desc, DWORD count, DWORD realPage)
{
    if (a < base) return FALSE;
    DWORD pg = (a - base) / realPage, acc = 0;
    for (DWORD i = 0; i < count; i++) {
        DWORD v  = Be32(desc + i * 0x18);
        DWORD pc = v >> 4;
        if (pg < acc + pc) return (v & 0xF) == XEX_SECTION_CODE;
        acc += pc;
    }
    return FALSE;
}

static BOOL SameNoCase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (x != y) return FALSE;
    }
    return *a == *b;
}

static void BuildImportSubst(BYTE* xex, DWORD hdrSize, DWORD base,
                             const BYTE* desc, DWORD count, DWORD realPage, XP_HASH* h)
{
    g_SubstCount = 0;
    BYTE* imp = FindOptHeader(xex, hdrSize, XEX_HEADER_IMPORT_LIBRARIES);
    if (!imp || !Readable(imp, 12)) { LogErr("Hash: no import table"); return; }
    DWORD total = Be32(imp), strSize = Be32(imp + 4), strCount = Be32(imp + 8);
    if (total < 12 || total > 0x100000 || !Readable(imp, total) || strSize >= total) {
        LogErr("Hash: odd import table (size %X)", total);
        return;
    }

    const char* names[32];
    memset(names, 0, sizeof(names));
    for (DWORD i = 0, o = 0; i < strSize && o < strCount && o < 32; o++) {
        names[o] = (const char*)(imp + 12 + i);
        i += (DWORD)strlen(names[o]) + 1;
        if (i % 4) i += 4 - (i % 4);
    }

    DWORD off = strSize + 12;
    while (off + 0x28 <= total) {
        const BYTE* lib = imp + off;
        DWORD lsize = Be32(lib);
        if (!lsize || off + lsize > total) break;
        DWORD nameIdx = (Be32(lib + 0x24) >> 16) & 0xFF;
        DWORD cnt     =  Be32(lib + 0x24) & 0xFFFF;
        const char* nm = (nameIdx < 32 && names[nameIdx]) ? names[nameIdx] : "?";
        BOOL kernel = SameNoCase(nm, "xboxkrnl.exe") || SameNoCase(nm, "xam.xex") ||
                      SameNoCase(nm, "xbdm.xex");
        if (0x28 + cnt * 4 > lsize) break;

        for (DWORD r = 0; r < cnt; r++) {
            DWORD a       = Be32(lib + 0x28 + r * 4);
            BOOL  inRange = (a >= h->Start && a < h->End);
            if (IsCodeAddr(a, base, desc, count, realPage)) {
                // thunk
                if (!inRange) continue;
                if (kernel) { AddSubst(a, g_XThunk, 16); h->Thunks++; }
                else        { h->UserThunks++; }
            } else {
                // descriptor: a function if the next record is its thunk
                BOOL func = (r + 1 < cnt) &&
                            IsCodeAddr(Be32(lib + 0x28 + (r + 1) * 4), base, desc, count, realPage);
                if (!inRange) continue;
                if (func && kernel) { AddSubst(a, g_XFunc, 4); h->FuncSlots++; }
                else                { h->VarSlots++; }
            }
        }
        off += lsize;
    }
    qsort(g_Subst, g_SubstCount, sizeof(XP_SUBST), CmpSubst);
}

static void ComputeModuleHash(XP_HASH* h, BOOL asBeforeWrites)
{
    memset(h, 0, sizeof(*h));

    // XexExecutableModuleHandle (xboxkrnl #0x193) holds the running title's
    // loader entry; +0x1C = image base, +0x58 = XEX header in memory.
    HANDLE krnl = NULL;
    PVOID  var  = NULL;
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) != 0 || !krnl) { LogErr("Hash: no xboxkrnl handle"); return; }
    if (XexGetProcedureAddress(krnl, 0x193, &var) != 0 || !var)  { LogErr("Hash: no XexExecutableModuleHandle"); return; }
    if (!Readable(var, 4)) { LogErr("Hash: handle variable %08X not readable", (DWORD)var); return; }

    BYTE* ldr = *(BYTE**)var;
    if (!ldr || !Readable(ldr, 0x5C)) { LogErr("Hash: bad loader entry %08X", (DWORD)ldr); return; }
    DWORD base = *(DWORD*)(ldr + 0x1C);
    BYTE* xex  = *(BYTE**)(ldr + 0x58);
    if (!xex || !Readable(xex, 0x18) || *(DWORD*)xex != 0x58455832) {   // 'XEX2'
        LogErr("Hash: XEX header not found (%08X)", (DWORD)xex);
        return;
    }

    // In the file this field is an offset from the header start. The console's
    // loader rewrites it into an absolute address (seen: header 8E050000,
    // field 8E050098), Xenia keeps the offset. Accept both, inside the header.
    DWORD hdrSize = *(DWORD*)(xex + 0x08);
    DWORD secFld  = *(DWORD*)(xex + 0x10);
    DWORD secOff  = (secFld >= (DWORD)xex) ? secFld - (DWORD)xex : secFld;
    if (secOff < 0x18 || secOff > 0x100000 || (hdrSize && secOff >= hdrSize)) {
        LogErr("Hash: odd security field %08X (header %08X, size %X)", secFld, (DWORD)xex, hdrSize);
        return;
    }
    BYTE* sec = xex + secOff;
    if (!Readable(sec, 0x184)) { LogErr("Hash: security info %08X not readable", (DWORD)sec); return; }
    DWORD count = *(DWORD*)(sec + 0x180);
    if (count == 0 || count > 0x10000) { LogErr("Hash: odd descriptor count %u", count); return; }
    BYTE* desc = sec + 0x184;                 // 0x18 bytes each: value + digest
    if (!Readable(desc, count * 0x18)) { LogErr("Hash: descriptors not readable"); return; }

    DWORD first = 0xFFFFFFFF, last = 0xFFFFFFFF;
    for (DWORD i = 0; i < count; i++) {
        DWORD v = *(DWORD*)(desc + i * 0x18);
        if ((v & 0xF) == XEX_SECTION_CODE) {
            if (first == 0xFFFFFFFF) first = i;
            last = i;
        }
    }
    if (first == 0xFFFFFFFF) { LogErr("Hash: no code section"); return; }

    // Xenia's heap page size for the base address (used with the descriptor index)
    DWORD page = (base >= 0x90000000 && base < 0xA0000000) ? 0x1000 : 0x10000;
    // real page size of the image (used to find which section an address is in)
    DWORD realPage = (*(DWORD*)(sec + 0x10C) & XEX_IMAGE_PAGE_SIZE_4KB) ? 0x1000 : 0x10000;

    h->Start = base + first * page;
    h->End   = base + (last + 1) * page;
    h->PageSize = page; h->Descriptors = count; h->FirstCode = first; h->LastCode = last;
    if (h->End <= h->Start || h->End - h->Start > 0x4000000) { LogErr("Hash: odd range %08X-%08X", h->Start, h->End); return; }
    if (!Readable((void*)h->Start, h->End - h->Start)) { LogErr("Hash: range %08X-%08X not readable", h->Start, h->End); return; }

    BuildImportSubst(xex, hdrSize, base, desc, count, realPage, h);
    if (asBeforeWrites) {
        // put XPatch360's own writes back to the original bytes while hashing
        for (int u = 0; u < g_UndoCount; u++)
            AddSubst(g_Undo[u].Addr, g_UndoData + g_Undo[u].Off, g_Undo[u].Len);
        qsort(g_Subst, g_SubstCount, sizeof(XP_SUBST), CmpSubst);
    }
    HashRange((const BYTE*)h->Start, h->Start, h->End - h->Start, h);
    h->Ok = TRUE;
}

// "797B4775D3BC9853" -> value; FALSE if not 16 hex digits
static BOOL ParseHash64(const char* s, UINT64* out)
{
    UINT64 v = 0; int n = 0;
    for (; *s && n < 16; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' :
                (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
                (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0) return FALSE;
        v = (v << 4) | (UINT64)d; n++;
    }
    if (n != 16) return FALSE;
    *out = v;
    return TRUE;
}

// --------------------------------------------------------------------------
// Minimal TOML reader for the Xenia patch format
// --------------------------------------------------------------------------
static char* Trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

// cut "# comment" outside of quotes
static void StripComment(char* s)
{
    BOOL inStr = FALSE;
    for (; *s; s++) {
        if (*s == '\\' && inStr && s[1]) { s++; continue; }
        if (*s == '"') inStr = !inStr;
        else if (*s == '#' && !inStr) { *s = 0; return; }
    }
}

// "text" -> text (handles \" \\ \n \t); returns length, -1 if not a string
static int Unquote(const char* v, char* out, int max)
{
    if (*v != '"') return -1;
    v++;
    int n = 0;
    while (*v && *v != '"' && n < max - 1) {
        char c = *v++;
        if (c == '\\' && *v) {
            c = *v++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
        }
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static UINT64 ParseInt(const char* v)
{
    char tmp[64];
    if (Unquote(v, tmp, sizeof(tmp)) >= 0) v = tmp;
    // TOML allows 0x1234_5678
    char clean[64]; int n = 0;
    for (; *v && n < 63; v++) if (*v != '_') clean[n++] = *v;
    clean[n] = 0;
    if (clean[0] == '-') return (UINT64)_strtoi64(clean, NULL, 0);
    return _strtoui64(clean, NULL, 0);
}

static void Put(BYTE* out, UINT64 v, int size)
{
    for (int i = size - 1; i >= 0; i--) { out[i] = (BYTE)v; v >>= 8; }
}

// Encode a Xenia patch value into big-endian bytes; returns length or -1
static int EncodeValue(const char* type, const char* v, BYTE* out, int max)
{
    if (!strcmp(type, "be8"))  { Put(out, ParseInt(v), 1); return 1; }
    if (!strcmp(type, "be16")) { Put(out, ParseInt(v), 2); return 2; }
    if (!strcmp(type, "be32")) { Put(out, ParseInt(v), 4); return 4; }
    if (!strcmp(type, "be64")) { Put(out, ParseInt(v), 8); return 8; }
    if (!strcmp(type, "f32")) {
        float f = (float)atof(v);
        memcpy(out, &f, 4);                  // Xenon is big endian already
        return 4;
    }
    if (!strcmp(type, "f64")) {
        double d = atof(v);
        memcpy(out, &d, 8);
        return 8;
    }
    static char str[1024];
    int len = Unquote(v, str, sizeof(str));
    if (len < 0) return -1;

    if (!strcmp(type, "string")) {
        if (len > max) return -1;
        memcpy(out, str, len);
        return len;
    }
    if (!strcmp(type, "u16string")) {
        if (len * 2 > max) return -1;
        for (int i = 0; i < len; i++) { out[i * 2] = 0; out[i * 2 + 1] = (BYTE)str[i]; }
        return len * 2;
    }
    if (!strcmp(type, "array")) {
        const char* s = str;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        int n = 0, hi = -1;
        for (; *s; s++) {
            int h = HexVal(*s);
            if (h < 0) continue;              // skip spaces, separators
            if (hi < 0) hi = h;
            else {
                if (n >= max) return -1;
                out[n++] = (BYTE)((hi << 4) | h);
                hi = -1;
            }
        }
        return (hi < 0) ? n : -1;
    }
    return -1;
}

// media_id = "XXXXXXXX" or ["XXXXXXXX", ...]; TRUE if it lists 'media'
static BOOL MediaListed(const char* v, DWORD media, BOOL* anyListed)
{
    *anyListed = FALSE;
    const char* s = v;
    while (*s) {
        if (*s == '"') {
            DWORD m = 0; int digits = 0;
            s++;
            while (*s && *s != '"') {
                int h = HexVal(*s++);
                if (h >= 0) { m = (m << 4) | h; digits++; }
            }
            if (*s) s++;
            if (digits) {
                *anyListed = TRUE;
                if (m == media) return TRUE;
            }
        } else s++;
    }
    return FALSE;
}

// --------------------------------------------------------------------------
// Apply one TOML file
// --------------------------------------------------------------------------
typedef struct {
    char  Type[16];
    char  Value[1100];
    DWORD Address;
    BOOL  HasAddress, HasValue, Active;
    int   Line;                 // line of its [[patch.TYPE]], for error messages
} XP_WRITE;

static BYTE g_Data[2048];

// ---- TOML errors: logged with file name and line number (always, also
// with log = 0), at most 5 per file so a broken file can't flood the log ----
static const char* g_TomlFile = "";
static int         g_TomlErrors = 0;
static DWORD       g_TomlTitle = 0;

static void TomlError(int line, const char* fmt, ...)
{
    if (++g_TomlErrors > 5) {
        if (g_TomlErrors == 6) LogErr("[%08X] TOML: more errors in %s not shown", g_TomlTitle, g_TomlFile);
        return;
    }
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = 0;
    const char* name = strrchr(g_TomlFile, '\\');
    LogErr("[%08X] TOML error, %s line %d: %s", g_TomlTitle, name ? name + 1 : g_TomlFile, line, msg);
}

static BOOL KnownType(const char* t)
{
    static const char* types[] = { "be8", "be16", "be32", "be64", "f32", "f64", "string", "u16string", "array" };
    for (int i = 0; i < 9; i++) if (!strcmp(t, types[i])) return TRUE;
    return FALSE;
}

static void FlushWrite(XP_WRITE* w, DWORD titleId, const char* patchName, BOOL enabled,
                       int* writes, int* errors)
{
    if (!w->Active) return;
    w->Active = FALSE;
    if (!enabled) return;
    if (!KnownType(w->Type)) { (*errors)++; return; }          // reported at its section line
    if (!w->HasAddress || !w->HasValue) {
        TomlError(w->Line, "'%s': [[patch.%s]] without %s", patchName, w->Type,
                  !w->HasAddress ? "address" : "value");
        (*errors)++;
        return;
    }

    int len = EncodeValue(w->Type, w->Value, g_Data, sizeof(g_Data));
    if (len <= 0) {
        TomlError(w->Line, "'%s': bad %s value at %08X", patchName, w->Type, w->Address);
        (*errors)++;
        return;
    }
    BYTE* dst = (BYTE*)w->Address;
    if (!MmIsAddressValid(dst) || !MmIsAddressValid(dst + len - 1)) {
        LogErr("[%08X] '%s': invalid address %08X (line %d)", titleId, patchName, w->Address, w->Line);
        (*errors)++;
        return;
    }
    UndoSave(dst, len);                 // original bytes, for the cache check / undo
    memcpy(dst, g_Data, len);
    FlushRange(dst, len);
    TrackWrite(titleId, patchName, w->Address, len);
    (*writes)++;
}

// Totals for one game start (all of its .toml files), used for the notification
typedef struct {
    int PatchesOk;      // enabled patches whose every write succeeded
    int PatchesFailed;  // enabled patches with at least one failed write
    int PatchesSkipped; // enabled patches in files made for another game version
} XP_RESULT;

// called when an enabled [[patch]] block ends
static void EndPatch(DWORD titleId, const char* name, int errorsAtStart, int errors, XP_RESULT* r)
{
    if (errors == errorsAtStart) {
        Log("[%08X] ON  '%s'", titleId, name);
        r->PatchesOk++;
    } else {
        LogErr("[%08X] ON  '%s' - FAILED (%d errors)", titleId, name, errors - errorsAtStart);
        r->PatchesFailed++;
    }
}

// "(TU27)" in a file name -> 27; no TU tag -> 0 (base game). Case-insensitive.
static int TuFromFileName(const char* name)
{
    for (const char* s = name; *s; s++) {
        if (s[0] == '(' && (s[1] == 'T' || s[1] == 't') && (s[2] == 'U' || s[2] == 'u')
            && s[3] >= '0' && s[3] <= '9') {
            int n = atoi(s + 3);
            const char* e = s + 3;
            while (*e >= '0' && *e <= '9') e++;
            if (*e == ')') return n;
        }
    }
    return 0;
}

// The TU number is the "build" field of the version: 0x00001B09 -> 0x1B = TU27,
// 0x00000805 -> TU8, 0x00000005 -> 0 (no TU). Checked on GTA IV, GTA V, Gears of War.
static int TuFromVersion(DWORD version)
{
    return (int)((version >> 8) & 0xFFFF);
}

// every "XXXXXXXXXXXXXXXX" (16 hex digits) in a line -> list
static void CollectHashes(const char* s, UINT64* list, int* n, int max)
{
    while (*s) {
        if (*s == '"') {
            const char* q = s + 1;
            const char* e = strchr(q, '"');
            if (!e) return;
            char tmp[32];
            int len = (int)(e - q);
            if (len < (int)sizeof(tmp)) {
                memcpy(tmp, q, len); tmp[len] = 0;
                UINT64 v;
                if (ParseHash64(tmp, &v) && *n < max) list[(*n)++] = v;
            }
            s = e + 1;
        } else s++;
    }
}

// Which files are used:
//   - module hash available: only files whose "hash" lists the running game's hash
//   - otherwise (fallback, as in v1.0): the "(TUn)" tag in the file name must
//     match the running TU, and a media_id list (if any) must contain the disc
static void ApplyToml(const char* path, const XP_EXEC_ID* exec, XP_RESULT* r, BOOL tuMismatch,
                      const XP_HASH* mh)
{
    FILE* f = fopen(path, "rb");
    if (!f) return;
    DWORD size = (DWORD)fread(g_FileBuf, 1, XP_MAX_FILE, f);
    BOOL tooBig = (size == XP_MAX_FILE && fgetc(f) != EOF);
    fclose(f);
    g_FileBuf[size] = 0;
    if (tooBig) {                   // never cut a file silently
        LogErr("[%08X] %s is bigger than %u KB - not applied", exec->TitleId, path, XP_MAX_FILE / 1024);
        return;
    }

    char patchName[96] = "";
    BOOL inPatch = FALSE, enabled = FALSE, topLevel = TRUE;
    int  writes = 0, errors = 0, patchesOn = 0, errorsAtStart = 0;
    BOOL skipFile = FALSE;          // enabled patches are only counted, not applied
    BOOL decided = FALSE, mediaMismatch = FALSE;
    int  skippedBefore = r->PatchesSkipped;
    XP_WRITE w; memset(&w, 0, sizeof(w));

    UINT64 fileHashes[16]; int fileHashCount = 0;
    BOOL   inHashList = FALSE;
    BOOL   inList = FALSE;          // other multi-line  key = [ ... ]
    BOOL   inLongString = FALSE;    // key = """ ... """
    int    lineNo = 0;
    g_TomlFile = path; g_TomlErrors = 0; g_TomlTitle = exec->TitleId;

    char* line = g_FileBuf;
    while (*line) {
        char* next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;
        lineNo++;

        if (inLongString) {
            if (strstr(line, "\"\"\"")) inLongString = FALSE;
            line = next;
            continue;
        }
        StripComment(line);
        char* s = Trim(line);

        if (inHashList) {               // continuation of  hash = [ ... ]
            CollectHashes(s, fileHashes, &fileHashCount, 16);
            if (strchr(s, ']')) inHashList = FALSE;
            line = next;
            continue;
        }
        if (inList) {
            if (strchr(s, ']')) inList = FALSE;
            line = next;
            continue;
        }
        if (!*s) { line = next; continue; }

        if (*s == '[') {
            if (!decided) {
                // the top-level keys (hash, media_id) are all read by now
                decided = TRUE;
                if (mh->Ok) {
                    BOOL match = FALSE;
                    for (int k = 0; k < fileHashCount; k++)
                        if (fileHashes[k] == mh->XeniaStyle) match = TRUE;
                    skipFile = !match;
                } else {
                    skipFile = tuMismatch || mediaMismatch;
                }
            }
            FlushWrite(&w, exec->TitleId, patchName, enabled, &writes, &errors);
            topLevel = FALSE;

            if (!strncmp(s, "[[patch]]", 9)) {
                if (inPatch && enabled)
                    EndPatch(exec->TitleId, patchName, errorsAtStart, errors, r);
                inPatch = TRUE; enabled = FALSE; patchName[0] = 0;
                errorsAtStart = errors;
            } else if (!strncmp(s, "[[patch.", 8) && inPatch) {
                char* type = s + 8;
                char* end = strstr(type, "]]");
                if (end) {
                    *end = 0;
                    memset(&w, 0, sizeof(w));
                    strncpy(w.Type, Trim(type), sizeof(w.Type) - 1);
                    w.Active = TRUE;
                    w.Line = lineNo;
                    if (!KnownType(w.Type))
                        TomlError(lineNo, "unknown type [[patch.%s]] (be8/16/32/64, f32, f64, string, u16string, array)", w.Type);
                } else {
                    TomlError(lineNo, "section without closing ]]");
                }
            } else if (!strncmp(s, "[[patch.", 8)) {
                TomlError(lineNo, "%.40s before the first [[patch]]", s);
            } else {
                TomlError(lineNo, "unknown section %.40s", s);
            }
        } else {
            char* eq = strchr(s, '=');
            if (!eq) TomlError(lineNo, "not a 'key = value' line: %.40s", s);
            if (eq) {
                *eq = 0;
                char* key = Trim(s);
                char* val = Trim(eq + 1);
                if (!strncmp(val, "\"\"\"", 3) && !strstr(val + 3, "\"\"\"")) inLongString = TRUE;
                else if (*val == '[' && !strchr(val, ']') && strcmp(key, "hash")) inList = TRUE;

                if (topLevel) {
                    if (!strcmp(key, "hash")) {
                        CollectHashes(val, fileHashes, &fileHashCount, 16);
                        if (strchr(val, '[') && !strchr(val, ']')) inHashList = TRUE;
                    } else if (!strcmp(key, "media_id")) {
                        BOOL any;
                        if (!MediaListed(val, exec->MediaId, &any) && any)
                            mediaMismatch = TRUE;   // used only when the hash is unavailable
                    }
                } else if (w.Active) {
                    if (!strcmp(key, "address")) { w.Address = (DWORD)ParseInt(val); w.HasAddress = TRUE; }
                    else if (!strcmp(key, "value")) {
                        strncpy(w.Value, val, sizeof(w.Value) - 1);
                        w.Value[sizeof(w.Value) - 1] = 0;
                        w.HasValue = TRUE;
                    }
                } else if (inPatch) {
                    if (!strcmp(key, "name")) {
                        if (Unquote(val, patchName, sizeof(patchName)) < 0)
                            strncpy(patchName, val, sizeof(patchName) - 1);
                    } else if (!strcmp(key, "is_enabled")) {
                        if (strncmp(val, "true", 4) && strncmp(val, "false", 5))
                            TomlError(lineNo, "is_enabled must be true or false, not %.20s", val);
                        enabled = !strncmp(val, "true", 4);
                        if (enabled && skipFile) { r->PatchesSkipped++; enabled = FALSE; }
                        if (enabled) patchesOn++;
                    }
                }
            }
        }
        line = next;
    }
    FlushWrite(&w, exec->TitleId, patchName, enabled, &writes, &errors);
    if (inPatch && enabled)
        EndPatch(exec->TitleId, patchName, errorsAtStart, errors, r);

    if (skipFile && r->PatchesSkipped > skippedBefore) {
        if (mh->Ok) {
            char list[80]; int n = 0; list[0] = 0;
            for (int k = 0; k < fileHashCount && n < 60; k++)
                n += _snprintf(list + n, sizeof(list) - n, "%s%08X%08X", k ? "," : "",
                               (DWORD)(fileHashes[k] >> 32), (DWORD)fileHashes[k]);
            list[sizeof(list) - 1] = 0;
            LogErr("[%08X] %d enabled patches SKIPPED - file is for another game version "
                "(file hash %s) (%s)", exec->TitleId, r->PatchesSkipped - skippedBefore,
                fileHashCount ? list : "none", path);
        } else {
            LogErr("[%08X] %d enabled patches SKIPPED - %s does not match (%s)", exec->TitleId,
                r->PatchesSkipped - skippedBefore, tuMismatch ? "TU" : "media_id", path);
        }
    } else {
        Log("[%08X] %d patches on, %d writes, %d errors (%s)",
            exec->TitleId, patchesOn, writes, errors, path);
    }
}

// --------------------------------------------------------------------------
// On-screen notification (the standard Xbox toast)
// XNotifyQueueUICustom = xam.xex ordinal 656, as used by scene plugins
// --------------------------------------------------------------------------
extern "C" {
    DWORD XexGetModuleHandle(PCSTR ModuleName, PHANDLE Handle);
    DWORD XexGetProcedureAddress(HANDLE Module, DWORD Ordinal, PVOID* Address);
}
typedef VOID (*XNOTIFYQUEUEUICUSTOM)(DWORD Type, DWORD UserIndex, ULONGLONG Priority,
                                     LPCWSTR Text, ULONGLONG Param);

static XNOTIFYQUEUEUICUSTOM GetNotifyFn()
{
    static XNOTIFYQUEUEUICUSTOM fn = NULL;
    if (!fn) {
        HANDLE xam = NULL;
        DWORD st = XexGetModuleHandle("xam.xex", &xam);
        if (st != 0 || !xam) { LogErr("Notify: xam.xex handle failed (%08X)", st); return NULL; }
        st = XexGetProcedureAddress(xam, 656, (PVOID*)&fn);
        if (st != 0 || !fn) { LogErr("Notify: ordinal 656 not found (%08X)", st); fn = NULL; return NULL; }
        Log("Notify: XNotifyQueueUICustom at %08X", (DWORD)fn);
    }
    return fn;
}

static void ToWide(const char* text, WCHAR* out, int max)
{
    int i = 0;
    for (; text[i] && i < max - 1; i++) out[i] = (WCHAR)(BYTE)text[i];
    out[i] = 0;
}

// XAM only shows the toast when it is queued from a thread of the running
// title, not from a system thread like ours (tested on the console without
// JRPC2: system thread -> nothing, title thread -> toast). So every
// notification is sent from a short-lived thread created in the title.
static WCHAR g_NotifyText[128];

static void __cdecl NotifyThread(void*)
{
    XNOTIFYQUEUEUICUSTOM fn = GetNotifyFn();
    if (fn) fn(14, 0, 2, g_NotifyText, 0);   // 14 = smiley icon, user 0, priority 2
}

static void Notify(const char* text)
{
    if (!GetNotifyFn()) return;
    ToWide(text, g_NotifyText, 128);
    HANDLE h = NULL; DWORD tid = 0;
    // no 0x2 flag: not a system thread, so it belongs to the running title
    ExCreateThread(&h, 0, &tid, (PVOID)XapiThreadStartup,
                   (LPTHREAD_START_ROUTINE)NotifyThread, NULL, CREATE_SUSPENDED);
    if (!h) { LogErr("Notify: could not create thread"); return; }
    ResumeThread(h);
    CloseHandle(h);
}

// Notifications are queued and shown by the main loop (no Sleep in the
// patching path): one at a time, at least 3 s apart, only while the title
// that queued them is still running.
#define XP_MAX_NOTE 4
typedef struct { DWORD Due; DWORD TitleId; char Text[100]; } XP_NOTE;
static XP_NOTE g_Notes[XP_MAX_NOTE];
static int     g_NoteCount = 0;
static DWORD   g_NextNoteTime = 0;

static void NotifyLater(DWORD titleId, DWORD delayMs, const char* text)
{
    Log("Notification: %s", text);
    if (!g_Cfg.Notify || g_NoteCount >= XP_MAX_NOTE) return;
    XP_NOTE* n = &g_Notes[g_NoteCount++];
    n->Due = GetTickCount() + delayMs;
    n->TitleId = titleId;
    strncpy(n->Text, text, sizeof(n->Text) - 1);
    n->Text[sizeof(n->Text) - 1] = 0;
}

// called from the main loop
static void NotifyTick(DWORD title)
{
    if (g_NoteCount == 0) return;
    DWORD now = GetTickCount();
    if ((LONG)(now - g_Notes[0].Due) < 0 || (LONG)(now - g_NextNoteTime) < 0) return;
    if (g_Notes[0].TitleId == title) {
        Notify(g_Notes[0].Text);
        g_NextNoteTime = now + 3000;
    }
    memmove(&g_Notes[0], &g_Notes[1], (--g_NoteCount) * sizeof(XP_NOTE));
}

// --------------------------------------------------------------------------
// Hash cache: Hdd:\Patches\XPatch360\XPatch360.cache, one line per game version:
//   <TitleId> <Version> <BaseVersion> <MediaId> <hash>
// With a cached hash the patches are written at once (no time lost while the
// game starts, so early patches like "Skip Legal Screens" still work); the
// hash is then checked against the memory as it was before the writes, and
// everything is undone if it does not match.
// --------------------------------------------------------------------------
#define XP_CACHE XP_DATA "XPatch360.cache"
typedef struct { DWORD TitleId, Version, BaseVersion, MediaId; UINT64 Hash; } XP_CACHE_ENTRY;
#define XP_MAX_CACHE 256
static XP_CACHE_ENTRY g_Cache[XP_MAX_CACHE];
static int            g_CacheCount  = 0;
static BOOL           g_CacheLoaded = FALSE;

static void CacheLoad()
{
    g_CacheLoaded = TRUE;
    g_CacheCount  = 0;
    FILE* f = fopen(XP_CACHE, "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f) && g_CacheCount < XP_MAX_CACHE) {
        char* t[5]; int n = 0;
        char* p = line;
        while (*p && n < 5) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '\r' || *p == '\n') break;
            t[n++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            if (*p) *p++ = 0;
        }
        if (n != 5) continue;
        XP_CACHE_ENTRY e;
        e.TitleId     = strtoul(t[0], NULL, 16);
        e.Version     = strtoul(t[1], NULL, 16);
        e.BaseVersion = strtoul(t[2], NULL, 16);
        e.MediaId     = strtoul(t[3], NULL, 16);
        if (!ParseHash64(t[4], &e.Hash)) continue;
        g_Cache[g_CacheCount++] = e;
    }
    fclose(f);
}

static int CacheIndex(const XP_EXEC_ID* x)
{
    if (!g_CacheLoaded) CacheLoad();
    for (int i = 0; i < g_CacheCount; i++)
        if (g_Cache[i].TitleId == x->TitleId && g_Cache[i].Version == x->Version &&
            g_Cache[i].BaseVersion == x->BaseVersion && g_Cache[i].MediaId == x->MediaId)
            return i;
    return -1;
}

static void CacheStore(const XP_EXEC_ID* x, UINT64 hash)
{
    int i = CacheIndex(x);
    if (i < 0) {
        if (g_CacheCount >= XP_MAX_CACHE) return;
        i = g_CacheCount++;
        g_Cache[i].TitleId = x->TitleId;   g_Cache[i].Version = x->Version;
        g_Cache[i].BaseVersion = x->BaseVersion; g_Cache[i].MediaId = x->MediaId;
    }
    g_Cache[i].Hash = hash;
    FILE* f = fopen(XP_CACHE, "wb");
    if (!f) return;
    for (int k = 0; k < g_CacheCount; k++)
        fprintf(f, "%08X %08X %08X %08X %08X%08X\r\n", g_Cache[k].TitleId, g_Cache[k].Version,
                g_Cache[k].BaseVersion, g_Cache[k].MediaId,
                (DWORD)(g_Cache[k].Hash >> 32), (DWORD)g_Cache[k].Hash);
    fclose(f);
}

// one pass over the title's patch files
static void ApplyAllFiles(const XP_EXEC_ID* exec, const XP_HASH* mh, XP_RESULT* r)
{
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), XP_DIR "%08X*.toml", exec->TitleId);
    int runTu = TuFromVersion(exec->Version);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), XP_DIR "%s", fd.cFileName);
        path[sizeof(path) - 1] = 0;
        // fallback rule: "(TU27)" files only on TU27, untagged files only on the base game
        BOOL tuMismatch = (TuFromFileName(fd.cFileName) != runTu);
        ApplyToml(path, exec, r, tuMismatch, mh);
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

static void UndoAll()
{
    for (int u = g_UndoCount - 1; u >= 0; u--) {
        BYTE* dst = (BYTE*)g_Undo[u].Addr;
        memcpy(dst, g_UndoData + g_Undo[u].Off, g_Undo[u].Len);
        FlushRange(dst, g_Undo[u].Len);
    }
    g_UndoCount = 0; g_UndoUsed = 0;
}

static void ApplyPatches(const XP_EXEC_ID* exec)
{
    // any patch files for this title at all?
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), XP_DIR "%08X*.toml", exec->TitleId);
    WIN32_FIND_DATA fd;
    HANDLE fh = FindFirstFile(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;      // no patches for this title
    FindClose(fh);

    g_LogDefer = TRUE;                           // no disk writes until patched
    Log("[%08X] running TU%d", exec->TitleId, TuFromVersion(exec->Version));

    static XP_HASH mh;
    XP_RESULT r; memset(&r, 0, sizeof(r));
    memset(&mh, 0, sizeof(mh));
    UndoReset();

    int ci = g_HashWorks ? CacheIndex(exec) : -1;
    if (ci >= 0) {
        // fast path: cached hash -> patch now, check afterwards
        mh.Ok = TRUE;
        mh.XeniaStyle = g_Cache[ci].Hash;
        ApplyAllFiles(exec, &mh, &r);

        XP_HASH real;
        if (g_UndoFull) {
            Log("[%08X] module hash %08X%08X (cached, not re-checked: too many writes)",
                exec->TitleId, (DWORD)(mh.XeniaStyle >> 32), (DWORD)mh.XeniaStyle);
        } else {
            ComputeModuleHash(&real, TRUE);      // memory as it was before our writes
            if (real.Ok && real.XeniaStyle == mh.XeniaStyle) {
                Log("[%08X] module hash %08X%08X (cached, checked)", exec->TitleId,
                    (DWORD)(mh.XeniaStyle >> 32), (DWORD)mh.XeniaStyle);
            } else if (real.Ok) {
                // the cache was wrong: undo everything and decide again
                LogErr("[%08X] cached hash %08X%08X is outdated, real %08X%08X - %d writes undone",
                    exec->TitleId, (DWORD)(mh.XeniaStyle >> 32), (DWORD)mh.XeniaStyle,
                    (DWORD)(real.XeniaStyle >> 32), (DWORD)real.XeniaStyle, g_UndoCount);
                UndoAll();
                CacheStore(exec, real.XeniaStyle);
                memset(&r, 0, sizeof(r));
                mh = real;
                UndoReset();
                ApplyAllFiles(exec, &mh, &r);
            } else {
                Log("[%08X] module hash %08X%08X (cached, could not re-check)", exec->TitleId,
                    (DWORD)(mh.XeniaStyle >> 32), (DWORD)mh.XeniaStyle);
            }
        }
    } else {
        // first run of this game version: hash first, then patch
        if (g_HashWorks) ComputeModuleHash(&mh, FALSE);
        if (mh.Ok) {
            Log("[%08X] module hash %08X%08X (computed, cached for next time)", exec->TitleId,
                (DWORD)(mh.XeniaStyle >> 32), (DWORD)mh.XeniaStyle);
        } else {
            LogErr("[%08X] module hash not available - using the TU check of v1.0", exec->TitleId);
        }
        ApplyAllFiles(exec, &mh, &r);
        if (mh.Ok) {
            g_LogDefer = FALSE;                  // CacheStore writes to disk anyway
            CacheStore(exec, mh.XeniaStyle);
        }
    }
    LogFlush();

    // Nothing enabled for this game -> stay quiet
    if (r.PatchesOk + r.PatchesFailed + r.PatchesSkipped == 0) return;

    char msg[128];
    if (r.PatchesFailed == 0 && r.PatchesOk > 0 && r.PatchesSkipped == 0)
        _snprintf(msg, sizeof(msg), "XPatch360: %d patch%s applied successfully",
                  r.PatchesOk, r.PatchesOk == 1 ? "" : "es");
    else if (r.PatchesOk > 0 && r.PatchesFailed == 0)
        _snprintf(msg, sizeof(msg), "XPatch360: %d applied, %d skipped (other game version)",
                  r.PatchesOk, r.PatchesSkipped);
    else if (r.PatchesOk > 0)
        _snprintf(msg, sizeof(msg), "XPatch360: %d applied, %d failed", r.PatchesOk, r.PatchesFailed);
    else if (r.PatchesFailed > 0)
        _snprintf(msg, sizeof(msg), "XPatch360: failed to apply %d patch%s",
                  r.PatchesFailed, r.PatchesFailed == 1 ? "" : "es");
    else
        _snprintf(msg, sizeof(msg), "XPatch360: %d patch%s skipped - game version doesn't match",
                  r.PatchesSkipped, r.PatchesSkipped == 1 ? "" : "es");
    msg[sizeof(msg) - 1] = 0;
    if (g_Conflicts) {
        int n = (int)strlen(msg);
        _snprintf(msg + n, sizeof(msg) - n, " - %d conflict%s, see log", g_Conflicts, g_Conflicts == 1 ? "" : "s");
        msg[sizeof(msg) - 1] = 0;
    }

    // Patching is done already; show it a moment later so the toast isn't
    // lost while the game is still bringing up its first screen.
    NotifyLater(exec->TitleId, 3000, msg);
}

// --------------------------------------------------------------------------
// Debug mode (debug = 1 in the ini): memory dumps
// Hdd:\Patches\XPatch360\XPatch360.dump, one line per dump:
//   TITLEID ADDRESS LENGTH [DELAY]   range to the log (LENGTH up to 1000 hex) or to
//                                    Hdd:\Patches\XPatch360\<TITLEID>_<ADDRESS>.bin (bigger);
//                                    ADDRESS can be *PTR+OFF (pointer read at dump time)
//   TITLEID code [DELAY]             the game's whole image -> <TITLEID>_TU<n>_code.bin
//   next code [DELAY]                same, for the next game that starts
// DELAY = seconds after the game started (live bytes); no delay = right after
// patching, with XPatch360's own writes shown as the original bytes.
// "code" lines are done once and then removed from the file (the Aurora
// script writes them); address lines stay until removed by hand.
// Delayed dumps are run by the main loop, nothing waits.
// --------------------------------------------------------------------------
#define XP_DUMP      XP_DATA "XPatch360.dump"
#define XP_DUMP_MAX  0x1000
#define XP_MAX_JOBS  32

typedef struct {
    DWORD TitleId, Due, Delay;
    BOOL  Indirect, Code, OneShot;
    DWORD Addr, Off, Len;
    char  Line[96];          // the line as written, to remove a one-shot line
} XP_JOB;
static XP_JOB g_Jobs[XP_MAX_JOBS];
static int    g_JobCount = 0;
static int    g_JobLogDumps = 0;   // log dumps done for this title (one notification at the end)
static DWORD  g_JobVersion = 0;

// dashboards and system apps are not "the next game"
static BOOL IsGameTitle(DWORD t)
{
    DWORD hi = t >> 16;
    return t != 0 && hi != 0xFFFE && hi != 0xFFFF && hi != 0xF5D1;
}

// base and size of the running title's image (from its XEX header)
static BOOL GetTitleImage(DWORD* base, DWORD* size)
{
    HANDLE krnl = NULL; PVOID var = NULL;
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) != 0 || !krnl) return FALSE;
    if (XexGetProcedureAddress(krnl, 0x193, &var) != 0 || !var || !Readable(var, 4)) return FALSE;
    BYTE* ldr = *(BYTE**)var;
    if (!ldr || !Readable(ldr, 0x5C)) return FALSE;
    BYTE* xex = *(BYTE**)(ldr + 0x58);
    if (!xex || !Readable(xex, 0x18) || *(DWORD*)xex != 0x58455832) return FALSE;
    DWORD secFld = *(DWORD*)(xex + 0x10);
    DWORD secOff = (secFld >= (DWORD)xex) ? secFld - (DWORD)xex : secFld;
    if (secOff < 0x18 || secOff > 0x100000 || !Readable(xex + secOff, 0x114)) return FALSE;
    *base = *(DWORD*)(ldr + 0x1C);
    *size = *(DWORD*)(xex + secOff + 0x04);          // security info: image size
    return *size != 0 && *size <= 0x4000000;
}


// drop the first line equal to `text` from the dump file
static void RemoveDumpLine(const char* text)
{
    FILE* f = fopen(XP_DUMP, "rb");
    if (!f) return;
    DWORD n = (DWORD)fread(g_FileBuf, 1, XP_MAX_FILE, f);
    fclose(f);
    g_FileBuf[n] = 0;
    f = fopen(XP_DUMP, "wb");
    if (!f) return;
    BOOL done = FALSE;
    char* p = g_FileBuf;
    while (*p) {
        char* e = p; while (*e && *e != '\n') e++;
        char* next = *e ? e + 1 : e;
        char save = *e; *e = 0;
        char line[96]; strncpy(line, p, sizeof(line) - 1); line[sizeof(line) - 1] = 0;
        *e = save;
        int L = (int)strlen(line); while (L && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
        if (!done && !strcmp(line, text)) done = TRUE;
        else fwrite(p, 1, next - p, f);
        p = next;
    }
    fclose(f);
}

// .bin dumps are written by a separate low-priority thread, so the main loop
// (patching, notifications, the next title start) never waits for the hard
// disk. One file at a time; the main loop logs and notifies when it is done.
typedef struct { DWORD TitleId, Addr, Len; BOOL OneShot; char Name[48]; char Line[96]; } XP_FILEDUMP;
#define XP_MAX_FILEDUMPS 4
static XP_FILEDUMP   g_FdQueue[XP_MAX_FILEDUMPS];
static int           g_FdQueued = 0;
static XP_FILEDUMP   g_Fd;                       // the one being written
static volatile LONG g_FdState = 0;              // 0 idle, 1 writing, 2 finished
static DWORD         g_FdReadable = 0;
static BOOL          g_FdOk = FALSE;
// our own writes at the time the dump started: shown as the original bytes
static XP_UNDO       g_FdUndo[XP_MAX_UNDO];
static BYTE          g_FdUndoData[sizeof(g_UndoData)];
static int           g_FdUndoCount = 0;

static void __cdecl FileDumpThread(void*)
{
    char path[80];
    _snprintf(path, sizeof(path), XP_DATA "%s", g_Fd.Name);
    path[sizeof(path) - 1] = 0;
    g_FdReadable = 0;
    g_FdOk = FALSE;
    FILE* o = fopen(path, "wb");
    if (o) {
        static BYTE page[0x1000];
        DWORD addr = g_Fd.Addr, len = g_Fd.Len;
        for (DWORD off = 0; off < len; off += 0x1000) {
            DWORD a = addr + off, n = (len - off < 0x1000) ? len - off : 0x1000;
            if (MmIsAddressValid((PVOID)a)) {
                memcpy(page, (void*)a, n);
                for (int u = 0; u < g_FdUndoCount; u++)
                    for (DWORD k = 0; k < g_FdUndo[u].Len; k++) {
                        DWORD b = g_FdUndo[u].Addr + k;
                        if (b >= a && b < a + n) page[b - a] = g_FdUndoData[g_FdUndo[u].Off + k];
                    }
                g_FdReadable += n;
            } else {
                memset(page, 0, n);
            }
            fwrite(page, 1, n, o);
        }
        fclose(o);
        g_FdOk = TRUE;
    }
    InterlockedExchange(&g_FdState, 2);
}

static void QueueFileDump(DWORD titleId, const char* name, DWORD addr, DWORD len,
                          BOOL oneShot, const char* line)
{
    if (g_FdQueued >= XP_MAX_FILEDUMPS) { LogErr("[%08X] DUMP %s: too many dumps queued", titleId, name); return; }
    XP_FILEDUMP* d = &g_FdQueue[g_FdQueued++];
    memset(d, 0, sizeof(*d));
    d->TitleId = titleId; d->Addr = addr; d->Len = (len > 0x4000000) ? 0x4000000 : len;
    d->OneShot = oneShot;
    strncpy(d->Name, name, sizeof(d->Name) - 1);
    if (line) strncpy(d->Line, line, sizeof(d->Line) - 1);
}

// main loop: report a finished dump, start the next one
static void FileDumpTick()
{
    if (g_FdState == 2) {
        if (g_FdOk) {
            Log("[%08X] DUMP %08X+%X -> " XP_DATA "%s (%u KB readable, original bytes)",
                g_Fd.TitleId, g_Fd.Addr, g_Fd.Len, g_Fd.Name, g_FdReadable / 1024);
            if (g_Fd.OneShot) RemoveDumpLine(g_Fd.Line);
            char msg[64];
            _snprintf(msg, sizeof(msg), "XPatch360: dump done (%u MB)", (g_Fd.Len + 0x80000) >> 20);
            msg[sizeof(msg) - 1] = 0;
            NotifyLater(g_Fd.TitleId, 0, msg);
        } else {
            LogErr("[%08X] DUMP: cannot create " XP_DATA "%s", g_Fd.TitleId, g_Fd.Name);
        }
        g_FdState = 0;
    }
    if (g_FdState != 0 || g_FdQueued == 0) return;

    g_Fd = g_FdQueue[0];
    memmove(&g_FdQueue[0], &g_FdQueue[1], (--g_FdQueued) * sizeof(XP_FILEDUMP));
    // snapshot of our writes (only if they belong to this title)
    g_FdUndoCount = 0;
    if (XamGetCurrentTitleId() == g_Fd.TitleId) {
        memcpy(g_FdUndo, g_Undo, g_UndoCount * sizeof(XP_UNDO));
        memcpy(g_FdUndoData, g_UndoData, g_UndoUsed);
        g_FdUndoCount = g_UndoCount;
    }
    g_FdState = 1;
    HANDLE h = NULL; DWORD tid = 0;
    ExCreateThread(&h, 0x4000, &tid, (PVOID)XapiThreadStartup,
                   (LPTHREAD_START_ROUTINE)FileDumpThread, NULL, 0x2 | CREATE_SUSPENDED);
    if (!h) { LogErr("[%08X] DUMP: could not start the dump thread", g_Fd.TitleId); g_FdState = 0; return; }
    SetThreadPriority(h, THREAD_PRIORITY_LOWEST);   // the game comes first
    ResumeThread(h);
    CloseHandle(h);
}

static void RunJob(XP_JOB* j)
{
    static BYTE buf[XP_DUMP_MAX];
    static BYTE changed[XP_DUMP_MAX];
    BOOL live = (j->Delay != 0);

    if (j->Code) {
        DWORD base, size;
        if (!GetTitleImage(&base, &size)) { LogErr("[%08X] DUMP code: image not found", j->TitleId); return; }
        char name[48];
        _snprintf(name, sizeof(name), "%08X_TU%d_code.bin", j->TitleId, TuFromVersion(g_JobVersion));
        name[sizeof(name) - 1] = 0;
        QueueFileDump(j->TitleId, name, base, size, j->OneShot, j->Line);
        return;
    }

    DWORD addr = j->Addr;
    if (j->Indirect) {
        if (!Readable((void*)addr, 4)) { LogErr("[%08X] DUMP *%08X: pointer not readable", j->TitleId, addr); return; }
        addr = *(DWORD*)addr + j->Off;
    } else {
        addr += j->Off;
    }
    if (j->Len > XP_DUMP_MAX) {
        char name[48];
        _snprintf(name, sizeof(name), "%08X_%08X.bin", j->TitleId, addr & ~0xFFFu);
        name[sizeof(name) - 1] = 0;
        QueueFileDump(j->TitleId, name, addr & ~0xFFFu, j->Len, FALSE, NULL);
        return;
    }
    addr &= ~3u;
    DWORD len = j->Len;
    g_LogDefer = TRUE;
    if (!Readable((void*)addr, len)) {
        LogErr("[%08X] DUMP %08X+%X: not readable", j->TitleId, addr, len);
        LogFlush();
        return;
    }
    memcpy(buf, (void*)addr, len);
    memset(changed, 0, len);
    if (!live) {
        // put back the original bytes of our own writes
        for (int u = 0; u < g_UndoCount; u++)
            for (DWORD k = 0; k < g_Undo[u].Len; k++) {
                DWORD a = g_Undo[u].Addr + k;
                if (a >= addr && a < addr + len) {
                    buf[a - addr] = g_UndoData[g_Undo[u].Off + k];
                    changed[a - addr] = 1;
                }
            }
        Log("[%08X] DUMP %08X+%X (original bytes; * = line XPatch360 wrote to)", j->TitleId, addr, len);
    } else if (j->Indirect) {
        Log("[%08X] DUMP *%08X+%X = %08X+%X at %us (live bytes)", j->TitleId, j->Addr, j->Off, addr, len, j->Delay);
    } else {
        Log("[%08X] DUMP %08X+%X at %us (live bytes)", j->TitleId, addr, len, j->Delay);
    }
    for (DWORD o = 0; o < len; o += 16) {
        char hex[64]; int n = 0; BOOL mark = FALSE;
        for (DWORD k = 0; k < 16 && o + k < len; k++) {
            n += _snprintf(hex + n, sizeof(hex) - n, "%02X", buf[o + k]);
            if ((k & 3) == 3) hex[n++] = ' ';
            if (changed[o + k]) mark = TRUE;
            hex[n] = 0;
        }
        if (n > 0 && hex[n - 1] == ' ') hex[--n] = 0;
        Log("%c %08X: %s", mark ? '*' : ' ', addr + o, hex);
    }
    LogFlush();
    g_JobLogDumps++;
}

// at a title start (debug mode only): read the dump file, run the immediate
// dumps now and keep the delayed ones for DumpTick
static void DumpStart(const XP_EXEC_ID* exec)
{
    g_JobCount = 0;
    g_JobLogDumps = 0;
    g_JobVersion = exec->Version;
    if (!g_Cfg.Debug) return;
    FILE* f = fopen(XP_DUMP, "rb");
    if (!f) return;
    DWORD now = GetTickCount(), firstDelay = 0;
    char line[160];
    while (fgets(line, sizeof(line), f) && g_JobCount < XP_MAX_JOBS) {
        int L = (int)strlen(line);
        while (L && (line[L - 1] == '\r' || line[L - 1] == '\n' || line[L - 1] == ' ')) line[--L] = 0;
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || !*s) continue;

        XP_JOB j; memset(&j, 0, sizeof(j));
        strncpy(j.Line, line, sizeof(j.Line) - 1);
        char* e;
        if (!_strnicmp(s, "next", 4)) {
            if (!IsGameTitle(exec->TitleId)) continue;
            s += 4;
        } else {
            DWORD tid = strtoul(s, &e, 16);
            if (e == s || tid != exec->TitleId) continue;
            s = e;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (!_strnicmp(s, "code", 4)) {
            j.Code = TRUE; j.OneShot = TRUE; s += 4;
        } else {
            j.Indirect = (*s == '*');
            if (j.Indirect) s++;
            j.Addr = strtoul(s, &e, 16); s = e;
            if (*s == '+') { j.Off = strtoul(s + 1, &e, 16); s = e; }
            j.Len = strtoul(s, &e, 16); s = e;
            if (j.Len == 0) continue;
        }
        j.Delay   = strtoul(s, &e, 10);
        j.TitleId = exec->TitleId;
        j.Due     = now + j.Delay * 1000;
        if (j.Delay && (!firstDelay || j.Delay < firstDelay)) firstDelay = j.Delay;
        g_Jobs[g_JobCount++] = j;
    }
    fclose(f);

    // the immediate ones now (in file order), delayed ones stay queued
    int keep = 0;
    for (int i = 0; i < g_JobCount; i++) {
        if (g_Jobs[i].Delay == 0) RunJob(&g_Jobs[i]);
        else g_Jobs[keep++] = g_Jobs[i];
    }
    g_JobCount = keep;
    if (firstDelay) {
        char msg[64];
        _snprintf(msg, sizeof(msg), "XPatch360: dump in %u s", firstDelay);
        msg[sizeof(msg) - 1] = 0;
        NotifyLater(exec->TitleId, 0, msg);
    } else if (g_JobLogDumps) {
        NotifyLater(exec->TitleId, 0, "XPatch360: dump written to the log");
    }
}

// called from the main loop: run the delayed dumps that are due
static void DumpTick(DWORD title)
{
    if (g_JobCount == 0) return;
    DWORD now = GetTickCount();
    int keep = 0;
    for (int i = 0; i < g_JobCount; i++) {
        XP_JOB* j = &g_Jobs[i];
        if (j->TitleId != title) continue;                 // game left: drop it
        if ((LONG)(now - j->Due) >= 0) RunJob(j);
        else g_Jobs[keep++] = *j;
    }
    g_JobCount = keep;
    if (g_JobCount == 0 && g_JobLogDumps) {
        NotifyLater(title, 0, "XPatch360: dump written to the log");
        g_JobLogDumps = 0;
    }
}


// --------------------------------------------------------------------------
// Early patching (early_patch = 1): hook on xboxkrnl #416 XexStartExecutable.
// It runs after the title's XEX, its title update and its imports are loaded and
// BEFORE the title's main thread is created. The hook itself does almost nothing
// (the old title / Aurora is still alive, the loader lock is held): it points the
// title's entry (ldr+0x3C) at EpEntry. The thread starts in xam's title setup, then
// calls EpEntry, which waits (max 5 s) until the main loop has patched - the title's
// thread has no Hdd: of its own, so the main loop does the file work as always.
// Init-time settings (resolution etc.) can be patched this way. If anything goes
// wrong the main loop simply patches the normal way.
// --------------------------------------------------------------------------
static DWORD          g_EpTarget = 0;           // XexStartExecutable
static volatile DWORD g_EpRedirected = 0;       // the hook redirected this title's entry
static volatile DWORD g_EpTick = 0;             // ...at this time
static XP_EXEC_ID     g_EpExec;                 // copy of the title's execution info

// the 4 instructions we replace (kernel 17559): mflr r12 / bl __savegprlr_28 /
// stwu r1,-128(r1) / mr r28,r3
static const DWORD g_EpExpect[4] = { 0x7D8802A6, 0x48000001, 0x9421FF80, 0x7C7C1B78 };

typedef DWORD (*XP_START_FN)(DWORD);

// the replaced instructions + a jump back, written at install time
static void __declspec(naked) EpTrampoline()
{
    __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
    __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
    __emit(0x60000000); __emit(0x60000000); __emit(0x60000000); __emit(0x60000000);
}

static volatile DWORD g_EpOrigEntry = 0;        // the title's real entry point
static volatile DWORD g_EpAtEntry = 0;          // this title waits at its entry
static volatile DWORD g_EpRelease = 0;          // main loop: this title is patched, go
static volatile DWORD g_EpEntryTick = 0;

typedef DWORD (*XP_ENTRY_FN)(DWORD, DWORD, DWORD, DWORD);

// on the title's main thread, before the title's first instruction
static DWORD EpEntry(DWORD a, DWORD b, DWORD c, DWORD d)
{
    DWORD orig = g_EpOrigEntry;
    DWORD t = g_EpExec.TitleId;
    g_EpEntryTick = GetTickCount();
    g_EpAtEntry = t;
    while (g_EpRelease != t && GetTickCount() - g_EpEntryTick < 5000)
        Sleep(1);
    return ((XP_ENTRY_FN)(void*)orig)(a, b, c, d);
}

// runs on the loader's thread, before the title starts: no file access, no
// heavy work here - only reads a few fields and redirects the entry point.
static void EarlyLook()
{
    if (!g_Cfg.EarlyPatch) return;            // turned off in Aurora: the hook stays, does nothing
    HANDLE krnl = NULL; PVOID var = NULL;
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) != 0 || !krnl) return;
    if (XexGetProcedureAddress(krnl, 0x193, &var) != 0 || !var || !Readable(var, 4)) return;
    BYTE* ldr = *(BYTE**)var;                 // XexExecutableModuleHandle: the new title
    if (!ldr || !Readable(ldr, 0x5C)) return;
    BYTE* xex = *(BYTE**)(ldr + 0x58);
    if (!xex || !Readable(xex, 0x18) || *(DWORD*)xex != 0x58455832) return;   // 'XEX2'
    BYTE* ei = FindOptHeader(xex, 0, 0x00040006);                             // execution info
    if (!ei || !Readable(ei, sizeof(XP_EXEC_ID))) return;
    const XP_EXEC_ID* id = (const XP_EXEC_ID*)ei;
    if (!IsGameTitle(id->TitleId)) return;    // Aurora / dashboard: left alone

    DWORD* pEntry = (DWORD*)(ldr + 0x3C);
    DWORD  e = *pEntry;
    DWORD  mine = (DWORD)(void*)EpEntry;
    if (!e || e == mine) return;              // never twice
    memcpy(&g_EpExec, id, sizeof(g_EpExec));
    g_EpAtEntry = 0; g_EpRelease = 0;         // a new start
    g_EpOrigEntry = e;
    g_EpTick = GetTickCount();
    *pEntry = mine;
    g_EpRedirected = id->TitleId;
}

static DWORD EpHook(DWORD arg)
{
    EarlyLook();
    return ((XP_START_FN)(void*)EpTrampoline)(arg);
}

static void EpWrite(DWORD* at, const DWORD* words, int n)
{
    for (int i = 0; i < n; i++) at[i] = words[i];
    FlushRange((BYTE*)at, n * 4);
}

static BOOL g_EpInstallTried = FALSE;

static void InstallEarlyHook()
{
    if (g_EpInstallTried) return;                  // once per boot (also when turned on later)
    g_EpInstallTried = TRUE;
    HANDLE krnl = NULL; PVOID p = NULL;
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) != 0 || !krnl ||
        XexGetProcedureAddress(krnl, 416, &p) != 0 || !p || !Readable(p, 16)) {
        LogErr("Early patching: XexStartExecutable not found - turned off");
        return;
    }
    DWORD* f = (DWORD*)p;
    BOOL ok = f[0] == g_EpExpect[0] && (f[1] & 0xFC000003) == g_EpExpect[1] &&
              f[2] == g_EpExpect[2] && f[3] == g_EpExpect[3];
    if (!ok) {
        LogErr("Early patching: XexStartExecutable at %08X is not as expected (%08X %08X %08X %08X) - turned off",
               (DWORD)f, f[0], f[1], f[2], f[3]);
        return;
    }
    // where the original "bl __savegprlr_28" goes
    LONG rel = (LONG)(f[1] & 0x03FFFFFC); if (rel & 0x02000000) rel -= 0x04000000;
    DWORD save = (DWORD)f + 4 + rel;
    DWORD back = (DWORD)f + 16;
    DWORD hook = (DWORD)(void*)EpHook;

    DWORD tramp[11] = {
        0x7D8802A6,                                   // mflr r12
        0x3D600000 | (save >> 16), 0x616B0000 | (save & 0xFFFF),
        0x7D6903A6, 0x4E800421,                       // mtctr r11 / bctrl  (__savegprlr_28)
        0x9421FF80,                                   // stwu r1,-128(r1)
        0x7C7C1B78,                                   // mr r28,r3
        0x3D600000 | (back >> 16), 0x616B0000 | (back & 0xFFFF),
        0x7D6903A6, 0x4E800420                        // mtctr r11 / bctr  -> original + 16
    };
    DWORD jump[4] = {
        0x3D600000 | (hook >> 16), 0x616B0000 | (hook & 0xFFFF),
        0x7D6903A6, 0x4E800420                        // -> EpHook
    };
    EpWrite((DWORD*)(void*)EpTrampoline, tramp, 11);
    EpWrite(f, jump, 4);
    g_EpTarget = (DWORD)f;
    Log("Early patching on (hook on XexStartExecutable at %08X)", (DWORD)f);
}

// --------------------------------------------------------------------------
// Main thread: watch for title changes
// --------------------------------------------------------------------------
static void __cdecl MainThread(void*)
{
    if (!WaitForHdd()) return;   // no hard disk -> nothing to patch from
    LoadSettings();
    TrimLog();
    g_LogError = TRUE;           // the start line is always written
    Log("XPatch360 v" XPATCH_VERSION " started%s", g_Cfg.Debug ? " - DEBUG MODE" : "");
    g_LogError = FALSE;
    Log("Settings: notify %d, log %s, log_max_kb %u, early_patch %d, debug %d",
        g_Cfg.Notify, g_Cfg.FullLog ? "full" : "errors only", g_Cfg.LogMaxKb, g_Cfg.EarlyPatch, g_Cfg.Debug);
    g_HashWorks = HashSelfTest();
    CacheLoad();                 // read once now, not while a game is starting
    if (g_Cfg.EarlyPatch) InstallEarlyHook();

    DWORD lastTitle = 0;
    int iniTick = 0;
    FILETIME iniTime = { 0, 0 };
    while (g_Running) {
        DWORD title = XamGetCurrentTitleId();
        if (title != lastTitle) {
            lastTitle = title;
            g_JobCount = 0;              // dumps of the previous game: dropped

            XP_EXEC_ID* exec = NULL;
            if (XamGetExecutionId((PVOID*)&exec) == 0 && exec && exec->TitleId == title) {
                Log("Title %08X started (Version %08X, Base %08X, Media %08X)",
                    exec->TitleId, exec->Version, exec->BaseVersion, exec->MediaId);
                // Early patching: the title id changes while xam is still setting the
                // title up, before the title's entry. If the hook redirected this title,
                // wait until its thread stands at the entry, patch, then let it go.
                BOOL early = FALSE;
                DWORD waited = 0;
                if (g_EpTarget && g_EpRedirected == exec->TitleId) {
                    DWORD t0 = GetTickCount();
                    while (g_EpAtEntry != exec->TitleId && GetTickCount() - t0 < 5000)
                        Sleep(1);
                    waited = GetTickCount() - t0;
                    early = (g_EpAtEntry == exec->TitleId);
                    if (!early)
                        LogErr("[%08X] early patching: the game did not reach its entry in %u ms - patched the normal way",
                            exec->TitleId, waited);
                } else if (g_EpTarget && g_Cfg.EarlyPatch && IsGameTitle(exec->TitleId)) {
                    LogErr("[%08X] early patching: the hook did not see this game - patched the normal way",
                        exec->TitleId);
                }
                UndoReset();             // no stale entries from the last game
                ApplyPatches(exec);      // first: every millisecond counts here
                g_EpRelease = exec->TitleId;                 // the title may run now
                g_EpRedirected = 0;
                if (early) {
                    DWORD held = GetTickCount() - g_EpEntryTick;
                    Log("[%08X] early patching: patched before the game's first instruction (the game waited %u ms)",
                        exec->TitleId, held);
                }
                LoadSettings();          // then re-read the ini (changes made in Aurora)
                if (g_Cfg.EarlyPatch && !g_EpTarget) InstallEarlyHook();   // turned on without a reboot
                DumpStart(exec);
            }
        }
        DumpTick(title);
        FileDumpTick();
        NotifyTick(title);
        // the ini changed (Aurora Settings): read it now, not only at the next game start,
        // so early_patch is right already for the game started next. Only while no game
        // runs (Aurora / dashboard), one check per second; nothing at all during a game.
        if (!IsGameTitle(title) && ++iniTick >= 200) {
            iniTick = 0;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesEx(XP_INI, GetFileExInfoStandard, &fa) &&
                (fa.ftLastWriteTime.dwLowDateTime != iniTime.dwLowDateTime ||
                 fa.ftLastWriteTime.dwHighDateTime != iniTime.dwHighDateTime)) {
                BOOL first = (iniTime.dwLowDateTime == 0 && iniTime.dwHighDateTime == 0);
                iniTime = fa.ftLastWriteTime;
                if (!first) {
                    LoadSettings();
                    if (g_Cfg.EarlyPatch && !g_EpTarget) InstallEarlyHook();
                }
            }
        }
        Sleep(5);   // poll often: the earlier we patch, the fewer init-time misses
    }
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE hThread;
        DWORD tid;
        // 0x2 = system thread, survives title switches
        // 32 KB stack: enough for the hashing code (its state is static)
        ExCreateThread(&hThread, 0x8000, &tid, (PVOID)XapiThreadStartup,
                       (LPTHREAD_START_ROUTINE)MainThread, NULL, 0x2 | CREATE_SUSPENDED);
        XSetThreadProcessor(hThread, 4);
        ResumeThread(hThread);
        CloseHandle(hThread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_Running = FALSE;
    }
    return TRUE;
}
