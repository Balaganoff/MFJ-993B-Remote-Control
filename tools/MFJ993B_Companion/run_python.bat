@echo off
setlocal
cd /d "%~dp0"

where py.exe >nul 2>nul
if not errorlevel 1 (
  start "" pyw.exe -3 "%~dp0MFJ993B_Companion.py"
  exit /b 0
)

where pythonw.exe >nul 2>nul
if not errorlevel 1 (
  start "" pythonw.exe "%~dp0MFJ993B_Companion.py"
  exit /b 0
)

echo 64-bit Python 3 with Tkinter was not found.
echo https://www.python.org/downloads/windows/
pause
exit /b 1
