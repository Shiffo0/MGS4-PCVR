/* Support-hand swing, independently reimplemented from the recovered geometric
   contract (Titanfall2VR.dll 7e2f8231..., RVA 0x770f0). No borrowed binary code.
   Row vectors: rotate the entire hand/weapon around the reached right-hand
   pivot. The minimal swing preserves the calibrated wrist's existing twist. */
static volatile LONG twohand_options = 1, twohand_allowed = 1, twohand_revision;
static LONG twohand_seen;
static int twohand_history, twohand_filter;
static float twohand_previous[16], twohand_foregrip[16], twohand_q[4];
static DWORD twohand_tick;
static volatile LONG twohand_frames, twohand_resets;

void mgs4vr_armsrc_configure(int enabled, int smoothing_ms, unsigned recenter) {
    static unsigned previous_recenter;
    LONG options;
    if (smoothing_ms != 30 && smoothing_ms != 60 && smoothing_ms != 90) smoothing_ms = 0;
    options = (!!enabled) | (smoothing_ms << 1);
    if (InterlockedExchange(&twohand_options, options) != options || previous_recenter != recenter) {
        previous_recenter = recenter; InterlockedIncrement(&twohand_revision);
    }
}
void mgs4vr_armsrc_gameplay(int allowed) {
    if (InterlockedExchange(&twohand_allowed, !!allowed) != !!allowed) InterlockedIncrement(&twohand_revision);
}
static void twohand_reset(void) {
    twohand_history = twohand_filter = 0;
    InterlockedIncrement(&twohand_resets);
}
static void twohand_quat(const float m[16], float q[4]) {
    float r[9], n = 0; int i,j;
    for (i=0;i<3;++i) for (j=0;j<3;++j) r[i*3+j]=m[i*4+j];
    mat_quat(r,q);
    for(i=0;i<4;++i)n+=q[i]*q[i];n=sqrtf(n);
    for(i=0;i<4;++i)q[i]/=n;
}
static void twohand_matrix(const float q[4], float m[16]) {
    float r[9];int i,j;quat_col(q,r);memset(m,0,64);
    for(i=0;i<3;++i)for(j=0;j<3;++j)m[i*4+j]=r[i*3+j];m[15]=1;
}
/* Shortest-arc SLERP; exact exponential alpha makes the response independent
   of frame rate. Quaternions represent a right-hand-local swing, so camera
   turns/translations and wrist motion remain immediate. */
static void twohand_slerp(const float a[4], const float b[4], float w, float q[4]) {
    float d=0,sign=1,wa,wb,n=0;int i;
    for(i=0;i<4;++i)d+=a[i]*b[i];
    if(d<0){d=-d;sign=-1;}d=fminf(1,d);
    if(d>.9995f){wa=1-w;wb=w;}
    else{float angle=acosf(d),s=sinf(angle);wa=sinf((1-w)*angle)/s;wb=sinf(w*angle)/s;}
    for(i=0;i<4;++i){q[i]=wa*a[i]+wb*sign*b[i];n+=q[i]*q[i];}
    n=sqrtf(n);for(i=0;i<4;++i)q[i]/=n;
}
/* Rigid swing only: target position and model scale are unchanged. Returns 0
   for an unusable lever rather than inventing an aiming direction. */
static int twohand_swing(const float hand[16], const float F[16], const float support[16], float local[16]) {
    float grip[16],u[3],v[3],lu,lv,R[9],G[16],swung[16],inv[16];int i;
    if(!finite16(hand)||!finite16(F)||!finite16(support))return 0;
    mul(F,hand,grip);
    for(i=0;i<3;++i){u[i]=grip[12+i]-hand[12+i];v[i]=support[12+i]-hand[12+i];}
    lu=len3(u);lv=len3(v);
    if(lu<100 || lu>=GRIP_MAX || lv<100 || lv>=GRIP_MAX+SNAP_EXIT)return 0;
    for(i=0;i<3;++i){u[i]/=lu;v[i]/=lv;}
    rot_between(u,v,R);about(R,hand+12,G);mul(hand,G,swung);
    rigid_inverse(hand,inv);mul(swung,inv,local);
    /* Algebraically zero; avoid large-world translation roundoff accumulation. */
    local[12]=local[13]=local[14]=0;
    return finite16(local);
}
/* Run once before the hand is published. The left IK then uses this same
   attachment blend and the final hand, as does the game's weapon attachment.
   Release checks use the PREVIOUS swung grip transported by today's right
   hand, so support-hand steering does not test against an unswung weapon. */
