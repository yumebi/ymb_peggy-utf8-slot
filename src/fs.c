/*
 * Unicode file names.
 *
 * Inside Peggy a file name is an ANSI string; characters CP932 cannot express are slot byte
 * pairs (see slot.c).  The file system needs the real UTF-16 name, so every ANSI file API that
 * receives such a string is redirected to its W counterpart.  Strings without slot characters
 * take the original A function untouched.
 *
 * Output direction (directory listing, command line, drag&drop) converts UTF-16 names back to
 * ANSI+slots, assigning slots on demand.
 *
 * NOTE: msvcrt belongs to Peggy's bundled (old) CRT, so CRT functions are resolved from that
 * module at run time; this DLL's own CRT is never used for them.
 */
#include "slot.h"
#include <shellapi.h>
#include <stdint.h>
#include <string.h>

int Hook_Iat(HMODULE mod, const char *dll, const char *name, void *repl, void **orig);
void Slot_Log(const char *fmt, ...);

#define PBUF 1024

/* ---- helpers ------------------------------------------------------------------------ */
static int has_slot_z(const char *s)
{
    unsigned cp;
    if (!s || Slot_Used() == 0) return 0;
    for (; *s; s++) {
        if (Slot_DecodeAt(s, 2, &cp)) return 1;
        if (IsDBCSLeadByte((BYTE)*s) && s[1]) s++;
    }
    return 0;
}

/* ANSI(+slots) -> newly usable wide path, or NULL when the string needs no conversion */
WCHAR *Fs_PathW(const char *a, WCHAR *stk, int cap)
{
    if (!has_slot_z(a)) return NULL;
    int n = Slot_MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = n <= cap ? stk : (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(WCHAR));
    if (!w) return NULL;
    Slot_MultiByteToWideChar(CP_ACP, 0, a, -1, w, n);
    return w;
}
static void free_w(WCHAR *w, WCHAR *stk) { if (w && w != stk) HeapFree(GetProcessHeap(), 0, w); }

/* wide -> ANSI+slots into caller buffer; returns bytes including NUL, 0 on failure */
static int w2a(const WCHAR *w, char *out, int cap)
{
    return Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL);
}


static void log_path(const char *tag, const char *p)
{
    char hex[200]; int n = 0;
    if (!p) { Slot_Log("%s: (null)", tag); return; }
    int len = lstrlenA(p), st = len > 36 ? len - 36 : 0;
    for (int i = st; p[i] && n < 190; i++) n += wsprintfA(hex + n, "%02X", (unsigned char)p[i]);
    hex[n] = 0;
    Slot_Log("%s: slot=%d used=%d hex=%s", tag, has_slot_z(p), Slot_Used(), hex);
}

#define IN_PATH(var, wvar, stk) WCHAR stk[PBUF]; WCHAR *wvar = Fs_PathW(var, stk, PBUF)

