@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "PYTHON_CMD="
set "PYTHON_ARGS="

where py.exe >nul 2>nul
if not errorlevel 1 (
  py -3 -c "import struct, tkinter; raise SystemExit(0 if struct.calcsize('P') == 8 else 1)" >nul 2>nul
  if not errorlevel 1 (
    set "PYTHON_CMD=py"
    set "PYTHON_ARGS=-3"
  )
)

if not defined PYTHON_CMD (
  set "SYSTEM_PYTHON="
  for /f "delims=" %%P in ('where python.exe 2^>nul ^| findstr.exe /v /i /c:"WindowsApps"') do (
    if not defined SYSTEM_PYTHON set "SYSTEM_PYTHON=%%P"
  )
  if defined SYSTEM_PYTHON (
    "!SYSTEM_PYTHON!" -c "import struct, tkinter; raise SystemExit(0 if struct.calcsize('P') == 8 else 1)" >nul 2>nul
    if not errorlevel 1 set "PYTHON_CMD=!SYSTEM_PYTHON!"
  )
)

if not defined PYTHON_CMD (
  echo Dlya sborki EXE nuzhen ustanovlennyy 64-bit Python 3 s Tkinter.
  echo Sam klient zapuskaetsya bez ustanovki cherez START_CLIENT.bat.
  echo Python dlya sborki: https://www.python.org/downloads/windows/
  pause
  exit /b 1
)

if not exist .buildenv\Scripts\python.exe (
  "%PYTHON_CMD%" %PYTHON_ARGS% -m venv .buildenv
  if errorlevel 1 goto :error
)

call .buildenv\Scripts\activate.bat
python -m pip install --upgrade pip pyinstaller
if errorlevel 1 goto :error

python -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --onefile ^
  --windowed ^
  --name MFJ993B_Companion ^
  MFJ993B_Companion.py
if errorlevel 1 goto :error

echo.
echo Gotovo: %CD%\dist\MFJ993B_Companion.exe
pause
exit /b 0

:error
echo.
echo Oshibka sborki.
pause
exit /b 1
