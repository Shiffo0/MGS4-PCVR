#include "mgs4vr_packet.h"
#include <math.h>
#include <string.h>
static int sane(const MGS4VR_EYE_PACKET *p){
 int i;float norm=0;
 if(!p || !p->valid || p->eye<0 || p->eye>1 || !p->pose_sequence || p->display_time<=0)return 0;
 for(i=0;i<16;++i)if(!isfinite(p->combined[i]))return 0;
 for(i=0;i<4;++i){if(!isfinite(p->quat[i]) || !isfinite(p->fov[i]) || fabsf(p->fov[i])>=1.5707f)return 0;norm+=p->quat[i]*p->quat[i];}
 for(i=0;i<3;++i)if(!isfinite(p->pos[i]))return 0;
 return fabsf(norm-1)<.01f && p->fov[0]<p->fov[1] && p->fov[3]<p->fov[2];
}
void mgs4vr_packet_reset(MGS4VR_PACKET_HISTORY_STATE *h,uint32_t epoch){memset(h,0,sizeof(*h));h->epoch=epoch;}
int mgs4vr_packet_store(MGS4VR_PACKET_HISTORY_STATE *h,const MGS4VR_EYE_PACKET *p){
 if(!h || !p)return 0;
 /* A rejected update revokes the old slot too; never keep stale pose ownership. */
 memset(&h->slots[p->camera_serial%MGS4VR_PACKET_HISTORY],0,sizeof(*p));
 if(p->epoch!=h->epoch || !sane(p))return 0;
 h->slots[p->camera_serial%MGS4VR_PACKET_HISTORY]=*p;return 1;
}
int mgs4vr_packet_candidate(const MGS4VR_PACKET_HISTORY_STATE *h,uint32_t serial,MGS4VR_EYE_PACKET *out){
 const MGS4VR_EYE_PACKET *p,*prev;
 if(!out)return 0;memset(out,0,sizeof(*out));
 if(!h)return 0;
 p=&h->slots[(serial-1)%MGS4VR_PACKET_HISTORY];prev=&h->slots[(serial-2)%MGS4VR_PACKET_HISTORY];
 if(!p->valid || !prev->valid || p->epoch!=h->epoch || prev->epoch!=h->epoch ||
    p->camera_serial!=serial-1 || prev->camera_serial!=serial-2 || p->eye==prev->eye)return 0;
 *out=*p;return 1;
}
int mgs4vr_packet_matrix_matches(const MGS4VR_EYE_PACKET *p,const float m[16]){
 int i,direct=1,transposed=1;if(!p || !m || !p->valid)return 0;
 for(i=0;i<16;++i){if(!isfinite(m[i]))return 0;
  /* Uniform writer copies/transposes float32, so use exact equality. */
  if(m[i]!=p->combined[i])direct=0;
  if(m[i]!=p->combined[(i%4)*4+i/4])transposed=0;
 }return direct||transposed;
}
int mgs4vr_packet_pair(const MGS4VR_EYE_PACKET *l,const MGS4VR_EYE_PACKET *r){
 if(!sane(l)||!sane(r)||l->eye!=0||r->eye!=1||l->epoch!=r->epoch)return 0;
 return (uint32_t)(l->camera_serial-r->camera_serial)==1 || (uint32_t)(r->camera_serial-l->camera_serial)==1;
}

void mgs4vr_packet_begin(MGS4VR_PACKET_SUBMISSION *s,const MGS4VR_PACKET_HISTORY_STATE *h,uint32_t serial){
 memset(s,0,sizeof(*s));s->render_serial=serial;mgs4vr_packet_candidate(h,serial,&s->packet);
}
void mgs4vr_packet_uniform(MGS4VR_PACKET_SUBMISSION *s,const float combined[16]){
 if(!s->complete && mgs4vr_packet_matrix_matches(&s->packet,combined))s->matched=1;
}
void mgs4vr_packet_finish(MGS4VR_PACKET_SUBMISSION *s,int decode_ok){
 if(!decode_ok)s->packet.valid=0;
 s->complete=1;
}
int mgs4vr_packet_take(MGS4VR_PACKET_SUBMISSION *s,uint32_t current_epoch,MGS4VR_EYE_PACKET *out){
 int ok=s->complete && s->matched && s->packet.valid && s->packet.epoch==current_epoch;
 memset(out,0,sizeof(*out));if(ok)*out=s->packet;
 memset(s,0,sizeof(*s));return ok;
}
