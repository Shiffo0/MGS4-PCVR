/* Read-only, caller-supplied bounded reader. No process access or game writes here.
   Evidence: shot_owner_static_20260929. Coordinates remain native game units. */
#ifndef MGS4_MUZZLE_CONTRACT_H
#define MGS4_MUZZLE_CONTRACT_H
#include <stdint.h>
#include <string.h>
#include <math.h>
typedef int (*mc_read)(void *,uint64_t,void *,unsigned);
typedef struct { uint64_t owner,weapon,controller,model_actor; float native_shot[16],effect[16]; unsigned anchor_offset; } MC_RESULT;
static int mc_q(mc_read read,void *u,uint64_t a,uint64_t *v){return a>=0x10000 && a<0x800000000000ULL && read(u,a,v,8);}
static int mc_handle(mc_read read,void *u,uint64_t h,uint64_t *p){uint64_t check;*p=h&0xfffffffffffffULL;return h && mc_q(read,u,*p+0x30,&check)&&check==h;}
static int mc_matrix(mc_read read,void *u,uint64_t a,float *m){unsigned i;if(a<0x10000||a>=0x800000000000ULL||!read(u,a,m,64))return 0;for(i=0;i<16;++i)if(!isfinite(m[i]))return 0;return 1;}
static void mc_mul(float *out,const float *a,const float *b){unsigned r,c,k;for(r=0;r<4;++r)for(c=0;c<4;++c){float v=0;for(k=0;k<4;++k)v+=a[4*r+k]*b[4*k+c];out[4*r+c]=v;}}
/* Native D458B0: controller+90 is the child-list head, child+98 next.
   Read the entire bounded list so cycles after the target also fail closed.
   Fixed storage, no allocation/I/O, and generation/parent checks per node. */
#define MC_CHILD_LIMIT 64
typedef struct { unsigned count;uint64_t handles[MC_CHILD_LIMIT],next[MC_CHILD_LIMIT]; } MC_LIST;
static int mc_membership(mc_read read,void*u,uint64_t controller,uint64_t controller_handle,uint64_t wanted,MC_LIST*out){
 MC_LIST result;uint64_t h,p,parent,next;unsigned j;int found=0;
 memset(&result,0,sizeof(result));
 if(!wanted||!mc_q(read,u,controller+0x90,&h))return 0;
 while(h){
  if(result.count==MC_CHILD_LIMIT)return 0;
  for(j=0;j<result.count;++j)if(result.handles[j]==h)return 0;
  if(!mc_handle(read,u,h,&p)||!mc_q(read,u,p+0x90,&parent)||parent!=controller_handle||!mc_q(read,u,p+0x98,&next))return 0;
  result.handles[result.count]=h;result.next[result.count++]=next;
  if(h==wanted)found=1;h=next;
 }
 if(!found)return 0;*out=result;return 1;
}
/* expected_weapon comes from validated caller97EAB1 RSI; owner from constructor RCX.
   False rejects safely; output remains untouched. This is not an installed hook. */
static int mc_resolve_reason(mc_read read,void *u,uint64_t image,uint64_t owner,uint64_t expected_weapon,MC_RESULT *out,unsigned *reason){
 int slot;uint64_t slotp,active,actor,data,w,hc,hm,c,m,model,mat,q;unsigned flags,i;
 float base[16],local[16];MC_RESULT r;MC_LIST before,after;
 *reason=1;
 if(!read(u,image+0x23eb5150,&slot,4)||slot< -1||slot>23)return 0;
 slotp=image+0x23eb5160+(uint64_t)(slot+1)*0x80;
 if(!mc_q(read,u,slotp,&active)||active!=owner||!mc_handle(read,u,owner,&actor))return 0;
 *reason=2;
 if(!mc_q(read,u,actor+0x90,&data)||!mc_q(read,u,data+0xc8,&w)||w!=expected_weapon||!w)return 0;
 *reason=3;
 if(!mc_q(read,u,w+0x60,&hc)||!mc_q(read,u,w+0x68,&hm)||!mc_handle(read,u,hc,&c)||!mc_handle(read,u,hm,&m))return 0;
 *reason=4;if(!mc_q(read,u,c+0xa0,&q)||q!=owner)return 0;
 *reason=5;if(!mc_membership(read,u,c,hc,hm,&before))return 0;
 *reason=6;if(!mc_q(read,u,m+0x90,&q)||q!=hc)return 0;
 *reason=7;
 if(!mc_q(read,u,m+0x3b8,&model)||!mc_q(read,u,model+0x100,&mat)||!mc_matrix(read,u,mat,base)||!read(u,m+0xc0,&flags,4))return 0;
 memset(&r,0,sizeof(r));r.anchor_offset=(flags&0x1000000)?0x670:0x460;
 *reason=8;
 if(!mc_matrix(read,u,m+r.anchor_offset,local))return 0;mc_mul(r.native_shot,local,base);
 if(!mc_matrix(read,u,m+0x4a0,local))return 0;mc_mul(r.effect,local,base);
 for(i=0;i<16;++i)if(!isfinite(r.native_shot[i])||!isfinite(r.effect[i]))return 0;
 *reason=9;
 /* Recheck identity links after reads. Matrices are not an atomic snapshot. */
 {int s2;if(!read(u,image+0x23eb5150,&s2,4)||s2!=slot)return 0;}
 if(!mc_q(read,u,slotp,&q)||q!=owner||!mc_handle(read,u,owner,&actor)||!mc_q(read,u,actor+0x90,&q)||q!=data||!mc_q(read,u,data+0xc8,&q)||q!=w)return 0;
 if(!mc_q(read,u,w+0x60,&q)||q!=hc||!mc_q(read,u,w+0x68,&q)||q!=hm||!mc_handle(read,u,hc,&c)||!mc_handle(read,u,hm,&m))return 0;
 if(!mc_q(read,u,m+0x3b8,&q)||q!=model||!mc_q(read,u,model+0x100,&q)||q!=mat)return 0;
 if(!mc_q(read,u,c+0xa0,&q)||q!=owner||!mc_membership(read,u,c,hc,hm,&after)||memcmp(&before,&after,sizeof(before)))return 0;
 r.owner=owner;r.weapon=w;r.controller=c;r.model_actor=m;*out=r;*reason=0;return 1;
}
static int mc_resolve(mc_read read,void*u,uint64_t image,uint64_t owner,uint64_t weapon,MC_RESULT*out){unsigned reason;return mc_resolve_reason(read,u,image,owner,weapon,out,&reason);}
#endif
