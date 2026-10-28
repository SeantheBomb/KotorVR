@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /DUNICODE /D_UNICODE kvr_launch.cpp /Fobuild\ /Febuild\kvr_launch.exe /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib || exit /b 1
echo built host\build\kvr_launch.exe
