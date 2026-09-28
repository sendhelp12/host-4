@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Install Visual Studio 2022 with "Desktop development with C++"
  echo and the CMake tools component, then run this again.
  pause
  exit /b 1
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail

cmake --build build --config Release
if errorlevel 1 goto :fail

echo.
echo Built successfully:
echo   %~dp0build\Release\SmoothMotionHost.exe
echo.
pause
exit /b 0

:fail
echo.
echo Build failed. Copy the error output into ChatGPT and I can fix it.
pause
exit /b 1
