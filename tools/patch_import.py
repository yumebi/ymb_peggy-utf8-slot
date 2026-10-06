"""Add an import of <dll>!DllInit to a 32bit PE without adding sections.
Technique (same as the earlier utf8fix patch):
  * copy the import descriptor table into the slack at the end of .rdata
  * put the new IAT slot into zeroed slack of .data
Usage: patch_import.py in.exe out.exe utf8slot.dll
"""
import struct
import sys

import pefile


def main(src, dst, dllname):
    pe = pefile.PE(src)
    data = bytearray(open(src, "rb").read())
    imp = pe.OPTIONAL_HEADER.DATA_DIRECTORY[1]
    old_rva, old_size = imp.VirtualAddress, imp.Size
    sec = {s.Name.rstrip(b"\0").decode(): s for s in pe.sections}
    rdata, dsec = sec[".rdata"], sec[".data"]

    def rva2off(rva):
        return pe.get_offset_from_rva(rva)

    # new table location: end of .rdata virtual data (rounded to 16), inside the raw slack
    new_rva = (rdata.VirtualAddress + rdata.Misc_VirtualSize + 15) & ~15
    raw_end_rva = rdata.VirtualAddress + rdata.SizeOfRawData
    ndesc = old_size // 20 - 1  # without terminator
    table_len = (ndesc + 2) * 20
    ilt_rva = new_rva + table_len
    hn_rva = ilt_rva + 8
    name_rva = hn_rva + 2 + len(b"DllInit") + 1
    name_rva = (name_rva + 1) & ~1
    end_rva = name_rva + len(dllname) + 1
    assert end_rva <= raw_end_rva, "no slack in .rdata"
    assert not any(data[rva2off(new_rva): rva2off(end_rva)]), ".rdata slack not zero"

    # IAT slot inside initialised part of .data (must be zero 8 bytes)
    iat_rva = dsec.VirtualAddress + dsec.SizeOfRawData - 0x20
    # find 8 zero bytes close to the end of raw data
    iat_rva = (iat_rva + 3) & ~3
    off = rva2off(iat_rva)
    assert not any(data[off: off + 8]), ".data slot not zero"

    o = rva2off(new_rva)
    data[o: o + ndesc * 20] = data[rva2off(old_rva): rva2off(old_rva) + ndesc * 20]
    # new descriptor: OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk
    data[o + ndesc * 20: o + ndesc * 20 + 20] = struct.pack("<5I", ilt_rva, 0, 0, name_rva, iat_rva)
    # terminator already zero
    data[rva2off(ilt_rva): rva2off(ilt_rva) + 4] = struct.pack("<I", hn_rva)
    data[off: off + 4] = struct.pack("<I", hn_rva)  # IAT image before binding = hint/name rva
    data[rva2off(hn_rva) + 2: rva2off(hn_rva) + 2 + 7] = b"DllInit"
    data[rva2off(name_rva): rva2off(name_rva) + len(dllname)] = dllname.encode()

    # patch data directory (offset of import dir entry in optional header)
    opt_off = pe.OPTIONAL_HEADER.get_file_offset()
    dir_off = opt_off + 96 + 8  # DataDirectory[1]
    data[dir_off: dir_off + 8] = struct.pack("<II", new_rva, table_len)
    open(dst, "wb").write(data)
    print(f"ok: import table @RVA {new_rva:#x} size {table_len:#x}, IAT slot @RVA {iat_rva:#x}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
