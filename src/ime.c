/*
 * Direct input of characters outside CP932.
 *
 * Peggy's windows are ANSI, so the OS converts IME / Unicode input to the system code page
 * and everything outside CP932 turns into '?'.  Two entry points are fixed up here:
 *   1. IME: the editor window is subclassed; WM_IME_COMPOSITION(GCS_RESULTSTR) is read as
 *      UTF-16 and, when it holds characters CP932 cannot express, re-fed as WM_IME_CHAR
 *      with slot bytes.
 *   2. Unicode packets (emoji panel, on-screen keyboard: VK_PACKET): TranslateMessage is
 *      hooked and the same slot bytes are posted instead of the lossy WM_CHAR.
 */
#include "slot.h"
#include <imm.h>
#include <shlobj.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define MAXSUB 256
static struct { HWND h; WNDPROC old; } g_sub[MAXSUB];
static HHOOK g_hook;
void Dlg_NoteFindDlg(HWND h);
void Dlg_PatchAbout(HWND h);
int Dlg_TestGetWindowText(HWND h, char *buf, int cap);
LRESULT Dlg_TestSendMessage(HWND h, UINT m, WPARAM wp, LPARAM lp);
static void log_menu_tree(HMENU m, int depth);
static DWORD WINAPI PrefillDriver(LPVOID p);
static DWORD WINAPI CtrlFDriver(LPVOID p);
static DWORD WINAPI DlgDriver(LPVOID p);
static DWORD WINAPI FindDriver(LPVOID p);
static void ll_on(void);
static void ll_off(void);
#define SELFTEST_MSG (WM_USER + 0x4242)
void Slot_Log(const char *fmt, ...);

static WNDPROC find_old(HWND h)
{
    for (int i = 0; i < MAXSUB; i++) if (g_sub[i].h == h) return g_sub[i].old;
    return NULL;
}

static void drop_sub(HWND h)
{
    for (int i = 0; i < MAXSUB; i++) if (g_sub[i].h == h) { g_sub[i].h = NULL; g_sub[i].old = NULL; }
}

void Slot_Log(const char *fmt, ...);
/* Feed one code point as ANSI bytes.  WM_IME_CHAR would make the system validate the
 * DBCS code against CP932 (slot pairs are undefined there and get replaced), so the bytes
 * are delivered as plain WM_CHAR messages, exactly what the default handling ends up doing. */
static void feed_cp(HWND h, unsigned cp, int post)
{
    unsigned char b[2];
    int n;
    if (IsWindowUnicode(h)) {                         /* Unicode edit: plain UTF-16 units, no slot detour */
        WCHAR u[2]; int k = 1;
        if (cp >= 0x10000) { u[0] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); u[1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); k = 2; }
        else u[0] = (WCHAR)cp;
        for (int i = 0; i < k; i++) {
            if (post) PostMessageW(h, WM_CHAR, u[i], 1);
            else SendMessageW(h, WM_CHAR, u[i], 1);
        }
        return;
    }
    n = Slot_CodePointToAnsi(cp, b, NULL);
    if (n == 0) { b[0] = '?'; n = 1; }
    Slot_Log("feed cp=%x n=%d b0=%02x b1=%02x used=%d", cp, n, b[0], b[1], Slot_Used());
    for (int i = 0; i < n; i++) {
        if (post) PostMessageA(h, WM_CHAR, b[i], 1);
        else SendMessageA(h, WM_CHAR, b[i], 1);
    }
}

/* returns 1 when the result string was consumed */
static int handle_result(HWND h)
{
    HIMC imc = ImmGetContext(h);
    if (!imc) return 0;
    LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0);
    if (bytes <= 0) { ImmReleaseContext(h, imc); return 0; }
    WCHAR *w = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes + 2);
    if (!w) { ImmReleaseContext(h, imc); return 0; }
    ImmGetCompositionStringW(imc, GCS_RESULTSTR, w, bytes);
    ImmReleaseContext(h, imc);
    int n = bytes / 2, need = 0;
    for (int i = 0; i < n; i++) {
        unsigned cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++;
        }
        if (!Slot_IsNative(cp)) { need = 1; break; }
    }
    if (!need) { HeapFree(GetProcessHeap(), 0, w); return 0; }
    for (int i = 0; i < n; i++) {
        unsigned cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++;
        }
        feed_cp(h, cp, 0);
    }
    HeapFree(GetProcessHeap(), 0, w);
    return 1;
}

/* ---- paste into ANSI edit controls (search box, dialogs) ----------------------------------
 * USER32's own edit control converts the Unicode clipboard to the ACP (best fit: U+301C becomes
 * U+FF5E, others become '?'), so a character copied from the document never matches the slot bytes
 * stored in the document.  Insert the slot-encoded text instead whenever the clipboard holds
 * characters CP932 cannot express. */
static int is_edit(HWND h)
{
    char c[16];
    return GetClassNameA(h, c, sizeof c) && lstrcmpiA(c, "Edit") == 0;
}

