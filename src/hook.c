#include "slot.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

/* debug log: enabled by env SLOT_DEBUG=<path> */
void Slot_Log(const char *fmt, ...)
{
    static char path[MAX_PATH];
    static int init;
    if (!init) { init = 1; if (!GetEnvironmentVariableA("SLOT_DEBUG", path, MAX_PATH)) path[0] = 0; }
    if (!path[0]) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; static const char crlf[2] = { 13, 10 }; WriteFile(h, buf, n, &w, NULL); WriteFile(h, crlf, 2, &w, NULL); CloseHandle(h); }
}

/* ---------- IAT hooking (main module only) ---------- */
typedef struct { const char *dll; const char *name; void *repl; void **orig; } HookDef;

static BOOL (WINAPI *o_TextOutA)(HDC, int, int, LPCSTR, int);
static LONG (WINAPI *o_TabbedTextOutA)(HDC, int, int, LPCSTR, int, int, const INT *, int);
static BOOL (WINAPI *o_ExtTextOutA)(HDC, int, int, UINT, const RECT *, LPCSTR, UINT, const INT *);
static int  (WINAPI *o_DrawTextA)(HDC, LPCSTR, int, LPRECT, UINT);
static BOOL (WINAPI *o_GetTextExtentPoint32A)(HDC, LPCSTR, int, LPSIZE);
static int  (WINAPI *o_MultiByteToWideChar)(UINT, DWORD, LPCCH, int, LPWSTR, int);
static int  (WINAPI *o_WideCharToMultiByte)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);

/* convert ANSI text to UTF-16 when slot chars are in play; returns NULL when no conversion is needed */
#define STACKW 512
static WCHAR *to_wide(LPCSTR s, int n, int *outn, WCHAR *stackbuf)
{
    if (!s || n == 0 || Slot_Used() == 0) return NULL;
    int need = Slot_MultiByteToWideChar(CP_ACP, 0, s, n, NULL, 0);
    if (need <= 0) return NULL;
    WCHAR *w = need <= STACKW ? stackbuf : (WCHAR *)HeapAlloc(GetProcessHeap(), 0, need * sizeof(WCHAR));
    if (!w) return NULL;
    *outn = Slot_MultiByteToWideChar(CP_ACP, 0, s, n, w, need);
    return w;
}
static void free_wide(WCHAR *w, WCHAR *stackbuf) { if (w && w != stackbuf) HeapFree(GetProcessHeap(), 0, w); }

/* ---- slot-aware drawing: native runs go through the original A API, slot chars get a fixed 2-cell box ---- */
static int cell_width(HDC dc)
{
    SIZE sz;
    if (o_GetTextExtentPoint32A(dc, "0", 1, &sz) && sz.cx > 0) return sz.cx;
    return 8;
}

static int slot_wide(unsigned cp, WCHAR w[2])
{
    if (cp >= 0x10000) { cp -= 0x10000; w[0] = (WCHAR)(0xD800 + (cp >> 10)); w[1] = (WCHAR)(0xDC00 + (cp & 0x3FF)); return 2; }
    w[0] = (WCHAR)cp; return 1;
}

static int has_slot(LPCSTR s, int n)
{
    unsigned cp;
    for (int i = 0; i < n; i++) {
        if (Slot_DecodeAt(s + i, n - i, &cp)) return 1;
        if (IsDBCSLeadByte((BYTE)s[i]) && i + 1 < n) i++;
    }
    return 0;
}

/* emoji / pictographs: the editor font (MS Gothic) has no glyphs, so the system substitutes a
 * small one that does not fit the 2-cell box.  Use an emoji font sized to the box, centred. */
static HFONT g_emoji_font;
static int g_emoji_h;

static int is_emoji_cp(unsigned cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2300 && cp <= 0x23FF) ||
           (cp >= 0x2B00 && cp <= 0x2BFF);
}

