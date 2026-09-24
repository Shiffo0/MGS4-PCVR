# v0.1.1

Adds Stereo On / Off to the Home menu and supports Steam game build 25292043.

- Switch between mono theater and stereo gameplay without restarting.
- The Home panel stays in theater; closing it restores the selected mode.
- Stereo uses headset tracking. Mono retains optional camera following.
- Recenter resets the screen and neutral head pose; distance controls theater placement.
- Stereo preference is saved in mgs4vr.ini. Mono remains the default.
- No development probes, measurement files or camera captures.

Stereo uses alternating gameframes, not two views of the same simulation state. Menus/cutscenes use theater fallback when no valid eye pair exists. Motion controls remain planned; play with a gamepad.

Requires DirectX 11 and an active OpenXR headset. Eleven automated test programs passed, including mode switching, native camera transforms, frame ownership and D3D11 transport through a simulated OpenXR runtime. Physical-headset acceptance of this exact build is pending.