static int paste_unicode(HWND h)
{
    if (GetWindowLongA(h, GWL_STYLE) & ES_READONLY) return 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(h)) return 0;
    char *ansi = NULL;
    HANDLE hg = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *w = hg ? (const WCHAR *)GlobalLock(hg) : NULL;
    if (w) {
        int nonnative = 0;
        for (const WCHAR *q = w; *q; q++) {
            unsigned cp = *q;
            if (cp < 0x80) continue;
            if (cp >= 0xD800 && cp < 0xDC00 && q[1] >= 0xDC00 && q[1] < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (q[1] - 0xDC00); q++;
            }
            if (!Slot_IsNative(cp)) { nonnative = 1; break; }
        }
        if (nonnative) {
            int need = Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
            if (need > 0 && (ansi = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need)))
                Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, ansi, need, NULL, NULL);
        }
        GlobalUnlock(hg);
    }
    CloseClipboard();
    if (!ansi) return 0;
    SendMessageA(h, EM_REPLACESEL, TRUE, (LPARAM)ansi);
    HeapFree(GetProcessHeap(), 0, ansi);
    return 1;
}

/* Unicode Edit controls (the toolbar search box, the inner Edits of the search dialog).  The system's own handling
 * of text is right for them, except that Peggy fetches messages with the ANSI APIs, which reduces typed characters
 * to the ACP - so IME results are inserted here as real UTF-16. */
