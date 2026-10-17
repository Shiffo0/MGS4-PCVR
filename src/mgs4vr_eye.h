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
 /* Two-eye mode (13 Oct 2026, mgs4vr_twoeye): the main camera is always the LEFT eye and the second native view
    renders the right eye in the same frame from adjust_right; packet_right is what XR submits for it. */
 int twoeye;
 MGS4VR_EYE_PACKET packet_right;
 MGS4VR_CAM_ADJUST adjust_right;
} MGS4VR_EYE_TICKET;
/* Two-eye mode on/off (worker). Changes revoke pending ownership. */
void mgs4vr_eye_twoeye(int enabled);
/* Pure: 1 when the right eye's FOV is the mirror image of the left one (then the right projection is the left one
   with its horizontal skew negated). Tolerance 0.005 rad. */
int mgs4vr_eye_fov_mirrored(const float left[4],const float right[4]);
/* Right-eye packet committed together with this camera serial (two-eye mode); 0 when none. */
int mgs4vr_eye_right(unsigned camera_serial,MGS4VR_EYE_PACKET *out);
void mgs4vr_eye_enable(int enabled);
/* Diagnostic only: 0=normal stereo, 1=left pose/FOV for both slots,
   2=right pose/FOV for both slots. Changes revoke pending ownership. */
void mgs4vr_eye_control(int mode);
void mgs4vr_eye_views(const MGS4VR_XR_VIEWS *views,uint64_t now_ms);
int mgs4vr_eye_prepare(unsigned serial,int cinematic,int width,int height,uint64_t now_ms,MGS4VR_EYE_TICKET *ticket);
int mgs4vr_eye_commit(const MGS4VR_EYE_TICKET *ticket,int applied,const float combined[16]);
int mgs4vr_eye_candidate(unsigned render_serial,uint64_t now_ms,MGS4VR_EYE_PACKET *out);
unsigned mgs4vr_eye_epoch(void);
/* Scope view (rifle at the eye, MGS3 recipe): zoom > 1.05 renders BOTH eyes from the midpoint between the
   eyes with a symmetric frustum tan = s/zoom (s = smallest half-tangent of the eye) and submits the image
   with the symmetric tan s, so it is magnified zoom times and identical in both eyes. 0 = off.
   A change of zoom revokes pending ownership (no mixed pairs). */
void mgs4vr_eye_scope(float zoom);
float mgs4vr_eye_scope_zoom(void);
/* Pure: render/submit FOV angles (left,right,up,down radians) for an eye FOV and a zoom. 0 = invalid. */
int mgs4vr_eye_scope_fov(const float eye_fov[4],float zoom,float render_fov[4],float submit_fov[4]);
#endif
