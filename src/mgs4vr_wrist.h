/* Wrist panels (9-10 Oct 2026): right inner wrist = weapon and
   ammo, left wrist = watch (life / camo / item). They open only like checking a watch: the panel faces the head
   and the eyes look at it. Pure math here (desk-testable). OpenXR LOCAL space, metres, quaternions x,y,z,w. */
#ifndef MGS4VR_WRIST_H
#define MGS4VR_WRIST_H
#include <math.h>
typedef struct { float q[4], p[3]; } WR_POSE;
typedef struct {
    float along, out, side;       /* offset from the grip: along grip +Z (towards the elbow), out along the normal */
    int normal_axis;              /* 0 = grip X, 1 = grip Y: the panel's front faces side * that axis */
    float width;                  /* quad width in metres */
    float open_face, open_gaze, close_face, close_gaze;   /* degrees */
    unsigned open_ms;
} WR_CFG;
typedef struct { int open, counting; unsigned long long since; float face, gaze; } WR_GATE;
static void wr_rotate(const float q[4], const float v[3], float o[3]) {
    float x=q[0],y=q[1],z=q[2],w=q[3], t[3];
    t[0]=2*(y*v[2]-z*v[1]); t[1]=2*(z*v[0]-x*v[2]); t[2]=2*(x*v[1]-y*v[0]);
    o[0]=v[0]+w*t[0]+(y*t[2]-z*t[1]); o[1]=v[1]+w*t[1]+(z*t[0]-x*t[2]); o[2]=v[2]+w*t[2]+(x*t[1]-y*t[0]);
}
static void wr_norm3(float v[3]) { float n=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); if(n>1e-9f){v[0]/=n;v[1]/=n;v[2]/=n;} }
static float wr_angle(const float a[3], const float b[3]) {
    float d=(a[0]*b[0]+a[1]*b[1]+a[2]*b[2])/(sqrtf((a[0]*a[0]+a[1]*a[1]+a[2]*a[2])*(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]))+1e-12f);
    if(d>1)d=1; if(d<-1)d=-1; return acosf(d)*57.29578f;
}
/* Quaternion from an orthonormal basis given as columns X, Y, Z (each a world vector). */
static void wr_quat_from_axes(const float X[3], const float Y[3], const float Z[3], float q[4]) {
    float m00=X[0],m11=Y[1],m22=Z[2],tr=m00+m11+m22,s;
    if(tr>0){s=sqrtf(tr+1)*2;q[3]=.25f*s;q[0]=(Y[2]-Z[1])/s;q[1]=(Z[0]-X[2])/s;q[2]=(X[1]-Y[0])/s;}
    else if(m00>m11&&m00>m22){s=sqrtf(1+m00-m11-m22)*2;q[3]=(Y[2]-Z[1])/s;q[0]=.25f*s;q[1]=(Y[0]+X[1])/s;q[2]=(Z[0]+X[2])/s;}
    else if(m11>m22){s=sqrtf(1+m11-m00-m22)*2;q[3]=(Z[0]-X[2])/s;q[0]=(Y[0]+X[1])/s;q[1]=.25f*s;q[2]=(Z[1]+Y[2])/s;}
    else{s=sqrtf(1+m22-m00-m11)*2;q[3]=(X[1]-Y[0])/s;q[0]=(Z[0]+X[2])/s;q[1]=(Z[1]+Y[2])/s;q[2]=.25f*s;}
}
/* Panel pose: front (+Z) along side * grip axis, top (+Y) towards the hand (grip -Z). */
static void wr_quad_pose(const WR_CFG *c, const WR_POSE *grip, WR_POSE *out) {
    float ax[3]={0,0,0}, n[3], up[3], x[3], off[3], gz[3]={0,0,1}, mz[3]={0,0,-1};
    ax[c->normal_axis?1:0]=c->side; wr_rotate(grip->q,ax,n); wr_rotate(grip->q,mz,up);
    x[0]=up[1]*n[2]-up[2]*n[1]; x[1]=up[2]*n[0]-up[0]*n[2]; x[2]=up[0]*n[1]-up[1]*n[0]; wr_norm3(x);
    wr_quat_from_axes(x,up,n,out->q);
    wr_rotate(grip->q,gz,off);
    out->p[0]=grip->p[0]+off[0]*c->along+n[0]*c->out; out->p[1]=grip->p[1]+off[1]*c->along+n[1]*c->out; out->p[2]=grip->p[2]+off[2]*c->along+n[2]*c->out;
}
/* Returns 1 while the panel is shown. face: angle between the panel front and the direction to the head;
   gaze: angle between the head's forward (-Z) and the direction to the panel. */
static int wr_gate(WR_GATE *g, const WR_CFG *c, const WR_POSE *quad, const WR_POSE *head, int allowed, unsigned long long now) {
    float n[3], toh[3], fwd[3], top[3], z[3]={0,0,1}, mz[3]={0,0,-1}; int i, want;
    wr_rotate(quad->q,z,n); wr_rotate(head->q,mz,fwd);
    for(i=0;i<3;++i){toh[i]=head->p[i]-quad->p[i]; top[i]=-toh[i];}
    g->face=wr_angle(n,toh); g->gaze=wr_angle(fwd,top);
    if(!allowed){g->open=0;g->counting=0;return 0;}
    if(g->open){ if(g->face>c->close_face||g->gaze>c->close_gaze){g->open=0;g->counting=0;} return g->open; }
    want=g->face<=c->open_face&&g->gaze<=c->open_gaze;
    if(!want){g->counting=0;return 0;}
    if(!g->counting){g->counting=1;g->since=now;}
    if(now-g->since>=c->open_ms)g->open=1;
    return g->open;
}
#endif
