@echo off
setlocal
cd /d "%~dp0"
if exist "dist\9288Debug-PC.exe" (
  start "" "dist\9288Debug-PC.exe"
  exit /b
)
python -m host.gui
if errorlevel 1 (
  echo.
  echo 9288 Debug failed to start. Install dependencies with:
  echo python -m pip install -r requirements.txt
  pause
)
endlocal
