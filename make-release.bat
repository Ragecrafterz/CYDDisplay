@echo off
rem Builds dist\cyd-now-playing.bin: bootloader + partitions + app in one file, flashable at 0x0.
cd /d "%~dp0"
.venv\Scripts\python.exe -m platformio run -d firmware || exit /b 1
set BUILD=%USERPROFILE%\.cyd-build\build\cyd2usb
set BOOTAPP=%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin
if not exist dist mkdir dist
.venv\Scripts\python.exe -m esptool --chip esp32 merge-bin -o dist\cyd-now-playing.bin ^
  0x1000 "%BUILD%\bootloader.bin" 0x8000 "%BUILD%\partitions.bin" ^
  0xe000 "%BOOTAPP%" 0x10000 "%BUILD%\firmware.bin" || exit /b 1
echo.
echo Wrote dist\cyd-now-playing.bin
