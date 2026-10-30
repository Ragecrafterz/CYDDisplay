@echo off
rem One-time setup: creates .venv and installs the companion app + PlatformIO.
cd /d "%~dp0"
where python >nul 2>nul || (
  echo Python 3.10+ is needed. Get it from https://www.python.org/downloads/ and tick "Add python.exe to PATH".
  pause
  exit /b 1
)
python -m venv .venv || exit /b 1
.venv\Scripts\python.exe -m pip install --upgrade pip
.venv\Scripts\python.exe -m pip install -r requirements.txt || exit /b 1
echo.
echo Done. Plug in the board and run flash.bat, then "Start CYD Companion.bat".
pause