static LRESULT CALLBACK UniProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = find_old(h);
    if (!old) return DefWindowProcW(h, m, w, l);
    if (m == WM_IME_COMPOSITION && (l & GCS_RESULTSTR)) {
        HIMC imc = ImmGetContext(h);
        LONG bytes = imc ? ImmGetCompositionStringW(imc, GCS_RESULTSTR, NULL, 0) : 0;
        WCHAR *t = bytes > 0 ? (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes + 2) : NULL;
        if (t) {
            ImmGetCompositionStringW(imc, GCS_RESULTSTR, t, bytes);
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)t);
            HeapFree(GetProcessHeap(), 0, t);
        }
        if (imc) ImmReleaseContext(h, imc);
        if (t) {
            LPARAM rest = l & ~(LPARAM)(GCS_RESULTSTR | GCS_RESULTREADSTR | GCS_RESULTCLAUSE | GCS_RESULTREADCLAUSE);
            return rest ? CallWindowProcW(old, h, m, w, rest) : 0;
        }
    }
    if (m == SELFTEST_MSG) { feed_cp(h, 0x301C, 0); feed_cp(h, 0x1F600, 0); return 0; }      /* what the packet hook feeds */
    if (m == WM_NCDESTROY) drop_sub(h);
    return CallWindowProcW(old, h, m, w, l);
}

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = find_old(h);
    if (!old) return DefWindowProcA(h, m, w, l);
    if ((m == WM_PASTE || (m == WM_CHAR && w == 0x16)) && is_edit(h) && paste_unicode(h)) return 0;
    if (m == WM_IME_COMPOSITION) Slot_Log("WM_IME_COMPOSITION lp=%08x", (unsigned)l);
    if (m == WM_IME_COMPOSITION && (l & GCS_RESULTSTR) && handle_result(h)) {
        LPARAM rest = l & ~(LPARAM)(GCS_RESULTSTR | GCS_RESULTREADSTR | GCS_RESULTCLAUSE | GCS_RESULTREADCLAUSE);
        if (rest) return CallWindowProcA(old, h, m, w, rest);
        return 0;
    }
    if (m == SELFTEST_MSG && w == 8) {
        CreateThread(NULL, 0, CtrlFDriver, NULL, 0, NULL);
        return 0;
    }
    if (m == SELFTEST_MSG && w == 7) {
        HWND root = GetAncestor(h, GA_ROOT);
        Slot_Log("selftest: select all (57642), then open search dialog (57636)");
        SendMessageA(root, WM_COMMAND, 57642, 0);
        CreateThread(NULL, 0, PrefillDriver, NULL, 0, NULL);
        SendMessageA(root, WM_COMMAND, 57636, 0);
        return 0;
    }
    if (m == SELFTEST_MSG && w == 6) {
        HWND root = GetAncestor(h, GA_ROOT);
        HMENU bar = GetMenu(root);
        Slot_Log("selftest: menu tree");
        if (bar) log_menu_tree(bar, 0);
        return 0;
    }
    if (m == SELFTEST_MSG && w == 5) {
        /* open the first item of the 3rd top-level menu (Search) and paste into its edit box */
        HWND root = GetAncestor(h, GA_ROOT);
        char mnu[8] = "2";
        GetEnvironmentVariableA("SLOT_FIND_MENU", mnu, sizeof mnu);
        HMENU bar = GetMenu(root), sub = bar ? GetSubMenu(bar, atoi(mnu)) : NULL;
        char itm[8] = "0";
        GetEnvironmentVariableA("SLOT_FIND_ITEM", itm, sizeof itm);
        UINT id = sub ? GetMenuItemID(sub, atoi(itm)) : 0;
        Slot_Log("selftest: search menu item id=%u", id);
        if (sub) {
            int cnt = GetMenuItemCount(sub);
            for (int i = 0; i < cnt && i < 40; i++) {
                WCHAR t[96] = L""; char u[300];
                GetMenuStringW(sub, i, t, 96, MF_BYPOSITION);
                WideCharToMultiByte(CP_UTF8, 0, t, -1, u, sizeof u, NULL, NULL);
                Slot_Log("  menu item %d id=%u text=%s", i, GetMenuItemID(sub, i), u);
            }
        }
        if (id && id != (UINT)-1) {
            CreateThread(NULL, 0, FindDriver, NULL, 0, NULL);
            SendMessageA(root, WM_COMMAND, id, 0);
        }
        return 0;
    }
    if (m == SELFTEST_MSG && w == 4) {
        /* simulate Explorer dropping the file named by SLOT_SELFTEST_FILE (a wide DROPFILES) */
        WCHAR file[1024] = L"";
        GetEnvironmentVariableW(L"SLOT_SELFTEST_FILE", file, 1024);
        SIZE_T chars = (SIZE_T)lstrlenW(file);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (chars + 2) * sizeof(WCHAR));
        if (g) {
            DROPFILES *d = (DROPFILES *)GlobalLock(g);
            d->pFiles = sizeof(DROPFILES);
            d->fWide = TRUE;
            memcpy((BYTE *)d + sizeof(DROPFILES), file, chars * sizeof(WCHAR));
            GlobalUnlock(g);
            HWND root = GetAncestor(h, GA_ROOT);
            Slot_Log("selftest: posting WM_DROPFILES to %p", root);
            PostMessageA(root, WM_DROPFILES, (WPARAM)g, 0);
        }
        return 0;
    }
    if (m == SELFTEST_MSG && w == 3) {
        HWND root = GetAncestor(h, GA_ROOT);
        CreateThread(NULL, 0, DlgDriver, NULL, 0, NULL);
        Slot_Log("selftest: File>Open via WM_COMMAND");
        SendMessageA(root, WM_COMMAND, 0xE101, 0);
        Slot_Log("selftest: dialog returned");
        return 0;
    }
    if (m == SELFTEST_MSG && w == 2) {
        HWND root = GetAncestor(h, GA_ROOT);
        SendMessageA(h, WM_CHAR, 'x', 1);
        Slot_Log("selftest: typed x, saving via WM_COMMAND from root=%p", root);
        SendMessageA(root, WM_COMMAND, 0xE103, 0);
        return 0;
    }
    if (m == SELFTEST_MSG && w == 1) {
        HIMC imc = ImmGetContext(h);
        if (imc) {
            BOOL o = ImmSetOpenStatus(imc, TRUE);
            BOOL c = ImmSetConversionStatus(imc, IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, IME_SMODE_PHRASEPREDICT);
            Slot_Log("selftest: IME on open=%d conv=%d now=%d", o, c, ImmGetOpenStatus(imc));
            ImmReleaseContext(h, imc);
        }
        return 0;
    }
    if (m == WM_IME_NOTIFY && (w == IMN_CHANGECANDIDATE || w == IMN_OPENCANDIDATE)) {
        HIMC imc = ImmGetContext(h);
        if (imc) {
            DWORD sz = ImmGetCandidateListW(imc, 0, NULL, 0);
            CANDIDATELIST *cl = sz ? (CANDIDATELIST *)HeapAlloc(GetProcessHeap(), 0, sz) : NULL;
            if (cl && ImmGetCandidateListW(imc, 0, cl, sz)) {
                for (DWORD i = 0; i < cl->dwCount && i < 40; i++) {
                    const WCHAR *c = (const WCHAR *)((BYTE *)cl + cl->dwOffset[i]);
                    char u[128]; WideCharToMultiByte(CP_UTF8, 0, c, -1, u, sizeof u, NULL, NULL);
                    Slot_Log("cand[%lu]%s %s (U+%04X)", i, i == cl->dwSelection ? "*" : "", u, c[0]);
                }
            }
            if (cl) HeapFree(GetProcessHeap(), 0, cl);
            ImmReleaseContext(h, imc);
        }
    }
    if (m == SELFTEST_MSG) {
        /* debug: complete an IME composition from inside the process */
        static const WCHAR t[] = { 0xD83D, 0xDE00, 0xD842, 0xDFB7, 0x4E2D, 0xD55C, 0xE9, 0 };
        HIMC imc = ImmGetContext(h);
        Slot_Log("selftest: imc=%p", imc);
        if (imc) {
            BOOL r1 = ImmSetCompositionStringW(imc, SCS_SETSTR, (LPVOID)t, 7 * 2, NULL, 0);
            BOOL r2 = ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            Slot_Log("selftest: set=%d complete=%d", r1, r2);
            ImmReleaseContext(h, imc);
            if (GetEnvironmentVariableA("SLOT_SELFTEST_SAVE", NULL, 0)) SendMessageA(GetAncestor(h, GA_ROOT), WM_COMMAND, 0xE103, 0);
        }
        return 0;
    }
    if (m == WM_NCDESTROY) {
        drop_sub(h);
        return CallWindowProcA(old, h, m, w, l);
    }
    return CallWindowProcA(old, h, m, w, l);
}

