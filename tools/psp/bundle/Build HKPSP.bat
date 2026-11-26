@echo off
rem HKPSP builder: builds the PSP game folder from your Steam copy of Hollow Knight.
rem Arguments are passed on (for example --game "D:\Games\Hollow Knight" or --check).
"%~dp0runtime\python\python.exe" -I "%~dp0tools\psp\hkpsp_build.py" %*
echo.
pause
