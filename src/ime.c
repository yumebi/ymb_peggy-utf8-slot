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
#include <stdint.h>
#include <string.h>

#define MAXSUB 256
static struct { HWND h; WNDPROC old; } g_sub[MAXSUB];
static HHOOK g_hook;
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
    int n = Slot_CodePointToAnsi(cp, b, NULL);
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

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = find_old(h);
    if (!old) return DefWindowProcA(h, m, w, l);
    if (m == WM_IME_COMPOSITION) Slot_Log("WM_IME_COMPOSITION lp=%08x", (unsigned)l);
    if (m == WM_IME_COMPOSITION && (l & GCS_RESULTSTR) && handle_result(h)) {
        LPARAM rest = l & ~(LPARAM)(GCS_RESULTSTR | GCS_RESULTREADSTR | GCS_RESULTCLAUSE | GCS_RESULTREADCLAUSE);
        if (rest) return CallWindowProcA(old, h, m, w, rest);
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
    if (!h) return;
    if (IsWindowUnicode(h) || find_old(h)) return;
    if (GetWindowThreadProcessId(h, NULL) != GetCurrentThreadId()) return;
    for (int i = 0; i < MAXSUB; i++) {
        if (g_sub[i].h) continue;
        WNDPROC old = (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
        if (!old || old == SubProc) return;
        g_sub[i].h = h; g_sub[i].old = old;
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
        Slot_Log("subclassed %p", h);
        return;
    }
}

static DWORD g_ui_tid;
static DWORD WINAPI SelfTestThread(LPVOID p)
{
    (void)p;
    Sleep(14000);
    GUITHREADINFO gi; gi.cbSize = sizeof gi;
    if (GetGUIThreadInfo(g_ui_tid, &gi) && gi.hwndFocus) {
        Slot_Log("selftest: focus=%p", gi.hwndFocus);
        char mode[8] = "";
        GetEnvironmentVariableA("SLOT_SELFTEST", mode, sizeof mode);
        PostMessageA(gi.hwndFocus, SELFTEST_MSG, mode[0] == 'i' ? 1 : (mode[0] == 's' ? 2 : 0), 0);
    } else Slot_Log("selftest: no focus window");
    return 0;
}

static LRESULT CALLBACK CallWndHook(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0) {
        const CWPSTRUCT *c = (const CWPSTRUCT *)lp;
        if (c->message == WM_SETFOCUS || c->message == WM_IME_SETCONTEXT || c->message == WM_IME_STARTCOMPOSITION)
            ensure_sub(c->hwnd);
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
