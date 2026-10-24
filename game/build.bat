@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /MT /W3 /EHsc kvr_game.cpp vr.cpp input.cpp scan.cpp enginecam.cpp /Fobuild\ /Febuild\dinput8.dll /link /DEF:dinput8.def user32.lib kernel32.lib d3d11.lib dxgi.lib dxguid.lib || exit /b 1
echo built game\build\dinput8.dll
