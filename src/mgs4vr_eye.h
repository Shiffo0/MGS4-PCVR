#ifndef MGS4VR_EYE_H
#define MGS4VR_EYE_H
#include "mgs4vr_packet.h"
#include "mgs4vr_xr.h"
#include "mgs4vr_cam.h"
/* Experimental W06 camera-to-render ownership.
   Caller keeps a ticket in the builder-call stash and commits ONLY after both
   pose and projection were applied and that same builder returned successfully. */
typedef struct MGS4VR_EYE_TICKET {
 MGS4VR_EYE_PACKET packet;
 MGS4VR_CAM_ADJUST adjust;
 float args[4]; /* scale, p4, p5, aspect */
} MGS4VR_EYE_TICKET;
void mgs4vr_eye_enable(int enabled);
/* Diagnostic only: 0=normal stereo, 1=left pose/FOV for both slots,
   2=right pose/FOV for both slots. Changes revoke pending ownership. */
void mgs4vr_eye_control(int mode);
void mgs4vr_eye_views(const MGS4VR_XR_VIEWS *views,uint64_t now_ms);
int mgs4vr_eye_prepare(unsigned serial,int cinematic,int width,int height,uint64_t now_ms,MGS4VR_EYE_TICKET *ticket);
int mgs4vr_eye_commit(const MGS4VR_EYE_TICKET *ticket,int applied,const float combined[16]);
int mgs4vr_eye_candidate(unsigned render_serial,uint64_t now_ms,MGS4VR_EYE_PACKET *out);
unsigned mgs4vr_eye_epoch(void);
#endif