static void ensure_sub(HWND h)
{
    int uni;
    if (!h || find_old(h)) return;
    uni = IsWindowUnicode(h);
    if (uni && !is_edit(h)) return;
    if (GetWindowThreadProcessId(h, NULL) != GetCurrentThreadId()) return;
    for (int i = 0; i < MAXSUB; i++) {
        if (g_sub[i].h) continue;
        WNDPROC old = uni ? (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) : (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
        if (!old || old == SubProc || old == UniProc) return;
        g_sub[i].h = h; g_sub[i].old = old;
        if (uni) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)UniProc);
        else SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
        Slot_Log("subclassed %p (%s)", h, uni ? "unicode" : "ansi");
        return;
    }
}

static DWORD g_ui_tid;
BOOL Fs_TestCreateProcess(LPSTR cmd, LPPROCESS_INFORMATION pi);

/* SLOT_SELFTEST=proc: run "cmd /c echo ok> <SLOT_SELFTEST_FILE>" through the CreateProcessA hook code, with the
 * path (which may hold slot characters) in the ANSI command line, then look for the file by its real name */
static void proc_selftest(void)
{
    WCHAR file[1024] = L"", cmdw[1200];
    char cmda[2400];
    PROCESS_INFORMATION pi;
    GetEnvironmentVariableW(L"SLOT_SELFTEST_FILE", file, 1024);
    DeleteFileW(file);
    wsprintfW(cmdw, L"cmd.exe /c echo ok> \"%ls\"", file);
    Slot_WideCharToMultiByte(CP_ACP, 0, cmdw, -1, cmda, sizeof cmda, NULL, NULL);
    memset(&pi, 0, sizeof pi);
    if (Fs_TestCreateProcess(cmda, &pi)) {
        WaitForSingleObject(pi.hProcess, 8000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        Slot_Log("proc selftest: started, file %s", GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES ? "CREATED" : "MISSING");
    } else Slot_Log("proc selftest: CreateProcess failed %lu", GetLastError());
}

static DWORD WINAPI SelfTestThread(LPVOID p)
{
    char m0[8] = "";
    (void)p;
    GetEnvironmentVariableA("SLOT_SELFTEST", m0, sizeof m0);
    if (!lstrcmpA(m0, "proc")) { Sleep(6000); proc_selftest(); return 0; }
    Sleep(14000);
    GUITHREADINFO gi; gi.cbSize = sizeof gi;
    if (GetGUIThreadInfo(g_ui_tid, &gi) && gi.hwndFocus) {
        Slot_Log("selftest: focus=%p", gi.hwndFocus);
        char mode[8] = "";
        GetEnvironmentVariableA("SLOT_SELFTEST", mode, sizeof mode);
        PostMessageA(gi.hwndFocus, SELFTEST_MSG, mode[0] == 'i' ? 1 : (mode[0] == 's' ? 2 : (mode[0] == 'o' ? 3 : (mode[0] == 'd' ? 4 : (mode[0] == 'f' ? 5 : (mode[0] == 'm' ? 6 : (mode[0] == 'p' ? 7 : (mode[0] == 'c' ? 8 : 0))))))), 0);
    } else Slot_Log("selftest: no focus window");
    return 0;
}

static BOOL CALLBACK LogChild(HWND h, LPARAM l)
{
    WCHAR c[64] = L"", t[64] = L"";
    GetClassNameW(h, c, 64);
    GetWindowTextW(h, t, 64);
    char u[160]; WideCharToMultiByte(CP_UTF8, 0, t, -1, u, sizeof u, NULL, NULL);
    Slot_Log("  child %p id=%04x class=%ls text=%s", h, GetDlgCtrlID(h), c, u);
    (void)l;
    return TRUE;
}

static DWORD WINAPI DlgDriver(LPVOID p)
{
    (void)p;
    Sleep(3500);
    GUITHREADINFO gi; gi.cbSize = sizeof gi;
    if (!GetGUIThreadInfo(g_ui_tid, &gi) || !gi.hwndActive) { Slot_Log("dlgdriver: no active window"); return 0; }
    WCHAR cls[64] = L"", ttl[128] = L"", file[1024] = L"";
    GetClassNameW(gi.hwndActive, cls, 64);
    GetWindowTextW(gi.hwndActive, ttl, 128);
    Slot_Log("dlgdriver: active=%p class=%ls", gi.hwndActive, cls);
    EnumChildWindows(gi.hwndActive, LogChild, 0);
    GetEnvironmentVariableW(L"SLOT_SELFTEST_FILE", file, 1024);
    HWND edit = NULL;
    {
        HWND cb = GetDlgItem(gi.hwndActive, 0x47c);        /* ComboBoxEx32 "File name" */
        if (cb) edit = FindWindowExW(cb, NULL, L"ComboBox", NULL);
        if (edit) edit = FindWindowExW(edit, NULL, L"Edit", NULL);
    }
    BOOL r = edit ? (BOOL)SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)file) : FALSE;
    Slot_Log("dlgdriver: set filename=%d", r);
    Sleep(600);
    PostMessageW(gi.hwndActive, WM_COMMAND, IDOK, 0);
    return 0;
}


static BOOL CALLBACK FirstEditProc(HWND h, LPARAM l)
{
    char c[32];
    GetClassNameA(h, c, sizeof c);
    if (lstrcmpiA(c, "Edit") == 0 && !*(HWND *)l) *(HWND *)l = h;
    return TRUE;
}