/* ---- kernel32: input-only wrappers --------------------------------------------------- */
static HANDLE (WINAPI *o_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI f_CreateFileA(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t)
{
    log_path("CreateFileA", p);
    IN_PATH(p, w, s);
    if (!w) return o_CreateFileA(p, acc, share, sa, disp, fl, t);
    HANDLE h = CreateFileW(w, acc, share, sa, disp, fl, t);
    free_w(w, s);
    return h;
}

static DWORD (WINAPI *o_GetFileAttributesA)(LPCSTR);
static DWORD WINAPI f_GetFileAttributesA(LPCSTR p)
{
    log_path("GetFileAttributesA", p);
    IN_PATH(p, w, s);
    if (!w) return o_GetFileAttributesA(p);
    DWORD r = GetFileAttributesW(w);
    free_w(w, s);
    return r;
}

static BOOL (WINAPI *o_SetFileAttributesA)(LPCSTR, DWORD);
static BOOL WINAPI f_SetFileAttributesA(LPCSTR p, DWORD a)
{
    IN_PATH(p, w, s);
    if (!w) return o_SetFileAttributesA(p, a);
    BOOL r = SetFileAttributesW(w, a);
    free_w(w, s);
    return r;
}

static BOOL (WINAPI *o_DeleteFileA)(LPCSTR);
static BOOL WINAPI f_DeleteFileA(LPCSTR p)
{
    IN_PATH(p, w, s);
    if (!w) return o_DeleteFileA(p);
    BOOL r = DeleteFileW(w);
    free_w(w, s);
    return r;
}

static BOOL (WINAPI *o_CreateDirectoryA)(LPCSTR, LPSECURITY_ATTRIBUTES);
static BOOL WINAPI f_CreateDirectoryA(LPCSTR p, LPSECURITY_ATTRIBUTES sa)
{
    IN_PATH(p, w, s);
    if (!w) return o_CreateDirectoryA(p, sa);
    BOOL r = CreateDirectoryW(w, sa);
    free_w(w, s);
    return r;
}

static BOOL (WINAPI *o_RemoveDirectoryA)(LPCSTR);
static BOOL WINAPI f_RemoveDirectoryA(LPCSTR p)
{
    IN_PATH(p, w, s);
    if (!w) return o_RemoveDirectoryA(p);
    BOOL r = RemoveDirectoryW(w);
    free_w(w, s);
    return r;
}

static BOOL (WINAPI *o_MoveFileA)(LPCSTR, LPCSTR);
static BOOL WINAPI f_MoveFileA(LPCSTR a, LPCSTR b)
{
    WCHAR s1[PBUF], s2[PBUF];
    WCHAR *w1 = Fs_PathW(a, s1, PBUF), *w2 = Fs_PathW(b, s2, PBUF);
    if (!w1 && !w2) return o_MoveFileA(a, b);
    WCHAR t1[PBUF], t2[PBUF];
    if (!w1) { MultiByteToWideChar(CP_ACP, 0, a, -1, t1, PBUF); w1 = t1; }
    if (!w2) { MultiByteToWideChar(CP_ACP, 0, b, -1, t2, PBUF); w2 = t2; }
    BOOL r = MoveFileW(w1, w2);
    if (w1 != t1) free_w(w1, s1);
    if (w2 != t2) free_w(w2, s2);
    return r;
}

static BOOL (WINAPI *o_MoveFileExA)(LPCSTR, LPCSTR, DWORD);
static BOOL WINAPI f_MoveFileExA(LPCSTR a, LPCSTR b, DWORD fl)
{
    WCHAR s1[PBUF], s2[PBUF];
    WCHAR *w1 = Fs_PathW(a, s1, PBUF), *w2 = Fs_PathW(b, s2, PBUF);
    if (!w1 && !w2) return o_MoveFileExA(a, b, fl);
    WCHAR t1[PBUF], t2[PBUF];
    if (!w1) { MultiByteToWideChar(CP_ACP, 0, a, -1, t1, PBUF); w1 = t1; }
    if (!w2 && b) { MultiByteToWideChar(CP_ACP, 0, b, -1, t2, PBUF); w2 = t2; }
    BOOL r = MoveFileExW(w1, w2, fl);
    if (w1 != t1) free_w(w1, s1);
    if (w2 != t2) free_w(w2, s2);
    return r;
}

static BOOL (WINAPI *o_CopyFileA)(LPCSTR, LPCSTR, BOOL);
static BOOL WINAPI f_CopyFileA(LPCSTR a, LPCSTR b, BOOL fail)
{
    WCHAR s1[PBUF], s2[PBUF];
    WCHAR *w1 = Fs_PathW(a, s1, PBUF), *w2 = Fs_PathW(b, s2, PBUF);
    if (!w1 && !w2) return o_CopyFileA(a, b, fail);
    WCHAR t1[PBUF], t2[PBUF];
    if (!w1) { MultiByteToWideChar(CP_ACP, 0, a, -1, t1, PBUF); w1 = t1; }
    if (!w2) { MultiByteToWideChar(CP_ACP, 0, b, -1, t2, PBUF); w2 = t2; }
    BOOL r = CopyFileW(w1, w2, fail);
    if (w1 != t1) free_w(w1, s1);
    if (w2 != t2) free_w(w2, s2);
    return r;
}

/* ---- kernel32: directory listing (always via W so Unicode names are visible) --------- */
static HANDLE (WINAPI *o_FindFirstFileA)(LPCSTR, LPWIN32_FIND_DATAA);
static BOOL (WINAPI *o_FindNextFileA)(HANDLE, LPWIN32_FIND_DATAA);

static void fd_w2a(const WIN32_FIND_DATAW *w, WIN32_FIND_DATAA *a)
{
    a->dwFileAttributes = w->dwFileAttributes;
    a->ftCreationTime = w->ftCreationTime;
    a->ftLastAccessTime = w->ftLastAccessTime;
    a->ftLastWriteTime = w->ftLastWriteTime;
    a->nFileSizeHigh = w->nFileSizeHigh;
    a->nFileSizeLow = w->nFileSizeLow;
    a->dwReserved0 = w->dwReserved0;
    a->dwReserved1 = w->dwReserved1;
    if (!w2a(w->cFileName, a->cFileName, sizeof a->cFileName)) a->cFileName[0] = 0;
    if (!w2a(w->cAlternateFileName, a->cAlternateFileName, sizeof a->cAlternateFileName)) a->cAlternateFileName[0] = 0;
}

static HANDLE WINAPI f_FindFirstFileA(LPCSTR p, LPWIN32_FIND_DATAA fd)
{
    WCHAR s[PBUF], t[PBUF];
    WCHAR *w = Fs_PathW(p, s, PBUF);
    if (!w) {
        if (!p || !MultiByteToWideChar(CP_ACP, 0, p, -1, t, PBUF)) return o_FindFirstFileA(p, fd);
        w = t;
    }
    WIN32_FIND_DATAW wd;
    HANDLE h = FindFirstFileW(w, &wd);
    if (h != INVALID_HANDLE_VALUE) fd_w2a(&wd, fd);
    if (w != t) free_w(w, s);
    return h;
}

static BOOL WINAPI f_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA fd)
{
    WIN32_FIND_DATAW wd;
    if (!FindNextFileW(h, &wd)) return FALSE;
    fd_w2a(&wd, fd);
    return TRUE;
}

