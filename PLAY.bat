@echo off
rem PLAY.bat - builds the accelerator, then starts Age of the Ring with it from out\ (nothing is installed).
rem   PLAY.bat        release build
rem   PLAY.bat diag   diagnostics build (F7 panel, aotr_report.txt, measurement lines in out\bfme2_accel.log)
rem An installed copy (PACKAGE.bat's zip) that is running in the tray steps aside for this one until the next logon.
cd /d "%~dp0"
echo === Building (build.bat %1) ===
call "%~dp0build.bat" %1
if errorlevel 1 (
  echo.
  echo The build failed - see the lines above.
  pause
  exit /b 1
)
echo.
echo === Starting AOTR (Windows asks for administrator rights; the ring appears near the clock) ===
start "" "%~dp0out\aotr_start.exe"
echo If the AOTR launcher does not open: answer Yes to the administrator question; if it still does not,
echo send out\aotr_start.log - it says why (launcher not found, the game or the launcher already running, ...).
timeout /t 8 >nul