static void log_hex(const char *tag, const char *s)
{
    char hex[200]; int n = 0;
    for (int i = 0; s[i] && n < 190; i++) n += wsprintfA(hex + n, "%02X", (unsigned char)s[i]);
    hex[n] = 0;
    Slot_Log("%s: %s", tag, hex);
}

static BOOL CALLBACK LogEditProc(HWND h, LPARAM l)
{
    char c[32];
    (void)l;
    GetClassNameA(h, c, sizeof c);
    if (!lstrcmpiA(c, "Edit") || !lstrcmpiA(c, "ComboBox") || !lstrcmpiA(c, "ComboBoxEx32") || !lstrcmpiA(c, "RichEdit20A") || !lstrcmpiA(c, "RichEdit20W"))
        Slot_Log("  ctrl %p id=%04x class=%s unicode=%d", h, GetDlgCtrlID(h), c, IsWindowUnicode(h));
    return TRUE;
}

static BOOL CALLBACK FindNextBtnProc(HWND h, LPARAM l)
{
    WCHAR c[32] = L"", x[64] = L"";
    GetClassNameW(h, c, 32);
    if (lstrcmpiW(c, L"Button")) return TRUE;
    GetWindowTextW(h, x, 64);
    if (x[0] == 0x6B21 && x[1] == 0x3092 && !*(HWND *)l) *(HWND *)l = h;      /* next (tsugi) wo kensaku */
    return TRUE;
}

static BOOL CALLBACK StatusBarProc(HWND h, LPARAM l)
{
    WCHAR c[32] = L"";
    GetClassNameW(h, c, 32);
    if (!lstrcmpiW(c, L"msctls_statusbar32") && !*(HWND *)l) *(HWND *)l = h;
    return TRUE;
}

static void log_statusbar(HWND root)
{
    HWND sb = NULL;
    EnumChildWindows(root, StatusBarProc, (LPARAM)&sb);
    if (!sb) { Slot_Log("finddriver: no status bar"); return; }
    int parts = (int)SendMessageW(sb, 0x406 /* SB_GETPARTS */, 0, 0);
    for (int i = 0; i < parts && i < 8; i++) {
        WCHAR t[96] = L""; char u[300];
        SendMessageW(sb, 0x40D /* SB_GETTEXTW */, i, (LPARAM)t);
        WideCharToMultiByte(CP_UTF8, 0, t, -1, u, sizeof u, NULL, NULL);
        Slot_Log("  status[%d] = %s", i, u);
    }
}

static void log_menu_tree(HMENU m, int depth)
{
    int cnt = GetMenuItemCount(m);
    for (int i = 0; i < cnt && i < 80; i++) {
        WCHAR t[120] = L""; char u[360];
        GetMenuStringW(m, i, t, 120, MF_BYPOSITION);
        WideCharToMultiByte(CP_UTF8, 0, t, -1, u, sizeof u, NULL, NULL);
        UINT id = GetMenuItemID(m, i);
        if (u[0]) Slot_Log("  menu[%d] %s  id=%u", depth, u, id);
        HMENU sub = GetSubMenu(m, i);
        if (sub && depth < 3) log_menu_tree(sub, depth + 1);
    }
}

static BOOL CALLBACK DumpEditProc(HWND h, LPARAM l)
{
    char c[32];
    (void)l;
    GetClassNameA(h, c, sizeof c);
    if (!lstrcmpiA(c, "Edit")) {
        WCHAR w[200] = L""; char hex[400]; int n = 0;
        GetWindowTextW(h, w, 200);
        for (int i = 0; w[i] && n < 380; i++) n += wsprintfA(hex + n, "%04X ", w[i]);
        hex[n] = 0;
        Slot_Log("  Edit %p id=%04x unicode=%d text(UTF-16)=%s", h, GetDlgCtrlID(h), IsWindowUnicode(h), hex);
    }
    return TRUE;
}

static DWORD WINAPI PrefillDriver(LPVOID p)
{
    (void)p;
    Sleep(3500);
    GUITHREADINFO gi; gi.cbSize = sizeof gi;
    if (!GetGUIThreadInfo(g_ui_tid, &gi) || !gi.hwndActive) { Slot_Log("prefill: no active window"); return 0; }
    WCHAR t[128] = L"";
    GetWindowTextW(gi.hwndActive, t, 128);
    char u[300]; WideCharToMultiByte(CP_UTF8, 0, t, -1, u, sizeof u, NULL, NULL);
    Slot_Log("prefill: active title=%s", u);
    EnumChildWindows(gi.hwndActive, DumpEditProc, 0);
    HWND root = GetAncestor(gi.hwndActive, GA_ROOTOWNER);
    if (root) EnumChildWindows(root, DumpEditProc, 0);
    PostMessageW(gi.hwndActive, WM_CLOSE, 0, 0);
    return 0;
}

static BOOL CALLBACK FindIdProc(HWND h, LPARAM l)
{
    HWND *r = (HWND *)l;
    char c[24];
    if (!*r && GetDlgCtrlID(h) == 8018 && GetClassNameA(h, c, sizeof c) && !lstrcmpiA(c, "ComboBox")) *r = h;
    return TRUE;
}

