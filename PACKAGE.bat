@echo off
rem PACKAGE.bat - builds and makes AOTR_accelerator.zip for players: one click installs it.
rem Inside: AOTR_ACCELERATOR.exe (installs, then starts with Windows and loads the accelerator into AOTR),
rem bfme2_accel.dll (the accelerator itself), README_EN.txt and QOLLANMA_UZ.txt (the guides), LICENSE.txt and NOTICE.txt.
cd /d "%~dp0"
echo === Building (build.bat) ===
call "%~dp0build.bat"
if errorlevel 1 (
  echo The build failed - see the lines above.
  pause
  exit /b 1
)
set "P=%~dp0package\AOTR_accelerator"
if exist "%~dp0package" rmdir /s /q "%~dp0package"
mkdir "%P%"
copy /y "%~dp0out\bfme2_accel.dll" "%P%\bfme2_accel.dll" >nul
copy /y "%~dp0out\aotr_start.exe" "%P%\AOTR_ACCELERATOR.exe" >nul
copy /y "%~dp0guide\README_EN.txt" "%P%\README_EN.txt" >nul
copy /y "%~dp0guide\QOLLANMA_UZ.txt" "%P%\QOLLANMA_UZ.txt" >nul
rem the MIT licence asks for the copyright notice in every copy
copy /y "%~dp0LICENSE" "%P%\LICENSE.txt" >nul
copy /y "%~dp0NOTICE" "%P%\NOTICE.txt" >nul
rem the learned list (file names of models and textures that first appeared in earlier battles) gives a new install a warm first game
if exist "%~dp0out\aotr_learned.txt" copy /y "%~dp0out\aotr_learned.txt" "%P%\aotr_learned.txt" >nul
if exist "%~dp0AOTR_accelerator.zip" del /q "%~dp0AOTR_accelerator.zip"
tar.exe -a -c -f "%~dp0AOTR_accelerator.zip" -C "%~dp0package" AOTR_accelerator
echo.
if exist "%~dp0AOTR_accelerator.zip" (
  echo READY: %~dp0AOTR_accelerator.zip
  explorer.exe /select,"%~dp0AOTR_accelerator.zip"
) else (
  echo The zip was not made - zip the folder package\AOTR_accelerator yourself.
)
pause
