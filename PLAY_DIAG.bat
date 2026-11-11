@echo off
rem PLAY_DIAG.bat - double-click: builds the DIAGNOSTICS build and starts AOTR with it (the same as "PLAY.bat diag").
rem The game shows the F7 panel; afterwards send out\bfme2_accel.log and out\aotr_report.txt.
call "%~dp0PLAY.bat" diag