static void log_toolbar_search(HWND root, const char *tag)
{
    HWND combo = NULL;
    EnumChildWindows(root, FindIdProc, (LPARAM)&combo);
    HWND edit = combo ? FindWindowExA(combo, NULL, "Edit", NULL) : NULL;
    char buf[200] = "";
    if (edit) SendMessageA(edit, WM_GETTEXT, sizeof buf, (LPARAM)buf);
    Slot_Log("%s: toolbar combo=%p edit=%p", tag, combo, edit);
    if (edit) log_hex(tag, buf);
}

static DWORD WINAPI CtrlFDriver(LPVOID p)
{
    (void)p;
    Sleep(2500);
    GUITHREADINFO gi; gi.cbSize = sizeof gi;
    HWND root = NULL;
    if (GetGUIThreadInfo(g_ui_tid, &gi) && gi.hwndActive) root = GetAncestor(gi.hwndActive, GA_ROOTOWNER);
    if (!root) { Slot_Log("ctrlf: no window"); return 0; }
    log_toolbar_search(root, "ctrlf: before");
    SendMessageA(root, WM_COMMAND, 57642, 0);                 /* select all */
    Sleep(300);
    SendMessageA(root, WM_COMMAND, 32786, 0);                 /* FindTool = Ctrl+F, 1st press */
    Sleep(1200);
    log_toolbar_search(root, "ctrlf: after 1st Ctrl+F");
    CreateThread(NULL, 0, PrefillDriver, NULL, 0, NULL);
    SendMessageA(root, WM_COMMAND, 32786, 0);                 /* 2nd press -> search dialog */
    return 0;
}