static HFONT emoji_font(int h)
{
    if (g_emoji_font && g_emoji_h == h) return g_emoji_font;
    HFONT f = CreateFontW(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Emoji");
    if (!f) return NULL;
    if (g_emoji_font) DeleteObject(g_emoji_font);
    g_emoji_font = f; g_emoji_h = h;
    return f;
}

static void draw_slot_char(HDC dc, int cx, int y, const WCHAR *w, int wn, unsigned cp, int cell, int height, UINT opt)
{
    RECT rc = { cx, y, cx + 2 * cell, y + height };
    HFONT f = NULL;
    if (is_emoji_cp(cp)) {
        int h = 2 * cell;
        if (h > height) h = height;
        f = emoji_font(h);
    }
    if (!f) { ExtTextOutW(dc, cx, y, opt, &rc, w, wn, NULL); return; }
    HFONT old = (HFONT)SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, w, wn, &sz);
    int dx = (2 * cell - sz.cx) / 2, dy = (height - sz.cy) / 2;
    ExtTextOutW(dc, cx + dx, y + dy, opt, &rc, w, wn, NULL);
    SelectObject(dc, old);
}

/* tabbed != 0: use TabbedTextOutA for native runs, else TextOutA. returns packed width|height<<16 */
static LONG draw_run(HDC dc, int x, int y, LPCSTR s, int n, int nt, const INT *tabs, int org, int tabbed)
{
    int cell = cell_width(dc), cx = x, i = 0;
    LONG total = 0, height = 0;
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    UINT opt = ETO_CLIPPED | (GetBkMode(dc) == OPAQUE ? ETO_OPAQUE : 0);
    while (i < n) {
        int j = i; unsigned cp = 0;
        while (j < n && !Slot_DecodeAt(s + j, n - j, &cp)) j += (IsDBCSLeadByte((BYTE)s[j]) && j + 1 < n) ? 2 : 1;
        if (j > i) {
            if (tabbed) {
                LONG r = o_TabbedTextOutA(dc, cx, y, s + i, j - i, nt, tabs, org);
                total += LOWORD(r); cx += LOWORD(r); height = HIWORD(r);
            } else {
                SIZE sz;
                o_TextOutA(dc, cx, y, s + i, j - i);
                if (o_GetTextExtentPoint32A(dc, s + i, j - i, &sz)) { total += sz.cx; cx += sz.cx; height = sz.cy; }
            }
        }
        if (j < n) {
            WCHAR w[2]; int wn = slot_wide(cp, w);
            draw_slot_char(dc, cx, y, w, wn, cp, cell, tm.tmHeight, opt);
            cx += 2 * cell; total += 2 * cell; if (!height) height = tm.tmHeight;
            i = j + 2;
        } else i = n;
    }
    return (LONG)((total & 0xFFFF) | (height << 16));
}

static BOOL WINAPI h_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
    if (!s || n <= 0 || Slot_Used() == 0 || !has_slot(s, n)) return o_TextOutA(dc, x, y, s, n);
    draw_run(dc, x, y, s, n, 0, NULL, 0, 0);
    return TRUE;
}

static LONG WINAPI h_TabbedTextOutA(HDC dc, int x, int y, LPCSTR s, int n, int nt, const INT *tabs, int org)
{
    if (!s || n <= 0 || Slot_Used() == 0 || !has_slot(s, n)) return o_TabbedTextOutA(dc, x, y, s, n, nt, tabs, org);
    return draw_run(dc, x, y, s, n, nt, tabs, org, 1);
}

static BOOL WINAPI h_ExtTextOutA(HDC dc, int x, int y, UINT opt, const RECT *rc, LPCSTR s, UINT n, const INT *dx)
{
    if (!s || n == 0 || dx) return o_ExtTextOutA(dc, x, y, opt, rc, s, n, dx);
    WCHAR sb[STACKW]; int wn = 0;
    WCHAR *w = to_wide(s, (int)n, &wn, sb);
    if (!w) return o_ExtTextOutA(dc, x, y, opt, rc, s, n, dx);
    BOOL r = ExtTextOutW(dc, x, y, opt, rc, w, wn, NULL);
    free_wide(w, sb);
    return r;
}

