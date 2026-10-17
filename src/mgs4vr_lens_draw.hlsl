// Lens scope drawn INTO the eye image (Titanfall 2 VR style, mgs4vr_lens_round.c mgs4vr_lens_round_draw).
// P[0..3] = clip-space corners of the ocular disk's bounding square (uv 00, 10, 01, 11; strip), opt.x = reticle on,
// opt.y = red dot sight (16 Oct 2026): no picture, clear glass with a faint rim and a glowing red dot at uv opt.zw = the
// barrel's point at infinity seen from this eye (mgs4vr_lens_dot_uv; off the glass when the eye is off the sight line).
cbuffer C : register(b0) { float4 P[4]; float4 opt; };
Texture2D T : register(t0);
SamplerState S : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) { V o; o.p = P[id]; o.uv = float2(id & 1, id >> 1); return o; }
float4 ps(V i) : SV_Target {
    float2 d = i.uv - 0.5; float ax = abs(d.x), ay = abs(d.y), r = length(d) * 2;
    if (opt.y > 0) {                                                          // red dot sight: 1 = clear glass (no sampling), 2 = lens picture
        float px = max(fwidth(i.uv.x), fwidth(i.uv.y));
        float inside = 1 - smoothstep(0.97, 1.0, r);                          // the glass window
        float rim = saturate(1 - abs(r - 0.95) / max(0.04, px * 2)) * 0.45;   // thin dark rim: where the window is
        float dr = max(0.03, px * 2.0), g = length(i.uv - opt.zw);           // the dot at the barrel's point at infinity
        float dot = (1 - smoothstep(dr * 0.6, dr, g)) * inside, glow = 0.4 * (1 - smoothstep(dr, dr * 3, g)) * inside;
        float3 red = float3(1.0, 0.1, 0.06);
        if (opt.y > 1.5) {                                                    // picture: opaque disk, faint blue-green coating, dark rim
            float3 p = T.Sample(S, i.uv).rgb * float3(0.94, 0.98, 1.0) * (1 - rim);
            p = lerp(p, red, saturate(dot + glow));
            return float4(p * inside, inside);
        }
        float tint = 0.06;                                                    // faint coated-glass tint
        float a = saturate(max(rim, tint * inside) + dot + glow);
        float3 col = red * saturate(dot + glow) + float3(0.05, 0.1, 0.12) * tint * inside;
        return float4(col, a);                                                // premultiplied: rim/tint darken, dot adds red
    }
    float3 c = T.Sample(S, i.uv).rgb;
    float a = 1 - smoothstep(0.94, 0.98, r);                                  // round lens, soft edge
    float px = max(fwidth(i.uv.x), fwidth(i.uv.y));                          // one screen pixel in uv: lines never thinner
    float thin = max(1.0 / 256, px * 0.75), thick = max(1.0 / 64, px * 2), ret = saturate((r - 0.88) / 0.1);   // dark ring at the edge
    if ((ay <= thin && ax >= 0.03) || (ax <= thin && ay >= 0.03)) ret = 1;   // thin crosshair with a centre gap
    if ((ay <= thick && ax >= 0.30) || (ax <= thick && ay >= 0.30)) ret = 1; // thick posts outside
    if (length(d) <= max(thin * 1.5, px * 1.2)) ret = 1;                      // centre dot
    c *= 1 - ret * opt.x;
    return float4(c * a, a);                                                  // premultiplied
}
