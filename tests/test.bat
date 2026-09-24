@echo off
setlocal enabledelayedexpansion
pushd "%~dp0.."
if not defined OPENXR_INCLUDE set "OPENXR_INCLUDE=%CD%\deps\OpenXR-SDK\include"
if not defined VSCMD_VER call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
set "rc=1"
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\menu_test.c src\mgs4vr_menu.c /link /OUT:build\menu_test.exe user32.lib gdi32.lib
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\head_desk_test.c src\mgs4vr_head.c /link /OUT:build\head_desk_test.exe
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\cam_adjust_test.c src\mgs4vr_cam.c /link /OUT:build\cam_adjust_test.exe
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\gfx_desk_test.c src\mgs4vr_gfx.c src\mgs4vr_log.c /link /OUT:build\gfx_desk_test.exe dxguid.lib user32.lib
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /I "%OPENXR_INCLUDE%" /Fo"build\\" tests\xr_desk_test.c src\mgs4vr_xr.c src\mgs4vr_menu.c src\mgs4vr_packet.c /link /OUT:build\xr_desk_test.exe d3d11.lib dxgi.lib dxguid.lib user32.lib gdi32.lib
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\asi_load_test.c /link /OUT:build\asi_load_test.exe
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /I "%OPENXR_INCLUDE%" /Fo"build\\" tests\settings_test.c src\mgs4vr_cam.c src\mgs4vr_gfx.c src\mgs4vr_head.c src\mgs4vr_menu.c src\mgs4vr_xr.c src\mgs4vr_log.c src\mgs4vr_render.c src\mgs4vr_eye.c src\mgs4vr_packet.c src\mgs4vr_stereo.c src\mgs4vr_uniform.c src\mgs4vr_scan.c /link /OUT:build\settings_test.exe dxguid.lib user32.lib gdi32.lib bcrypt.lib
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\render_test.c src\mgs4vr_eye.c src\mgs4vr_packet.c src\mgs4vr_stereo.c src\mgs4vr_uniform.c src\mgs4vr_head.c /link /OUT:build\render_test.exe
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\eye_desk_test.c src\mgs4vr_packet.c src\mgs4vr_stereo.c src\mgs4vr_head.c /link /OUT:build\eye_desk_test.exe
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\packet_desk_test.c src\mgs4vr_packet.c /link /OUT:build\packet_desk_test.exe
if errorlevel 1 goto end
cl /nologo /W3 /O2 /DDG_ENABLE_DIAGNOSTICS=0 /Fo"build\\" tests\stereo_desk_test.c src\mgs4vr_stereo.c /link /OUT:build\stereo_desk_test.exe
if errorlevel 1 goto end
for %%t in (render_test eye_desk_test packet_desk_test stereo_desk_test menu_test settings_test head_desk_test cam_adjust_test gfx_desk_test xr_desk_test) do (
 build\%%t.exe
 if errorlevel 1 goto end
)
build\asi_load_test.exe build\mgs4vr.asi
if errorlevel 1 goto end
set "rc=0"
:end
popd
exit /b %rc%