/* ---- kernel32: path functions with output -------------------------------------------- */
static DWORD (WINAPI *o_GetFullPathNameA)(LPCSTR, DWORD, LPSTR, LPSTR *);
static DWORD WINAPI f_GetFullPathNameA(LPCSTR p, DWORD cap, LPSTR out, LPSTR *file)
{
    log_path("GetFullPathNameA", p);
    IN_PATH(p, w, s);
    if (!w) return o_GetFullPathNameA(p, cap, out, file);
    WCHAR full[PBUF];
    DWORD n = GetFullPathNameW(w, PBUF, full, NULL);
    free_w(w, s);
    if (n == 0 || n >= PBUF) return 0;
    int need = w2a(full, NULL, 0);
    if (need <= 0) return 0;
    if ((DWORD)need > cap) return (DWORD)need;       /* buffer too small: required size incl. NUL */
    w2a(full, out, (int)cap);
    if (file) {
        char *last = out, *q = out;
        for (; *q; q++) {
            if (IsDBCSLeadByte((BYTE)*q) && q[1]) { q++; continue; }
            if (*q == '\\' || *q == '/') last = q + 1;
        }
        *file = *last ? last : NULL;
    }
    return (DWORD)(need - 1);
}

static DWORD (WINAPI *o_GetShortPathNameA)(LPCSTR, LPSTR, DWORD);
static DWORD WINAPI f_GetShortPathNameA(LPCSTR p, LPSTR out, DWORD cap)
{
    IN_PATH(p, w, s);
    if (!w) return o_GetShortPathNameA(p, out, cap);
    WCHAR sh[PBUF];
    DWORD n = GetShortPathNameW(w, sh, PBUF);
    free_w(w, s);
    if (n == 0 || n >= PBUF) return 0;
    int need = w2a(sh, NULL, 0);
    if (need <= 0) return 0;
    if ((DWORD)need > cap) return (DWORD)need;
    w2a(sh, out, (int)cap);
    return (DWORD)(need - 1);
}

