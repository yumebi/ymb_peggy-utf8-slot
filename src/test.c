#include "slot.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } }while(0)
/* persistence: a child process assigns slots from an empty file, another one (asking for the characters in a
 * different order, plus a new one) must get the same bytes for the characters of the first */
static int child(const char *mode)
{
    Slot_Init();
    Slot_LoadPersisted();
    WCHAR in1[] = { 0x301C, 0 }, in2[] = { 0x2016, 0x301C, 0x2260, 0 };
    char out[32];
    const WCHAR *w = mode[0] == '1' ? in1 : in2;
    int n = Slot_WideCharToMultiByte(CP_ACP, 0, w, -1, out, sizeof out, NULL, NULL);
    for (int i = 0; i < n - 1; i++) printf("%02X", (unsigned char)out[i]);
    return 0;
}

static void run_child(const char *mode, char *res, int cap)
{
    char cmd[MAX_PATH + 32], self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", self, mode);
    FILE *f = _popen(cmd, "r");
    res[0] = 0;
    if (f) { if (!fgets(res, cap, f)) res[0] = 0; _pclose(f); }
}

int main(int argc, char **argv)
{
    if (argc > 1) return child(argv[1]);
    {
        char tmp[MAX_PATH], res1[64], res2[64];
        GetTempPathA(MAX_PATH, tmp);
        lstrcatA(tmp, "slot_persist_test.dat");
        DeleteFileA(tmp);
        SetEnvironmentVariableA("SLOT_PERSIST_FILE", tmp);
        run_child("1", res1, sizeof res1);
        run_child("2", res2, sizeof res2);       /* U+2016 (native), U+301C, U+2260 (native) */
        printf("persist: first=%s second=%s\n", res1, res2);
        CHECK(strlen(res1) == 4 && strstr(res2, res1) != NULL);       /* same bytes for U+301C in both sessions */
        DeleteFileA(tmp);
        SetEnvironmentVariableA("SLOT_PERSIST_FILE", NULL);
        SetEnvironmentVariableA("SLOT_PERSIST", "0");
    }
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
