// XPatch - runtime game patcher Dashlaunch plugin (Xbox 360 RGH/JTAG)
// Authors: bobber360 & Claude
//
// Reads Xenia Canary patch files directly from the console:
//   Hdd:\Patches\<TITLEID> - <Game>.patch.toml
// When a title starts (TU already applied by the loader), it applies every
// [[patch]] with is_enabled = true. Toggle them with the Aurora script
// (Utilities -> XPatch) or by editing the TOML.
//
// Build: XDK, "Xbox 360 DLL" project -> output .xex
// Load:  launch.ini ->  plugin1 = Hdd:\XPatch.xex   (any path works)
// Log:   Hdd:\Patches\XPatch.log
//
// NOTE: not compiled/tested in this form. Kernel/XAM prototypes follow the
// usual scene declarations; if your headers (e.g. xkelib) already declare
// them, delete the duplicates.

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define XP_DIR       "Hdd:\\Patches\\"
#define XP_LOG       XP_DIR "XPatch.log"
#define XP_MAX_FILE  (512 * 1024)

static BOOL g_Running = TRUE;
static char g_FileBuf[XP_MAX_FILE + 1];

// --------------------------------------------------------------------------
// Logging
// --------------------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    strcat(line, "\r\n");

    FILE* f = fopen(XP_LOG, "a");
    if (f) { fputs(line, f); fclose(f); }
}

static void MountHdd()
{
    XP_STRING link, dev;
    // For system threads the "\System??\" namespace is used; if this fails on
    // your setup try "\??\Hdd:".
    RtlInitAnsiString(&link, "\\System??\\Hdd:");
    RtlInitAnsiString(&dev,  "\\Device\\Harddisk0\\Partition1");
    ObCreateSymbolicLink(&link, &dev);
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
} XP_WRITE;

static BYTE g_Data[2048];

static void FlushWrite(XP_WRITE* w, DWORD titleId, const char* patchName, BOOL enabled,
                       int* writes, int* errors)
{
    if (!w->Active) return;
    w->Active = FALSE;
    if (!enabled) return;
    if (!w->HasAddress || !w->HasValue) { (*errors)++; return; }

    int len = EncodeValue(w->Type, w->Value, g_Data, sizeof(g_Data));
    if (len <= 0) {
        Log("[%08X] '%s': bad %s value at %08X", titleId, patchName, w->Type, w->Address);
        (*errors)++;
        return;
    }
    BYTE* dst = (BYTE*)w->Address;
    if (!MmIsAddressValid(dst) || !MmIsAddressValid(dst + len - 1)) {
        Log("[%08X] '%s': invalid address %08X", titleId, patchName, w->Address);
        (*errors)++;
        return;
    }
    memcpy(dst, g_Data, len);
    FlushRange(dst, len);
    (*writes)++;
}

// Totals for one game start (all of its .toml files), used for the notification
typedef struct {
    int PatchesOk;      // enabled patches whose every write succeeded
    int PatchesFailed;  // enabled patches with at least one failed write
    int PatchesSkipped; // enabled patches in files skipped for a media_id mismatch
} XP_RESULT;

