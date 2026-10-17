#ifndef MGS4VR_GPU_TIMER_H
#define MGS4VR_GPU_TIMER_H
#if defined(MGS4VR_GPU_TIMER) && MGS4VR_GPU_TIMER
void mgs4vr_gpu_open(const char *exe);
void mgs4vr_gpu_drain(void);
int mgs4vr_gpu_begin(void *context,unsigned serial,int eye_valid);
void mgs4vr_gpu_end(int token);
void mgs4vr_gpu_xr_begin(void);
void mgs4vr_gpu_xr_end(void);
#else
#define mgs4vr_gpu_open(exe) ((void)0)
#define mgs4vr_gpu_drain() ((void)0)
#define mgs4vr_gpu_begin(c,s,v) (-1)
#define mgs4vr_gpu_end(t) ((void)0)
#define mgs4vr_gpu_xr_begin() ((void)0)
#define mgs4vr_gpu_xr_end() ((void)0)
#endif
#endif
