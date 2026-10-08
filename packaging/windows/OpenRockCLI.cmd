@echo off
chcp 65001 >nul
cd /d "%~dp0"
set "PATH=%~dp0;%PATH%"
title OpenRockCLI Native Preview
openrockcli.exe --help
echo.
echo Enter openrockcli scan to list Rockchip devices.
echo Enter openrockcli tui for the interactive menu.
echo.
