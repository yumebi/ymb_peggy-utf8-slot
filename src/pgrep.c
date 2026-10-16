/*
 * "Grep from files": Peggy runs the bundled pgrep.exe and parses what it prints.  pgrep reads UTF-8 / UTF-16 /
 * EUC / JIS files, converts them to CP932 with WideCharToMultiByte (everything else becomes '?'), matches its
 * pattern against that and prints "file:line: text".
 *
 * With this DLL imported by pgrep.exe too, the same slot table (shared through slots.dat) applies there: the
 * conversion yields the slot bytes Peggy knows, the pattern Peggy sends matches them, and file names with such
 * characters can be opened.  Peggy merges the slots pgrep assigned before it reads the output.
 */
#include "slot.h"
#include <string.h>

void Slot_Log(const char *fmt, ...);

/* ---- pgrep side: file names with slot characters, and 4-byte UTF-8 (its reader drops those) ---- */
extern int g_io_all_reads;
extern DWORD g_io_read_cap;
extern void *o_fopen, *o_fclose;
void *Io_fopen_addr(void);
void *Io_fclose_addr(void);
int Hook_Iat(HMODULE mod, const char *dll, const char *name, void *repl, void **orig);

void Pg_Install(HMODULE exe)
{
    g_io_all_reads = 1;
    g_io_read_cap = 64u << 20;                          /* larger files are read as they are */
    int a = Hook_Iat(exe, "MSVCRT.dll", "fopen", Io_fopen_addr(), &o_fopen);
    int b = Hook_Iat(exe, "MSVCRT.dll", "fclose", Io_fclose_addr(), &o_fclose);
    Slot_Log("pgrep: fopen hook=%d fclose hook=%d", a, b);
}

/* ---- Peggy side: pick up slots pgrep assigned before the output is read ---- */
static BOOL (WINAPI *o_PeekNamedPipe)(HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD);
static BOOL WINAPI h_PeekNamedPipe(HANDLE h, LPVOID buf, DWORD sz, LPDWORD rd, LPDWORD avail, LPDWORD left)
{
    BOOL r = o_PeekNamedPipe(h, buf, sz, rd, avail, left);
    if (r && avail && *avail) Slot_ReloadPersisted();            /* data is coming from a child: merge its slots */
    return r;
}

void Pg_InstallPeggy(HMODULE exe)
{
    int r = Hook_Iat(exe, "KERNEL32.dll", "PeekNamedPipe", (void *)h_PeekNamedPipe, (void **)&o_PeekNamedPipe);
    Slot_Log("peggy: PeekNamedPipe hook=%d", r);
}
