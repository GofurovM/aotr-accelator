@echo off
rem build.bat - builds out\bfme2_accel.dll and out\aotr_start.exe with the MSVC Build Tools (x86).
rem   build.bat        release build: what players use (no read-out on the screen, no measurement)
rem   build.bat diag   diagnostics build: F7 panel, aotr_report.txt and the measurement lines in the log
setlocal
set "DIAG="
if /i "%~1"=="diag" set "DIAG=/DAOTR_DIAG"
rem find vcvars32.bat: AOTR_VCVARS if set, else vswhere, else the usual places
set "VCVARS=%AOTR_VCVARS%"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
)
if not defined VCVARS set "VCVARS=D:\VS2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" ( echo vcvars32.bat not found - install "Visual Studio Build Tools" with "Desktop development with C++", or set AOTR_VCVARS to its vcvars32.bat & exit /b 1 )
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo vcvars32 FAILED & exit /b 1 )
cd /d "%~dp0src"
if not exist "%~dp0out" mkdir "%~dp0out"
echo === [1/3] rpmalloc ===
cl /nologo /O2 /MT /W3 /I..\vendor\rpmalloc /c ..\vendor\rpmalloc\rpmalloc.c
if errorlevel 1 ( echo RPMALLOC FAILED & exit /b 5 )
echo === [2/3] bfme2_accel.dll %DIAG% ===
cl /nologo /O2 /MT /W3 /DAOTR_PROD %DIAG% /LD /I..\vendor\rpmalloc /Fe:"%~dp0out\bfme2_accel.dll" aotr_accel.cpp rpmalloc.obj /link kernel32.lib user32.lib advapi32.lib gdi32.lib /MAP:"%~dp0out\bfme2_accel.map"
if errorlevel 1 ( echo DLL BUILD FAILED & exit /b 2 )
echo === [3/3] aotr_start.exe (the starter: install, tray, autostart) ===
rc /nologo /fo aotr_start.res aotr_start.rc
if errorlevel 1 ( echo STARTER RESOURCES FAILED & exit /b 3 )
rem asInvoker: it asks for administrator rights itself, only when it installs (the autostart task gives them afterwards)
cl /nologo /O2 /MT /W3 /Fe:"%~dp0out\aotr_start.exe" aotr_start.cpp aotr_start.res /link /SUBSYSTEM:WINDOWS kernel32.lib user32.lib advapi32.lib comdlg32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib taskschd.lib /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'"
if errorlevel 1 ( echo STARTER BUILD FAILED & exit /b 3 )
echo === done ===
dir /b "%~dp0out\bfme2_accel.dll" "%~dp0out\aotr_start.exe"
