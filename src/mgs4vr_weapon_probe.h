/* Read-only identity snapshot; binary WP_PROBE layout retained for REC v2.
   Not an automatic profile selector and not a muzzle resolver. */
#ifndef MGS4VR_WEAPON_PROBE_H
#define MGS4VR_WEAPON_PROBE_H
#include "mgs4vr_weapon_identity.h"
typedef struct { uint64_t owner,weapon,controller,model_actor; unsigned char raw[3][256]; } WP_PROBE;
static int wp_probe(wi_read read,void *u,uint64_t image,WP_PROBE *out){
 WI_IDENTITY a,b;WP_PROBE p;
 if(!wi_resolve(read,u,image,&a))return 0;
 memset(&p,0,sizeof(p));p.owner=a.owner;p.weapon=a.weapon;p.controller=a.controller;p.model_actor=a.model_actor;
 if(!wi_get(read,u,a.weapon,p.raw[0],256)||!wi_get(read,u,a.controller,p.raw[1],256)||!wi_get(read,u,a.model_actor,p.raw[2],256))return 0;
 if(!wi_resolve(read,u,image,&b)||memcmp(&a,&b,sizeof(a)))return 0;
 /* Confirm identity-bearing words in copied payload, not only the endpoints. */
 {uint64_t hc,hm,table;unsigned id,id2,packed;memcpy(&hc,p.raw[0]+0x60,8);memcpy(&hm,p.raw[0]+0x68,8);memcpy(&table,p.raw[0]+0x58,8);
  memcpy(&id,p.raw[0]+0x70,4);memcpy(&id2,p.raw[0]+0x7c,4);memcpy(&packed,p.raw[2]+0xa8,4);
  if(hc!=a.controller_handle||hm!=a.model_handle||table!=a.table||id!=a.id||id2!=a.id||(packed>>16)!=a.id)return 0;}
 *out=p;return 1;
}
#endif
