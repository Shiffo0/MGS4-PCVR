/* Native frame ownership only. No telemetry, file output or replay probes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "mgs4vr_render.h"
#include "mgs4vr_profile.h"
#include "mgs4vr_eye.h"
#include "mgs4vr_uniform.h"
typedef void (__fastcall *SUBMIT)(void *,void *,void *,void *);
static SUBMIT original;
static BYTE *image;static void **slot;static volatile LONG enabled;
static __declspec(thread) MGS4VR_PACKET_SUBMISSION delivery;
static void matrix(unsigned location,const float *m,void *user){
 unsigned projection=*(unsigned *)user;if(location!=projection)mgs4vr_packet_uniform(&delivery,m);
}
static int decode(void *frame){
 __try{
  void **buffers=*(void ***)((BYTE *)frame+0x24d35a0);unsigned *buffer,sizes[6],capacity,projection;unsigned short p,c;
  if(!buffers || !buffers[0])return 0;buffer=buffers[0];capacity=buffer[0];
  if(capacity<4 || capacity>16*1024*1024)return 0;
  memcpy(sizes,image+MGS4VR_RVA_UNIFORM_SIZES,24);
  memcpy(&p,image+MGS4VR_RVA_PROJECTION_ID,2);memcpy(&c,image+MGS4VR_RVA_COMBINED_ID,2);projection=p;
  return mgs4vr_uniform_walk((BYTE *)buffer+8,capacity,sizes,p,c,matrix,&projection);
 }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static void __fastcall submit(void *self,void *frame,void *a,void *b){
 unsigned serial=0;int ok=0;memset(&delivery,0,sizeof(delivery));
 __try{
  if(enabled && frame){serial=*(unsigned *)((BYTE *)frame+0x2503728);delivery.render_serial=serial;
   if(mgs4vr_eye_candidate(serial,GetTickCount64(),&delivery.packet))ok=decode(frame);
  }
 }__except(EXCEPTION_EXECUTE_HANDLER){ok=0;}
 original(self,frame,a,b);mgs4vr_packet_finish(&delivery,ok);
}
int mgs4vr_render_start(void *base){
 BYTE *b=base;void *renderer;void **vt;DWORD old,tmp;
 static const BYTE sig[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x08};
 if(enabled)return 1;
 __try{
  if(memcmp(b+MGS4VR_RVA_SUBMIT,sig,sizeof(sig)))return 0;
  renderer=*(void **)(b+MGS4VR_RVA_RENDERER);if(!renderer)return 0;vt=*(void ***)renderer;
  if(vt!=(void **)(b+MGS4VR_RVA_RENDERER_VTABLE)||vt[41]!=b+MGS4VR_RVA_SUBMIT)return 0;
 }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
 slot=&vt[41];original=(SUBMIT)*slot;image=b;
 if(!VirtualProtect(slot,sizeof(void *),PAGE_READWRITE,&old))return 0;
 InterlockedExchange(&enabled,1);
 if(InterlockedCompareExchangePointer(slot,(void *)submit,(void *)original)!=(void *)original){InterlockedExchange(&enabled,0);VirtualProtect(slot,sizeof(void *),old,&tmp);return 0;}
 VirtualProtect(slot,sizeof(void *),old,&tmp);return 1;
}
void mgs4vr_render_stop(void){DWORD old,tmp;InterlockedExchange(&enabled,0);
 if(slot && VirtualProtect(slot,sizeof(void *),PAGE_READWRITE,&old)){InterlockedCompareExchangePointer(slot,(void *)original,(void *)submit);VirtualProtect(slot,sizeof(void *),old,&tmp);}}
int mgs4vr_render_serial(unsigned *serial){
 void *ctx,*enc,*frame;if(!enabled)return 0;
 __try{ctx=*(void **)(image+MGS4VR_RVA_CONTEXT);if(!ctx)return 0;enc=*(void **)((BYTE *)ctx+0x3d8);if(!enc)return 0;
 frame=*(void **)enc;if(!frame)return 0;*serial=*(unsigned *)((BYTE *)frame+0x2503728);return 1;
 }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
int mgs4vr_render_take(MGS4VR_EYE_PACKET *packet){
 if(!enabled){memset(packet,0,sizeof(*packet));return 0;}
 return mgs4vr_packet_take(&delivery,mgs4vr_eye_epoch(),packet);
}
