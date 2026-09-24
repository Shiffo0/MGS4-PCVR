#include "../src/mgs4vr_stereo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){printf("FAIL line %d\n",__LINE__);return 1;} }while(0)
int main(void){
    float p[16]={1.8f,0,0,0,0,-3.2f,0,0,0,0,-.000038f,1,0,0,50,0};
    float f[4]={-.9f,.7f,.8f,-.75f},o[16],keep[16]; int depth,i,j; double z;
    for(depth=0;depth<3;++depth){
        p[14]=depth==0?50.0f:depth==1?50.05f:8002.0f;
        CHECK(mgs4vr_stereo_projection(p,f,o));
        for(i=0;i<16;++i) if(i!=0 && i!=5 && i!=8 && i!=9) CHECK(o[i]==p[i]);
        /* Independent oracle: rays through each runtime FOV boundary must land
           on the corresponding NDC edge, at near AND far distances. */
        for(j=0;j<2;++j){
            z=j?100000.0:100.0;
            CHECK(fabs((tan(f[0])*z*o[0]+z*o[8])/z+1)<1e-6);
            CHECK(fabs((tan(f[1])*z*o[0]+z*o[8])/z-1)<1e-6);
            CHECK(fabs((-tan(f[2])*z*o[5]+z*o[9])/z-1)<1e-6);
            CHECK(fabs((-tan(f[3])*z*o[5]+z*o[9])/z+1)<1e-6);
        }
        memcpy(keep,o,sizeof(o));CHECK(mgs4vr_stereo_projection(o,f,o));CHECK(memcmp(keep,o,sizeof(o))==0);
    }
    {
        float scale,p4,p5,aspect;int w;
        for(w=1280;w<=3840;w+=1280){
            CHECK(mgs4vr_stereo_builder(f,w,1080,&scale,&p4,&p5,&aspect));
            /* Independent native builder mapping, checked against boundary rays. */
            CHECK(fabs(tan(f[0])*scale+p4+1)<1e-6);
            CHECK(fabs(tan(f[1])*scale+p4-1)<1e-6);
            CHECK(fabs(tan(f[2])*scale*(float)w/1080*aspect+p5-1)<1e-6);
            CHECK(fabs(tan(f[3])*scale*(float)w/1080*aspect+p5+1)<1e-6);
        }
        CHECK(!mgs4vr_stereo_builder(f,0,1080,&scale,&p4,&p5,&aspect));
        CHECK(!mgs4vr_stereo_builder(f,1920,-1,&scale,&p4,&p5,&aspect));
    }
    f[1]=(float)NAN;CHECK(!mgs4vr_stereo_projection(p,f,o));CHECK(memcmp(keep,o,sizeof(o))==0);
    f[1]=f[0];CHECK(!mgs4vr_stereo_projection(p,f,o));
    f[1]=.7f;p[11]=0;CHECK(!mgs4vr_stereo_projection(p,f,o));
    CHECK(!mgs4vr_stereo_projection(NULL,f,o));
    puts("PASS: stereo_desk_test (asymmetric frustum edges, all depth variants, alias/idempotence, rejected input)");return 0;
}
