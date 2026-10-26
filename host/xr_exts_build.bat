@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set XR=..\third_party\openxr-1.1.65
cl /nologo /O2 /MT /EHsc /I%XR%\include xr_exts.cpp /Fobuild\xr_exts_ /Febuild\xr_exts.exe /link /LIBPATH:%XR%\x64\lib openxr_loader.lib >nul || exit /b 1
