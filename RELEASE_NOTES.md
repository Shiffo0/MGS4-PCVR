# v0.1.0

Initial mono-theater release for OpenXR headsets and gamepad play.

- Home-key settings panel visible in the headset.
- Optional headset rotation controls the game camera; off by default.
- Recenter the screen and adjust its distance from 0.5 to 10.0 m.
- Saved distance and camera-follow preference.
- No development measurement logs, captures, or experimental stereo mode.

Motion controls are planned for a future version.

Requires DirectX 11. Seven automated test programs passed, including real D3D11 texture transport with a simulated OpenXR runtime. A fresh physical-headset acceptance test of this exact release build remains pending. See the README for supported game version and setup.