// called when an enabled [[patch]] block ends
static void EndPatch(DWORD titleId, const char* name, int errorsAtStart, int errors, XP_RESULT* r)
{
    if (errors == errorsAtStart) {
        Log("[%08X] ON  '%s'", titleId, name);
        r->PatchesOk++;
    } else {
        Log("[%08X] ON  '%s' - FAILED (%d errors)", titleId, name, errors - errorsAtStart);
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

static void ApplyToml(const char* path, const XP_EXEC_ID* exec, XP_RESULT* r, BOOL wrongVersion)
{
    FILE* f = fopen(path, "rb");
    if (!f) return;
    DWORD size = (DWORD)fread(g_FileBuf, 1, XP_MAX_FILE, f);
    fclose(f);
    g_FileBuf[size] = 0;

    char patchName[96] = "";
    BOOL inPatch = FALSE, enabled = FALSE, topLevel = TRUE;
    int  writes = 0, errors = 0, patchesOn = 0, errorsAtStart = 0;
    BOOL skipFile = wrongVersion;   // enabled patches are only counted, not applied
    int  skippedBefore = r->PatchesSkipped;
    XP_WRITE w; memset(&w, 0, sizeof(w));

    char* line = g_FileBuf;
    while (*line) {
        char* next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;

        StripComment(line);
        char* s = Trim(line);

        if (*s == '[') {
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
                }
            }
        } else {
            char* eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char* key = Trim(s);
                char* val = Trim(eq + 1);

                if (topLevel) {
                    if (!strcmp(key, "media_id")) {
                        BOOL any;
                        if (!MediaListed(val, exec->MediaId, &any) && any) {
                            Log("[%08X] %s: media_id does not match %08X - file skipped",
                                exec->TitleId, path, exec->MediaId);
                            skipFile = TRUE;   // keep reading only to count enabled patches
                        }
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

    if (skipFile && r->PatchesSkipped > skippedBefore)
        Log("[%08X] v%08X: %d enabled patches SKIPPED - file is for another version (%s)",
            exec->TitleId, exec->Version, r->PatchesSkipped - skippedBefore, path);
    else
        Log("[%08X] v%08X: %d patches on, %d writes, %d errors (%s)",
            exec->TitleId, exec->Version, patchesOn, writes, errors, path);
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

static void Notify(const char* text)
{
    static XNOTIFYQUEUEUICUSTOM fn = NULL;
    if (!fn) {
        HANDLE xam = NULL;
        if (XexGetModuleHandle("xam.xex", &xam) != 0 || !xam) return;
        if (XexGetProcedureAddress(xam, 656, (PVOID*)&fn) != 0) { fn = NULL; return; }
    }
    static WCHAR wtext[128];
    int i = 0;
    for (; text[i] && i < 127; i++) wtext[i] = (WCHAR)(BYTE)text[i];
    wtext[i] = 0;
    fn(14, 0xFF, 2, wtext, 0);   // 14 = generic/preferred-review icon, 0xFF = all users
}

static void ApplyPatches(const XP_EXEC_ID* exec)
{
    // Xenia naming: "<TITLEID> - <Game>.patch.toml" (more than one file is fine)
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), XP_DIR "%08X*.toml", exec->TitleId);

    XP_RESULT r; memset(&r, 0, sizeof(r));
    int runTu = TuFromVersion(exec->Version);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;      // no patches for this title
    Log("[%08X] running TU%d", exec->TitleId, runTu);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), XP_DIR "%s", fd.cFileName);
        path[sizeof(path) - 1] = 0;
        // "(TU27)" files only on TU27; files without a TU tag only on the base game
        BOOL wrongVersion = (TuFromFileName(fd.cFileName) != runTu);
        ApplyToml(path, exec, &r, wrongVersion);
    } while (FindNextFile(h, &fd));
    FindClose(h);

    // Nothing enabled for this game -> stay quiet
    if (r.PatchesOk + r.PatchesFailed + r.PatchesSkipped == 0) return;

    char msg[128];
    if (r.PatchesFailed == 0 && r.PatchesOk > 0 && r.PatchesSkipped == 0)
        _snprintf(msg, sizeof(msg), "XPatch: %d patch%s applied successfully",
                  r.PatchesOk, r.PatchesOk == 1 ? "" : "es");
    else if (r.PatchesOk > 0 && r.PatchesFailed == 0)
        _snprintf(msg, sizeof(msg), "XPatch: %d applied, %d skipped (other TU)",
                  r.PatchesOk, r.PatchesSkipped);
    else if (r.PatchesOk > 0)
        _snprintf(msg, sizeof(msg), "XPatch: %d applied, %d failed", r.PatchesOk, r.PatchesFailed);
    else if (r.PatchesFailed > 0)
        _snprintf(msg, sizeof(msg), "XPatch: failed to apply %d patch%s",
                  r.PatchesFailed, r.PatchesFailed == 1 ? "" : "es");
    else
        _snprintf(msg, sizeof(msg), "XPatch: %d patch%s skipped (made for another TU)",
                  r.PatchesSkipped, r.PatchesSkipped == 1 ? "" : "es");
    msg[sizeof(msg) - 1] = 0;

    // Patching is done already; wait a moment so the toast isn't lost
    // while the game is still bringing up its first screen.
    Sleep(3000);
    Notify(msg);
    Log("Notification: %s", msg);
}

// --------------------------------------------------------------------------
// Main thread: watch for title changes
// --------------------------------------------------------------------------
static void __cdecl MainThread(void*)
{
    MountHdd();
    CreateDirectory("Hdd:\\Patches", NULL);
    Log("XPatch started");

    DWORD lastTitle = 0;
    while (g_Running) {
        DWORD title = XamGetCurrentTitleId();
        if (title != lastTitle) {
            lastTitle = title;

            XP_EXEC_ID* exec = NULL;
            if (XamGetExecutionId((PVOID*)&exec) == 0 && exec && exec->TitleId == title) {
                Log("Title %08X started (Version %08X, Base %08X, Media %08X)",
                    exec->TitleId, exec->Version, exec->BaseVersion, exec->MediaId);
                ApplyPatches(exec);
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
        ExCreateThread(&hThread, 0, &tid, (PVOID)XapiThreadStartup,
                       (LPTHREAD_START_ROUTINE)MainThread, NULL, 0x2 | CREATE_SUSPENDED);
        XSetThreadProcessor(hThread, 4);
        ResumeThread(hThread);
        CloseHandle(hThread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_Running = FALSE;
    }
    return TRUE;
}
