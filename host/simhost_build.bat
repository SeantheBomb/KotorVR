@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHsc kvr_simhost.cpp /Fobuild\simhost_ /Febuild\kvr_simhost.exe || exit /b 1
echo built host\build\kvr_simhost.exe
