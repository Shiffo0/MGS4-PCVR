#ifndef MGS4VR_D3DPROBE_H
#define MGS4VR_D3DPROBE_H
#if defined(MGS4VR_D3D_PROBE) && MGS4VR_D3D_PROBE
/* Resolves the game's immediate context from the renderer, then polls. */
void mgs4vr_d3dprobe_poll(void *base,const char *exe,unsigned frame);
/* Same, with an explicit immediate context (desk tests). */
void mgs4vr_d3dprobe_poll_context(void *context,void *base,const char *exe,unsigned frame);
int mgs4vr_d3dprobe_active(void);
/* Precise frame boundary for the render-thread hooks (present counter). */
void mgs4vr_d3dprobe_set_frame_source(unsigned (*fn)(void));
#else
#define mgs4vr_d3dprobe_poll(base,exe,frame) ((void)0)
#define mgs4vr_d3dprobe_poll_context(context,base,exe,frame) ((void)0)
#define mgs4vr_d3dprobe_active() 0
#define mgs4vr_d3dprobe_set_frame_source(fn) ((void)0)
#endif
#endif
