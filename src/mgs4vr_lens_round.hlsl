// Lens scope round mask (mgs4vr_lens_round.c). Compiled by tools: fxc /T vs_4_0 /E vs, /T ps_4_0 /E ps.
Texture2D T : register(t0);
SamplerState S : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; float2 u = float2((id << 1) & 2, id & 2); o.uv = u;
    o.p = float4(u.x * 2 - 1, 1 - u.y * 2, 0, 1); return o;
}
// premultiplied: opaque inside 0.94 of the radius, soft edge to 0.98, transparent outside
float4 ps(V i) : SV_Target {
    float3 c = T.SampleLevel(S, i.uv, 0).rgb;
    float r = length(i.uv - 0.5) * 2;
    float a = 1 - smoothstep(0.94, 0.98, r);
    return float4(c * a, a);
}
