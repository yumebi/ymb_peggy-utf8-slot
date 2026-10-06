#pragma once
#include <windows.h>

/* slot-aware replacements for the ANSI<->UTF-16 conversion (CP_ACP == CP932 only) */
int WINAPI Slot_MultiByteToWideChar(UINT cp, DWORD fl, LPCCH src, int cb, LPWSTR dst, int cch);
int WINAPI Slot_WideCharToMultiByte(UINT cp, DWORD fl, LPCWCH src, int cch, LPSTR dst, int cb,
                                    LPCCH defch, LPBOOL used);
void Slot_Init(void);
int  Slot_Capacity(void);
int  Slot_Used(void);
/* if s[0..1] is an assigned slot pair: returns 2 and the code point, else 0 */
int  Slot_DecodeAt(const char *s, int n, unsigned *cp);
int  Slot_HasSupplementary(void);
/* one code point -> ANSI bytes (native CP932 or slot). returns 1/2 bytes, 0 if slots exhausted.
   *is_slot is set when a slot had to be used. */
int  Slot_CodePointToAnsi(unsigned cp, unsigned char b[2], int *is_slot);
int  Slot_IsNative(unsigned cp);
