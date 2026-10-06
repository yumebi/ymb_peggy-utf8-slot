#include "slot.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } }while(0)
int main(void)
{
    Slot_Init();
    printf("ACP=%u slots=%d\n", GetACP(), Slot_Capacity());
    /* nichi hon, U+301C, e-acute, U+1F600, U+20BB7, 'A', U+2016 */
    const WCHAR w[] = { 0x65E5,0x672C,0x301C,0x00E9,0xD83D,0xDE00,0xD842,0xDFB7,'A',0x2016,0 };
    char a[64];
    BOOL used;
    int n = Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof a, NULL, &used);
    printf("W->A bytes=%d used=%d : ", n, used);
    for (int i = 0; i < n; i++) printf("%02X ", (unsigned char)a[i]);
    printf("\nslots used=%d\n", Slot_Used());
    CHECK(n > 0 && !used);
    WCHAR back[64];
    int m = Slot_MultiByteToWideChar(CP_ACP, 0, a, n, back, 64);
    CHECK(m == 11);
    CHECK(memcmp(back, w, sizeof w) == 0);
    int used0 = Slot_Used();
    char a2[64];
    int n2 = Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, a2, sizeof a2, NULL, &used);
    CHECK(n2 == n && memcmp(a, a2, n) == 0 && Slot_Used() == used0);
    CHECK(Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL) == n);
    CHECK(Slot_MultiByteToWideChar(CP_ACP, 0, a, n, NULL, 0) == 11);
    const char sj[] = "\x93\xfa\x96\x7b\x8c\xea"; /* nihongo in SJIS */
    WCHAR sw[8];
    CHECK(Slot_MultiByteToWideChar(CP_ACP, 0, sj, 6, sw, 8) == 3 && sw[0] == 0x65E5);
    printf(fails ? "NG (%d)\n" : "ALL OK\n", fails);
    return fails;
}
