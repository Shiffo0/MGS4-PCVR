#ifndef MGS4VR_FPSHOLD_H
#define MGS4VR_FPSHOLD_H
/* VR "always first person": the A button on the right controller toggles a hold
   of right mouse button (aim) followed by F (first person), exactly as the
   player would hold them. The game reads keyboard/mouse through Raw Input, so
   injected down events stay "held" until the matching up events.
   Safety: nothing is held while the game is not the foreground window or the
   Home menu is open; the hold is re-applied in order (aim, then F) when it
   comes back. mgs4vr_fpshold_release() lets go of everything (process exit). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mgs4vr_xr.h"
typedef UINT (WINAPI *MGS4VR_SEND_INPUT)(UINT count, LPINPUT inputs, int size);
/* XR thread: rising edge of A (right hand, primary) toggles the wish. */
void mgs4vr_fpshold_hands(const MGS4VR_XR_HANDS *hands);
/* Worker: applies the wish. allowed = game in foreground and no menu open. */
void mgs4vr_fpshold_tick(int allowed, DWORD now_ms);
void mgs4vr_fpshold_release(void);
/* Worker, before mgs4vr_fpshold_tick: 1 = let go of the aim (right mouse) for a moment while first person (F) stays held
   (CQC gesture, mgs4vr_cqc.h); 0 = aim again. */
void mgs4vr_fpshold_pause_aim(int pause);
/* Worker: let go of F and the aim and apply them again (the game drops first person for a CQC grab). */
void mgs4vr_fpshold_reapply(void);
/* 0 released, 1 aim held, 2 aim + F held. */
int mgs4vr_fpshold_stage(void);
int mgs4vr_fpshold_wanted(void);
/* Desk tests replace SendInput. */
void mgs4vr_fpshold_set_sender(MGS4VR_SEND_INPUT send);
#endif
