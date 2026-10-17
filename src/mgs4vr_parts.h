#ifndef MGS4VR_PARTS_H
#define MGS4VR_PARTS_H
#if defined(MGS4VR_PART_PROBE) && MGS4VR_PART_PROBE
int mgs4vr_parts_active(void);
void mgs4vr_parts_poll(void *base,const char *exe,unsigned frame);
#else
#define mgs4vr_parts_active() 0
#define mgs4vr_parts_poll(base,exe,frame) ((void)0)
#endif
#endif

