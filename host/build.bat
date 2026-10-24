@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set XR=..\third_party\openxr-1.1.65
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHsc /I%XR%\include kvr_host.cpp /Fobuild\ /Febuild\kvr_host.exe /link /LIBPATH:%XR%\x64\lib || exit /b 1
copy /y %XR%\x64\bin\openxr_loader.dll build\ >nul
echo built host\build\kvr_host.exe
