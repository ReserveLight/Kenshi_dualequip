@echo off
REM ===========================================================================
REM  Build the two DualEquip plugin DLLs that ship.
REM
REM  Toolchain: MSVC v143 (VS2022; edit VSDIR below)
REM  No KenshiLib.lib, no boost, no VS2010 needed: every engine function is
REM  resolved at runtime by mangled name out of KenshiLib.dll.
REM
REM  HISTORY - do not re-add these. They lived here until 2026-10-06 and only
REM  produced dead DLLs that were never deployed:
REM      DualEquip.cpp        -> DualEquip.dll        (first attempt, abandoned)
REM      DualEquipTest.cpp    -> DualEquipTest.dll    (autoload/slot probe)
REM      DualEquipSlots.cpp   -> DualEquipSlots.dll   (v1, superseded by ...Slots2)
REM      DualEquipProbe2.cpp  -> DualEquipProbe2.dll  (appearance-name probe)
REM      ProbeSlot.cpp        -> DualEquipSlotProbe.dll (probe)
REM      AutoLoad.cpp         (never referenced by any build)
REM ===========================================================================
setlocal
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=%~dp0"
set "OUTDIR=%ROOT%build"
REM MyGUI UI support: put MyGUI headers under third_party\MyGUI\Include;
REM generate the import library from the game's MyGUIEngine_x64.dll with
REM tools\gen_mygui_lib.py (run once; output goes to third_party\).
set "MYGUI_INC=%ROOT%third_party\MyGUI\Include"
set "MYGUI_LIB=%ROOT%third_party\MyGUIEngine_x64.lib"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

if not exist "%VCVARS%" ( echo [ERROR] vcvars64.bat not found at %VCVARS% & exit /b 1 )
if not exist "%MYGUI_LIB%" ( echo [ERROR] missing %MYGUI_LIB% - run tools\gen_mygui_lib.py first & exit /b 1 )
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [ERROR] failed to init MSVC environment & exit /b 1 )

set FAILED=0

echo.
echo === DualEquipSlots2.dll (extra equipment slots + layout + guards + UI) ===
cl /nologo /LD /MT /O2 /EHsc /W3 /GS- /I"%MYGUI_INC%" /Fo:"%OUTDIR%\\" /Fe:"%OUTDIR%\DualEquipSlots2.dll" "%ROOT%src\DualEquipSlots2.cpp" /link user32.lib "%MYGUI_LIB%"
if errorlevel 1 ( echo [ERROR] DualEquipSlots2.dll FAILED & exit /b 1 )

echo.
echo === DualEquip2.dll (hide/show appearance + hotkeys) ===
cl /nologo /LD /MT /O2 /EHsc /W3 /GS- /Fo:"%OUTDIR%\\" /Fe:"%OUTDIR%\DualEquip2.dll" "%ROOT%src\DualEquip2.cpp" /link user32.lib
if errorlevel 1 ( echo [ERROR] DualEquip2.dll FAILED & exit /b 1 )

echo.
echo ALL BUILDS OK -^> %OUTDIR%
endlocal
