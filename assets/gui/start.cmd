@echo off
setlocal
set "ROOT=%~dp0.."
set "PORT=%~1"
if "%PORT%"=="" set "PORT=4317"
set "CC_GUI_PORT=%PORT%"
set "CC_GUI_OPEN=1"
echo Starting Claude Code GUI on http://127.0.0.1:%PORT%
"%ROOT%\runtime\node\node.exe" "%~dp0server.js"
pause
