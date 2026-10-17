#ifndef MGS4VR_WEAPON_IDENTITY_H
#define MGS4VR_WEAPON_IDENTITY_H
#include <stdint.h>
#include <string.h>
typedef int (*wi_read)(void *,uint64_t,void *,unsigned);
typedef struct { uint64_t owner,actor,data,weapon,controller_handle,model_handle,controller,model_actor,table; unsigned id; int slot; } WI_IDENTITY;
static int wi_get(wi_read read,void*u,uint64_t a,void*out,unsigned n){return a>=0x10000&&a<0x800000000000ULL&&n<=256&&a+n>=a&&a+n<=0x800000000000ULL&&read(u,a,out,n);}
static int wi_q(wi_read read,void*u,uint64_t a,uint64_t*v){return wi_get(read,u,a,v,8);}
static int wi_handle(wi_read read,void*u,uint64_t h,uint64_t*p){uint64_t check;*p=h&0xfffffffffffffULL;return h&&wi_q(read,u,*p+0x30,&check)&&check==h;}
/* Observation only, deliberately limited to the three sampled weapon keys.
   Module/table constants are bound to executable 656EDE90. No guessed IDs. */
static int wi_once(wi_read read,void*u,uint64_t image,WI_IDENTITY*out){
 WI_IDENTITY r;uint64_t q;unsigned id2,packed;memset(&r,0,sizeof(r));
 if(!wi_get(read,u,image+0x23eb5150,&r.slot,4)||r.slot< -1||r.slot>23)return 0;
 if(!wi_q(read,u,image+0x23eb5160+(uint64_t)(r.slot+1)*0x80,&r.owner)||!wi_handle(read,u,r.owner,&r.actor)||
    !wi_q(read,u,r.actor+0x90,&r.data)||!wi_q(read,u,r.data+0xc8,&r.weapon))return 0;
 if(!wi_q(read,u,r.weapon+0x60,&r.controller_handle)||!wi_q(read,u,r.weapon+0x68,&r.model_handle)||
    !wi_handle(read,u,r.controller_handle,&r.controller)||!wi_handle(read,u,r.model_handle,&r.model_actor))return 0;
 if(!wi_q(read,u,r.controller+0xa0,&q)||q!=r.owner||!wi_q(read,u,r.model_actor+0x90,&q)||q!=r.controller_handle)return 0;
 /* controller+90 is NOT required to equal model_handle: all desktop witnesses
    disagree with that projectile-specific assumption. Muzzle guards unchanged. */
 if(!wi_get(read,u,r.weapon+0x70,&r.id,4)||!wi_get(read,u,r.weapon+0x7c,&id2,4)||!wi_get(read,u,r.model_actor+0xa8,&packed,4))return 0;
 /* 3/24/25 sampled 4 Oct; 0x27..0x32 (VSS, DSR-1, M82A2, M14 EBR, Mosin, XM25 grenade launcher 0x2F, Rail Gun 0x2D,
    MGL-140, FIM-92A 0x30, Javelin 0x31, RPG-7 0x32) seen with their HUD name in the simulator 12 Oct 2026. */
 if((r.id!=3&&r.id!=24&&r.id!=25&&(r.id<0x27||r.id>0x32))||r.id!=id2||r.id!=(packed>>16))return 0;
 if(!wi_q(read,u,r.weapon+0x58,&r.table)||r.table!=image+0x1d82580+(uint64_t)r.id*80)return 0;
 *out=r;return 1;
}
static int wi_resolve(wi_read read,void*u,uint64_t image,WI_IDENTITY*out){
 WI_IDENTITY a,b;if(!wi_once(read,u,image,&a)||!wi_once(read,u,image,&b)||memcmp(&a,&b,sizeof(a)))return 0;*out=a;return 1;
}
#endif
