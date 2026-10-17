#ifndef MGS4VR_WRIST_API_H
#define MGS4VR_WRIST_API_H
#include "mgs4vr_xr.h"
/* One wrist panel as an OpenXR quad (LOCAL space). pixels: w*h RGBA, alpha 255. */
typedef struct { int visible; float q[4], p[3]; float width, height; int w, h; unsigned version; } MGS4VR_XR_QUAD;
void mgs4vr_wrist_poll(const char *exe, int fp, void *held_weapon);
void mgs4vr_wrist_set_use(void (*fn)(void));   /* item ring: left trigger = use the highlighted item */
int mgs4vr_wrist_trigger_edge(int *armed, float trigger);
void mgs4vr_wrist_hands(const MGS4VR_XR_HANDS *h);
int mgs4vr_wrist_quads(MGS4VR_XR_QUAD out[3], unsigned char *pixels[3]);   /* [2] = radial ring */
int mgs4vr_wrist_ring_open(void);
int mgs4vr_wrist_item_ring_open(void);   /* left stick click held: item ring (16 Oct 2026); the pad stops walking */
int mgs4vr_wrist_item_toggle(int cur, int *last, const short *codes, int n);   /* pure: item after a short click */

/* Player heading in degrees for closed-loop turning (any thread); 0 when unknown. */
int mgs4vr_wrist_heading(float *deg);
/* First-person view yaw (radians, camera controller +0x34) for direct turning; NULL when not found. Worker
   refreshes it at 10 Hz; the pad writes it on the game thread (XInput poll). */
float *mgs4vr_wrist_yaw(void);
/* Worker, on a recenter in first person: first-person view pitch (controller +0x30) back to 0. 1 when written. */
int mgs4vr_wrist_level_view(void);
int mgs4vr_wrist_mag(int *mag, int *mag_max);   /* held weapon magazine, 0 = unknown */
#endif
