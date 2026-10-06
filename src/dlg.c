/*
 * Unicode file names, part 2: file dialogs and the registry (recent-file list).
 *
 * File dialogs: the ANSI GetOpenFileNameA/GetSaveFileNameA return '?' for names CP932 cannot
 * express.  The calls are redirected to the W versions; the OPENFILENAME is converted in both
 * directions (UTF-16 <-> ANSI+slots).  While the dialog is up, the CDM_* messages that carry
 * strings are converted too, so MFC's CFileDialog helpers never receive UTF-16 in an ANSI buffer.
 *
 * Registry: slot bytes only mean something inside one session, so REG_SZ values are stored as
 * real UTF-16 and converted back to ANSI+slots when read.
 *
 * MFC42.DLL loads advapi32/shell32/comdlg32 lazily (delay import), which an IAT patch cannot
 * reach, so MFC42's GetProcAddress is hooked and returns these replacements.
 */
#include "slot.h"
#include <commdlg.h>
#include <shellapi.h>
#include <stdint.h>
#include <string.h>

int Hook_Iat(HMODULE mod, const char *dll, const char *name, void *repl, void **orig);
void Slot_Log(const char *fmt, ...);

#define PBUF 1024

/* ---- small helpers -------------------------------------------------------------------- */
static int has_slot_n(const char *s, int n)
{
    unsigned cp;
    if (!s || Slot_Used() == 0) return 0;
    for (int i = 0; i < n; i++) {
        if (Slot_DecodeAt(s + i, n - i, &cp)) return 1;
        if (IsDBCSLeadByte((BYTE)s[i]) && i + 1 < n) i++;
    }
    return 0;
}

static WCHAR *a2w_dup(const char *a, int cb)    /* cb < 0: NUL-terminated; result is HeapAlloc'ed */
{
    if (!a) return NULL;
    int n = Slot_MultiByteToWideChar(CP_ACP, 0, a, cb, NULL, 0);
    if (n <= 0) n = 1;
    WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)n + 2) * sizeof(WCHAR));
    if (w && n > 1) Slot_MultiByteToWideChar(CP_ACP, 0, a, cb, w, n);
    return w;
}
static void hfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static int w2a(const WCHAR *w, char *out, int cap)
{
    return Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL);
}

/* double-NUL terminated list (filters) */
static int ansi_list_bytes(const char *a)      /* bytes including the final double NUL */
{
    const char *p = a;
    while (*p) p += lstrlenA(p) + 1;
    return (int)(p - a) + 1;
}
static int wide_list_chars(const WCHAR *w)
{
    const WCHAR *p = w;
    while (*p) p += lstrlenW(p) + 1;
    return (int)(p - w) + 1;
}

/* ---- file dialogs ----------------------------------------------------------------------- */
static BOOL (WINAPI *o_GetOpenFileNameA)(LPOPENFILENAMEA);
static BOOL (WINAPI *o_GetSaveFileNameA)(LPOPENFILENAMEA);
static volatile LONG g_in_ofn;

static int last_sep_end(const char *s)              /* byte offset just after the last \ or / */
{
    int last = 0;
    for (int i = 0; s[i]; i++) {
        if (IsDBCSLeadByte((BYTE)s[i]) && s[i + 1]) { i++; continue; }
        if (s[i] == '\\' || s[i] == '/') last = i + 1;
    }
    return last;
}

