# MGS4-PCVR

Play Metal Gear Solid 4 on a virtual screen in an OpenXR headset using a gamepad.

**v0.1.1 adds optional stereo gameplay.** Mono theater is the default. Enable Stereo in the Home menu for separate eye views with headset tracking. Menus and cutscenes fall back to theater when no valid stereo pair is available. Optional headset rotation can also steer the camera in mono mode. Motion controls are planned for a future version.

## Download and install

1. Download **MGS4-PCVR-v0.1.1-win64.zip** from [Releases](https://github.com/Shiffo0/MGS4-PCVR/releases/latest).
2. Close the game. Extract the ZIP into the **MGS4 folder containing `mgs4.exe`**.
3. Start your headset connection and select its OpenXR runtime. Connect your gamepad.
4. Launch the game normally. Keep the game window focused when using the mod menu.

The package contains `mgs4vr.asi`, Ultimate ASI Loader (`winmm.dll`), the OpenXR loader, and documentation/licenses. It contains no game executable, game assets, measurement logs, camera captures, or development reports.

If another mod already supplies `winmm.dll`, back it up and check that mod's loader setup before replacing it. Remove the experimental `mgs4vr.on` file if upgrading from a development build; v0.1.1 uses `mgs4vr.ini` instead and ignores experimental marker settings.

## Home menu

Press **Home** on the PC keyboard to open or close the settings panel in the headset. Pause the game first if you do not want keyboard navigation to affect the game underneath.

| Option | Behavior |
| --- | --- |
| Camera follows headset: Yes / No | Enable or disable headset **rotation** driving the game camera. Default: No in mono mode. Stereo uses headset tracking automatically; switch stereo off to change this option. Movement stays on the gamepad. |
| Stereo: On / Off | Switch immediately between stereo gameplay and mono theater. Default: Off. Unavailable if the game version or required hooks are not verified. |
| Recenter screen | Put the virtual screen in front of your current head position/direction. Also reset the neutral head pose for camera following or stereo. |
| Screen distance | Adjust from **0.5 to 10.0 metres**, in 0.1 m steps. Default: 2.5 m. |

Use **Up / Down** to select, **Enter** to toggle or recenter, and **Left / Right** to adjust distance. **Esc** closes the panel. Distance, camera-follow preference and stereo preference are saved in `mgs4vr.ini` beside the game. Recenter is session-local. The screen width is 3.2 m; distance changes its apparent size. Distance applies to theater, including the Home panel; it does not change stereo world scale. Opening Home temporarily switches to theater; closing it restores the selected mode.

The gamepad retains the game's normal controls. There are no tracked-controller bindings or motion controls in this release.

## Requirements and current limits

- Windows 64-bit, a working OpenXR runtime, and the game's **DirectX 11** renderer. DirectX 12 and Vulkan are not supported.
- An OpenXR headset and gamepad. Development testing used Quest 3 with the Meta/Oculus runtime; PS VR2/SteamVR hardware acceptance has not been established.
- Camera following and stereo are version-gated to the supported PC executable (Steam build 25292043). If the executable differs, theater can still run but the menu shows camera following and stereo as **Unavailable**.
- Stereo alternates native gameframes between eyes; at 60 gameframes/s each eye receives about 30 new images/s. The eyes are not rendered from the same simulation state. Mono camera following is rotation-only. Authored cutscene cameras retain theater fallback.
- Eleven automated test programs cover menu navigation/rendering, live mode changes, camera transforms, native frame ownership, stale-image rejection, graphics hooks, OpenXR transport/recovery and loading outside the game. Physical-headset acceptance of this exact v0.1.1 build remains pending.

## Uninstall

Close the game and remove `mgs4vr.asi` and `mgs4vr.ini`. Remove `winmm.dll` and `openxr_loader.dll` only if they were installed solely for this mod; restore any files you backed up.

## Build from source

Install Visual Studio 2022 C++ Build Tools (x64, Windows SDK) and [OpenXR SDK 1.1.59](https://github.com/KhronosGroup/OpenXR-SDK/tree/release-1.1.59).

In an **x64 Native Tools Command Prompt**:

```bat
set OPENXR_INCLUDE=C:\path\to\OpenXR-SDK\include
build.bat
tests\test.bat
```

The resulting mod is `build\mgs4vr.asi`. Use an x64 OpenXR loader alongside it. The source corresponds to the v0.1.1 release; the package manifest records the shipped file hashes. Build output and local test output are not checked into this repository.

## Roadmap

- Motion controls.
- Further headset compatibility and comfort improvements.

## License and credits

Project-owned mod code is MIT licensed; see [LICENSE](LICENSE). Bundled components and reference-project credits are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This is an unofficial mod and does not include original game source code or assets.