static BOOL CALLBACK TbComboProc(HWND h, LPARAM l)
{
    char c[16];
    if (GetDlgCtrlID(h) == 32792 && GetClassNameA(h, c, sizeof c) && !lstrcmpiA(c, "ComboBox")) { *(HWND *)l = h; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK FindDlgProc(HWND h, LPARAM l)
{
    char c[16];
    if (IsWindowVisible(h) && GetClassNameA(h, c, sizeof c) && !lstrcmpA(c, "#32770") && GetDlgItem(h, 3002)) {
        *(HWND *)l = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI FindDriver(LPVOID p)
{
    (void)p;
    Sleep(3500);
    GUITHREADINFO gi; gi.cbSize = sizeof gi;
    if (!GetGUIThreadInfo(g_ui_tid, &gi)) gi.hwndActive = NULL;
    HWND dlg = gi.hwndActive, edit = NULL;
    if (!dlg || !GetDlgItem(dlg, 3002)) {                        /* the search dialog may not be the active window (no foreground) */
        HWND f = NULL;
        EnumThreadWindows(g_ui_tid, FindDlgProc, (LPARAM)&f);
        if (f) dlg = f;
    }
    if (!dlg) { Slot_Log("finddriver: no dialog"); return 0; }
    EnumChildWindows(dlg, FirstEditProc, (LPARAM)&edit);
    {
        WCHAR ttl[128] = L""; char u[300];
        GetWindowTextW(dlg, ttl, 128);
        WideCharToMultiByte(CP_UTF8, 0, ttl, -1, u, sizeof u, NULL, NULL);
        Slot_Log("finddriver: title=%s", u);
    }
    EnumChildWindows(dlg, LogEditProc, 0);
    Slot_Log("finddriver: dlg=%p edit=%p edit_unicode=%d", dlg, edit, edit ? IsWindowUnicode(edit) : -1);
    if (!edit) return 0;

    SendMessageA(edit, WM_SETFOCUS, 0, 0);               /* lets the lazy sub-classing see this window */
    char rxv[8] = "";
    if (GetEnvironmentVariableA("SLOT_FIND_REGEX", rxv, sizeof rxv)) {      /* "regular expression" (id 3007): 1 = on, 0 = off */
        HWND rx = GetDlgItem(dlg, 3007);
        int want = rxv[0] != '0';
        if (rxv[0] == '?') want = rx ? SendMessageW(rx, BM_GETCHECK, 0, 0) == BST_CHECKED : 0;      /* only report */
        if (rx && (SendMessageW(rx, BM_GETCHECK, 0, 0) == BST_CHECKED) != want) SendMessageW(rx, BM_CLICK, 0, 0);
        Slot_Log("finddriver: regex checked=%d", rx ? (int)SendMessageW(rx, BM_GETCHECK, 0, 0) : -1);
    }
    char tbtext[64] = "";
    if (GetEnvironmentVariableA("SLOT_TB_TEXT", tbtext, sizeof tbtext)) {   /* toolbar search box: does it honour the dialog's options? */
        HWND owner = GetWindow(dlg, GW_OWNER), nxt = NULL, tb = NULL;
        EnumChildWindows(dlg, FindNextBtnProc, (LPARAM)&nxt);
        SendMessageA(edit, WM_SETTEXT, 0, (LPARAM)"a");                 /* present at column 1: commits the options without a not-found box */
        if (nxt) SendMessageW(nxt, BM_CLICK, 0, 0);
        Sleep(500);
        SendMessageW(GetDlgItem(dlg, 1), BM_CLICK, 0, 0);               /* 閉じる */
        Sleep(800);
        if (owner) log_statusbar(owner);
        EnumChildWindows(owner, TbComboProc, (LPARAM)&tb);
        Slot_Log("finddriver: toolbar combo=%p", tb);
        if (tb) {
            HWND te0 = FindWindowExA(tb, NULL, "Edit", NULL);
            if (!lstrcmpA(tbtext, "@feed") && te0) {                        /* packet-style input into the toolbar box */
                WCHAR got[32] = L""; char u[200];
                SendMessageW(te0, WM_SETFOCUS, 0, 0);
                SendMessageW(te0, WM_SETTEXT, 0, (LPARAM)L"");
                SendMessageA(te0, SELFTEST_MSG, 0, 0);
                GetWindowTextW(te0, got, 32);
                for (int i = 0, n = 0; got[i] && i < 8; i++) n += wsprintfA(u + n, "%04X ", got[i]);
                Slot_Log("finddriver: toolbar after feed: %ls", got);
                { int n = 0; for (int i = 0; got[i] && i < 8; i++) n += wsprintfA(u + n, "%04X ", got[i]); u[n] = 0; Slot_Log("finddriver: toolbar feed utf16=%s", u); }
                return 0;
            }
            if (!lstrcmpA(tbtext, "@tilde") && te0) { WCHAR tw[3] = { L'b', (WCHAR)0xFF5E, 0 }; SetWindowTextW(te0, tw); }            /* b + FULLWIDTH TILDE */
            else SendMessageA(tb, WM_SETTEXT, 0, (LPARAM)tbtext);
            HWND te = FindWindowExA(tb, NULL, "Edit", NULL);
            PostMessageA(te ? te : tb, WM_KEYDOWN, VK_RETURN, 0x001C0001);       /* Enter in the toolbar box runs the search */
            PostMessageA(te ? te : tb, WM_KEYUP, VK_RETURN, 0xC01C0001);
            Sleep(1200);
            Slot_Log("finddriver: after toolbar search:");
            log_statusbar(owner);
        }
        return 0;
    }
    static const WCHAR t[] = { 0x301C, 0 };            /* WAVE DASH, not representable in CP932 */
    if (OpenClipboard(dlg)) {
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof t);
        if (g) { memcpy(GlobalLock(g), t, sizeof t); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        CloseClipboard();
    }
    char buf[64];
    SendMessageA(edit, WM_SETTEXT, 0, (LPARAM)"");
    SendMessageA(edit, WM_PASTE, 0, 0);
    buf[0] = 0; SendMessageA(edit, WM_GETTEXT, sizeof buf, (LPARAM)buf);
    log_hex("finddriver: after WM_PASTE", buf);

    SendMessageA(edit, WM_SETTEXT, 0, (LPARAM)"");
    SendMessageA(edit, WM_CHAR, 0x16, 0);               /* Ctrl+V */
    buf[0] = 0; SendMessageA(edit, WM_GETTEXT, sizeof buf, (LPARAM)buf);
    log_hex("finddriver: after Ctrl+V", buf);

    /* what Peggy sees through its ANSI calls (hooked paths) */
    {
        char b2[64] = "";
        int n = Dlg_TestGetWindowText(edit, b2, sizeof b2);
        Slot_Log("finddriver: hooked GetWindowTextA returned %d", n);
        log_hex("finddriver: hooked GetWindowTextA text", b2);
        b2[0] = 0;
        LRESULT r = Dlg_TestSendMessage(edit, WM_GETTEXT, sizeof b2, (LPARAM)b2);
        Slot_Log("finddriver: hooked WM_GETTEXT returned %d", (int)r);
        log_hex("finddriver: hooked WM_GETTEXT text", b2);
        Slot_Log("finddriver: hooked WM_GETTEXTLENGTH = %d", (int)Dlg_TestSendMessage(edit, WM_GETTEXTLENGTH, 0, 0));
    }

    /* really search: paste again, press "find next", then read the caret column */
    SendMessageA(edit, WM_SETTEXT, 0, (LPARAM)"");
    SendMessageA(edit, WM_PASTE, 0, 0);
    HWND btn = NULL, owner = GetWindow(dlg, GW_OWNER);
    EnumChildWindows(dlg, FindNextBtnProc, (LPARAM)&btn);
    Slot_Log("finddriver: next button=%p owner=%p", btn, owner);
    if (owner) log_statusbar(owner);
    if (btn) {
        SendMessageW(btn, BM_CLICK, 0, 0);
        Sleep(1200);
        Slot_Log("finddriver: after find next:");
        if (owner) log_statusbar(owner);
    }
    PostMessageW(dlg, WM_CLOSE, 0, 0);
    return 0;
}

int Slot_LogOn(void);

/* debug: every text-related message that reaches an edit/combo box (SLOT_DEBUG only) */
static void trace_wnd_msg(const CWPSTRUCT *c)
{
    char cls[24], hex[200]; int n = 0, show = 0;
    UINT m = c->message;
    if (m != WM_SETTEXT && m != EM_REPLACESEL && m != WM_CHAR && m != WM_PASTE && m != WM_CUT && m != WM_COPY &&
        m != WM_CLEAR && m != CB_SETEDITSEL && m != EM_SETSEL && m != CB_SETCURSEL && m != CB_SELECTSTRING &&
        m != WM_IME_COMPOSITION && m != CB_ADDSTRING && m != CB_INSERTSTRING)
        return;
    if (!GetClassNameA(c->hwnd, cls, sizeof cls)) return;
    if (lstrcmpiA(cls, "Edit") && lstrcmpiA(cls, "ComboBox")) return;
    if (m == WM_SETTEXT || m == EM_REPLACESEL || m == CB_SELECTSTRING || m == CB_ADDSTRING || m == CB_INSERTSTRING) {
        show = 1;
        if (c->lParam) {
            if (IsWindowUnicode(c->hwnd)) {
                const WCHAR *w = (const WCHAR *)c->lParam;
                for (int i = 0; w[i] && i < 30; i++) n += wsprintfA(hex + n, "%04X ", w[i]);
            } else {
                const unsigned char *a = (const unsigned char *)c->lParam;
                for (int i = 0; a[i] && i < 40; i++) n += wsprintfA(hex + n, "%02X", a[i]);
            }
        }
    }
    hex[n] = 0;
    if (m == CB_ADDSTRING || m == CB_INSERTSTRING) return;       /* history population is noisy */
    Slot_Log("MSG hwnd=%p id=%04x %s unicode=%d msg=%04X wp=%X %s%s", c->hwnd, GetDlgCtrlID(c->hwnd), cls,
             IsWindowUnicode(c->hwnd), m, (unsigned)c->wParam, show ? "text=" : "", hex);
}

static LRESULT CALLBACK CallWndHook(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0) {
        const CWPSTRUCT *c = (const CWPSTRUCT *)lp;
        if (c->message == WM_SETFOCUS || c->message == WM_IME_SETCONTEXT || c->message == WM_IME_STARTCOMPOSITION)
            ensure_sub(c->hwnd);
        if (c->message == WM_SHOWWINDOW && c->wParam) { Dlg_NoteFindDlg(c->hwnd); Dlg_PatchAbout(c->hwnd); }
        else if (c->message == WM_COMMAND && (LOWORD(c->wParam) == 3007 || LOWORD(c->wParam) == 3004 || LOWORD(c->wParam) == 3001))
            Dlg_NoteFindDlg(c->hwnd);
        if (Slot_LogOn()) trace_wnd_msg(c);
        if (c->message == WM_ACTIVATEAPP) { if (c->wParam) ll_on(); else ll_off(); }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

/* ---- Unicode packets (VK_PACKET: emoji panel, on-screen keyboard, SendInput) ----
 * Peggy fetches messages with the ANSI APIs, so the system has already reduced the packet
 * to a lossy WM_CHAR by the time it reaches TranslateMessage.  A low-level keyboard hook
 * still sees the UTF-16 unit (KBDLLHOOKSTRUCT.scanCode). */
void Slot_Log(const char *fmt, ...);
static HHOOK g_ll;
static HINSTANCE g_inst;
static LRESULT CALLBACK LlHook(int code, WPARAM wp, LPARAM lp);
static void ll_on(void) { if (!g_ll) g_ll = SetWindowsHookExA(WH_KEYBOARD_LL, LlHook, g_inst, 0); }
static void ll_off(void) { if (g_ll) { UnhookWindowsHookEx(g_ll); g_ll = NULL; } }
static WCHAR g_hi;

static LRESULT CALLBACK LlHook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        if (k->vkCode == VK_PACKET) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId()) {
                int up = (k->flags & LLKHF_UP) != 0;
                WCHAR ch = (WCHAR)k->scanCode;
                Slot_Log("LL packet ch=%04x up=%d", ch, up);
                unsigned cp = ch;
                int swallow = 0, deliver = 0;
                if (ch >= 0xD800 && ch < 0xDC00) { if (!up) g_hi = ch; swallow = 1; }
                else if (ch >= 0xDC00 && ch < 0xE000 && g_hi) {
                    if (!up) { cp = 0x10000 + (((unsigned)g_hi - 0xD800) << 10) + (ch - 0xDC00); g_hi = 0; deliver = 1; }
                    swallow = 1;
                } else if (!Slot_IsNative(cp)) { swallow = 1; deliver = !up; }
                if (deliver) {
                    HWND tgt = GetFocus();
                    if (tgt) feed_cp(tgt, cp, 1);
                }
                if (swallow) return 1;
            }
        }
    }
    return CallNextHookEx(g_ll, code, wp, lp);
}

void Ime_Init(HINSTANCE inst)
{
    g_ui_tid = GetCurrentThreadId();
    if (GetEnvironmentVariableA("SLOT_SELFTEST", NULL, 0)) CreateThread(NULL, 0, SelfTestThread, NULL, 0, NULL);
    g_hook = SetWindowsHookExA(WH_CALLWNDPROC, CallWndHook, NULL, GetCurrentThreadId());
    g_inst = inst;
    Slot_Log("Ime_Init: callwnd=%p", g_hook);
}
