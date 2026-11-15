// XPatch360 v1.1
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
// Log:   Hdd:\Patches\XPatch360.log
//
// Uses xxHash (BSD 2-Clause, https://github.com/Cyan4973/xxHash) for the
// XXH3-64 module hash that Xenia Canary writes into the "hash" field.

#define XPATCH_VERSION "1.1"

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

#define XP_DIR       "Hdd:\\Patches\\"
#define XP_LOG       XP_DIR "XPatch360.log"
#define XP_MAX_FILE  (512 * 1024)

static BOOL g_Running = TRUE;
static char g_FileBuf[XP_MAX_FILE + 1];

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

static void Log(const char* fmt, ...)
{
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
        if (CreateDirectory("Hdd:\\Patches", NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
            return TRUE;
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

static void UndoReset() { g_UndoCount = 0; g_UndoUsed = 0; g_UndoFull = FALSE; }

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
    Log("Hash self-test: %s (short %08X%08X, long %08X%08X)", ok ? "OK" : "FAILED",
        (DWORD)(ha >> 32), (DWORD)ha, (DWORD)(hb >> 32), (DWORD)hb);
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
    if (!imp || !Readable(imp, 12)) { Log("Hash: no import table"); return; }
    DWORD total = Be32(imp), strSize = Be32(imp + 4), strCount = Be32(imp + 8);
    if (total < 12 || total > 0x100000 || !Readable(imp, total) || strSize >= total) {
        Log("Hash: odd import table (size %X)", total);
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
    if (XexGetModuleHandle("xboxkrnl.exe", &krnl) != 0 || !krnl) { Log("Hash: no xboxkrnl handle"); return; }
    if (XexGetProcedureAddress(krnl, 0x193, &var) != 0 || !var)  { Log("Hash: no XexExecutableModuleHandle"); return; }
    if (!Readable(var, 4)) { Log("Hash: handle variable %08X not readable", (DWORD)var); return; }

    BYTE* ldr = *(BYTE**)var;
    if (!ldr || !Readable(ldr, 0x5C)) { Log("Hash: bad loader entry %08X", (DWORD)ldr); return; }
    DWORD base = *(DWORD*)(ldr + 0x1C);
    BYTE* xex  = *(BYTE**)(ldr + 0x58);
    if (!xex || !Readable(xex, 0x18) || *(DWORD*)xex != 0x58455832) {   // 'XEX2'
        Log("Hash: XEX header not found (%08X)", (DWORD)xex);
        return;
    }

    // In the file this field is an offset from the header start. The console's
    // loader rewrites it into an absolute address (seen: header 8E050000,
    // field 8E050098), Xenia keeps the offset. Accept both, inside the header.
    DWORD hdrSize = *(DWORD*)(xex + 0x08);
    DWORD secFld  = *(DWORD*)(xex + 0x10);
    DWORD secOff  = (secFld >= (DWORD)xex) ? secFld - (DWORD)xex : secFld;
    if (secOff < 0x18 || secOff > 0x100000 || (hdrSize && secOff >= hdrSize)) {
        Log("Hash: odd security field %08X (header %08X, size %X)", secFld, (DWORD)xex, hdrSize);
        return;
    }
    BYTE* sec = xex + secOff;
    if (!Readable(sec, 0x184)) { Log("Hash: security info %08X not readable", (DWORD)sec); return; }
    DWORD count = *(DWORD*)(sec + 0x180);
    if (count == 0 || count > 0x10000) { Log("Hash: odd descriptor count %u", count); return; }
    BYTE* desc = sec + 0x184;                 // 0x18 bytes each: value + digest
    if (!Readable(desc, count * 0x18)) { Log("Hash: descriptors not readable"); return; }

    DWORD first = 0xFFFFFFFF, last = 0xFFFFFFFF;
    for (DWORD i = 0; i < count; i++) {
        DWORD v = *(DWORD*)(desc + i * 0x18);
        if ((v & 0xF) == XEX_SECTION_CODE) {
            if (first == 0xFFFFFFFF) first = i;
            last = i;
        }
    }
    if (first == 0xFFFFFFFF) { Log("Hash: no code section"); return; }

    // Xenia's heap page size for the base address (used with the descriptor index)
    DWORD page = (base >= 0x90000000 && base < 0xA0000000) ? 0x1000 : 0x10000;
    // real page size of the image (used to find which section an address is in)
    DWORD realPage = (*(DWORD*)(sec + 0x10C) & XEX_IMAGE_PAGE_SIZE_4KB) ? 0x1000 : 0x10000;

    h->Start = base + first * page;
    h->End   = base + (last + 1) * page;
    h->PageSize = page; h->Descriptors = count; h->FirstCode = first; h->LastCode = last;
    if (h->End <= h->Start || h->End - h->Start > 0x4000000) { Log("Hash: odd range %08X-%08X", h->Start, h->End); return; }
    if (!Readable((void*)h->Start, h->End - h->Start)) { Log("Hash: range %08X-%08X not readable", h->Start, h->End); return; }

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
    UndoSave(dst, len);                 // original bytes, for the cache check / undo
    memcpy(dst, g_Data, len);
    FlushRange(dst, len);
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
    fclose(f);
    g_FileBuf[size] = 0;

    char patchName[96] = "";
    BOOL inPatch = FALSE, enabled = FALSE, topLevel = TRUE;
    int  writes = 0, errors = 0, patchesOn = 0, errorsAtStart = 0;
    BOOL skipFile = FALSE;          // enabled patches are only counted, not applied
    BOOL decided = FALSE, mediaMismatch = FALSE;
    int  skippedBefore = r->PatchesSkipped;
    XP_WRITE w; memset(&w, 0, sizeof(w));

    UINT64 fileHashes[16]; int fileHashCount = 0;
    BOOL   inHashList = FALSE;

    char* line = g_FileBuf;
    while (*line) {
        char* next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;

        StripComment(line);
        char* s = Trim(line);

        if (inHashList) {               // continuation of  hash = [ ... ]
            CollectHashes(s, fileHashes, &fileHashCount, 16);
            if (strchr(s, ']')) inHashList = FALSE;
            line = next;
            continue;
        }

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
                }
            }
        } else {
            char* eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char* key = Trim(s);
                char* val = Trim(eq + 1);

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
            Log("[%08X] %d enabled patches SKIPPED - file is for another game version "
                "(file hash %s) (%s)", exec->TitleId, r->PatchesSkipped - skippedBefore,
                fileHashCount ? list : "none", path);
        } else {
            Log("[%08X] %d enabled patches SKIPPED - %s does not match (%s)", exec->TitleId,
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
        if (st != 0 || !xam) { Log("Notify: xam.xex handle failed (%08X)", st); return NULL; }
        st = XexGetProcedureAddress(xam, 656, (PVOID*)&fn);
        if (st != 0 || !fn) { Log("Notify: ordinal 656 not found (%08X)", st); fn = NULL; return NULL; }
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
    if (!h) { Log("Notify: could not create thread"); return; }
    ResumeThread(h);
    CloseHandle(h);
}

// --------------------------------------------------------------------------
// Hash cache: Hdd:\Patches\XPatch360.cache, one line per game version:
//   <TitleId> <Version> <BaseVersion> <MediaId> <hash>
// With a cached hash the patches are written at once (no time lost while the
// game starts, so early patches like "Skip Legal Screens" still work); the
// hash is then checked against the memory as it was before the writes, and
// everything is undone if it does not match.
// --------------------------------------------------------------------------
#define XP_CACHE XP_DIR "XPatch360.cache"
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
                Log("[%08X] cached hash %08X%08X is outdated, real %08X%08X - %d writes undone",
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
            Log("[%08X] module hash not available - using the TU check of v1.0", exec->TitleId);
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
    if (!WaitForHdd()) return;   // no hard disk -> nothing to patch from
    Log("XPatch360 v" XPATCH_VERSION " started");
    g_HashWorks = HashSelfTest();
    CacheLoad();                 // read once now, not while a game is starting

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