/* ---- msvcrt (Peggy's bundled CRT) ------------------------------------------------------ */
static void *crt_fn(const char *name)
{
    HMODULE m = GetModuleHandleA("MSVCRT.dll");
    return m ? (void *)GetProcAddress(m, name) : NULL;
}

static int (__cdecl *o_access)(const char *, int);
static int __cdecl f_access(const char *p, int mode)
{
    IN_PATH(p, w, s);
    int (__cdecl *fn)(const WCHAR *, int) = w ? (int (__cdecl *)(const WCHAR *, int))crt_fn("_waccess") : NULL;
    if (!w || !fn) { free_w(w, s); return o_access(p, mode); }
    int r = fn(w, mode);
    free_w(w, s);
    return r;
}

static int (__cdecl *o_stat)(const char *, void *);
static int __cdecl f_stat(const char *p, void *st)
{
    IN_PATH(p, w, s);
    int (__cdecl *fn)(const WCHAR *, void *) = w ? (int (__cdecl *)(const WCHAR *, void *))crt_fn("_wstat") : NULL;
    if (!w || !fn) { free_w(w, s); return o_stat(p, st); }
    int r = fn(w, st);
    free_w(w, s);
    return r;
}

#define CRT_DIR_HOOK(NAME, WNAME)                                                        \
    static int (__cdecl *o_##NAME)(const char *);                                        \
    static int __cdecl f_##NAME(const char *p)                                           \
    {                                                                                    \
        IN_PATH(p, w, s);                                                                \
        int (__cdecl *fn)(const WCHAR *) = w ? (int (__cdecl *)(const WCHAR *))crt_fn(WNAME) : NULL; \
        if (!w || !fn) { free_w(w, s); return o_##NAME(p); }                             \
        int r = fn(w);                                                                   \
        free_w(w, s);                                                                    \
        return r;                                                                        \
    }
CRT_DIR_HOOK(mkdir, "_wmkdir")
CRT_DIR_HOOK(rmdir, "_wrmdir")
CRT_DIR_HOOK(chdir, "_wchdir")


/* _fullpath: msvcrt resolves it with its own (unhooked) GetFullPathNameA, which would mangle slot bytes */
static char *(__cdecl *o_fullpath)(char *, const char *, size_t);
static char *__cdecl f_fullpath(char *abs, const char *rel, size_t max)
{
    log_path("_fullpath", rel);
    IN_PATH(rel, w, s);
    char *(__cdecl *wfull)(WCHAR *, const WCHAR *, size_t) = NULL;
    (void)wfull;
    if (!w) return o_fullpath(abs, rel, max);
    WCHAR full[PBUF];
    DWORD n = GetFullPathNameW(w, PBUF, full, NULL);
    free_w(w, s);
    if (n == 0 || n >= PBUF) return NULL;
    int need = w2a(full, NULL, 0);
    if (need <= 0) return NULL;
    if (!abs) {
        void *(__cdecl *crt_malloc)(size_t) = (void *(__cdecl *)(size_t))crt_fn("malloc");
        if (!crt_malloc) return NULL;
        abs = (char *)crt_malloc((size_t)need);
        if (!abs) return NULL;
        max = (size_t)need;
    }
    if ((size_t)need > max) return NULL;
    w2a(full, abs, (int)max);
    return abs;
}

/* ---- shell32 / user32 ------------------------------------------------------------------ */
static UINT (WINAPI *o_DragQueryFileA)(HDROP, UINT, LPSTR, UINT);
static UINT WINAPI f_DragQueryFileA(HDROP h, UINT i, LPSTR buf, UINT cap)
{
    if (i == 0xFFFFFFFFu) return o_DragQueryFileA(h, i, buf, cap);
    UINT n = DragQueryFileW(h, i, NULL, 0);
    if (!n) return o_DragQueryFileA(h, i, buf, cap);
    WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)n + 1) * sizeof(WCHAR));
    if (!w) return o_DragQueryFileA(h, i, buf, cap);
    DragQueryFileW(h, i, w, n + 1);
    int need = w2a(w, NULL, 0);
    UINT ret = 0;
    if (need > 0) {
        if (!buf) ret = (UINT)(need - 1);
        else {
            char *tmp = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need);
            if (tmp) {
                w2a(w, tmp, need);
                UINT copy = (UINT)(need - 1) < cap ? (UINT)(need - 1) : (cap ? cap - 1 : 0);
                if (cap) { memcpy(buf, tmp, copy); buf[copy] = 0; }
                ret = copy;
                HeapFree(GetProcessHeap(), 0, tmp);
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, w);
    return ret;
}

