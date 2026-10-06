#include "slot.h"
#include <stdint.h>
#include <string.h>

#define MAXSLOT 4096
#define CP_SJIS 932

static CRITICAL_SECTION g_cs;
static int       g_acp932;
static uint16_t  g_slot_pair[MAXSLOT];     /* slot index -> (lead<<8|trail) */
static uint32_t  g_slot_cp[MAXSLOT];       /* slot index -> code point (0 = unassigned) */
static int       g_nslot, g_nused, g_has_supp;
static int16_t   g_pair2slot[65536];       /* pair -> slot index, -1 = not a slot */
static uint8_t   g_lead_has_slot[256];
static uint32_t  g_cache[0x10000];         /* BMP code point -> ansi bytes cache */
#define HASHN 16384
static int32_t   g_hash[HASHN];            /* code point -> slot index, -1 empty */

#define CACHE_NONE   0xFF000000u
#define CACHE_ONE(b) (0x01000000u | (b))
#define CACHE_TWO(p) (0x02000000u | (p))

static int is_lead(uint8_t b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }

void Slot_Init(void)
{
    static int done;
    if (done) return;
    done = 1;
    InitializeCriticalSection(&g_cs);
    g_acp932 = (GetACP() == CP_SJIS);
    memset(g_pair2slot, 0xFF, sizeof g_pair2slot);
    memset(g_hash, 0xFF, sizeof g_hash);
    if (!g_acp932) return;
    /* pass 0: user-defined area (lead F0-F9) first - these are legal DBCS cells and survive
     * Peggy's keyboard-input validation; pass 1: remaining undefined cells (paste/IME only). */
    for (int pass = 0; pass < 2; pass++) {
        for (int l = 0x81; l <= 0xFC; l++) {
            if (!is_lead((uint8_t)l)) continue;
            if ((pass == 0) != (l >= 0xF0 && l <= 0xF9)) continue;
            for (int t = 0x40; t <= 0xFC; t++) {
                if (t == 0x7F) continue;
                char b[2] = { (char)l, (char)t };
                WCHAR w[2];
                int n = MultiByteToWideChar(CP_SJIS, MB_ERR_INVALID_CHARS, b, 2, w, 2);
                int freeslot = (n == 0) || (n == 1 && w[0] >= 0xE000 && w[0] <= 0xF8FF);
                if (!freeslot || g_nslot >= MAXSLOT) continue;
                g_slot_pair[g_nslot] = (uint16_t)((l << 8) | t);
                g_pair2slot[(l << 8) | t] = (int16_t)g_nslot;
                g_lead_has_slot[l] = 1;
                g_nslot++;
            }
        }
    }
}

int Slot_Capacity(void) { return g_nslot; }
int Slot_Used(void)     { return g_nused; }

/* code point -> real CP932 bytes (1 or 2). CACHE_NONE = not representable */
static uint32_t native_lookup(uint32_t cp)
{
    if (cp >= 0x10000) return CACHE_NONE;
    uint32_t c = g_cache[cp];
    if (c) return c;
    WCHAR w = (WCHAR)cp;
    char buf[4];
    BOOL used = FALSE;
    int n = WideCharToMultiByte(CP_SJIS, WC_NO_BEST_FIT_CHARS, &w, 1, buf, 4, NULL, &used);
    if (n == 1 && !used) c = CACHE_ONE((uint8_t)buf[0]);
    else if (n == 2 && !used) {
        uint16_t p = (uint16_t)(((uint8_t)buf[0] << 8) | (uint8_t)buf[1]);
        c = g_pair2slot[p] >= 0 ? CACHE_NONE : CACHE_TWO(p); /* never alias a slot pair */
    } else c = CACHE_NONE;
    g_cache[cp] = c;
    return c;
}

static int hash_find(uint32_t cp)
{
    uint32_t h = (cp * 2654435761u) & (HASHN - 1);
    for (;;) {
        int32_t s = g_hash[h];
        if (s < 0) return -1;
        if (g_slot_cp[s] == cp) return s;
        h = (h + 1) & (HASHN - 1);
    }
}

static int slot_for(uint32_t cp)
{
    EnterCriticalSection(&g_cs);
    int s = hash_find(cp);
    if (s < 0 && g_nused < g_nslot) {
        s = g_nused++;
        g_slot_cp[s] = cp;
        if (cp >= 0x10000) g_has_supp = 1;
        uint32_t h = (cp * 2654435761u) & (HASHN - 1);
        while (g_hash[h] >= 0) h = (h + 1) & (HASHN - 1);
        g_hash[h] = s;
    }
    LeaveCriticalSection(&g_cs);
    return s;
}

