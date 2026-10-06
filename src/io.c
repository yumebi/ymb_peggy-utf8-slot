/*
 * Stream fix-ups for 4-byte UTF-8 (code points above U+FFFF).
 *
 * Peggy's own UTF-8 reader/writer only handle 1..3 byte sequences and treat each UTF-16
 * unit separately.  So:
 *   load: 4-byte sequences are rewritten to the equivalent CESU-8 form (two 3-byte
 *         surrogates) in a temporary file; the stock reader then yields a UTF-16 surrogate
 *         pair, which the conversion hook maps to a slot.
 *   save: the stock writer emits CESU-8 for such characters; once the file is closed it
 *         is rewritten with proper 4-byte sequences.
 *
 * NOTE: Peggy uses its bundled (old) MSVCRT.  FILE* values belong to that CRT, so this
 * file must never call fread/fseek/... itself; it only uses Win32 file I/O plus the
 * original fopen/fclose pointers.
 */
#include "slot.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct _FILE_OPAQUE FILE;

/* RVA range of the document load function (FUN_004bf530 .. FUN_004bfd30 in peggypro.exe 4.63) */
#define LOAD_RVA_LO 0x000BF530u
#define LOAD_RVA_HI 0x000BFD30u

WCHAR *Fs_PathW(const char *a, WCHAR *stk, int cap);
void Slot_Log(const char *fmt, ...);

FILE *(__cdecl *o_fopen)(const char *, const char *);
int (__cdecl *o_fclose)(FILE *);

static uint8_t *read_file(const char *path, DWORD *len)
{
    WCHAR sp[1024];
    WCHAR *wp = Fs_PathW(path, sp, 1024);
    HANDLE h = wp ? CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)
                  : CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (wp && wp != sp) HeapFree(GetProcessHeap(), 0, wp);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(h, NULL), got = 0;
    uint8_t *b = NULL;
    if (n != INVALID_FILE_SIZE && n > 0 && n < 0x30000000 && (b = (uint8_t *)malloc(n))) {
        if (!ReadFile(h, b, n, &got, NULL) || got != n) { free(b); b = NULL; }
    }
    CloseHandle(h);
    if (b) *len = n;
    return b;
}

static int write_file(const char *path, const void *data, DWORD n)
{
    WCHAR sp[1024];
    WCHAR *wp = Fs_PathW(path, sp, 1024);
    HANDLE h = wp ? CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
                  : CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (wp && wp != sp) HeapFree(GetProcessHeap(), 0, wp);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    int ok = WriteFile(h, data, n, &w, NULL) && w == n;
    CloseHandle(h);
    return ok;
}

static int is_cont(uint8_t c) { return (c & 0xC0) == 0x80; }

