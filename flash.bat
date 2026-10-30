@echo off
rem Builds the firmware and uploads it to the board (PlatformIO finds the USB port itself).
rem Close the companion app first: it holds the serial port.
cd /d "%~dp0"
.venv\Scripts\python.exe -m platformio run -d firmware -t upload
pause
