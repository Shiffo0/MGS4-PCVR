@echo off
setlocal
pushd "%~dp0"
if not defined OPENXR_INCLUDE set "OPENXR_INCLUDE=%~dp0deps\OpenXR-SDK\include"
if not exist "%OPENXR_INCLUDE%\openxr\openxr.h" (echo Set OPENXR_INCLUDE to the OpenXR SDK include directory. & popd & exit /b 1)
if not exist build mkdir build
if not defined VSCMD_VER call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (popd & exit /b 1)
cl /nologo /W3 /O2 /I "%OPENXR_INCLUDE%" /Fo"build\\" /LD src\mgs4vr.c src\mgs4vr_gfx.c src\mgs4vr_cam.c src\mgs4vr_xr.c src\mgs4vr_head.c src\mgs4vr_menu.c src\mgs4vr_log.c /link /OUT:build\mgs4vr.asi /IMPLIB:build\mgs4vr.lib dxguid.lib user32.lib gdi32.lib bcrypt.lib
set "rc=%ERRORLEVEL%"
popd
exit /b %rc%
