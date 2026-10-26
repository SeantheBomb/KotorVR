@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 stacks.cpp /Fobuild\ /Febuild\stacks.exe psapi.lib || exit /b 1
echo built tools\build\stacks.exe