static BOOL ofn_run(LPOPENFILENAMEA a, int save)
{
    OPENFILENAMEW w;
    WCHAR *filter = NULL, *custom = NULL, *file = NULL, *title = NULL, *initdir = NULL, *ttl = NULL, *defext = NULL, *tmpl = NULL;
    BOOL ok;
    DWORD sz = a->lStructSize < sizeof w ? a->lStructSize : (DWORD)sizeof w;

    ZeroMemory(&w, sizeof w);
    memcpy(&w, a, sz);
    w.lStructSize = a->lStructSize;

    if (a->lpstrFilter) { filter = a2w_dup(a->lpstrFilter, ansi_list_bytes(a->lpstrFilter)); w.lpstrFilter = filter; }
    if (a->lpstrCustomFilter && a->nMaxCustFilter) {
        custom = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)a->nMaxCustFilter + 2) * sizeof(WCHAR));
        if (custom) {
            const char *d = a->lpstrCustomFilter;
            int l1 = lstrlenA(d);
            if (l1 && l1 + 1 < (int)a->nMaxCustFilter) {
                int n1 = MultiByteToWideChar(CP_ACP, 0, d, l1 + 1, custom, (int)a->nMaxCustFilter);
                int l2 = lstrlenA(d + l1 + 1);
                if (n1 && l2 && n1 + l2 + 1 < (int)a->nMaxCustFilter)
                    MultiByteToWideChar(CP_ACP, 0, d + l1 + 1, l2 + 1, custom + n1, (int)a->nMaxCustFilter - n1);
            }
            w.lpstrCustomFilter = custom;
        }
    }
    if (a->lpstrFile && a->nMaxFile) {
        file = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)a->nMaxFile + 2) * sizeof(WCHAR));
        if (file) {
            int n = Slot_MultiByteToWideChar(CP_ACP, 0, a->lpstrFile, -1, NULL, 0);
            if (n > 0 && n <= (int)a->nMaxFile) Slot_MultiByteToWideChar(CP_ACP, 0, a->lpstrFile, -1, file, n);
            w.lpstrFile = file;
        }
    }
    if (a->lpstrFileTitle && a->nMaxFileTitle) {
        title = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)a->nMaxFileTitle + 2) * sizeof(WCHAR));
        w.lpstrFileTitle = title;
    }
    if (a->lpstrInitialDir) { initdir = a2w_dup(a->lpstrInitialDir, -1); w.lpstrInitialDir = initdir; }
    if (a->lpstrTitle)      { ttl = a2w_dup(a->lpstrTitle, -1);          w.lpstrTitle = ttl; }
    if (a->lpstrDefExt)     { defext = a2w_dup(a->lpstrDefExt, -1);      w.lpstrDefExt = defext; }
    if (a->lpTemplateName && HIWORD((ULONG_PTR)a->lpTemplateName)) { tmpl = a2w_dup(a->lpTemplateName, -1); w.lpTemplateName = tmpl; }

    InterlockedIncrement(&g_in_ofn);
    ok = save ? GetSaveFileNameW(&w) : GetOpenFileNameW(&w);
    InterlockedDecrement(&g_in_ofn);

    /* ---- results back to the caller's ANSI structure ---- */
    a->Flags = w.Flags;
    a->nFilterIndex = w.nFilterIndex;
    if (ok && file && a->lpstrFile) {
        int multi = (w.nFileOffset > 0 && file[w.nFileOffset - 1] == 0);
        int total = wide_list_chars(file), pos = 0, needed = 0;
        char *tmp = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)total * 4 + 8);
        if (tmp) {
            const WCHAR *p = file;
            if (!multi) {
                needed = w2a(file, tmp, total * 4 + 4);              /* includes NUL */
                pos = needed;
            } else {
                while (*p) {
                    int n = w2a(p, tmp + pos, total * 4 + 4 - pos);
                    if (n <= 0) break;
                    pos += n;
                    p += lstrlenW(p) + 1;
                }
                needed = pos;
            }
            if (needed + 1 > (int)a->nMaxFile) {                      /* ANSI buffer too small */
                if (a->nMaxFile >= 2) *(WORD *)a->lpstrFile = (WORD)(needed + 1);
                ok = FALSE;
            } else {
                memcpy(a->lpstrFile, tmp, (size_t)pos);
                a->lpstrFile[pos] = 0;
                if (multi) {
                    a->nFileOffset = (WORD)(lstrlenA(a->lpstrFile) + 1);
                    a->nFileExtension = 0;
                } else {
                    int off = last_sep_end(a->lpstrFile);
                    int ext = 0;
                    for (int i = off; a->lpstrFile[i]; i++) {
                        if (IsDBCSLeadByte((BYTE)a->lpstrFile[i]) && a->lpstrFile[i + 1]) { i++; continue; }
                        if (a->lpstrFile[i] == '.') ext = i + 1;
                    }
                    a->nFileOffset = (WORD)off;
                    a->nFileExtension = (WORD)ext;
                }
            }
            hfree(tmp);
        }
    }
    if (ok && title && a->lpstrFileTitle) w2a(title, a->lpstrFileTitle, (int)a->nMaxFileTitle);
    if (ok && custom && a->lpstrCustomFilter && a->nMaxCustFilter) {
        int pos = 0;
        const WCHAR *p = custom;
        for (int k = 0; k < 2 && *p; k++) {
            int n = w2a(p, a->lpstrCustomFilter + pos, (int)a->nMaxCustFilter - pos);
            if (n <= 0) break;
            pos += n;
            p += lstrlenW(p) + 1;
        }
        if (pos < (int)a->nMaxCustFilter) a->lpstrCustomFilter[pos] = 0;
    }
    if (a->lStructSize >= sizeof(OPENFILENAMEA)) a->FlagsEx = w.FlagsEx;

    hfree(filter); hfree(custom); hfree(file); hfree(title); hfree(initdir); hfree(ttl); hfree(defext); hfree(tmpl);
    return ok;
}
static BOOL WINAPI f_GetOpenFileNameA(LPOPENFILENAMEA a) { return a ? ofn_run(a, 0) : o_GetOpenFileNameA(a); }
static BOOL WINAPI f_GetSaveFileNameA(LPOPENFILENAMEA a) { return a ? ofn_run(a, 1) : o_GetSaveFileNameA(a); }

