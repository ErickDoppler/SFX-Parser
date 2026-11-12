@echo off
set PATH=C:\workenv\w64devkit\bin;%PATH%
cd /d "%~dp0"
if not exist build mkdir build
windres src\app.rc -O coff -o build\app_res.o || exit /b 1
g++ -std=c++17 -O2 -Wall -municode -mwindows -static -s src\main.cpp build\app_res.o -o SFX-Parser.exe -lmfplat -lmfreadwrite -lmfuuid -lole32 -luuid -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -lpropsys || exit /b 1
echo Built SFX-Parser.exe
