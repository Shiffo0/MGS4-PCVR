# MGS4-PCVR

Play Metal Gear Solid 4 in VR with an OpenXR headset and motion controllers: stereo first person, your own hands on the weapons, and a VR settings page inside the game's Options menu.

**v0.2.0 is the first release with motion controls.** Earlier versions (v0.1.x) were a theater screen with gamepad play.

## Features

- **Stereo first person** with headset tracking, wide field of view and optional *Two Eye Rendering* (both eyes from the same game frame).
- **Hands and arms follow your controllers** (two-bone IK on Snake's own arms). The weapon sits in your right hand; the left hand snaps to the foregrip for two-handed aiming.
- **Scopes**: a lens scope shows the zoomed picture inside the ocular. Dot sights get a VR red dot.
- **Wrist panels**: left inner wrist = watch with LIFE / PSYCHE / CAMO index and current item; right inner forearm = weapon and ammo.
- **Radial menus**: hold the right stick click for weapons, hold the left stick click for items. In the item ring, pull the left trigger to use an item (e.g. a ration).
- **CQC with the left hand**, OctoCamo reset by shaking both controllers, Solid Eye night vision by holding the left grip at your left ear.
- **First-person action button** (wall press, context actions) and first-person view during CQC.
- **HUD hidden in first person** (life bar and weapon/item panels); it stays in third person.
- **VR Settings** inside the game: Options > CONTROLS > *VR Settings*.

## Roadmap
- **VR Interactions on weapons** Manual reloading etc.
- **Calibration** Calibration step.


## Requirements

- Windows 64-bit, the Steam PC version of Metal Gear Solid 4 (game build 25292043) with the **DirectX 11** renderer.
- An OpenXR headset with motion controllers and a working OpenXR runtime. Developed and tested on Quest 3 (Meta/Oculus runtime and Quest Link).
- Other headsets/runtimes are untested.

## Install

1. Download **MGS4-PCVR-v0.2.0-win64.zip** from [Releases](https://github.com/Shiffo0/MGS4-PCVR/releases/latest).
2. Close the game. Extract the ZIP into the **MGS4 folder that contains `mgs4.exe`** and allow it to overwrite older MGS4-PCVR files.
3. Connect the headset and make sure its OpenXR runtime is active.
4. Start the game from Steam. Keep the game window focused.

If another mod already uses `winmm.dll`, back it up first and check that mod's loader setup.

The ZIP contains the mod (`mgs4vr.asi`), Ultimate ASI Loader (`winmm.dll`), the OpenXR loader, the default settings files (`mgs4vr_*.on`, `mgs4vr.ini`, `mgs4vr_poses.txt`, `mgs4vr_weapons.txt`) and documentation/licenses. It contains no game files.

## Controls (Quest Touch)

| Input | Action |
| --- | --- |
| **A** | Toggle first person |
| **Right trigger** | Fire |
| **Left stick** | Move (half deflection = walk/sneak, full = run) |
| **Right stick** | Turn (smooth or snap, see VR Settings) |
| **Right stick click (hold)** | Weapon ring: point and release to equip |
| **Left stick click** | Tap: item on/off. Hold: item ring (point and release to equip, **left trigger** to use) |
| **B** | Crouch / prone (Space) |
| **X** | Action button (E): wall press and context actions |
| **Menu button** | Tap: pause menu. Hold 1 s: recenter |
| **Left grip at the foregrip** | Two-handed aiming |
| **Left grip at the left ear** | Solid Eye night vision on/off |
| **Left grip near an enemy** | CQC grab (punch forward, swing to throw) |
| **Shake both controllers** | Reset OctoCamo |
| **Look at your inner left wrist / right forearm** | Watch / weapon panel |
| **Pistol: left grip on the slide, pull back** | Rack the slide |
| **Pistol: left grip below the grip, push up** | Insert a magazine (reload) |
| **Scoped rifle to your eye** | Scope view |

Keyboard **Home** opens the mod's theater panel (distance, recenter, stereo).

## VR Settings

Options > CONTROLS > **VR Settings** has four tabs:

- **VR DISPLAY**: Stereo, Two Eye Rendering, Camera Follows Headset, Screen Distance, Desktop Observer, HUD, Light Sprites, Shoulder Offset.
- **VR TURNING**: smooth/snap turning and speed.
- **VR WEAPONS**: Red Dot Sight, grip blend speed.
- **VR ACTIONS**: CQC first person, camo shake.

Settings are stored in `mgs4vr.ini` and the `mgs4vr_*.on` files next to the game.

## Known issues

- After a wall press via the action button the view can stay in third person; press **A** to return to first person.
- Solid Eye labels are no longer doubled with Two Eye Rendering, but do not stay attached to enemies.
- A scope mounted on the M4 Custom (Customize) can be invisible in first person with Two Eye Rendering.
- The "WEAPON ACQUIRED" pop-up only shows in third person.
- The lens scope does not line up on the D.E. (L.B.) and the Rail Gun.
- An arm calibration step (arm length/height) is not available yet.

## Bug reports

Each module writes a small status file `mgs4vr_<module>_<process id>.txt` next to the game while it runs (overwritten every second). Please attach the files of the session where the problem happened, plus `mgs4vr.ini`, when you open an issue. They can be deleted at any time.

## Uninstall

Close the game and remove `mgs4vr.asi`, `mgs4vr.ini`, `mgs4vr_*.on`, `mgs4vr_*.txt`, `mgs4vr_poses.txt` and `mgs4vr_weapons.txt`. Remove `winmm.dll` and `openxr_loader.dll` only if they were installed for this mod.

## Build from source

Install Visual Studio 2022 C++ Build Tools (x64, Windows SDK) and the [OpenXR SDK 1.1.59](https://github.com/KhronosGroup/OpenXR-SDK/tree/release-1.1.59) headers, then in an **x64 Native Tools Command Prompt**:

```bat
set OPENXR_INCLUDE=C:\path\to\OpenXR-SDK\include
build.bat
```

The result is `build\mgs4vr.asi` (player build, diagnostics off).

## License and credits

Project-owned mod code is MIT licensed; see [LICENSE](LICENSE). Bundled components and reference-project credits are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This is an unofficial mod. It does not include original game code or assets.
