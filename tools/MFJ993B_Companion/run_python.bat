@echo off
setlocal
cd /d "%~dp0"

rem Compatibility name. The recommended entry point is START_CLIENT.bat.
call "%~dp0START_CLIENT.bat"
