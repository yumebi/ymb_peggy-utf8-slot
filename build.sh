#!/bin/bash
# Build utf8slot.dll and a patched copy of peggypro.exe from YOUR OWN original Peggy Pro 4.63 exe.
#
#   usage:  ./build.sh "/c/Program Files (x86)/Anchor/Peggy/peggypro.exe.bak"
#           CC=/path/to/i686-w64-mingw32-clang ./build.sh original.exe
#
# The original exe must be the unmodified 4.63 build (2,383,872 bytes).
# pgrep.exe (the grep tool behind "Search in files") is patched too, from the file next to the exe
# or from the path in $2 (unmodified, 65,536 bytes).
set -e
cd "$(dirname "$0")"

ORIG="${1:?usage: ./build.sh <original peggypro.exe (4.63, unmodified)>}"
CC="${CC:-i686-w64-mingw32-clang}"

size=$(wc -c < "$ORIG")
if [ "$size" != "2383872" ]; then
    echo "warning: $ORIG is $size bytes; Peggy Pro 4.63 is 2383872 bytes. Other builds are not supported." >&2
fi

mkdir -p build dist

$CC -O2 -Wall -shared -static -Wl,--kill-at -o build/utf8slot.dll \
    src/hook.c src/slot.c src/io.c src/ime.c src/fs.c src/dlg.c src/pgrep.c \
    -luser32 -lgdi32 -limm32 -lshell32 -ladvapi32 -lcomdlg32
$CC -O2 -Wall -o build/test.exe src/test.c src/slot.c -static -luser32
./build/test.exe

python tools/patch_import.py "$ORIG" build/peggypro.exe utf8slot.dll
PGREP="${2:-$(dirname "$ORIG")/pgrep.exe}"
if [ -f "$PGREP" ]; then
    python tools/patch_import.py "$PGREP" build/pgrep.exe utf8slot.dll
    cp build/pgrep.exe dist/
else
    echo "warning: pgrep.exe not found ($PGREP); \"Search in files\" will not handle non-CP932 characters" >&2
fi

cp build/peggypro.exe build/utf8slot.dll dist/
echo "OK: dist/peggypro.exe + dist/utf8slot.dll"
echo "Installer (optional, Inno Setup 6):  ISCC.exe installer/peggy_utf8.iss"