/* CDM_* messages carry strings: CDM_GETSPEC..CDM_GETFOLDERPATH (read), CDM_SETCONTROLTEXT, CDM_SETDEFEXT (write) */
static LRESULT (WINAPI *o_SendMessageA)(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI f_SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (g_in_ofn && m >= CDM_GETSPEC && m <= CDM_SETDEFEXT) {
        if (m == CDM_GETSPEC || m == CDM_GETFILEPATH || m == CDM_GETFOLDERPATH) {
            WCHAR tmp[PBUF * 2];
            LRESULT r = SendMessageW(h, m, ARRAYSIZE(tmp), (LPARAM)tmp);
            if (r < 0) return r;
            int need = w2a(tmp, NULL, 0);
            if (need <= 0) return -1;
            if (!lp || (SIZE_T)need > wp) return need;
            w2a(tmp, (char *)lp, (int)wp);
            return need;
        }
        if (m == CDM_SETCONTROLTEXT || m == CDM_SETDEFEXT) {
            WCHAR *w = a2w_dup((const char *)lp, -1);
            LRESULT r = SendMessageW(h, m, wp, (LPARAM)w);
            hfree(w);
            return r;
        }
    }
    return o_SendMessageA(h, m, wp, lp);
}

/* ---- registry --------------------------------------------------------------------------- */
static LONG (WINAPI *o_RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
static LONG (WINAPI *o_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);

static LONG WINAPI f_RegSetValueExA(HKEY k, LPCSTR name, DWORD res, DWORD type, const BYTE *data, DWORD cb)
{
    if ((type == REG_SZ || type == REG_EXPAND_SZ) && data && has_slot_n((const char *)data, (int)cb)) {
        WCHAR wn[260];
        WCHAR *wv = a2w_dup((const char *)data, (int)cb);
        if (wv && (!name || MultiByteToWideChar(CP_ACP, 0, name, -1, wn, 260))) {
            int chars = lstrlenW(wv) + 1;
            LONG r = RegSetValueExW(k, name ? wn : NULL, res, type, (const BYTE *)wv, (DWORD)chars * sizeof(WCHAR));
            Slot_Log("RegSetValueExA: stored Unicode string value as UTF-16 (r=%ld)", r);
            hfree(wv);
            return r;
        }
        hfree(wv);
    }
    return o_RegSetValueExA(k, name, res, type, data, cb);
}

static LONG WINAPI f_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    WCHAR wn[260];
    DWORD t = 0, wcb = 0;
    if (name && !MultiByteToWideChar(CP_ACP, 0, name, -1, wn, 260)) return o_RegQueryValueExA(k, name, res, type, data, cb);
    if (RegQueryValueExW(k, name ? wn : NULL, NULL, &t, NULL, &wcb) != ERROR_SUCCESS ||
        (t != REG_SZ && t != REG_EXPAND_SZ) || wcb == 0)
        return o_RegQueryValueExA(k, name, res, type, data, cb);

    WCHAR *wv = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)wcb + 4);
    if (!wv) return o_RegQueryValueExA(k, name, res, type, data, cb);
    DWORD wcb2 = wcb;
    if (RegQueryValueExW(k, name ? wn : NULL, NULL, &t, (LPBYTE)wv, &wcb2) != ERROR_SUCCESS) {
        hfree(wv);
        return o_RegQueryValueExA(k, name, res, type, data, cb);
    }
    int nonnative = 0;
    for (const WCHAR *p = wv; *p; p++) {
        unsigned cp = *p;
        if (cp < 0x80) continue;
        if (cp >= 0xD800 && cp < 0xDC00 && p[1] >= 0xDC00 && p[1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (p[1] - 0xDC00); p++;
        }
        if (!Slot_IsNative(cp)) { nonnative = 1; break; }
    }
    if (!nonnative) { hfree(wv); return o_RegQueryValueExA(k, name, res, type, data, cb); }
    Slot_Log("RegQueryValueExA: converted a Unicode string value (%ls)", name ? wn : L"(default)");

    int need = w2a(wv, NULL, 0);
    LONG ret = ERROR_SUCCESS;
    if (need <= 0) ret = ERROR_INVALID_DATA;
    else {
        if (type) *type = t;
        if (!data) { if (cb) *cb = (DWORD)need; }
        else if (!cb) ret = ERROR_INVALID_PARAMETER;
        else if (*cb < (DWORD)need) { *cb = (DWORD)need; ret = ERROR_MORE_DATA; }
        else { w2a(wv, (char *)data, (int)*cb); *cb = (DWORD)need; }
    }
    hfree(wv);
    return ret;
}

