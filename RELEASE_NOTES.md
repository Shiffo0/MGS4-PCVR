# v0.2.0

First public release with motion controls: MGS4 in stereo first person with your own hands.

## New

- Stereo first person with headset tracking and optional Two Eye Rendering (both eyes from one game frame).
- Snake's arms follow the controllers (IK); two-handed aiming with the left hand on the foregrip.
- Bullets, rockets and grenades leave along the VR weapon.
- Scoped rifles: scope view by raising the rifle to the eye; lens scope inside the ocular; VR red dot for dot sights (only when a dot sight is mounted).
- Wrist panels: watch with LIFE / PSYCHE / CAMO index and the current item; weapon and ammo panel.
- Radial weapon menu and item ring; use items such as a ration from the ring with the left trigger.
- CQC with the left hand, first-person view during CQC, OctoCamo reset by shaking, Solid Eye night vision gesture.
- Action button in first person (wall press and context actions).
- HUD hidden in first person only (life bar, weapon/item panels); full HUD in third person.
- VR Settings page inside the game's Options menu; recenter by holding the menu button for 1 second.
- Smooth or snap turning on the right stick.

## Known issues

- After a wall press via the action button the view can stay in third person (press A).
- Solid Eye labels do not stay attached to enemies.
- A Customize-mounted scope on the M4 can be invisible in first person with Two Eye Rendering.
- "WEAPON ACQUIRED" pop-up only in third person.
- Lens scope misaligned on the D.E. (L.B.) and the Rail Gun.
- No arm calibration step yet.

## Notes

- Requires the Steam PC version (game build 25292043), DirectX 11, an OpenXR runtime and motion controllers. Tested on Quest 3.
- Player build: no diagnostic probes or measurement captures; only small per-session status files for bug reports.
