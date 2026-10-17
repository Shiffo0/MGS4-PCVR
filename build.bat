@echo off
setlocal
pushd "%~dp0"
if not defined OPENXR_INCLUDE set "OPENXR_INCLUDE=%~dp0deps\OpenXR-SDK\include"
if not exist "%OPENXR_INCLUDE%\openxr\openxr.h" (echo Set OPENXR_INCLUDE to the OpenXR SDK include directory. & popd & exit /b 1)
if not exist build mkdir build
if not defined VSCMD_VER call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (popd & exit /b 1)
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /DMGS4VR_ARMSRC=1 /I "%OPENXR_INCLUDE%" /Fo"build\\" /LD src\mgs4vr_armsrc.c src\mgs4vr_pad.c src\mgs4vr_fpaction.c src\mgs4vr_poses.c src\mgs4vr_muzzle.c src\mgs4vr_rocket.c src\mgs4vr_overlay.c src\mgs4vr_vrmenu.c src\mgs4vr_hud.c src\mgs4vr_wrist.c src\mgs4vr_fpshold.c src\mgs4vr_cqc.c src\mgs4vr_camo.c src\mgs4vr_cqcfp.c src\mgs4vr_scope.c src\mgs4vr.c src\mgs4vr_gfx.c src\mgs4vr_cam.c src\mgs4vr_xr.c src\mgs4vr_closegate.c src\mgs4vr_head.c src\mgs4vr_menu.c src\mgs4vr_log.c src\mgs4vr_render.c src\mgs4vr_eye.c src\mgs4vr_twoeye.c src\mgs4vr_lens.c src\mgs4vr_lens_round.c src\mgs4vr_packet.c src\mgs4vr_stereo.c src\mgs4vr_uniform.c src\mgs4vr_scan.c /link /OUT:build\mgs4vr.asi /IMPLIB:build\mgs4vr.lib dxguid.lib user32.lib gdi32.lib bcrypt.lib
set "rc=%ERRORLEVEL%"
popd
exit /b %rc%