static HINSTANCE (WINAPI *o_ShellExecuteA)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
static HINSTANCE WINAPI f_ShellExecuteA(HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR par, LPCSTR dir, INT show)
{
    WCHAR sf[PBUF], sp[PBUF], sd[PBUF];
    WCHAR *wf = Fs_PathW(file, sf, PBUF), *wp = Fs_PathW(par, sp, PBUF), *wd = Fs_PathW(dir, sd, PBUF);
    if (!wf && !wp && !wd) return o_ShellExecuteA(hwnd, op, file, par, dir, show);
    WCHAR tf[PBUF], tp[PBUF], td[PBUF], to[64];
    if (!wf && file) { MultiByteToWideChar(CP_ACP, 0, file, -1, tf, PBUF); wf = tf; }
    if (!wp && par)  { MultiByteToWideChar(CP_ACP, 0, par, -1, tp, PBUF); wp = tp; }
    if (!wd && dir)  { MultiByteToWideChar(CP_ACP, 0, dir, -1, td, PBUF); wd = td; }
    WCHAR *wo = NULL;
    if (op) { MultiByteToWideChar(CP_ACP, 0, op, -1, to, 64); wo = to; }
    HINSTANCE r = ShellExecuteW(hwnd, wo, wf, wp, wd, show);
    if (wf != tf) free_w(wf, sf);
    if (wp != tp) free_w(wp, sp);
    if (wd != td) free_w(wd, sd);
    return r;
}

static BOOL (WINAPI *o_SetWindowTextA)(HWND, LPCSTR);
static BOOL WINAPI f_SetWindowTextA(HWND h, LPCSTR s)
{
    if (s && strlen(s) > 12) log_path("SetWindowTextA", s);
    WCHAR st[PBUF];
    WCHAR *w = Fs_PathW(s, st, PBUF);      /* same slot-aware conversion */
    if (!w) return o_SetWindowTextA(h, s);
    /* An ANSI window gets WM_SETTEXT converted to the ACP (lossy) by the system, so store the
     * Unicode caption through DefWindowProcW directly. */
    BOOL r = IsWindowUnicode(h) ? SetWindowTextW(h, w) : (DefWindowProcW(h, WM_SETTEXT, 0, (LPARAM)w) != 0);
    free_w(w, st);
    return r;
}

/* ---- command line: overwrite msvcrt's _acmdln before the exe start-up code reads it ---- */
static void patch_cmdline(void)
{
    HMODULE crt = GetModuleHandleA("MSVCRT.dll");
    if (!crt) return;
    char **p = (char **)GetProcAddress(crt, "_acmdln");
    if (!p || !*p) return;
    const WCHAR *wc = GetCommandLineW();
    int ascii = 1;
    for (const WCHAR *q = wc; *q; q++) if (*q >= 0x80) { ascii = 0; break; }
    if (ascii) return;
    int need = w2a(wc, NULL, 0);
    if (need <= 0) return;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need);
    if (!buf) return;
    w2a(wc, buf, need);
    *p = buf;
    Slot_Log("cmdline patched: %s", buf);
}

