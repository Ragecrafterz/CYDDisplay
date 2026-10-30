@echo off
rem Starts the CYD companion app without a console window.
start "" "%~dp0.venv\Scripts\pythonw.exe" "%~dp0companion\cyd_companion.py"
