// Embedded by tools/embed_observer_shader.py. No runtime file dependency.
cbuffer C:register(b0) {
    float4 L,R,crop,opt;
    float4 rot0,rot1,rot2,translation,dimensions;
};
Texture2D A:register(t0),B:register(t1),Field:register(t2);
SamplerState S:register(s0);
struct V {float4 p:SV_Position;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID) {
    V o;float2 u=float2((id<<1)&2,id&2);o.uv=u;
    o.p=float4(u.x*2-1,1-u.y*2,0,1);return o;
}
float2 uv(float2 p,float4 f){return float2((p.x-f.x)/(f.y-f.x),(f.w-p.y)/(f.w-f.z));}
float2 ray(float2 u){return float2(lerp(crop.x,crop.y,u.x),lerp(crop.w,crop.z,u.y));}
float3 rotate(float3 v){return float3(dot(rot0.xyz,v),dot(rot1.xyz,v),dot(rot2.xyz,v));}
float2 projectLeft(float2 p,float inverseDepth) {
    float3 v=rotate(float3(p,-1))+translation.xyz*inverseDepth;
    return v.z<-.01?v.xy/-v.z:float2(100,100);
}
bool inside(float2 p,float4 f){return p.x>f.x&&p.x<f.y&&p.y>f.z&&p.y<f.w;}
float3 left(float2 p){return A.SampleLevel(S,uv(p,L),0).rgb;}
float3 right(float2 p){return B.SampleLevel(S,uv(p,R),0).rgb;}
float difference(float3 a,float3 b){return dot(abs(a-b),float3(.333333,.333333,.333333));}
// Small colour patches; no global image offset can align multiple depths.
float cost(float2 p,float d) {
    float2 step=float2(R.y-R.x,R.w-R.z)/dimensions.xy*2;
    float error=0;
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++) {
        if(x!=0&&y!=0)continue;
        float2 pr=p+step*float2(x,y),pl=projectLeft(pr,d);
        if(!inside(pl,L)||!inside(pr,R))return 100;
        error+=difference(left(pl),right(pr));
    }
    return error/5;
}
float textureVariation(float2 p) {
    float2 step=float2(R.y-R.x,R.w-R.z)/dimensions.xy*2;
    float3 c=right(p);
    return difference(c,right(p+float2(step.x,0)))+difference(c,right(p+float2(0,step.y)));
}
float4 match(V i):SV_Target {
    float2 p=ray(float2(i.uv.x*.25,i.uv.y));
    if(dot(translation.xyz,translation.xyz)<1e-10)return float4(0,cost(p,0)<.035?1:0,0,0);
    if(textureVariation(p)<.012)return 0;
    float best=100,depth=0,second=100,secondDepth=-100;
    // One bounded search. Track a separated competing match without repeating
    // every texture lookup for the uniqueness test.
    [loop]for(int n=0;n<=48;n++) {
        float d=n*translation.w/48,e=cost(p,d);
        if(e<best){if(abs(d-depth)>.26){second=best;secondDepth=depth;}best=e;depth=d;}
        else if(abs(d-depth)>.26&&e<second){second=e;secondDepth=d;}
    }
    float step=translation.w/48;
    [loop]for(int level=0;level<4;level++) {
        step*=.25;float centre=depth;
        [loop]for(int j=-3;j<=3;j++) {
            float d=clamp(centre+j*step,0,translation.w),e=cost(p,d);
            if(e<best){best=e;depth=d;}
        }
    }
    float confidence=(best<.035 && abs(secondDepth-depth)>.26 && second>best*1.35+.006)?1:0;
    return float4(depth,confidence,best,0);
}
float4 ps(V i):SV_Target {
    float2 p=ray(i.uv);float3 r=right(p);
    if(opt.x<1.5||i.uv.x>=.25||opt.z<=0)return float4(r,1);
    int2 size=int2(dimensions.zw);
    int2 cell=clamp(int2(i.uv*float2(4,1)*size),int2(0,0),size-1);
    float4 m=Field.Load(int3(cell,0));
    if(m.y<.5)return float4(r,1);
    float2 pl=projectLeft(p,m.x);
    if(!inside(pl,L))return float4(r,1);
    // Do not interpolate disparity across a depth/occlusion edge. Validate
    // every full-resolution output pixel as well as the coarse match cell.
    float3 l=left(pl);float e=difference(l,r);
    if(e>.06)return float4(r,1);
    float w=saturate((opt.w-p.x)/max(opt.y,.0001))*opt.z;
    w*=1-smoothstep(.025,.06,e);
    return float4(lerp(r,l,w),1);
}
