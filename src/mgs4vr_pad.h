#ifndef MGS4VR_PAD_H
#define MGS4VR_PAD_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
/* Menu button state (16 Oct 2026): short press = Tab tap on release, hold = recenter (see mgs4vr_pad.c). */
typedef struct { int pressed, fired; DWORD since, tab_until; } MGS4VR_MENU_BTN;
int mgs4vr_pad_menu_button(MGS4VR_MENU_BTN *b, int down, DWORD now, DWORD hold_ms, int *tab);
/* Worker: 1 once after the menu button was held long enough -> the caller recenters (menu.recenter++). */
int mgs4vr_pad_take_recenter(void);
/* Quest controllers as a virtual Xbox pad. The game imports XINPUT1_4.dll by
   ordinal (2 = XInputGetState); we take that IAT slot (the DRM stub rewrites the
   import table after start, so the slot is re-checked like GetProcAddress).
   Pad 0 gets the VR state merged over a real pad, if one is connected: per axis
   the larger deflection wins. Mapping: left stick = left stick.
   Right trigger = fire = left mouse button (the game's UseWeapon, same keyboard/
   mouse route as the first-person hold), press > 0.6, release < 0.4.
   B (right controller) = Space (Crouch/Roll: tap = crouch/stand, hold = prone), held as long as B is held.
   X (left controller) = E (SpecialAction = the action button), held as long as X is held.
   Menu button (left controller) = Tab (PauseMenu): a short press taps Tab on release, holding it 1 s (recenterms=) recenters; in gameplay AND in menus.
   Right stick in first person = mouse look (the game's MouseX/MouseY): relative mouse moves sent from the
   XR thread every controller sample, fractions carried over; dead zone 0.15, squared response.
   Speed in mouse counts per second at full deflection, from the marker: lookx=<n> looky=<n> (defaults
   900 / 600), "invert" flips up/down, lookx=0 switches turning off. Never in third person or menus.
   Game menus (the game shows its mouse cursor; hidden + clipped in gameplay): A/B go to the virtual pad as
   A (select) / B (back), the stick navigates as pad stick; fire, Space, E and W/A/S/D are released and the
   first-person toggle ignores A. The context signal is logged to mgs4vr_padctx_<pid>.csv (10 Hz).
   No fresh controller sample (100 ms): neutral stick, trigger released.

   Marker mgs4vr_pad.on next to the exe:
     "on"    left stick on the virtual pad everywhere
     "wasd"  in first person the left stick presses W/A/S/D instead (pad stick neutral)
     "off"   installed, real pad passed through, nothing pressed
     add "nomenu" (e.g. "wasd nomenu") to switch the menu detection off
     sneak (14 Oct 2026): in wasd mode a half-deflected stick (below sneak=<0.3..0.99>, default 0.8) holds Ctrl (walk)
     with the move keys; the left stick click toggles sneaking (Ctrl with every move); "nosneak" switches both off
   Heartbeat mgs4vr_pad_<pid>.txt every second. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
void mgs4vr_pad_set_look_block(int (*fn)(void));
void mgs4vr_pad_set_move_block(int (*fn)(void));
void mgs4vr_pad_set_act_source(int (*fn)(void));   /* Snake's act for the first-person action machine (mgs4vr_fpaction.h) */   /* item ring open: no walking (16 Oct 2026; left stick click = item button: tap off/on, hold ring; no sneak toggle) */
void mgs4vr_pad_set_heading_source(int (*fn)(float *deg));
/* Direct turning (default for turn=smooth|snap; "turnvia=mouse" = old mouse route): fn returns the game's view yaw
   (radians) or NULL; written on the game thread in the XInput poll. */
void mgs4vr_pad_set_yaw_source(float *(*fn)(void));
/* Pure (desk tests): yaw after one game frame. mode 1 smooth, 2 snap; *armed = snap re-arm state. */
float mgs4vr_pad_turn_step(float yaw, float x, float dt, int mode, float rate_deg, float snap_deg, int *armed, int *snapped);
#include <windows.h>
#include <xinput.h>
#include "mgs4vr_xr.h"
/* XR thread. */
void mgs4vr_pad_hands(const MGS4VR_XR_HANDS *hands);
/* Worker: marker, install, slot repair, heartbeat. */
void mgs4vr_pad_poll(void *image_base, const char *exe);
/* Pure merge (desk tests): VR stick (-1..1, y up) over the real result. */
DWORD mgs4vr_pad_merge(DWORD real_rc, const XINPUT_STATE *real, int vr_fresh, const float stick[2], WORD buttons, XINPUT_STATE *out);
/* Worker: fire and WASD. allowed = game foreground and no menu; fps = first-person hold active. */
typedef UINT (WINAPI *MGS4VR_PAD_SEND)(UINT count, LPINPUT inputs, int size);
void mgs4vr_pad_tick(int allowed, int fps, int menu, DWORD now_ms);
/* 1 while a game menu owns A/B (the XR thread keeps A away from the first-person toggle). */
int mgs4vr_pad_menu(void);
/* Worker: the game's cursor. menu = visible (and detection not switched off). */
int mgs4vr_pad_cursor_menu(int *showing, int *clipped);
/* Worker, 10 Hz: context trace (time, cursor, clip, first person, gameplay camera builds, menu). */
void mgs4vr_pad_trace(DWORD now_ms, int showing, int clipped, int fps, long gameplay_builds, int menu);
void mgs4vr_pad_release(void);
/* Worker, before mgs4vr_pad_tick: the CQC gesture holds the left mouse (mgs4vr_cqc.h); the trigger's fire waits meanwhile. */
void mgs4vr_pad_cqc_lmb(int down);
/* Worker: CQC throw direction, +1 = W (forward), -1 = S (back), 0 = none. */
void mgs4vr_pad_cqc_dir(int dir);
/* Worker: tap a key once (100 ms down) on the next allowed first-person tick (camo reset T, mgs4vr_camo.h). */
void mgs4vr_pad_tap(WORD vk);
void mgs4vr_pad_set_sender(MGS4VR_PAD_SEND send);
/* XR thread (called from mgs4vr_pad_hands): right stick -> mouse look while allowed. */
void mgs4vr_pad_look(const float *rstick, DWORD now_ms);
void mgs4vr_pad_set_look_block(int (*fn)(void));
void mgs4vr_pad_set_heading_source(int (*fn)(float *deg));
#endif
