/* mgs4vr_lens_round.c - see mgs4vr_lens_round.h. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mgs4vr_lens_round.h"
#include "mgs4vr_lens_round_vs.h"
#include "mgs4vr_lens_round_ps.h"
#include "mgs4vr_lens_draw_vs.h"
#include "mgs4vr_lens_draw_ps.h"
static ID3D11VertexShader *ld_vs; static ID3D11PixelShader *ld_ps; static ID3D11Buffer *ld_cb; static ID3D11BlendState *ld_blend;
static ID3D11SamplerState *ld_lin; static ID3D11RasterizerState *ld_rs; static ID3D11Texture2D *ld_src, *ld_dst[2]; static ID3D11ShaderResourceView *ld_srv;
static ID3D11RenderTargetView *ld_rtv[2]; static int ld_next; static volatile LONG ld_draws, ld_fails;
static ID3D11Device *lr_dev; static ID3DDeviceContextState *lr_state; static ID3D11VertexShader *lr_vs; static ID3D11PixelShader *lr_ps;
static ID3D11SamplerState *lr_samp; static ID3D11Texture2D *lr_tex, *lr_src; static ID3D11ShaderResourceView *lr_srv; static ID3D11RenderTargetView *lr_rtv;
static unsigned lr_w, lr_h; static int lr_broken; static volatile LONG lr_runs, lr_fails;
#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
static void lr_release_views(void) { REL(lr_srv); REL(lr_rtv); REL(lr_tex); lr_src = NULL; lr_w = lr_h = 0; }
static void ld_release(void) { int i; REL(ld_srv); ld_src = NULL; for (i = 0; i < 2; ++i) { REL(ld_rtv[i]); ld_dst[i] = NULL; } REL(ld_cb); REL(ld_blend); REL(ld_lin); REL(ld_rs); REL(ld_ps); REL(ld_vs); }
void mgs4vr_lens_round_release(void) { ld_release(); lr_release_views(); REL(lr_samp); REL(lr_ps); REL(lr_vs); REL(lr_state); lr_dev = NULL; lr_broken = 0; }
long mgs4vr_lens_round_stat(long *fails) { if (fails) *fails = lr_fails + ld_fails; return lr_runs + ld_draws; }
static DXGI_FORMAT lr_view_format(DXGI_FORMAT f) {   /* typeless -> UNORM view; sRGB read as raw bytes (same as the copy route) */
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
static int lr_init(ID3D11Device *dev) {
    ID3D11Device1 *d1 = NULL; D3D_FEATURE_LEVEL fl; D3D11_SAMPLER_DESC sd; UINT flags = 0;
    if (lr_dev == dev && lr_state) return 1;
    mgs4vr_lens_round_release();
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&d1)) || !d1) return 0;
    fl = ID3D11Device_GetFeatureLevel(dev);
    if (ID3D11Device_GetCreationFlags(dev) & D3D11_CREATE_DEVICE_SINGLETHREADED) flags = D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED;
    if (FAILED(ID3D11Device1_CreateDeviceContextState(d1, flags, &fl, 1, D3D11_SDK_VERSION, &IID_ID3D11Device, NULL, &lr_state))) lr_state = NULL;
    ID3D11Device1_Release(d1);
    if (!lr_state) return 0;
    if (FAILED(ID3D11Device_CreateVertexShader(dev, lens_round_vs, sizeof(lens_round_vs), NULL, &lr_vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(dev, lens_round_ps, sizeof(lens_round_ps), NULL, &lr_ps))) { mgs4vr_lens_round_release(); return 0; }
    memset(&sd, 0, sizeof(sd)); sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &lr_samp))) { mgs4vr_lens_round_release(); return 0; }
    lr_dev = dev; return 1;
}
ID3D11Texture2D *mgs4vr_lens_round_run(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src) {
    D3D11_TEXTURE2D_DESC sdsc, d; D3D11_SHADER_RESOURCE_VIEW_DESC vd; ID3D11DeviceContext1 *c1 = NULL; ID3DDeviceContextState *prev = NULL;
    D3D11_VIEWPORT vp; float clear[4] = { 0, 0, 0, 0 };
    if (!dev || !ctx || !src || lr_broken) return NULL;
    if (!lr_init(dev)) { lr_broken = 1; InterlockedIncrement(&lr_fails); return NULL; }
    ID3D11Texture2D_GetDesc(src, &sdsc);
    if (!(sdsc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sdsc.SampleDesc.Count != 1) { InterlockedIncrement(&lr_fails); return NULL; }
    if (lr_src != src || lr_w != sdsc.Width || lr_h != sdsc.Height) {
        lr_release_views();
        memset(&d, 0, sizeof(d)); d.Width = sdsc.Width; d.Height = sdsc.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        memset(&vd, 0, sizeof(vd)); vd.Format = lr_view_format(sdsc.Format); vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &lr_tex)) || FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)lr_tex, NULL, &lr_rtv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)src, &vd, &lr_srv))) { lr_release_views(); lr_broken = 1; InterlockedIncrement(&lr_fails); return NULL; }
        lr_src = src; lr_w = sdsc.Width; lr_h = sdsc.Height;
    }
    if (FAILED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&c1)) || !c1) { lr_broken = 1; InterlockedIncrement(&lr_fails); return NULL; }
    ID3D11DeviceContext1_SwapDeviceContextState(c1, lr_state, &prev);            /* the game's whole state is parked in prev */
    ID3D11DeviceContext_ClearRenderTargetView(ctx, lr_rtv, clear);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &lr_rtv, NULL);
    vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)lr_w; vp.Height = (float)lr_h; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL); ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, lr_vs, NULL, 0); ID3D11DeviceContext_PSSetShader(ctx, lr_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &lr_srv); ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &lr_samp);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    { ID3D11ShaderResourceView *none = NULL; ID3D11RenderTargetView *nrt = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none); ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &nrt, NULL); }
    ID3D11DeviceContext1_SwapDeviceContextState(c1, prev, NULL);                 /* the game's state back, exactly */
    if (prev) ID3DDeviceContextState_Release(prev);
    ID3D11DeviceContext1_Release(c1);
    InterlockedIncrement(&lr_runs);
    return lr_tex;
}
/* src NULL = no picture (red dot sight: the shader does not sample). opt = the shader's opt (x reticle, y dot sight, zw dot uv). */
static int ld_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst, const float clip[16], const float opt[4]) {
    D3D11_TEXTURE2D_DESC sdsc, ddsc; ID3D11DeviceContext1 *c1 = NULL; ID3DDeviceContextState *prev = NULL; D3D11_VIEWPORT vp; D3D11_MAPPED_SUBRESOURCE m;
    ID3D11RenderTargetView *rtv = NULL; int i; float cb[20]; UINT mask = 0xffffffff; float bf[4] = { 0, 0, 0, 0 };
    if (!dev || !ctx || !dst || !clip || !opt || lr_broken) return 0;
    for (i = 0; i < 16; ++i) if (!(clip[i] == clip[i])) return 0;
    if (!lr_init(dev)) { lr_broken = 1; InterlockedIncrement(&ld_fails); return 0; }
    if (!ld_vs) {
        D3D11_BUFFER_DESC bd; D3D11_BLEND_DESC bl; D3D11_SAMPLER_DESC sd; D3D11_RASTERIZER_DESC rd;
        memset(&bd, 0, sizeof(bd)); bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        memset(&bl, 0, sizeof(bl)); bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        memset(&sd, 0, sizeof(sd)); sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        memset(&rd, 0, sizeof(rd)); rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE;
        if (FAILED(ID3D11Device_CreateVertexShader(dev, lens_draw_vs, sizeof(lens_draw_vs), NULL, &ld_vs)) || FAILED(ID3D11Device_CreatePixelShader(dev, lens_draw_ps, sizeof(lens_draw_ps), NULL, &ld_ps)) ||
            FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &ld_cb)) || FAILED(ID3D11Device_CreateBlendState(dev, &bl, &ld_blend)) ||
            FAILED(ID3D11Device_CreateSamplerState(dev, &sd, &ld_lin)) || FAILED(ID3D11Device_CreateRasterizerState(dev, &rd, &ld_rs))) { ld_release(); lr_broken = 1; InterlockedIncrement(&ld_fails); return 0; }
    }
    memset(&sdsc, 0, sizeof(sdsc)); sdsc.BindFlags = D3D11_BIND_SHADER_RESOURCE; if (src) ID3D11Texture2D_GetDesc(src, &sdsc); ID3D11Texture2D_GetDesc(dst, &ddsc);
    if (!(sdsc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(ddsc.BindFlags & D3D11_BIND_RENDER_TARGET)) { InterlockedIncrement(&ld_fails); return 0; }
    if (src && ld_src != src) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd; REL(ld_srv); ld_src = NULL;
        memset(&vd, 0, sizeof(vd)); vd.Format = lr_view_format(sdsc.Format); vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)src, &vd, &ld_srv))) { InterlockedIncrement(&ld_fails); return 0; }
        ld_src = src;
    }
    for (i = 0; i < 2; ++i) if (ld_dst[i] == dst) rtv = ld_rtv[i];
    if (!rtv) {                                                                 /* two eye images: cache two views */
        D3D11_RENDER_TARGET_VIEW_DESC rv; i = ld_next; ld_next ^= 1; REL(ld_rtv[i]); ld_dst[i] = NULL;
        memset(&rv, 0, sizeof(rv)); rv.Format = lr_view_format(ddsc.Format); rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)dst, &rv, &ld_rtv[i]))) { InterlockedIncrement(&ld_fails); return 0; }
        ld_dst[i] = dst; rtv = ld_rtv[i];
    }
    memcpy(cb, clip, 64); memcpy(cb + 16, opt, 16);
    if (FAILED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&c1)) || !c1) { lr_broken = 1; InterlockedIncrement(&ld_fails); return 0; }
    ID3D11DeviceContext1_SwapDeviceContextState(c1, lr_state, &prev);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ld_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { ID3D11DeviceContext1_SwapDeviceContextState(c1, prev, NULL); if (prev) ID3DDeviceContextState_Release(prev); ID3D11DeviceContext1_Release(c1); InterlockedIncrement(&ld_fails); return 0; }
    memcpy(m.pData, cb, sizeof(cb)); ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ld_cb, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL); ID3D11DeviceContext_OMSetBlendState(ctx, ld_blend, bf, mask);
    vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)ddsc.Width; vp.Height = (float)ddsc.Height; vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); ID3D11DeviceContext_RSSetState(ctx, ld_rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL); ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, ld_vs, NULL, 0); ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &ld_cb);
    ID3D11DeviceContext_PSSetShader(ctx, ld_ps, NULL, 0); ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &ld_cb);
    { ID3D11ShaderResourceView *sv = src ? ld_srv : NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &sv); } ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &ld_lin);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
    { ID3D11ShaderResourceView *none = NULL; ID3D11RenderTargetView *nrt = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none); ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &nrt, NULL); }
    ID3D11DeviceContext1_SwapDeviceContextState(c1, prev, NULL);
    if (prev) ID3DDeviceContextState_Release(prev);
    ID3D11DeviceContext1_Release(c1);
    InterlockedIncrement(&ld_draws);
    return 1;
}
int mgs4vr_lens_round_draw(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst, const float clip[16], int reticle) {
    float opt[4] = { reticle ? 1.0f : 0.0f, 0, 0, 0 };
    return src ? ld_draw(dev, ctx, src, dst, clip, opt) : 0;
}
int mgs4vr_lens_round_draw_dot(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, ID3D11Texture2D *dst, const float clip[16], const float dot[2]) {
    float opt[4] = { 0, 1, 0.5f, 0.5f };   /* opt.y 1 = clear glass, 2 = the lens picture under the dot */
    if (!dot || !(dot[0] == dot[0]) || !(dot[1] == dot[1])) return 0;
    opt[1] = src ? 2.0f : 1.0f; opt[2] = dot[0]; opt[3] = dot[1];
    return ld_draw(dev, ctx, src, dst, clip, opt);
}
