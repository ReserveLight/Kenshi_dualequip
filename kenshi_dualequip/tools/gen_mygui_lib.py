#!/usr/bin/env python3
"""Generate MyGUIEngine_x64.lib (import library) from the game's DLL exports.

Kenshi ships MyGUIEngine_x64.dll but no .lib; linking the MyGUI C++ API needs
one. Run this once after a game update:

    python tools/gen_mygui_lib.py "C:/Steam/steamapps/common/Kenshi/MyGUIEngine_x64.dll"

The output goes to third_party/MyGUIEngine_x64.lib (and a generated .def).
`lib.exe` must be on PATH (run from a Visual Studio Developer PowerShell, or set
the LIBEXE environment variable to the full path of lib.exe).
"""
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, "third_party")
DEF = os.path.join(OUTDIR, "MyGUIEngine_x64.def")
LIB = os.path.join(OUTDIR, "MyGUIEngine_x64.lib")


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    n = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    off = opt + optsz
    out = []
    for i in range(n):
        b = off + i * 40
        name = data[b:b + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, ra = struct.unpack_from("<IIII", data, b + 8)
        out.append((name, va, vs, ra, rs))
    return opt, out


def r2o(rva, secs):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)
    return None


def main():
    if len(sys.argv) > 1:
        dll = sys.argv[1]
    else:
        game_dir = os.environ.get("KENSHI_DIR")
        if not game_dir:
            sys.exit('usage: python tools/gen_mygui_lib.py "<Kenshi>/MyGUIEngine_x64.dll"')
        dll = os.path.join(game_dir, "MyGUIEngine_x64.dll")
    if not os.path.isfile(dll):
        sys.exit("DLL not found: " + dll)

    libexe = os.environ.get("LIBEXE") or shutil.which("lib")
    if not libexe:
        sys.exit("lib.exe not found - run from a VS Developer PowerShell or set LIBEXE")

    os.makedirs(OUTDIR, exist_ok=True)
    data = open(dll, "rb").read()
    opt, secs = sections(data)
    eo = r2o(struct.unpack_from("<I", data, opt + 112)[0], secs)
    nfun, nnames = struct.unpack_from("<II", data, eo + 20)
    afun, anames, aord = struct.unpack_from("<III", data, eo + 28)
    names = []
    for i in range(nnames):
        nr = struct.unpack_from("<I", data, r2o(anames, secs) + 4 * i)[0]
        off = r2o(nr, secs)
        end = data.index(b"\0", off)
        names.append(data[off:end].decode("latin1"))
    names = sorted(set(names))

    with open(DEF, "w", encoding="ascii", errors="replace") as f:
        f.write("LIBRARY MyGUIEngine_x64\nEXPORTS\n")
        for n in names:
            f.write("    " + n + "\n")
    print("exports:", len(names))
    subprocess.check_call([libexe, "/def:" + DEF, "/machine:x64", "/out:" + LIB])
    print("wrote", LIB)


if __name__ == "__main__":
    main()