static int twohand_prepare(float hand[16], const float (*old)[16], LONG code, const HAND_VIEW *h,
                           const float ref_q[4], const float ref_p[3], const float C[16]) {
    float inv[16],F[16],ctrl[16],probe[16],grip[16],d[3],local[16],q[4],outq[4],out[16];
    static const float identity[4]={0,0,0,1};
    DWORD now=left_clock?left_clock():GetTickCount(),dt=now-twohand_tick;
    LONG options=InterlockedCompareExchange(&twohand_options,0,0),revision=InterlockedCompareExchange(&twohand_revision,0,0);
    int i,changed=0;float want;
    if(twohand_seen!=revision){twohand_seen=revision;twohand_reset();blend_init=snapped=0;}
    if(!(options&1)||!twohand_allowed||code==IK_FREE||code==IK_RIGHT||!h->lvalid ||
       !finite16(old[L_CLAV])||!finite16(old[L_ROOT])||!finite16(old[L_ELBOW])||!finite16(old[L_HAND])) {
        if(twohand_history){twohand_reset();blend_init=snapped=0;}return 0;
    }
    rigid_inverse(old[HAND],inv);mul(old[L_HAND],inv,F);
    controller_target(ref_q,ref_p,h->lgrip_q,h->lgrip_p,GRIP_O_L,GRIP_P_L,C,ctrl);
    /* No usable swing lever (pistols: foregrip < 100 mm from the right hand). Drop only the swing; the grip blend
       and snap state belong to the left hand (left_arm) and must survive, or every frame is an instant new grab
       (16 Oct headset: Operator, snap_enters 1530 vs exits 1). */
    if(!twohand_swing(hand,F,ctrl,local)){if(twohand_history)twohand_reset();twohand_history=twohand_filter=0;return 0;}
    if(twohand_history){
        for(i=0;i<12;++i)if(fabsf(F[i]-twohand_foregrip[i])>.03f)changed=1;
        for(i=12;i<15;++i)if(fabsf(F[i]-twohand_foregrip[i])>5)changed=1;
        if(changed||dt>STALE_MS){twohand_reset();blend_init=snapped=0;}
    }
    if(twohand_history && snapped)mul(twohand_previous,hand,probe);else memcpy(probe,hand,64);
    mul(F,probe,grip);
    for(i=0;i<3;++i)d[i]=ctrl[12+i]-grip[12+i];
    if(code==IK_GRIP)want=1;
    else {
        if(!snapped&&len3(d)<SNAP_ENTER){snapped=1;InterlockedIncrement(&snap_enters);}
        else if(snapped&&len3(d)>SNAP_EXIT){snapped=0;InterlockedIncrement(&snap_exits);}
        want=snapped?1.f:0.f;
    }
    if(!blend_init){blend=want;blend_init=1;}
    else blend_advance(want,now);
    blend_tick=now;
    if(want>0){
        twohand_quat(local,q);
        if(!twohand_filter || !(options>>1))memcpy(twohand_q,q,16);
        else twohand_slerp(twohand_q,q,1-expf(-(float)dt/(options>>1)),twohand_q);
        twohand_filter=1;
    }
    if(blend>0 && twohand_filter){
        /* On release retain the last swing and fade it out; pulling away must
           not steer toward the departing hand during the release transition. */
        twohand_slerp(identity,twohand_q,blend_ease(blend),outq);twohand_matrix(outq,out);
        memcpy(twohand_previous,out,64);mul(out,hand,hand);InterlockedIncrement(&twohand_frames);
    }else{twohand_matrix(identity,twohand_previous);twohand_filter=0;}
    memcpy(twohand_foregrip,F,64);twohand_history=1;twohand_tick=now;
    return 1;
}
