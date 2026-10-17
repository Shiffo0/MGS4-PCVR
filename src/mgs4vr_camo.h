#ifndef MGS4VR_CAMO_H
#define MGS4VR_CAMO_H
/* Camo reset by shaking both controllers (14 Oct 2026, user: "T resets the camo; shake both Quest controllers").
   Each hand: a "reversal" = a fast sample (> speed m/s) pointing against the previous fast direction (angle > ~107 deg).
   Both hands with >= revs reversals inside window s = one tap of T (pad module), then cooldown s.
   Only in first person, never during a CQC gesture or with the left hand on the foregrip (worker gates).
   Marker mgs4vr_camo.on: "on [revs=<n>] [window=<s>] [speed=<m/s>] [cooldown=<s>]" (defaults 3, 0.6, 1.0, 2.0).
   Status mgs4vr_camo_<pid>.txt. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mgs4vr_xr.h"
typedef struct { int wanted, revs; float window_s, speed, cooldown_s; } MGS4VR_CAMO_CFG;
int mgs4vr_camo_parse(const char *text, MGS4VR_CAMO_CFG *cfg);
/* Pure detector (desk tests). */
typedef struct { float dir[2][3]; int have[2]; double rev[2][8]; int nrev[2]; double last_fire; long reversals[2], shakes; } MGS4VR_CAMO_DET;
void mgs4vr_camo_sample(MGS4VR_CAMO_DET *d, const MGS4VR_CAMO_CFG *cfg, int hand, const float v[3], double t);
int mgs4vr_camo_check(MGS4VR_CAMO_DET *d, const MGS4VR_CAMO_CFG *cfg, double t);   /* 1 = shake: fire once */
/* XR thread: both hands' velocities. */
void mgs4vr_camo_hands(const MGS4VR_XR_HANDS *hands);
/* Worker: marker/status; allowed = first person, no CQC gesture, left hand not on the foregrip. 1 = tap T now. */
int mgs4vr_camo_tick(const char *exe, int allowed, DWORD now);
#endif