static int WINAPI h_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT rc, UINT fmt)
{
    WCHAR sb[STACKW]; int wn = 0;
    WCHAR *w = to_wide(s, n, &wn, sb);
    if (!w) return o_DrawTextA(dc, s, n, rc, fmt);
    int r = DrawTextW(dc, w, (n < 0) ? -1 : wn, rc, fmt);
    free_wide(w, sb);
    return r;
}

static BOOL WINAPI h_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE sz)
{
    if (!s || n <= 0 || Slot_Used() == 0 || !has_slot(s, n)) return o_GetTextExtentPoint32A(dc, s, n, sz);
    int cell = cell_width(dc), i = 0;
    LONG cx = 0, cy = 0;
    while (i < n) {
        int j = i; unsigned cp;
        while (j < n && !Slot_DecodeAt(s + j, n - j, &cp)) j += (IsDBCSLeadByte((BYTE)s[j]) && j + 1 < n) ? 2 : 1;
        if (j > i) { SIZE t; if (o_GetTextExtentPoint32A(dc, s + i, j - i, &t)) { cx += t.cx; if (t.cy > cy) cy = t.cy; } }
        if (j < n) { cx += 2 * cell; i = j + 2; } else i = n;
    }
    if (!cy) { TEXTMETRICA tm; GetTextMetricsA(dc, &tm); cy = tm.tmHeight; }
    sz->cx = cx; sz->cy = cy;
    return TRUE;
}

/* ---- clipboard: keep CF_TEXT (slot bytes) and CF_UNICODETEXT consistent ---- */
static HANDLE (WINAPI *o_SetClipboardData)(UINT, HANDLE);
static HANDLE (WINAPI *o_GetClipboardData)(UINT);
static HGLOBAL g_clipA;

static HANDLE WINAPI h_SetClipboardData(UINT fmt, HANDLE h)
{
    HGLOBAL wmem = NULL;
    if (fmt == CF_TEXT && h && Slot_Used() > 0) {
        const char *s = (const char *)GlobalLock(h);
        if (s) {
            int n = lstrlenA(s);
            if (has_slot(s, n)) {
                int wn = Slot_MultiByteToWideChar(CP_ACP, 0, s, n + 1, NULL, 0);
                if (wn > 0 && (wmem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wn * sizeof(WCHAR)))) {
                    WCHAR *w = (WCHAR *)GlobalLock(wmem);
                    Slot_MultiByteToWideChar(CP_ACP, 0, s, n + 1, w, wn);
                    GlobalUnlock(wmem);
                }
            }
            GlobalUnlock(h);
        }
    }
    HANDLE r = o_SetClipboardData(fmt, h);
    if (wmem && !o_SetClipboardData(CF_UNICODETEXT, wmem)) GlobalFree(wmem);
    return r;
}

static HANDLE WINAPI h_GetClipboardData(UINT fmt)
{
    if (fmt == CF_TEXT && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hw = o_GetClipboardData(CF_UNICODETEXT);
        if (hw) {
            const WCHAR *w = (const WCHAR *)GlobalLock(hw);
            if (w) {
                int n = lstrlenW(w), wide = 0;
                for (int i = 0; i < n; i++) if (w[i] >= 0x80) { wide = 1; break; }
                HGLOBAL out = NULL;
                if (wide) {
                    int bn = Slot_WideCharToMultiByte(CP_ACP, 0, w, n + 1, NULL, 0, NULL, NULL);
                    if (bn > 0 && (out = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)bn))) {
                        char *b = (char *)GlobalLock(out);
                        Slot_WideCharToMultiByte(CP_ACP, 0, w, n + 1, b, bn, NULL, NULL);
                        GlobalUnlock(out);
                    }
                }
                GlobalUnlock(hw);
                if (out) {
                    if (g_clipA) GlobalFree(g_clipA);
                    g_clipA = out;
                    return out;
                }
            }
        }
    }
    return o_GetClipboardData(fmt);
}

