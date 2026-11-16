@echo off
"%~dp0Island.Preload.Install.exe" --remove --launcher "%~dp0Island.Start.exe"
set "IslandRemoveResult=%errorlevel%"
if not "%IslandRemoveResult%"=="0" (
    echo Service removal failed. Error: %IslandRemoveResult%
    pause
)
exit /b %IslandRemoveResult%
