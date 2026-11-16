@echo off
start "" "%~dp0Island.Setup.exe" --uninstall
exit /b %errorlevel%