/* ---- UTF-16 -> ANSI ---- */
int WINAPI Slot_WideCharToMultiByte(UINT cp, DWORD fl, LPCWCH src, int cch, LPSTR dst, int cb,
                                    LPCCH defch, LPBOOL used)
{
    if (!g_acp932 || (cp != CP_ACP && cp != CP_SJIS) || !src)
        return WideCharToMultiByte(cp, fl, src, cch, dst, cb, defch, used);
    if (cch < 0) cch = lstrlenW(src) + 1;
    if (used) *used = FALSE;
    int out = 0;
    for (int i = 0; i < cch; i++) {
        uint32_t c = src[i];
        uint8_t b[2] = {0, 0};
        int nb;
        if (c < 0x80) { b[0] = (uint8_t)c; nb = 1; }
        else {
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < cch && src[i + 1] >= 0xDC00 && src[i + 1] < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00);
                i++;
            }
            uint32_t r = native_lookup(c);
            if ((r >> 24) == 1) { b[0] = (uint8_t)r; nb = 1; }
            else if ((r >> 24) == 2) { b[0] = (uint8_t)(r >> 8); b[1] = (uint8_t)r; nb = 2; }
            else {
                int s = slot_for(c);
                if (s >= 0) { b[0] = (uint8_t)(g_slot_pair[s] >> 8); b[1] = (uint8_t)g_slot_pair[s]; nb = 2; }
                else { b[0] = '?'; nb = 1; if (used) *used = TRUE; }
            }
        }
        if (cb > 0) {
            if (out + nb > cb) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            dst[out] = (char)b[0];
            if (nb == 2) dst[out + 1] = (char)b[1];
        }
        out += nb;
    }
    return out;
}

/* ---- ANSI -> UTF-16 ---- */
static int has_slot_pair(const uint8_t *s, int cb)
{
    for (int i = 0; i < cb; i++) {
        if (g_lead_has_slot[s[i]] && i + 1 < cb) {
            int sl = g_pair2slot[(s[i] << 8) | s[i + 1]];
            if (sl >= 0 && g_slot_cp[sl] != 0) return 1;
        }
        if (is_lead(s[i])) i++;
    }
    return 0;
}

int WINAPI Slot_MultiByteToWideChar(UINT cp, DWORD fl, LPCCH src, int cb, LPWSTR dst, int cch)
{
    if (!g_acp932 || (cp != CP_ACP && cp != CP_SJIS) || !src || g_nused == 0)
        return MultiByteToWideChar(cp, fl, src, cb, dst, cch);
    if (cb < 0) cb = lstrlenA(src) + 1;
    const uint8_t *s = (const uint8_t *)src;
    if (!has_slot_pair(s, cb)) return MultiByteToWideChar(cp, fl, src, cb, dst, cch);

    int out = 0, i = 0;
    while (i < cb) {
        int j = i;
        int hit = -1;
        while (j < cb) {
            if (g_lead_has_slot[s[j]] && j + 1 < cb) {
                int sl = g_pair2slot[(s[j] << 8) | s[j + 1]];
                if (sl >= 0 && g_slot_cp[sl]) { hit = sl; break; }
            }
            j += (is_lead(s[j]) && j + 1 < cb) ? 2 : 1;
        }
        int seglen = j - i;
        if (seglen > 0) {
            int n = MultiByteToWideChar(CP_SJIS, 0, (LPCCH)s + i, seglen, cch > 0 ? dst + out : NULL,
                                        cch > 0 ? cch - out : 0);
            if (n == 0) return 0;
            out += n;
        }
        if (hit >= 0) {
            uint32_t c = g_slot_cp[hit];
            int nw = c >= 0x10000 ? 2 : 1;
            if (cch > 0) {
                if (out + nw > cch) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
                if (nw == 2) { c -= 0x10000; dst[out] = (WCHAR)(0xD800 + (c >> 10)); dst[out + 1] = (WCHAR)(0xDC00 + (c & 0x3FF)); }
                else dst[out] = (WCHAR)c;
            }
            out += nw;
            i = j + 2;
        } else i = j;
    }
    return out;
}

int Slot_DecodeAt(const char *s, int n, unsigned *cp)
{
    if (n < 2 || g_nused == 0) return 0;
    uint8_t l = (uint8_t)s[0];
    if (!g_lead_has_slot[l]) return 0;
    int sl = g_pair2slot[(l << 8) | (uint8_t)s[1]];
    if (sl < 0 || !g_slot_cp[sl]) return 0;
    *cp = g_slot_cp[sl];
    return 2;
}

int Slot_HasSupplementary(void) { return g_has_supp; }

int Slot_IsNative(unsigned cp)
{
    if (!g_acp932) return 1;
    if (cp < 0x80) return 1;
    return (native_lookup(cp) >> 24) != 0xFF;
}

int Slot_CodePointToAnsi(unsigned cp, unsigned char b[2], int *is_slot)
{
    if (is_slot) *is_slot = 0;
    if (cp < 0x80 || !g_acp932) { b[0] = (unsigned char)cp; return 1; }
    uint32_t r = native_lookup(cp);
    if ((r >> 24) == 1) { b[0] = (unsigned char)r; return 1; }
    if ((r >> 24) == 2) { b[0] = (unsigned char)(r >> 8); b[1] = (unsigned char)r; return 2; }
    int s = slot_for(cp);
    if (s < 0) return 0;
    if (is_slot) *is_slot = 1;
    b[0] = (unsigned char)(g_slot_pair[s] >> 8); b[1] = (unsigned char)g_slot_pair[s];
    return 2;
}