static int is_utf8_4(const uint8_t *p, const uint8_t *end)
{
    if (end - p < 4 || p[0] < 0xF0 || p[0] > 0xF4) return 0;
    if (!is_cont(p[1]) || !is_cont(p[2]) || !is_cont(p[3])) return 0;
    uint32_t cp = ((p[0] & 7u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
    return cp >= 0x10000 && cp <= 0x10FFFF;
}

static void put3(uint8_t *o, uint32_t u)
{
    o[0] = (uint8_t)(0xE0 | (u >> 12)); o[1] = (uint8_t)(0x80 | ((u >> 6) & 0x3F)); o[2] = (uint8_t)(0x80 | (u & 0x3F));
}

/* temp copies created for loading: FILE* -> path, deleted when the FILE is closed */
#define MAXT 8
static struct { FILE *f; char path[MAX_PATH]; } g_tmp[MAXT];
#define MAXW 16
static struct { FILE *f; char path[MAX_PATH]; } g_w[MAXW];

static FILE *convert_for_load(FILE *f, const char *path)
{
    DWORD n;
    uint8_t *b = read_file(path, &n);
    if (!b) return f;
    const uint8_t *end = b + n;
    long cnt = 0;
    for (const uint8_t *p = b; p < end; p++)
        if (*p >= 0xF0 && is_utf8_4(p, end)) { cnt++; p += 3; }
    if (!cnt) { free(b); return f; }

    uint8_t *o = (uint8_t *)malloc((size_t)n + (size_t)cnt * 2);
    if (!o) { free(b); return f; }
    uint8_t *q = o;
    for (const uint8_t *p = b; p < end;) {
        if (*p >= 0xF0 && is_utf8_4(p, end)) {
            uint32_t cp = ((p[0] & 7u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            cp -= 0x10000;
            put3(q, 0xD800 + (cp >> 10)); put3(q + 3, 0xDC00 + (cp & 0x3FF));
            q += 6; p += 4;
        } else *q++ = *p++;
    }
    FILE *res = f;
    char dir[MAX_PATH], tmp[MAX_PATH];
    if (GetTempPathA(MAX_PATH, dir) && GetTempFileNameA(dir, "pgu", 0, tmp) && write_file(tmp, o, (DWORD)(q - o))) {
        FILE *t = o_fopen(tmp, "rb");
        int slot = -1;
        for (int i = 0; i < MAXT; i++) if (!g_tmp[i].f) { slot = i; break; }
        if (t && slot >= 0) {
            g_tmp[slot].f = t; lstrcpynA(g_tmp[slot].path, tmp, MAX_PATH);
            o_fclose(f);
            res = t;
        } else {
            if (t) o_fclose(t);
            DeleteFileA(tmp);
        }
    }
    free(b); free(o);
    return res;
}

static void fix_saved(const char *path)
{
    DWORD n;
    uint8_t *b = read_file(path, &n);
    if (!b) return;
    const uint8_t *end = b + n;
    long cnt = 0;
    for (const uint8_t *p = b; p + 6 <= end; p++)
        if (p[0] == 0xED && p[1] >= 0xA0 && p[1] <= 0xAF && p[3] == 0xED && p[4] >= 0xB0 && p[4] <= 0xBF) { cnt++; p += 5; }
    if (!cnt) { free(b); return; }
    uint8_t *o = (uint8_t *)malloc(n), *q = o;
    for (const uint8_t *p = b; p < end;) {
        if (p + 6 <= end && p[0] == 0xED && p[1] >= 0xA0 && p[1] <= 0xAF && p[3] == 0xED && p[4] >= 0xB0 && p[4] <= 0xBF &&
            is_cont(p[2]) && is_cont(p[5])) {
            uint32_t hi = 0xD000u | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            uint32_t lo = 0xD000u | ((p[4] & 0x3Fu) << 6) | (p[5] & 0x3Fu);
            uint32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
            *q++ = (uint8_t)(0xF0 | (cp >> 18)); *q++ = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
            *q++ = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); *q++ = (uint8_t)(0x80 | (cp & 0x3F));
            p += 6;
        } else *q++ = *p++;
    }
    write_file(path, o, (DWORD)(q - o));
    free(b); free(o);
}

__attribute__((noinline)) FILE *__cdecl Io_fopen(const char *path, const char *mode)
{
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    FILE *f;
    Slot_Log("fopen: %s mode=%s", path ? path : "(null)", mode ? mode : "(null)");
    {
        WCHAR sp[1024];
        WCHAR *wp = path ? Fs_PathW(path, sp, 1024) : NULL;
        FILE *(__cdecl *wfopen)(const WCHAR *, const WCHAR *) = NULL;
        if (wp) {
            HMODULE crt = GetModuleHandleA("MSVCRT.dll");
            if (crt) wfopen = (FILE *(__cdecl *)(const WCHAR *, const WCHAR *))GetProcAddress(crt, "_wfopen");
        }
        if (wp && wfopen && mode) {
            WCHAR wm[16];
            MultiByteToWideChar(CP_ACP, 0, mode, -1, wm, 16);
            f = wfopen(wp, wm);
        } else f = o_fopen(path, mode);
        if (wp && wp != sp) HeapFree(GetProcessHeap(), 0, wp);
    }
    if (!f || !path || !mode) return f;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    int rd = strchr(mode, 'r') != NULL && strchr(mode, '+') == NULL;
    int wr = strchr(mode, 'w') != NULL;
    if (rd && ra >= base + LOAD_RVA_LO && ra < base + LOAD_RVA_HI) {
        f = convert_for_load(f, path);
    } else if (wr) {
        for (int i = 0; i < MAXW; i++)
            if (!g_w[i].f) { g_w[i].f = f; lstrcpynA(g_w[i].path, path, MAX_PATH); break; }
    }
    return f;
}

int __cdecl Io_fclose(FILE *f)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    int fix = 0, del = 0;
    if (f) {
        for (int i = 0; i < MAXW; i++)
            if (g_w[i].f == f) {
                if (Slot_HasSupplementary()) { lstrcpynA(path, g_w[i].path, MAX_PATH); fix = 1; }
                g_w[i].f = NULL;
                break;
            }
        for (int i = 0; i < MAXT; i++)
            if (g_tmp[i].f == f) { lstrcpynA(tmp, g_tmp[i].path, MAX_PATH); del = 1; g_tmp[i].f = NULL; break; }
    }
    int r = o_fclose(f);
    if (del) DeleteFileA(tmp);
    if (fix && r == 0) fix_saved(path);
    return r;
}
