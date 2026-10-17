#ifndef MGS4VR_ARMS_H
#define MGS4VR_ARMS_H
#if defined(MGS4VR_ARMS_PROBE) && MGS4VR_ARMS_PROBE
int mgs4vr_arms_active(void);
void mgs4vr_arms_poll(void *image,const char *exe,unsigned frame);
#else
#define mgs4vr_arms_active() 0
#define mgs4vr_arms_poll(image,exe,frame) ((void)0)
#endif
#endif

