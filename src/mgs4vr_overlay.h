#ifndef MGS4VR_OVERLAY_H
#define MGS4VR_OVERLAY_H
#include "mgs4vr_xr.h"
void mgs4vr_overlay_poll(void *base,const char *exe,int fp);
void mgs4vr_overlay_hands(const MGS4VR_XR_HANDS *h);
/* 1 while the rifle is at the eye in VR first person (overlay marker on): the game's own scope is in use. */
int mgs4vr_overlay_gesture(void);
#endif
