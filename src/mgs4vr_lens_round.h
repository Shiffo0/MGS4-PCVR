#ifndef MGS4VR_LENS_ROUND_H
#define MGS4VR_LENS_ROUND_H
/* Lens scope round mask (13 Oct 2026, user: "maak het scopevlak rond"). One full-screen triangle samples the lens square
   and writes it premultiplied with a round alpha mask (opaque to 0.94 of the radius, soft to 0.98) into an R8G8B8A8_UNORM
   texture of the same size; the XR lens quad is then submitted with source-alpha blending.
   Runs on the game thread at Present, inside its own ID3DDeviceContextState (SwapDeviceContextState), so none of the
   game's pipeline state is touched. Shaders precompiled from mgs4vr_lens_round.hlsl (fxc, vs_4_0 / ps_4_0). */
#include <d3d11_1.h>
/* Returns the round texture (owned here, valid until the next run/release) or NULL (then use the square source). */
ID3D11Texture2D *mgs4vr_lens_round_run(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src);
/* Titanfall 2 VR style: draw the lens square as a round, perspective-correct disk (with the crosshair, reticle != 0)
   straight into dst (an eye image) at the clip-space corners clip[4][4] (uv 00, 10, 01, 11), premultiplied over the
   picture. Same private context state as above. 1 = drawn. */
int mgs4vr_lens_round_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst, const float clip[16], int reticle);
/* Red dot sight (16 Oct 2026): the disk with the red dot at dot = disk uv, over src (the 1x lens picture) or, src NULL, clear glass. */
int mgs4vr_lens_round_draw_dot(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst, const float clip[16], const float dot[2]);
void mgs4vr_lens_round_release(void);
long mgs4vr_lens_round_stat(long *fails);
#endif