UINT WINAPI Fs_DragQueryFileA(HDROP, UINT, LPSTR, UINT);
void Fs_SetDragQueryOrig(void *p);

/* ---- delay-load interception ---------------------------------------------------------------- */
static FARPROC (WINAPI *o_GetProcAddress)(HMODULE, LPCSTR);
static FARPROC WINAPI f_GetProcAddress(HMODULE m, LPCSTR n)
{
    FARPROC p = o_GetProcAddress(m, n);
    if (p && ((ULONG_PTR)n >> 16)) {
        if (!lstrcmpA(n, "RegSetValueExA"))   { if (!o_RegSetValueExA)   o_RegSetValueExA   = (void *)p; return (FARPROC)f_RegSetValueExA; }
        if (!lstrcmpA(n, "RegQueryValueExA")) { if (!o_RegQueryValueExA) o_RegQueryValueExA = (void *)p; return (FARPROC)f_RegQueryValueExA; }
        if (!lstrcmpA(n, "DragQueryFileA"))   { Fs_SetDragQueryOrig((void *)p); return (FARPROC)Fs_DragQueryFileA; }
        if (!lstrcmpA(n, "GetOpenFileNameA")) { if (!o_GetOpenFileNameA) o_GetOpenFileNameA = (void *)p; return (FARPROC)f_GetOpenFileNameA; }
        if (!lstrcmpA(n, "GetSaveFileNameA")) { if (!o_GetSaveFileNameA) o_GetSaveFileNameA = (void *)p; return (FARPROC)f_GetSaveFileNameA; }
    }
    return p;
}

void Dlg_Install(HMODULE exe)
{
    HMODULE mfc = GetModuleHandleA("MFC42.DLL");
    int r[8];
    r[0] = Hook_Iat(exe, "COMDLG32.dll", "GetOpenFileNameA", (void *)f_GetOpenFileNameA, (void **)&o_GetOpenFileNameA);
    r[1] = Hook_Iat(exe, "COMDLG32.dll", "GetSaveFileNameA", (void *)f_GetSaveFileNameA, (void **)&o_GetSaveFileNameA);
    r[2] = Hook_Iat(exe, "ADVAPI32.dll", "RegSetValueExA",   (void *)f_RegSetValueExA,   (void **)&o_RegSetValueExA);
    r[3] = Hook_Iat(exe, "ADVAPI32.dll", "RegQueryValueExA", (void *)f_RegQueryValueExA, (void **)&o_RegQueryValueExA);
    r[4] = Hook_Iat(exe, "USER32.dll",   "SendMessageA",     (void *)f_SendMessageA,     (void **)&o_SendMessageA);
    r[5] = mfc ? Hook_Iat(mfc, "USER32.dll", "SendMessageA", (void *)f_SendMessageA, (void **)&o_SendMessageA) : -1;
    r[6] = mfc ? Hook_Iat(mfc, "KERNEL32.dll", "GetProcAddress", (void *)f_GetProcAddress, (void **)&o_GetProcAddress) : -1;
    Slot_Log("dlg hooks: open=%d save=%d regset=%d regquery=%d sendmsg=%d/%d getprocaddr=%d",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
}