/* ---- installation ---------------------------------------------------------------------- */
typedef struct { const char *dll, *name; void *repl; void **orig; } FsHook;
static FsHook g_fs[] = {
    { "KERNEL32.dll", "CreateFileA",        (void *)f_CreateFileA,        (void **)&o_CreateFileA },
    { "KERNEL32.dll", "GetFileAttributesA", (void *)f_GetFileAttributesA, (void **)&o_GetFileAttributesA },
    { "KERNEL32.dll", "SetFileAttributesA", (void *)f_SetFileAttributesA, (void **)&o_SetFileAttributesA },
    { "KERNEL32.dll", "DeleteFileA",        (void *)f_DeleteFileA,        (void **)&o_DeleteFileA },
    { "KERNEL32.dll", "CreateDirectoryA",   (void *)f_CreateDirectoryA,   (void **)&o_CreateDirectoryA },
    { "KERNEL32.dll", "RemoveDirectoryA",   (void *)f_RemoveDirectoryA,   (void **)&o_RemoveDirectoryA },
    { "KERNEL32.dll", "MoveFileA",          (void *)f_MoveFileA,          (void **)&o_MoveFileA },
    { "KERNEL32.dll", "MoveFileExA",        (void *)f_MoveFileExA,        (void **)&o_MoveFileExA },
    { "KERNEL32.dll", "CopyFileA",          (void *)f_CopyFileA,          (void **)&o_CopyFileA },
    { "KERNEL32.dll", "FindFirstFileA",     (void *)f_FindFirstFileA,     (void **)&o_FindFirstFileA },
    { "KERNEL32.dll", "FindNextFileA",      (void *)f_FindNextFileA,      (void **)&o_FindNextFileA },
    { "KERNEL32.dll", "GetFullPathNameA",   (void *)f_GetFullPathNameA,   (void **)&o_GetFullPathNameA },
    { "KERNEL32.dll", "GetShortPathNameA",  (void *)f_GetShortPathNameA,  (void **)&o_GetShortPathNameA },
    { "USER32.dll",   "SetWindowTextA",     (void *)f_SetWindowTextA,     (void **)&o_SetWindowTextA },
};
static FsHook g_fs_exe[] = {            /* only the exe imports these */
    { "MSVCRT.dll", "_access",   (void *)f_access, (void **)&o_access },
    { "MSVCRT.dll", "_stat",     (void *)f_stat,   (void **)&o_stat },
    { "MSVCRT.dll", "_mkdir",    (void *)f_mkdir,  (void **)&o_mkdir },
    { "MSVCRT.dll", "_rmdir",    (void *)f_rmdir,  (void **)&o_rmdir },
    { "MSVCRT.dll", "_chdir",    (void *)f_chdir,  (void **)&o_chdir },
    { "MSVCRT.dll", "_fullpath", (void *)f_fullpath, (void **)&o_fullpath },
    { "SHELL32.dll", "DragQueryFileA", (void *)f_DragQueryFileA, (void **)&o_DragQueryFileA },
    { "SHELL32.dll", "ShellExecuteA",  (void *)f_ShellExecuteA,  (void **)&o_ShellExecuteA },
};

void Fs_Install(HMODULE exe)
{
    HMODULE mfc = GetModuleHandleA("MFC42.DLL");
    for (size_t i = 0; i < sizeof g_fs / sizeof g_fs[0]; i++) {
        int a = Hook_Iat(exe, g_fs[i].dll, g_fs[i].name, g_fs[i].repl, g_fs[i].orig);
        int b = mfc ? Hook_Iat(mfc, g_fs[i].dll, g_fs[i].name, g_fs[i].repl, g_fs[i].orig) : -1;
        Slot_Log("fs hook %s: exe=%d mfc42=%d", g_fs[i].name, a, b);
    }
    for (size_t i = 0; i < sizeof g_fs_exe / sizeof g_fs_exe[0]; i++) {
        int a = Hook_Iat(exe, g_fs_exe[i].dll, g_fs_exe[i].name, g_fs_exe[i].repl, g_fs_exe[i].orig);
        Slot_Log("fs hook %s: exe=%d", g_fs_exe[i].name, a);
    }
    patch_cmdline();
}
