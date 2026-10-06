import pefile, struct, collections
pe=pefile.PE('peggypro_orig.exe')
base=pe.OPTIONAL_HEADER.ImageBase
iat={}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports:
        if i.name: iat[i.address]=(e.dll.decode(),i.name.decode())
data=open('peggypro_orig.exe','rb').read()
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
raw=data[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
tb=base+text.VirtualAddress
cnt=collections.Counter()
# call dword ptr [iat] = FF 15 imm32 ; jmp = FF 25
for i in range(len(raw)-6):
    if raw[i]==0xFF and raw[i+1] in (0x15,0x25):
        a=struct.unpack_from('<I',raw,i+2)[0]
        if a in iat: cnt[iat[a]]+=1
import sys
keys=('A','W')
print("== user32/gdi32/kernel32 ANSI call sites ==")
for (d,n),c in sorted(cnt.items(),key=lambda x:-x[1]):
    if d.upper() in('USER32.DLL','GDI32.DLL','KERNEL32.DLL','COMDLG32.DLL','SHELL32.DLL','IMM32.DLL','ADVAPI32.DLL') : print(f"{c:5d} {d}:{n}")
