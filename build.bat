@echo off
rem Builds TimeWeather.asi (32-bit) with Zig's bundled clang and copies it into the game folder.
setlocal
cd /d "%~dp0"
set GAME=%~dp0..\Grand Theft Auto San Andreas
where zig >nul 2>nul
if errorlevel 1 (
  echo zig.exe not found on PATH. Install it with:  winget install -e --id zig.zig
  exit /b 1
)
if not exist build mkdir build
zig c++ -target x86-windows-gnu -std=c++17 -shared -O2 -fno-exceptions -fno-rtti -s -Wall ^
  -o build\TimeWeather.asi src\main.cpp -luser32 -lkernel32
if errorlevel 1 exit /b 1
del /q build\*.lib build\*.pdb 2>nul
echo Built build\TimeWeather.asi
if exist "%GAME%\gta_sa.exe" (
  tasklist /fi "imagename eq gta_sa.exe" | find /i "gta_sa.exe" >nul
  if not errorlevel 1 (
    echo gta_sa.exe is running - close the game, then run build.bat again to install.
  ) else (
    copy /y build\TimeWeather.asi "%GAME%\TimeWeather.asi" >nul
    if not exist "%GAME%\TimeWeather.ini" copy /y TimeWeather.ini "%GAME%\TimeWeather.ini" >nul
    echo Installed into "%GAME%"
  )
)
