@echo off
setlocal
set "HOST=%~dp0SmoothMotionHost.exe"
if not exist "%HOST%" set "HOST=%~dp0build\Release\SmoothMotionHost.exe"
if not exist "%HOST%" (
  echo Could not find SmoothMotionHost.exe.
  echo Build the project or place this CMD beside SmoothMotionHost.exe.
  pause
  exit /b 1
)
"%HOST%"
pause
