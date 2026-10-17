#ifndef MGS4_OBSERVER_LAYOUT_H
#define MGS4_OBSERVER_LAYOUT_H
#include <math.h>
#include <string.h>
typedef struct { double left,right,down,up; } MGS4_OBSERVER_FOV;
typedef struct { double qx,qy,qz,qw,px,py,pz; } MGS4_OBSERVER_POSE;
/* Tangent-space extents: left, right, bottom, top. Square screen pixels need
   equal pixels per tangent unit in X/Y, independent of source texture shape. */
typedef struct { float eye[2][4], crop[4], options[4], rotation[3][4], translation[4], dimensions[4]; } MGS4_OBSERVER_LAYOUT;
static int observer_bounds(const MGS4_OBSERVER_FOV *f, float b[4]) {
    double a[4]={f->left,f->right,f->down,f->up}; int i;
    for(i=0;i<4;i++) {
        if(!isfinite(a[i]) || fabs(a[i])>=1.55)return 0;
        b[i]=(float)tan(a[i]);
    }
    return b[1]-b[0]>.001f && b[3]-b[2]>.001f;
}
static int observer_layout(const MGS4_OBSERVER_FOV *left,const MGS4_OBSERVER_FOV *right,
                           int mode,MGS4_OBSERVER_LAYOUT *o) {
    float l,r,b,t,w,h,cx,cy; int e=mode==0?0:1;
    memset(o,0,sizeof *o);
    if(mode<0 || mode>2)return 0;
    if(!observer_bounds(e?right:left,o->eye[e]))return 0;
    memcpy(o->eye[!e],o->eye[e],sizeof o->eye[e]);
    l=o->eye[e][0];r=o->eye[e][1];b=o->eye[e][2];t=o->eye[e][3];
    if(mode==2) {
        if(!observer_bounds(left,o->eye[0]))return 0;
        /* Keep the right reference frustum in every mode. Left contributes
           inside this fixed canvas, never expands/recentres the desktop. */
        if(o->eye[0][1]<=l || o->eye[0][0]>=r)return 0;
    }
    if(t<=b)return 0;
    w=r-l;h=t-b;
    if(w/h>16.0f/9.0f)w=h*(16.0f/9.0f);else h=w*(9.0f/16.0f);
    cx=(l+r)*.5f;cy=(b+t)*.5f;
    /* Move the framing halfway towards the forward ray, capped at 5% of
       the original width. Fit the same 16:9 rectangle inside the reference
       eye: do not expose missing pixels or change crop on pair fallback. */
    cx-=fmaxf(-w*.05f,fminf(w*.05f,cx*.5f));
    w=fminf(w,2*fminf(cx-l,r-cx));
    h=w*(9.0f/16.0f);
    o->crop[0]=cx-w*.5f;o->crop[1]=cx+w*.5f;
    o->crop[2]=cy-h*.5f;o->crop[3]=cy+h*.5f;
    o->options[0]=(float)mode;
    o->options[2]=mode==2?1.0f:0.0f;
    o->options[3]=o->crop[0]+w*.25f; /* left quarter, right dominant */
    /* Feather only a small boundary strip; never average the whole overlap. */
    o->options[1]=fminf(.02f,(fminf(o->eye[0][1],o->eye[1][1])-
                              fmaxf(o->eye[0][0],o->eye[1][0]))*.1f);
    return 1;
}
static int observer_pose_pair(const MGS4_OBSERVER_POSE *a,const MGS4_OBSERVER_POSE *b) {
    double dot=a->qx*b->qx+a->qy*b->qy+a->qz*b->qz+a->qw*b->qw;
    double aa=a->qx*a->qx+a->qy*a->qy+a->qz*a->qz+a->qw*a->qw;
    double bb=b->qx*b->qx+b->qy*b->qy+b->qz*b->qz+b->qw*b->qw;
    double dx=a->px-b->px,dy=a->py-b->py,dz=a->pz-b->pz;
    /* The shader compensates capture rotation and searches local disparity.
       Keep a bounded 5-degree temporal/cant window; larger gaps reject. */
    return isfinite(dot)&&aa>.9&&aa<1.1&&bb>.9&&bb<1.1 &&
           dot*dot/(aa*bb)>.99809735 && dx*dx+dy*dy+dz*dz<.0225;
}
/* Right camera ray -> left camera. Translation in metres, inverse depth in
   1/metres. Both arrays share the exact capture pose, not the latest XR pose. */
static void observer_rotate(const MGS4_OBSERVER_POSE *q,const double v[3],double o[3],int inverse) {
    double n=sqrt(q->qx*q->qx+q->qy*q->qy+q->qz*q->qz+q->qw*q->qw);
    double x=q->qx/n,y=q->qy/n,z=q->qz/n,w=q->qw/n;
    double tx,ty,tz;if(inverse){x=-x;y=-y;z=-z;}
    tx=2*(y*v[2]-z*v[1]);ty=2*(z*v[0]-x*v[2]);tz=2*(x*v[1]-y*v[0]);
    o[0]=v[0]+w*tx+y*tz-z*ty;o[1]=v[1]+w*ty+z*tx-x*tz;o[2]=v[2]+w*tz+x*ty-y*tx;
}
static void observer_reprojection(MGS4_OBSERVER_LAYOUT *o,const MGS4_OBSERVER_POSE *l,const MGS4_OBSERVER_POSE *r) {
    double v[3],a[3],b[3];int i,j;
    for(i=0;i<3;i++){v[0]=v[1]=v[2]=0;v[i]=1;observer_rotate(r,v,a,0);observer_rotate(l,a,b,1);for(j=0;j<3;j++)o->rotation[j][i]=(float)b[j];}
    v[0]=r->px-l->px;v[1]=r->py-l->py;v[2]=r->pz-l->pz;observer_rotate(l,v,b,1);
    for(i=0;i<3;i++)o->translation[i]=(float)b[i];o->translation[3]=8; /* >= 12.5 cm */
}
#endif
