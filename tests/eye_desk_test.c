/* Include production implementation to exercise lock contention without
   adding a test-only API or a real game/VEH to the library. */
#include "../src/mgs4vr_eye.c"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){printf("FAIL eye line %d\n",__LINE__);return 1;}}while(0)
int main(void){
 MGS4VR_XR_VIEWS v={0};MGS4VR_EYE_TICKET a,b,again;MGS4VR_EYE_PACKET out;
 float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,5,6,7,1};int e;
 v.valid=1;v.sequence=1;v.display_time=100000;
 for(e=0;e<2;++e){v.quat[e][3]=1;v.pos[e][0]=e?.032f:-.032f;
  v.fov[e][0]=-.9f;v.fov[e][1]=.7f;v.fov[e][2]=.8f;v.fov[e][3]=-.75f;}
 mgs4vr_eye_enable(0);CHECK(mgs4vr_eye_prepare(100,0,1920,1080,100,&a)==0);
 mgs4vr_eye_enable(1);CHECK(mgs4vr_eye_prepare(100,0,1920,1080,100,&a)==-1);
 mgs4vr_eye_views(&v,100);CHECK(mgs4vr_eye_prepare(100,0,1920,1080,100,&a)==1);
 CHECK(fabsf(a.adjust.x+32)<.001f);CHECK(a.packet.eye==0);
 CHECK(!mgs4vr_eye_candidate(101,100,&out)); /* preparation is not publication */
 v.sequence=2;v.pos[0][0]=.1f;mgs4vr_eye_views(&v,101);
 CHECK(mgs4vr_eye_prepare(100,0,1920,1080,101,&again)==1);
 CHECK(memcmp(&a,&again,sizeof(a))==0); /* immutable across new XR samples */
 CHECK(mgs4vr_eye_commit(&a,1,matrix));
 CHECK(mgs4vr_eye_prepare(101,0,1920,1080,102,&b)==1);
 CHECK(fabsf(b.adjust.x-32)<.001f);CHECK(b.packet.pose_sequence==2);
 CHECK(mgs4vr_eye_commit(&b,1,matrix));CHECK(mgs4vr_eye_candidate(102,102,&out));
 CHECK(out.camera_serial==101 && out.pose_sequence==2);
 CHECK(!mgs4vr_eye_candidate(102,99,&out));CHECK(!mgs4vr_eye_candidate(102,502,&out));
 CHECK(!mgs4vr_eye_commit(&b,0,matrix));CHECK(!mgs4vr_eye_candidate(102,102,&out));
 CHECK(!mgs4vr_eye_commit(&a,1,matrix)); /* old epoch ticket */
 CHECK(mgs4vr_eye_prepare(102,1,1920,1080,103,&a)==-1);CHECK(!a.packet.valid);
 CHECK(mgs4vr_eye_prepare(102,0,0,1080,103,&a)==-1);
 CHECK(mgs4vr_eye_prepare(102,0,1920,1080,103,&a)==1);
 CHECK(mgs4vr_eye_prepare(102,0,1280,720,103,&b)==-1);CHECK(!mgs4vr_eye_commit(&a,1,matrix));
 CHECK(mgs4vr_eye_prepare(104,0,1920,1080,103,&a)==1);
 AcquireSRWLockExclusive(&lock);
 CHECK(mgs4vr_eye_prepare(105,0,1920,1080,103,&b)==-1);
 ReleaseSRWLockExclusive(&lock);
 CHECK(!b.packet.valid);CHECK(!mgs4vr_eye_commit(&a,1,matrix));
 v.valid=0;mgs4vr_eye_views(&v,104);CHECK(mgs4vr_eye_prepare(106,0,1920,1080,104,&a)==-1);
 v.valid=1;v.pos[0][0]=(float)NAN;mgs4vr_eye_views(&v,105);
 CHECK(mgs4vr_eye_prepare(106,0,1920,1080,105,&a)==-1);
 v.pos[0][0]=-.032f;mgs4vr_eye_views(&v,106);
 CHECK(mgs4vr_eye_prepare(106,0,1920,1080,106,&a)==1);CHECK(mgs4vr_eye_commit(&a,1,matrix));
 CHECK(mgs4vr_eye_prepare(107,0,1920,1080,106,&b)==1);CHECK(mgs4vr_eye_commit(&b,1,matrix));
 CHECK(mgs4vr_eye_candidate(108,106,&out));
 v.pos[0][0]=3;mgs4vr_eye_views(&v,107);
 CHECK(mgs4vr_eye_prepare(108,0,1920,1080,107,&a)==-1);CHECK(!a.packet.valid);
 CHECK(!mgs4vr_eye_candidate(108,107,&out));
 /* A tilted reference must rotate eye displacement in the same full basis
    as orientation: 90-degree roll maps world +/-Y separation to local +/-X. */
 mgs4vr_eye_enable(1);v.pos[0][0]=v.pos[1][0]=0;
 v.pos[0][1]=-.032f;v.pos[1][1]=.032f;
 for(e=0;e<2;++e){v.quat[e][2]=.70710678f;v.quat[e][3]=.70710678f;}
 mgs4vr_eye_views(&v,108);CHECK(mgs4vr_eye_prepare(110,0,1920,1080,108,&a)==1);
 CHECK(fabsf(a.adjust.x+32)<.001f && fabsf(a.adjust.y)<.001f && fabsf(a.adjust.roll)<.001f);
 /* Fixed-eye controls use identical geometry in alternating slots, including
    asymmetric FOV and orientation. Transport still owns two distinct frames. */
 memset(&v,0,sizeof(v));v.valid=1;v.sequence=20;v.display_time=200000;
 for(e=0;e<2;++e){v.quat[e][3]=1;v.pos[e][0]=e?.032f:-.032f;
  v.fov[e][0]=e?-.7f:-.9f;v.fov[e][1]=e?.9f:.7f;
  v.fov[e][2]=.8f;v.fov[e][3]=-.75f;}
 v.quat[1][1]=.01f;v.quat[1][3]=sqrtf(1-.0001f);
 mgs4vr_eye_views(&v,200);
 for(e=1;e<=2;++e){
  unsigned old_epoch=mgs4vr_eye_epoch();
  mgs4vr_eye_control(e);CHECK(mgs4vr_eye_epoch()!=old_epoch);
  CHECK(!mgs4vr_eye_commit(&a,1,matrix));
  CHECK(mgs4vr_eye_prepare(200,0,1920,1080,200,&a)==1);
  CHECK(mgs4vr_eye_commit(&a,1,matrix));
  CHECK(mgs4vr_eye_prepare(201,0,1920,1080,200,&b)==1);
  CHECK(mgs4vr_eye_commit(&b,1,matrix));
  CHECK(a.packet.eye==0 && b.packet.eye==1);
  CHECK(!memcmp(a.packet.pos,v.pos[e-1],sizeof(a.packet.pos)));
  CHECK(!memcmp(b.packet.pos,a.packet.pos,sizeof(a.packet.pos)));
  CHECK(!memcmp(a.packet.quat,v.quat[e-1],sizeof(a.packet.quat)));
  CHECK(!memcmp(b.packet.quat,a.packet.quat,sizeof(a.packet.quat)));
  CHECK(!memcmp(a.packet.fov,v.fov[e-1],sizeof(a.packet.fov)));
  CHECK(!memcmp(b.packet.fov,a.packet.fov,sizeof(a.packet.fov)));
  CHECK(!memcmp(&a.adjust,&b.adjust,sizeof(a.adjust)));
  CHECK(!memcmp(a.args,b.args,sizeof(a.args)));
  CHECK(mgs4vr_packet_pair(&a.packet,&b.packet));
  CHECK(mgs4vr_eye_candidate(202,200,&out));
  old_epoch=mgs4vr_eye_epoch();mgs4vr_eye_control(e);
  CHECK(mgs4vr_eye_epoch()==old_epoch); /* unchanged config keeps ownership */
 }
 mgs4vr_eye_control(99); /* invalid mode falls back to normal stereo */
 CHECK(!mgs4vr_eye_candidate(202,200,&out));
 CHECK(mgs4vr_eye_prepare(202,0,1920,1080,200,&a)==1);
 CHECK(mgs4vr_eye_commit(&a,1,matrix));
 CHECK(mgs4vr_eye_prepare(203,0,1920,1080,200,&b)==1);
 CHECK(a.adjust.x<0 && b.adjust.x>0);
 CHECK(fabsf(a.adjust.x+b.adjust.x)<.001f && fabsf(a.adjust.z+b.adjust.z)<.001f);
 CHECK(fabsf(a.adjust.x*a.adjust.x+a.adjust.z*a.adjust.z-1024)<.01f);
 CHECK(!memcmp(a.packet.pos,v.pos[0],sizeof(a.packet.pos)));
 CHECK(!memcmp(b.packet.pos,v.pos[1],sizeof(b.packet.pos)));
 CHECK(memcmp(a.args,b.args,sizeof(a.args))!=0);
 CHECK(mgs4vr_eye_prepare(204,1,1920,1080,200,&a)==-1);
 CHECK(mgs4vr_eye_prepare(204,0,1920,1080,601,&a)==-1);
 mgs4vr_eye_enable(0);CHECK(!mgs4vr_eye_candidate(108,106,&out));CHECK(!out.valid);
 puts("PASS: eye immutable tickets, explicit commit, offsets, tracking/stale/cinematic/resize/lock failure revocation and recovery");return 0;
}
