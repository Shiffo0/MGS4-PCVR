#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <string.h>
#include "mgs4vr_eye.h"
#include "mgs4vr_head.h"
#include "mgs4vr_stereo.h"
static SRWLOCK lock=SRWLOCK_INIT;
static volatile LONG on,generation=1;
static MGS4VR_XR_VIEWS views;
static uint64_t received;
static MGS4VR_PACKET_HISTORY_STATE history;
static MGS4VR_EYE_TICKET pending;
static float reference_q[4],reference_p[3];
static int have_reference,pending_width,pending_height,control_mode;
static volatile LONG scope_milli;static LONG scope_used;
static volatile LONG twoeye_mode;static MGS4VR_EYE_PACKET right_hist[MGS4VR_PACKET_HISTORY];   /* by camera serial: Present takes N-1 */
int mgs4vr_eye_fov_mirrored(const float l[4],const float r[4]){
 const float tol=.005f;int i;
 if(!l||!r)return 0;for(i=0;i<4;++i)if(!isfinite(l[i])||!isfinite(r[i]))return 0;
 return fabsf(l[0]+r[1])<=tol&&fabsf(l[1]+r[0])<=tol&&fabsf(l[2]-r[2])<=tol&&fabsf(l[3]-r[3])<=tol;
}
/* Eye translation in the reference basis -> camera adjust x/y/z (game units). 0 = rejected (> 1.5 m). */
static int eye_offset(const float eye_pos[3],const MGS4VR_HEAD_CFG *cfg,MGS4VR_CAM_ADJUST *a){
 float norm=0,d[3],q[4],t[3],local[3];int i;
 for(i=0;i<3;++i){d[i]=eye_pos[i]-reference_p[i];norm+=d[i]*d[i];q[i]=-reference_q[i];}
 q[3]=reference_q[3];if(norm>2.25f)return 0;
 t[0]=2*(q[1]*d[2]-q[2]*d[1]);t[1]=2*(q[2]*d[0]-q[0]*d[2]);t[2]=2*(q[0]*d[1]-q[1]*d[0]);
 local[0]=d[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1];
 local[1]=d[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2];
 local[2]=d[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0];
 a->x=local[0]*cfg->units_per_m;a->y=local[1]*cfg->units_per_m;a->z=-local[2]*cfg->units_per_m;return 1;
}
int mgs4vr_eye_scope_fov(const float f[4],float zoom,float r[4],float s4[4]){
 float s,a,b;int i;
 if(!f||!r||!s4||!isfinite(zoom)||zoom<1.05f||zoom>40.f)return 0;
 for(i=0;i<4;++i)if(!isfinite(f[i])||fabsf(f[i])>=1.5707f)return 0;
 if(f[0]>=0||f[1]<=0||f[2]<=0||f[3]>=0)return 0;
 s=tanf(-f[0]);if(tanf(f[1])<s)s=tanf(f[1]);if(tanf(f[2])<s)s=tanf(f[2]);if(tanf(-f[3])<s)s=tanf(-f[3]);
 a=atanf(s/zoom);b=atanf(s);
 r[0]=-a;r[1]=a;r[2]=a;r[3]=-a;s4[0]=-b;s4[1]=b;s4[2]=b;s4[3]=-b;return 1;
}
void mgs4vr_eye_scope(float zoom){LONG z=(isfinite(zoom)&&zoom>=1.05f&&zoom<=40.f)?(LONG)(zoom*1000.f+.5f):0;InterlockedExchange(&scope_milli,z);}
float mgs4vr_eye_scope_zoom(void){return InterlockedCompareExchange(&scope_milli,0,0)/1000.f;}
static unsigned epoch(void){return (unsigned)InterlockedCompareExchange(&generation,0,0);}
static void revoke(void){InterlockedIncrement(&generation);}
static void reset_locked(void){
 mgs4vr_packet_reset(&history,epoch());memset(&pending,0,sizeof(pending));have_reference=0;
}
static int fresh(uint64_t now){return views.valid && now>=received && now-received<=400;}
static int valid_views(const MGS4VR_XR_VIEWS *v){
 int e,i;float n;
 if(!v || !v->valid || !v->sequence || v->display_time<=0)return 0;
 for(e=0;e<2;++e){
  n=0;
  for(i=0;i<4;++i){
   if(!isfinite(v->quat[e][i]) || !isfinite(v->fov[e][i]) || fabsf(v->fov[e][i])>=1.5707f)return 0;
   n+=v->quat[e][i]*v->quat[e][i];
  }
  for(i=0;i<3;++i)if(!isfinite(v->pos[e][i]))return 0;
  if(fabsf(n-1)>.01f || v->fov[e][0]>=v->fov[e][1] || v->fov[e][3]>=v->fov[e][2])return 0;
 }
 return 1;
}
void mgs4vr_eye_enable(int enabled){
 AcquireSRWLockExclusive(&lock);InterlockedExchange(&on,enabled);revoke();reset_locked();
 memset(&views,0,sizeof(views));ReleaseSRWLockExclusive(&lock);
}
void mgs4vr_eye_control(int mode){
 if(mode<0 || mode>2)mode=0;
 AcquireSRWLockExclusive(&lock);
 if(control_mode!=mode){control_mode=mode;revoke();reset_locked();}
 ReleaseSRWLockExclusive(&lock);
}
void mgs4vr_eye_views(const MGS4VR_XR_VIEWS *v,uint64_t now){
 AcquireSRWLockExclusive(&lock);
 if(on){
  if(valid_views(v)){views=*v;received=now;}
  else {memset(&views,0,sizeof(views));revoke();reset_locked();}
 }
 ReleaseSRWLockExclusive(&lock);
}
int mgs4vr_eye_prepare(unsigned serial,int cinematic,int width,int height,uint64_t now,MGS4VR_EYE_TICKET *ticket){
 int e,source,i,ok=0;MGS4VR_HEAD_CFG cfg;float norm=0,dot=0,d[3],q[4],t[3],local[3];
 float render_fov[4],submit_fov[4],eye_pos[3];LONG zm;int scoped=0,two=0;
 if(!ticket)return -1;
 memset(ticket,0,sizeof(*ticket));
 if(!on)return 0;
 /* VEH never waits. A skipped preparation revokes ownership even if a writer
    is suspended while holding the lock. Readers also compare atomic epoch. */
 if(!TryAcquireSRWLockExclusive(&lock)){revoke();return -1;}
 if(history.epoch!=epoch())reset_locked();
 if(!on || cinematic || !fresh(now) || width<=0 || height<=0)goto failed;
 zm=InterlockedCompareExchange(&scope_milli,0,0);
 if(zm!=scope_used){scope_used=zm;revoke();reset_locked();}   /* never pair a zoomed with an unzoomed eye */
 if(pending.packet.valid && pending.packet.camera_serial==serial){
  if(width!=pending_width || height!=pending_height)goto failed;
  *ticket=pending;ok=1;goto done;
 }
 if(!have_reference){
  for(i=0;i<4;++i)dot+=views.quat[0][i]*views.quat[1][i];
  for(i=0;i<4;++i){reference_q[i]=views.quat[0][i]+(dot<0?-views.quat[1][i]:views.quat[1][i]);norm+=reference_q[i]*reference_q[i];}
  if(norm<.001f)goto failed;
  for(i=0;i<4;++i)reference_q[i]/=sqrtf(norm);
  for(i=0;i<3;++i)reference_p[i]=(views.pos[0][i]+views.pos[1][i])*.5f;
  have_reference=1;
 }
 e=serial&1;source=control_mode?control_mode-1:e;
 /* Two-eye mode: the main camera is always the left eye, the second view the right one (same frame). Only when the
    right FOV mirrors the left one (the right projection is then derived exactly), else ordinary AFR. */
 two=InterlockedCompareExchange(&twoeye_mode,0,0) && !control_mode && mgs4vr_eye_fov_mirrored(views.fov[0],views.fov[1]);
 if(two){e=0;source=0;}
 /* Keep alternating destination slots and frame ownership. Only the source
    eye changes; submitted pose/FOV must describe the actual rendered camera. */
 memset(&pending,0,sizeof(pending));
 memcpy(render_fov,views.fov[source],16);memcpy(submit_fov,views.fov[source],16);memcpy(eye_pos,views.pos[source],12);
 if(zm && mgs4vr_eye_scope_fov(views.fov[source],zm/1000.f,render_fov,submit_fov)){
  scoped=1;for(i=0;i<3;++i)eye_pos[i]=(views.pos[0][i]+views.pos[1][i])*.5f;   /* mono: no eye separation */
 }
 if(!mgs4vr_stereo_builder(render_fov,width,height,&pending.args[0],&pending.args[1],&pending.args[2],&pending.args[3]))goto failed;
 pending.adjust.enabled=1;pending.adjust.cine_auto=1;
 mgs4vr_head_cfg_defaults(&cfg);
 mgs4vr_head_to_offset(&cfg,reference_q,reference_p,views.quat[source],eye_pos,&pending.adjust);
 /* Packet orientation uses the full reference rotation. Translation must use
    that same basis (the ordinary head helper intentionally uses heading only).
    Reject excessive displacement instead of clamping while retaining a false
    unmodified OpenXR pose in the packet. World scale remains provisional. */
 if(!eye_offset(eye_pos,&cfg,&pending.adjust))goto failed;
 (void)norm;(void)dot;(void)d;(void)q;(void)t;(void)local;
 if(two){float rfov[4],sfov[4],rpos[3];MGS4VR_EYE_PACKET *r=&pending.packet_right;
  memcpy(rfov,views.fov[1],16);memcpy(sfov,views.fov[1],16);memcpy(rpos,views.pos[1],12);
  if(scoped&&mgs4vr_eye_scope_fov(views.fov[1],zm/1000.f,rfov,sfov))memcpy(rpos,eye_pos,12);
  pending.adjust_right.enabled=1;pending.adjust_right.cine_auto=1;
  mgs4vr_head_to_offset(&cfg,reference_q,reference_p,views.quat[1],rpos,&pending.adjust_right);
  if(!eye_offset(rpos,&cfg,&pending.adjust_right))goto failed;
  pending.twoeye=1;r->valid=1;r->eye=1;r->twoeye=1;r->epoch=history.epoch;r->camera_serial=serial;
  r->pose_sequence=views.sequence;r->display_time=views.display_time;
  memcpy(r->quat,views.quat[1],16);memcpy(r->pos,rpos,12);memcpy(r->fov,sfov,16);
 }
 pending.packet.twoeye=two;
 pending.packet.valid=1;pending.packet.eye=e;pending.packet.epoch=history.epoch;
 pending.packet.camera_serial=serial;pending.packet.pose_sequence=views.sequence;pending.packet.display_time=views.display_time;
 memcpy(pending.packet.quat,views.quat[source],sizeof(pending.packet.quat));
 memcpy(pending.packet.pos,eye_pos,sizeof(pending.packet.pos));
 memcpy(pending.packet.fov,submit_fov,sizeof(pending.packet.fov));(void)scoped;
 pending_width=width;pending_height=height;
 *ticket=pending;ok=1;goto done;
failed:
 revoke();reset_locked();
done:
 ReleaseSRWLockExclusive(&lock);return ok?1:-1;
}
int mgs4vr_eye_commit(const MGS4VR_EYE_TICKET *ticket,int applied,const float combined[16]){
 MGS4VR_EYE_PACKET packet;int ok=0;
 if(!on)return 0;
 if(!TryAcquireSRWLockExclusive(&lock)){revoke();return 0;}
 if(!on || !ticket || !applied || !combined || history.epoch!=epoch() ||
    !pending.packet.valid || memcmp(ticket,&pending,sizeof(pending)))goto failed;
 packet=ticket->packet;memcpy(packet.combined,combined,sizeof(packet.combined));
 ok=mgs4vr_packet_store(&history,&packet);
 if(ok){MGS4VR_EYE_PACKET *s=&right_hist[ticket->packet.camera_serial%MGS4VR_PACKET_HISTORY];if(ticket->twoeye)*s=ticket->packet_right;else memset(s,0,sizeof(*s));}
 if(ok)goto done;
failed:
 revoke();reset_locked();
done:
 ReleaseSRWLockExclusive(&lock);return ok;
}
int mgs4vr_eye_candidate(unsigned serial,uint64_t now,MGS4VR_EYE_PACKET *out){
 int ok;if(!out)return 0;memset(out,0,sizeof(*out));
 if(!on || !TryAcquireSRWLockShared(&lock))return 0;
 ok=on && history.epoch==epoch() && fresh(now) && mgs4vr_packet_candidate(&history,serial,out);
 if(ok && out->epoch!=epoch()){memset(out,0,sizeof(*out));ok=0;}
 ReleaseSRWLockShared(&lock);return ok;
}

unsigned mgs4vr_eye_epoch(void){return epoch();}
void mgs4vr_eye_twoeye(int enabled){
 AcquireSRWLockExclusive(&lock);
 if(InterlockedExchange(&twoeye_mode,enabled?1:0)!=(enabled?1:0)){revoke();reset_locked();memset(right_hist,0,sizeof(right_hist));}
 ReleaseSRWLockExclusive(&lock);
}
int mgs4vr_eye_right(unsigned serial,MGS4VR_EYE_PACKET *out){
 int ok;if(!out)return 0;memset(out,0,sizeof(*out));
 if(!on || !TryAcquireSRWLockShared(&lock))return 0;
 {const MGS4VR_EYE_PACKET *s=&right_hist[serial%MGS4VR_PACKET_HISTORY];
  ok=s->valid && s->camera_serial==serial && s->epoch==epoch();
  if(ok)*out=*s;}
 ReleaseSRWLockShared(&lock);return ok;
}
