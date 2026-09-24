#include "../src/mgs4vr_render.c"
#define CHECK(x) do{if(!(x)){printf("FAIL line %d\n",__LINE__);return 1;}}while(0)
static BYTE *mutate_array;
static unsigned *mutate_buffer;
static int calls;static void *seen[4];
static void __fastcall fake(void*a,void*b,void*c,void*d){calls++;seen[0]=a;seen[1]=b;seen[2]=c;seen[3]=d;if(mutate_array)*mutate_array^=1;if(mutate_buffer){mutate_buffer[1]=4;mutate_buffer[3]^=1;}}
static void commit(BYTE*b,SIZE_T off,SIZE_T size){VirtualAlloc(b+(off&~4095ull),(size+(off&4095)+4095)&~4095ull,MEM_COMMIT,PAGE_EXECUTE_READWRITE);}
int main(void){
 BYTE *b=VirtualAlloc(NULL,0x241be000,MEM_RESERVE,PAGE_NOACCESS),*fr=VirtualAlloc(NULL,0x2503728+4096,MEM_RESERVE,PAGE_NOACCESS);
 BYTE ctx[1024]={0},enc[0x300]={0};void **vt;void *renderer;SUBMIT f;ULONG64 fn=(ULONG64)fake;int i,n=0,kinds[13]={0};unsigned buffer[40]={0},sizes[6]={4,0,16,36,64,1};void *buffers[1]={buffer};float matrix[16];
 CHECK(b&&fr);commit(b,MGS4VR_RVA_SUBMIT,32);commit(b,MGS4VR_RVA_RENDERER_VTABLE,512);commit(b,MGS4VR_RVA_RENDERER,8);commit(b,MGS4VR_RVA_CONTEXT,8);commit(fr,0x2503728,4);
 memcpy(b+MGS4VR_RVA_SUBMIT,"\x48\x8b\xc4\x48\x89\x58\x08\x48\xb8",9);memcpy(b+(MGS4VR_RVA_SUBMIT+9),&fn,8);memcpy(b+(MGS4VR_RVA_SUBMIT+17),"\xff\xe0",2);
 vt=(void **)(b+MGS4VR_RVA_RENDERER_VTABLE);renderer=&vt;*(void **)(b+MGS4VR_RVA_RENDERER)=renderer;vt[41]=b+MGS4VR_RVA_SUBMIT;
 *(void **)(b+MGS4VR_RVA_CONTEXT)=ctx;*(void **)(ctx+0x3d8)=enc;*(void **)enc=fr;*(unsigned *)(fr+0x2503728)=77;
 commit(fr,0x24d35a0,8);*(void ***)(fr+0x24d35a0)=buffers;
 commit(b,MGS4VR_RVA_UNIFORM_SIZES,24);memcpy(b+MGS4VR_RVA_UNIFORM_SIZES,sizes,24);
 commit(b,MGS4VR_RVA_COMBINED_ID,8);*(unsigned short *)(b+MGS4VR_RVA_COMBINED_ID)=21;*(unsigned short *)(b+MGS4VR_RVA_PROJECTION_ID)=22;
 commit(b,MGS4VR_RVA_MAIN_CAMERA,0x200);
 for(i=0;i<16;++i)matrix[i]=(float)(i+1);
 memcpy(b+MGS4VR_RVA_MAIN_CAMERA+0xc0,matrix,64);memcpy(b+MGS4VR_RVA_MAIN_CAMERA+0x100,matrix,64);
 buffer[0]=140;buffer[1]=0;buffer[2]=(4u<<27)|(22u<<11)|3;memcpy(buffer+3,matrix,64);
 buffer[19]=(4u<<27)|(21u<<11)|3;memcpy(buffer+20,matrix,64);buffer[36]=1;
 CHECK(mgs4vr_render_start(b));
 f=(SUBMIT)vt[41];f(renderer,fr,(void *)123,(void *)456);
 CHECK(calls==1 && seen[0]==renderer && seen[1]==fr && seen[2]==(void *)123 && seen[3]==(void *)456);
 {
 MGS4VR_XR_VIEWS v={0};MGS4VR_EYE_TICKET ticket;MGS4VR_EYE_PACKET packet;unsigned serial;int e;
 CHECK(mgs4vr_render_serial(&serial) && serial==77);v.valid=1;v.sequence=1;v.display_time=123;
 for(e=0;e<2;++e){v.quat[e][3]=1;v.pos[e][0]=e?.032f:-.032f;v.fov[e][0]=-.9f;v.fov[e][1]=.7f;v.fov[e][2]=.8f;v.fov[e][3]=-.75f;}
 mgs4vr_eye_enable(1);mgs4vr_eye_views(&v,GetTickCount64());
 CHECK(mgs4vr_eye_prepare(75,0,1920,1080,GetTickCount64(),&ticket)==1);CHECK(mgs4vr_eye_commit(&ticket,1,matrix));
 CHECK(mgs4vr_eye_prepare(76,0,1920,1080,GetTickCount64(),&ticket)==1);CHECK(mgs4vr_eye_commit(&ticket,1,matrix));
 f(renderer,fr,0,0);CHECK(mgs4vr_render_take(&packet) && packet.camera_serial==76 && packet.eye==0);
 CHECK(!mgs4vr_render_take(&packet));
 buffer[0]=139;f(renderer,fr,0,0);CHECK(!mgs4vr_render_take(&packet));buffer[0]=140;
 buffer[20]+=1;f(renderer,fr,0,0);CHECK(!mgs4vr_render_take(&packet));buffer[20]-=1;
 f(renderer,fr,0,0);mgs4vr_eye_enable(0);CHECK(!mgs4vr_render_take(&packet));
 }
 mgs4vr_render_stop();CHECK(vt[41]==b+MGS4VR_RVA_SUBMIT);
 vt[41]=(void *)fake;CHECK(!mgs4vr_render_start(b));
 VirtualFree(fr,0,MEM_RELEASE);VirtualFree(b,0,MEM_RELEASE);
 puts("PASS render ownership: guarded hook, exactly-once forwarding, matrix identity, truncation and stale epoch rejection");return 0;
}