static int WINAPI h_MultiByteToWideChar(UINT cp, DWORD fl, LPCCH s, int cb, LPWSTR d, int cch)
{
    return Slot_MultiByteToWideChar(cp, fl, s, cb, d, cch);
}
static int WINAPI h_WideCharToMultiByte(UINT cp, DWORD fl, LPCWCH s, int cch, LPSTR d, int cb, LPCCH def, LPBOOL used)
{
    return Slot_WideCharToMultiByte(cp, fl, s, cch, d, cb, def, used);
}

extern FILE *(__cdecl *o_fopen)(const char *, const char *);
extern int (__cdecl *o_fclose)(FILE *);
FILE *__cdecl Io_fopen(const char *, const char *);
int __cdecl Io_fclose(FILE *);

void Ime_Init(HINSTANCE);

static HookDef g_hooks[] = {
    { "USER32.dll",   "TabbedTextOutA",        (void *)h_TabbedTextOutA,        (void **)&o_TabbedTextOutA },
    { "GDI32.dll",    "TextOutA",              (void *)h_TextOutA,              (void **)&o_TextOutA },
    { "GDI32.dll",    "ExtTextOutA",           (void *)h_ExtTextOutA,           (void **)&o_ExtTextOutA },
    { "USER32.dll",   "DrawTextA",             (void *)h_DrawTextA,             (void **)&o_DrawTextA },
    { "GDI32.dll",    "GetTextExtentPoint32A", (void *)h_GetTextExtentPoint32A, (void **)&o_GetTextExtentPoint32A },
    { "KERNEL32.dll", "MultiByteToWideChar",   (void *)h_MultiByteToWideChar,   (void **)&o_MultiByteToWideChar },
    { "KERNEL32.dll", "WideCharToMultiByte",   (void *)h_WideCharToMultiByte,   (void **)&o_WideCharToMultiByte },
    { "USER32.dll",   "SetClipboardData",      (void *)h_SetClipboardData,      (void **)&o_SetClipboardData },
    { "USER32.dll",   "GetClipboardData",      (void *)h_GetClipboardData,      (void **)&o_GetClipboardData },
    { "MSVCRT.dll",   "fopen",                 (void *)Io_fopen,                (void **)&o_fopen },
    { "MSVCRT.dll",   "fclose",                (void *)Io_fclose,               (void **)&o_fclose },
};

static int hook_one(HMODULE mod, const HookDef *h)
{
    uint8_t *base = (uint8_t *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((const char *)(base + d->Name), h->dll) != 0) continue;
        DWORD int_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        IMAGE_THUNK_DATA *nm = (IMAGE_THUNK_DATA *)(base + int_rva);
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; nm->u1.AddressOfData; nm++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(nm->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + nm->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, h->name) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            *h->orig = (void *)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)h->repl;
            VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

int Hook_Iat(HMODULE mod, const char *dll, const char *name, void *repl, void **orig)
{
    HookDef d = { dll, name, repl, orig };
    return hook_one(mod, &d);
}

void Fs_Install(HMODULE exe);

__declspec(dllexport) void DllInit(void) {}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Slot_Init();
        HMODULE exe = GetModuleHandleA(NULL);
        /* debug aid: SLOT_MASK=<bitmask> selects which hooks are installed */
        char mb[16];
        unsigned long mask = GetEnvironmentVariableA("SLOT_MASK", mb, sizeof mb) ? strtoul(mb, NULL, 0) : ~0ul;
        for (size_t i = 0; i < sizeof g_hooks / sizeof g_hooks[0]; i++)
            if (mask & (1ul << i)) hook_one(exe, &g_hooks[i]);
        if (mask & (1ul << 11)) Ime_Init(inst);
        if (mask & (1ul << 12)) Fs_Install(exe);
    }
    return TRUE;
}
