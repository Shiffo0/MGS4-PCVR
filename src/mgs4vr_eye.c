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
 if(!ticket)return -1;
 memset(ticket,0,sizeof(*ticket));
 if(!on)return 0;
 /* VEH never waits. A skipped preparation revokes ownership even if a writer
    is suspended while holding the lock. Readers also compare atomic epoch. */
 if(!TryAcquireSRWLockExclusive(&lock)){revoke();return -1;}
 if(history.epoch!=epoch())reset_locked();
 if(!on || cinematic || !fresh(now) || width<=0 || height<=0)goto failed;
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
 /* Keep alternating destination slots and frame ownership. Only the source
    eye changes; submitted pose/FOV must describe the actual rendered camera. */
 memset(&pending,0,sizeof(pending));
 if(!mgs4vr_stereo_builder(views.fov[source],width,height,&pending.args[0],&pending.args[1],&pending.args[2],&pending.args[3]))goto failed;
 pending.adjust.enabled=1;pending.adjust.cine_auto=1;
 mgs4vr_head_cfg_defaults(&cfg);
 mgs4vr_head_to_offset(&cfg,reference_q,reference_p,views.quat[source],views.pos[source],&pending.adjust);
 /* Packet orientation uses the full reference rotation. Translation must use
    that same basis (the ordinary head helper intentionally uses heading only).
    Reject excessive displacement instead of clamping while retaining a false
    unmodified OpenXR pose in the packet. World scale remains provisional. */
 norm=0;
 for(i=0;i<3;++i){d[i]=views.pos[source][i]-reference_p[i];norm+=d[i]*d[i];q[i]=-reference_q[i];}
 q[3]=reference_q[3];if(norm>2.25f)goto failed;
 t[0]=2*(q[1]*d[2]-q[2]*d[1]);t[1]=2*(q[2]*d[0]-q[0]*d[2]);t[2]=2*(q[0]*d[1]-q[1]*d[0]);
 local[0]=d[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1];
 local[1]=d[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2];
 local[2]=d[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0];
 pending.adjust.x=local[0]*cfg.units_per_m;pending.adjust.y=local[1]*cfg.units_per_m;pending.adjust.z=-local[2]*cfg.units_per_m;
 pending.packet.valid=1;pending.packet.eye=e;pending.packet.epoch=history.epoch;
 pending.packet.camera_serial=serial;pending.packet.pose_sequence=views.sequence;pending.packet.display_time=views.display_time;
 memcpy(pending.packet.quat,views.quat[source],sizeof(pending.packet.quat));
 memcpy(pending.packet.pos,views.pos[source],sizeof(pending.packet.pos));
 memcpy(pending.packet.fov,views.fov[source],sizeof(pending.packet.fov));
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
