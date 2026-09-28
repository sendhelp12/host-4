@echo off
setlocal
set "SM86=C:\Users\jack\AppData\Local\Programs\SmoothMotionSM86\sm86.exe"
set "HOST=%~dp0SmoothMotionHost.exe"
if not exist "%HOST%" set "HOST=%~dp0build\Release\SmoothMotionHost.exe"
for %%I in ("%HOST%") do set "HOSTDIR=%%~dpI"

if not exist "%SM86%" (
  echo Could not find SM86:
  echo   %SM86%
  pause
  exit /b 1
)

if not exist "%HOST%" (
  echo Could not find SmoothMotionHost.exe.
  echo Build the project or place this CMD beside SmoothMotionHost.exe.
  pause
  exit /b 1
)

"%SM86%" launch --exe "%HOST%" --game-root "%HOSTDIR%" --cwd "%HOSTDIR%"
pause
