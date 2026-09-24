#include "../src/mgs4vr_packet.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do{if(!(x)){printf("FAIL packet line %d\n",__LINE__);return 1;}}while(0)
static MGS4VR_EYE_PACKET make(unsigned serial){
 MGS4VR_EYE_PACKET p={0};int i;p.valid=1;p.epoch=7;p.camera_serial=serial;p.eye=serial&1;
 p.pose_sequence=99;p.display_time=1234567;p.quat[3]=1;
 p.fov[0]=-.9f;p.fov[1]=.7f;p.fov[2]=.8f;p.fov[3]=-.75f;
 for(i=0;i<16;++i)p.combined[i]=(float)(i+1);return p;
}
int main(void){
 MGS4VR_PACKET_HISTORY_STATE h;MGS4VR_EYE_PACKET a=make(100),b=make(101),out,keep,bad;
 MGS4VR_PACKET_SUBMISSION submission;float m[16];int i;
 mgs4vr_packet_reset(&h,7);
 CHECK(mgs4vr_packet_store(&h,&a));CHECK(!mgs4vr_packet_candidate(&h,102,&out));CHECK(!out.valid);
 CHECK(mgs4vr_packet_store(&h,&b));CHECK(mgs4vr_packet_candidate(&h,102,&out));
 CHECK(memcmp(&out,&b,sizeof(b))==0);keep=out;
 CHECK(!mgs4vr_packet_candidate(&h,101,&out));CHECK(!out.valid);
 CHECK(mgs4vr_packet_matrix_matches(&keep,b.combined));
 for(i=0;i<16;++i)m[i]=b.combined[(i%4)*4+i/4];
 CHECK(mgs4vr_packet_matrix_matches(&keep,m));m[3]+=.001f;CHECK(!mgs4vr_packet_matrix_matches(&keep,m));
 m[3]=(float)NAN;CHECK(!mgs4vr_packet_matrix_matches(&keep,m));
 CHECK(mgs4vr_packet_pair(&a,&b));CHECK(!mgs4vr_packet_pair(&b,&a));
 bad=b;bad.camera_serial+=2;CHECK(!mgs4vr_packet_pair(&a,&bad));
 bad=b;bad.epoch++;CHECK(!mgs4vr_packet_pair(&a,&bad));
 bad=b;bad.valid=0;CHECK(!mgs4vr_packet_store(&h,&bad));CHECK(!mgs4vr_packet_candidate(&h,102,&out));
 CHECK(mgs4vr_packet_store(&h,&b));bad=make(116);CHECK(mgs4vr_packet_store(&h,&bad));CHECK(!mgs4vr_packet_candidate(&h,102,&out));
 CHECK(mgs4vr_packet_store(&h,&a));bad=b;bad.eye=0;CHECK(mgs4vr_packet_store(&h,&bad));CHECK(!mgs4vr_packet_candidate(&h,102,&out));
 bad=b;bad.quat[3]=2;CHECK(!mgs4vr_packet_store(&h,&bad));
 bad=b;bad.combined[0]=(float)INFINITY;CHECK(!mgs4vr_packet_store(&h,&bad));
 bad=b;bad.pos[0]=(float)NAN;CHECK(!mgs4vr_packet_store(&h,&bad));
 bad=b;bad.fov[0]=bad.fov[1];CHECK(!mgs4vr_packet_store(&h,&bad));
 bad=b;bad.display_time=0;CHECK(!mgs4vr_packet_store(&h,&bad));
 bad=b;bad.pose_sequence=0;CHECK(!mgs4vr_packet_store(&h,&bad));
 mgs4vr_packet_reset(&h,8);CHECK(!mgs4vr_packet_store(&h,&a));
 mgs4vr_packet_reset(&h,7);a=make(0xffffffffu);b=make(0xfffffffeu);
 CHECK(mgs4vr_packet_store(&h,&a));CHECK(mgs4vr_packet_store(&h,&b));
 CHECK(mgs4vr_packet_candidate(&h,0,&out));CHECK(out.camera_serial==0xffffffffu);
 b=make(0);CHECK(mgs4vr_packet_pair(&b,&a));CHECK(mgs4vr_packet_store(&h,&b));
 CHECK(mgs4vr_packet_candidate(&h,1,&out));CHECK(out.camera_serial==0);
 mgs4vr_packet_begin(&submission,&h,1);
 mgs4vr_packet_uniform(&submission,b.combined);
 CHECK(!mgs4vr_packet_take(&submission,7,&out)); /* return not observed */
 mgs4vr_packet_begin(&submission,&h,1);mgs4vr_packet_uniform(&submission,b.combined);
 mgs4vr_packet_finish(&submission,0);CHECK(!mgs4vr_packet_take(&submission,7,&out));
 mgs4vr_packet_begin(&submission,&h,1);mgs4vr_packet_finish(&submission,1);
 mgs4vr_packet_uniform(&submission,b.combined);CHECK(!mgs4vr_packet_take(&submission,7,&out));
 mgs4vr_packet_begin(&submission,&h,1);mgs4vr_packet_uniform(&submission,b.combined);
 mgs4vr_packet_finish(&submission,1);CHECK(!mgs4vr_packet_take(&submission,8,&out));
 mgs4vr_packet_begin(&submission,&h,1);mgs4vr_packet_uniform(&submission,b.combined);
 mgs4vr_packet_finish(&submission,1);CHECK(mgs4vr_packet_take(&submission,7,&out));
 CHECK(memcmp(&out,&b,sizeof(b))==0);CHECK(!mgs4vr_packet_take(&submission,7,&out));
 puts("PASS: packet history, N-1 ownership, gap/epoch/slot revocation, exact matrix match, wrap, pair validation");return 0;
}
