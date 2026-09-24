#include "mgs4vr_stereo.h"
#include <math.h>
#include <string.h>
int mgs4vr_stereo_projection(const float native[16], const float fov[4], float out[16]) {
    float p[16]; double l,r,u,d; int i;
    if (!native || !fov || !out) return 0;
    for(i=0;i<16;++i) if(!isfinite(native[i])) return 0;
    for(i=0;i<4;++i) if(!isfinite(fov[i]) || fabsf(fov[i])>=1.5707f) return 0;
    if(fov[0]>=fov[1] || fov[3]>=fov[2]) return 0;
    /* Conservative perspective structure; off-axis terms 8/9 are allowed. */
    if(native[0]<=0 || native[5]>=0 || fabsf(native[11]-1)>1e-6f || native[14]<=0 ||
       fabsf(native[1])+fabsf(native[2])+fabsf(native[3])+fabsf(native[4])+fabsf(native[6])+fabsf(native[7])+
       fabsf(native[12])+fabsf(native[13])+fabsf(native[15])>1e-6f) return 0;
    l=tan((double)fov[0]);r=tan((double)fov[1]);u=tan((double)fov[2]);d=tan((double)fov[3]);
    if(r-l<1e-5 || u-d<1e-5) return 0;
    memcpy(p,native,sizeof(p));
    p[0]=(float)(2/(r-l)); p[5]=(float)(-2/(u-d));
    p[8]=(float)(-(r+l)/(r-l)); p[9]=(float)(-(u+d)/(u-d));
    memcpy(out,p,sizeof(p)); return 1;
}

int mgs4vr_stereo_builder(const float fov[4],int width,int height,float *scale,float *p4,float *p5,float *aspect){
 float native[16]={1,0,0,0,0,-1,0,0,0,0,0,1,0,0,50,0},out[16];
 if(width<=0 || height<=0 || !scale || !p4 || !p5 || !aspect || !mgs4vr_stereo_projection(native,fov,out))return 0;
 *scale=out[0];*p4=out[8];*p5=out[9];*aspect=-out[5]/out[0]*(float)height/(float)width;
 return isfinite(*aspect) && *aspect>0;
}
