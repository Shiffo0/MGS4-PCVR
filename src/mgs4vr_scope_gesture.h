/* Rifle-to-eye scope gesture (9 Oct 2026).
   Pure math, desk-testable. Space: OpenXR LOCAL, metres, quaternions x,y,z,w, -Z forward.
   Opens when the right grip is within OPEN_MM of the head and the controller's aim direction is within
   OPEN_DEG of the gaze; closes beyond CLOSE_MM or CLOSE_DEG (hysteresis). */
#ifndef MGS4VR_SCOPE_GESTURE_H
#define MGS4VR_SCOPE_GESTURE_H
#include <math.h>
#define SG_OPEN_MM 200.0f
#define SG_CLOSE_MM 300.0f
#define SG_OPEN_DEG 30.0f
#define SG_CLOSE_DEG 45.0f
/* -Z axis of a quaternion (forward). */
static void sg_forward(const float q[4], float f[3]){
 float x=q[0],y=q[1],z=q[2],w=q[3];
 f[0]=-(2*(x*z+w*y)); f[1]=-(2*(y*z-w*x)); f[2]=-(1-2*(x*x+y*y));
}
/* Returns the new state (1 open, 0 closed) from the previous one; dist_mm/angle_deg out for logging. */
static int sg_update(int open, const float head_q[4], const float head_p[3], const float aim_q[4], const float grip_p[3],
                     float *dist_mm, float *angle_deg){
 float hf[3],af[3],d[3],dist,dot,ang;int i;
 for(i=0;i<4;++i)if(!isfinite(head_q[i])||!isfinite(aim_q[i]))return 0;
 for(i=0;i<3;++i)if(!isfinite(head_p[i])||!isfinite(grip_p[i]))return 0;
 sg_forward(head_q,hf);sg_forward(aim_q,af);
 for(i=0;i<3;++i)d[i]=grip_p[i]-head_p[i];
 dist=sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2])*1000.0f;
 dot=hf[0]*af[0]+hf[1]*af[1]+hf[2]*af[2];
 dot/=sqrtf((hf[0]*hf[0]+hf[1]*hf[1]+hf[2]*hf[2])*(af[0]*af[0]+af[1]*af[1]+af[2]*af[2]))+1e-9f;
 if(dot>1)dot=1;if(dot<-1)dot=-1;ang=acosf(dot)*57.29578f;
 if(dist_mm)*dist_mm=dist;if(angle_deg)*angle_deg=ang;
 if(open)return !(dist>SG_CLOSE_MM||ang>SG_CLOSE_DEG);
 return dist<=SG_OPEN_MM&&ang<=SG_OPEN_DEG;
}
/* Line gesture (13 Oct 2026): the headset run of 12 Oct never opened (opens=0). With a real rifle the right hand holds
   the trigger grip 25-35 cm in front of the eye, never within SG_OPEN_MM. Scoping = the eye sits on the rifle's line,
   behind the hand: line_mm = distance of the eye from the aim ray through the grip (the scope sits above the bore and
   the hand below it, so ~10-18 cm), along_mm = how far the eye is behind the grip along the aim ray, angle = aim vs
   gaze. Thresholds come from the marker (defaults below); open uses the open values, closing the wider ones. */
typedef struct { float line_open, line_close, along_min, along_max, deg_open, deg_close; } SG_LINE_CFG;
static const SG_LINE_CFG SG_LINE_DEFAULT = { 170.0f, 230.0f, 100.0f, 650.0f, 30.0f, 45.0f };
static int sg_update_line(int open, const SG_LINE_CFG *c, const float head_q[4], const float head_p[3], const float aim_q[4],
                          const float grip_p[3], float *line_mm, float *along_mm, float *angle_deg){
 float hf[3],af[3],d[3],n,along,perp[3],line,dot,ang;int i;
 for(i=0;i<4;++i)if(!isfinite(head_q[i])||!isfinite(aim_q[i]))return 0;
 for(i=0;i<3;++i)if(!isfinite(head_p[i])||!isfinite(grip_p[i]))return 0;
 sg_forward(head_q,hf);sg_forward(aim_q,af);
 n=sqrtf(af[0]*af[0]+af[1]*af[1]+af[2]*af[2]);if(n<1e-6f)return 0;for(i=0;i<3;++i)af[i]/=n;
 n=sqrtf(hf[0]*hf[0]+hf[1]*hf[1]+hf[2]*hf[2]);if(n<1e-6f)return 0;for(i=0;i<3;++i)hf[i]/=n;
 for(i=0;i<3;++i)d[i]=head_p[i]-grip_p[i];                 /* grip -> eye */
 along=-(d[0]*af[0]+d[1]*af[1]+d[2]*af[2]);                   /* > 0: the eye is behind the grip */
 for(i=0;i<3;++i)perp[i]=d[i]+along*af[i];
 line=sqrtf(perp[0]*perp[0]+perp[1]*perp[1]+perp[2]*perp[2])*1000.0f;along*=1000.0f;
 dot=hf[0]*af[0]+hf[1]*af[1]+hf[2]*af[2];if(dot>1)dot=1;if(dot<-1)dot=-1;ang=acosf(dot)*57.29578f;
 if(line_mm)*line_mm=line;if(along_mm)*along_mm=along;if(angle_deg)*angle_deg=ang;
 if(along<c->along_min||along>c->along_max)return 0;
 if(open)return line<=c->line_close&&ang<=c->deg_close;
 return line<=c->line_open&&ang<=c->deg_open;
}
#endif
