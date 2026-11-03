# Building Hide / Show DualEquip

## Requirements

- Windows 10/11 x64
- **Visual Studio 2022** with the "Desktop development with C++" workload (MSVC v143, Windows SDK)
- Kenshi **1.0.65 x64** installation (only for `MyGUIEngine_x64.dll`; no game files are stored in this repo)
- **MyGUI headers** (the game's MyGUI API headers). You can use the MyGUI SDK or the headers from a KenshiLib reference tree; they must be arranged as:
  ```
  third_party/MyGUI/Include/mygui/*.h
  ```
- **RE_Kenshi / KenshiLib** are runtime requirements for the mod, but **not** link-time requirements. The source resolves engine functions by mangled name at runtime.

## 1. Generate MyGUIEngine_x64.lib

Kenshi ships `MyGUIEngine_x64.dll` but no import library. Open a **Developer PowerShell for VS 2022** (so `lib.exe` is on PATH), then:

```powershell
python tools/gen_mygui_lib.py "C:\Program Files (x86)\Steam\steamapps\common\Kenshi\MyGUIEngine_x64.dll"
```

Outputs:

```
third_party/MyGUIEngine_x64.lib
third_party/MyGUIEngine_x64.def   (generated)
```

If `lib.exe` is not on PATH, set `LIBEXE` to its full path, for example:

```powershell
$env:LIBEXE = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\lib.exe"
python tools/gen_mygui_lib.py "...\MyGUIEngine_x64.dll"
```

## 2. Configure the build script

Copy the template:

```powershell
Copy-Item build_all.example.bat build_all.bat
```

Edit `build_all.bat`:

```bat
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "MYGUI_INC=%ROOT%third_party\MyGUI\Include"
set "MYGUI_LIB=%ROOT%third_party\MyGUIEngine_x64.lib"
```

- `VSDIR` must contain `VC\Auxiliary\Build\vcvars64.bat`.
- `MYGUI_INC` must be the directory whose child is `mygui/`.
- `MYGUI_LIB` is the file generated in step 1.

## 3. Build

From the repository root:

```bat
build_all.bat
```

Outputs:

```
build/DualEquipSlots2.dll
build/DualEquip2.dll
```

Both are x64 `/MT` (static CRT) DLLs. **Do not change `/MT`**: the game uses an older MSVC runtime and the MyGUI C++ ABI must not cross CRT heaps.

## 4. Deploy

Copy the outputs into the mod:

```
Kenshi/mods/Hide_Show_DualEquip/Plugins/DualEquipSlots2.dll
Kenshi/mods/Hide_Show_DualEquip/Plugins/DualEquip2.dll
```

`RE_Kenshi.json` must list both:

```json
{
    "Plugins": [
        "Plugins/DualEquipSlots2.dll",
        "Plugins/DualEquip2.dll"
    ]
}
```

Start the game with RE_Kenshi. Logs are written to the Kenshi root:

- `DualEquipSlots.log`
- `DualEquip.log`

## Troubleshooting

- **`vcvars64.bat` not found** — fix `VSDIR`.
- **missing MyGUIEngine_x64.lib** — run `tools/gen_mygui_lib.py`; check the DLL path and `lib.exe`.
- **unresolved MyGUI symbols** — the headers and the generated `.lib` must come from the same game version.
- **game crashes on open** — check the load order: this mod must be **after** Dark UI if Dark UI is used; remove other mods that replace the character inventory layout when testing.
- **`cl` not found** — run from a normal shell; the script calls `vcvars64.bat` itself, so only `VSDIR` needs to be correct.
