/* Private deferred context: never mutate the game's graphics pipeline. The
   immediate context executes with state restoration, under the capture lock.
   All cached objects are released at XR teardown. No eye texture is written. */
#include <d3dcompiler.h>
#include "mgs4vr_observer_layout.h"
#include "mgs4vr_observer_shader.h"
/* Borrowed eye snapshots, only populated/read while the capture lock is held. */
static struct { ID3D11Texture2D *texture; int valid; uint32_t serial,epoch; uint64_t capture_ms; MGS4_OBSERVER_FOV fov; MGS4_OBSERVER_POSE raw; } g_ob_source[2];
static struct {
    ID3D11DeviceContext *dc;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps,*match;
    ID3D11Texture2D *field;
    ID3D11RenderTargetView *field_rt;
    ID3D11ShaderResourceView *field_srv;
    ID3D11Buffer *cb;
    ID3D11SamplerState *sampler;
    ID3D11RasterizerState *rs;
    ID3D11Texture2D *desktop;
    uint64_t desktop_ms;
    uint32_t serial,epoch;
    int have_frame,mode,good_pairs;
    float left_weight;
    int failed;
} g_ob;
#define OB_RELEASE(p) do { if(p){(p)->lpVtbl->Release(p);(p)=NULL;} } while(0)
static void observer_reset(void) {
    OB_RELEASE(g_ob.field_srv);OB_RELEASE(g_ob.field_rt);OB_RELEASE(g_ob.field);
    OB_RELEASE(g_ob.desktop);g_ob.have_frame=0;g_ob.good_pairs=0;g_ob.left_weight=0;
}
static void observer_free(void) {
    observer_reset();
    OB_RELEASE(g_ob.dc);OB_RELEASE(g_ob.vs);OB_RELEASE(g_ob.ps);OB_RELEASE(g_ob.match);
    OB_RELEASE(g_ob.cb);OB_RELEASE(g_ob.sampler);OB_RELEASE(g_ob.rs);
    g_ob.failed=0;
}
static int observer_init(void) {
    typedef HRESULT (WINAPI *COMPILE)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,
        ID3DInclude*,LPCSTR,LPCSTR,UINT,UINT,ID3DBlob**,ID3DBlob**);
    HMODULE dll; COMPILE compile; ID3DBlob *v=NULL,*p=NULL,*m=NULL,*err=NULL;
    D3D11_BUFFER_DESC bd; D3D11_SAMPLER_DESC sd; D3D11_RASTERIZER_DESC rd; int ok=0;
    if(g_ob.dc)return 1;
    if(g_ob.failed)return 0;
    dll=LoadLibraryExW(L"d3dcompiler_47.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dll)goto done;
    compile=(COMPILE)GetProcAddress(dll,"D3DCompile");if(!compile)goto done;
    if(FAILED(compile(observer_shader,sizeof observer_shader-1,"observer",NULL,NULL,"vs","vs_4_0",0,0,&v,&err)))goto done;
    OB_RELEASE(err);
    if(FAILED(compile(observer_shader,sizeof observer_shader-1,"observer",NULL,NULL,"ps","ps_4_0",0,0,&p,&err)))goto done;
    if(FAILED(g_dev->lpVtbl->CreateVertexShader(g_dev,v->lpVtbl->GetBufferPointer(v),v->lpVtbl->GetBufferSize(v),NULL,&g_ob.vs)))goto done;
    if(FAILED(g_dev->lpVtbl->CreatePixelShader(g_dev,p->lpVtbl->GetBufferPointer(p),p->lpVtbl->GetBufferSize(p),NULL,&g_ob.ps)))goto done;
    OB_RELEASE(err);
    if(FAILED(compile(observer_shader,sizeof observer_shader-1,"observer",NULL,NULL,"match","ps_4_0",0,0,&m,&err)))goto done;
    if(FAILED(g_dev->lpVtbl->CreatePixelShader(g_dev,m->lpVtbl->GetBufferPointer(m),m->lpVtbl->GetBufferSize(m),NULL,&g_ob.match)))goto done;
    memset(&bd,0,sizeof bd);bd.ByteWidth=sizeof(MGS4_OBSERVER_LAYOUT);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(g_dev->lpVtbl->CreateBuffer(g_dev,&bd,NULL,&g_ob.cb)))goto done;
    memset(&sd,0,sizeof sd);sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(g_dev->lpVtbl->CreateSamplerState(g_dev,&sd,&g_ob.sampler)))goto done;
    memset(&rd,0,sizeof rd);rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    if(FAILED(g_dev->lpVtbl->CreateRasterizerState(g_dev,&rd,&g_ob.rs)))goto done;
    if(FAILED(g_dev->lpVtbl->CreateDeferredContext(g_dev,0,&g_ob.dc)))goto done;
    ok=1;
done:
    if(!ok && g_log)g_log("  desktop observer: shader initialization failed%s%s\r\n",err?": ":"",err?(char*)err->lpVtbl->GetBufferPointer(err):"");
    OB_RELEASE(v);OB_RELEASE(p);OB_RELEASE(m);OB_RELEASE(err);if(dll)FreeLibrary(dll);
    if(!ok){observer_free();g_ob.failed=1;}
    return ok;
}
/* Returns 1 for a rendered desktop; 0 leaves the original desktop untouched.
   mode 1 right, 2 both (right dominant). All modes crop to 16:9. */
static int observer_draw(void *swapchain,int mode) {
    IDXGISwapChain *sc=(IDXGISwapChain*)swapchain;
    ID3D11Texture2D *back=NULL;ID3D11RenderTargetView *rt=NULL;
    ID3D11ShaderResourceView *srv[2]={NULL,NULL};ID3D11CommandList *list=NULL;
    D3D11_TEXTURE2D_DESC td,sd;D3D11_VIEWPORT vp;MGS4_OBSERVER_LAYOUT layout;
    ID3D11DeviceContext *c;uint64_t now,delta;int valid[2]={0,0},use[2],i,ok=0,requested=mode,pair=0;
    if(!sc||!(g_cs_state == 2)||mode<0||mode>2)return 0;
    if(FAILED(sc->lpVtbl->GetBuffer(sc,0,&IID_ID3D11Texture2D,(void**)&back))||!back)return 0;
    back->lpVtbl->GetDesc(back,&td);
    EnterCriticalSection(&g_cs);
    if(!g_ctx||!g_dev||td.SampleDesc.Count!=1||!td.Width||!td.Height)goto done;
    now=GetTickCount64();
    for(i=0;i<2;i++)if(g_ob_source[i].texture&&g_ob_source[i].valid&&now-g_ob_source[i].capture_ms<=150) {
        g_ob_source[i].texture->lpVtbl->GetDesc(g_ob_source[i].texture,&sd);
        valid[i]=sd.Width==td.Width&&sd.Height==td.Height&&sd.Format==td.Format&&sd.SampleDesc.Count==1;
    }
    if(g_ob.desktop) {
        g_ob.desktop->lpVtbl->GetDesc(g_ob.desktop,&sd);
        if(sd.Width!=td.Width||sd.Height!=td.Height||sd.Format!=td.Format||g_ob.mode!=requested ||
           (valid[1]&&g_ob.epoch!=g_ob_source[1].epoch))observer_reset();
    }
    /* Publish complete frames at right-eye cadence. Never show a left-only
       frame or an intermediate composite between two right captures. */
    if(g_ob.have_frame&&now-g_ob.desktop_ms<=150 &&
       (!valid[1]||g_ob.serial==g_ob_source[1].serial)) {
        g_ctx->lpVtbl->CopyResource(g_ctx,(ID3D11Resource*)back,(ID3D11Resource*)g_ob.desktop);ok=1;goto done;
    }
    if(!valid[1])goto done;
    if(requested==2) {
        delta=g_ob_source[0].capture_ms>g_ob_source[1].capture_ms?g_ob_source[0].capture_ms-g_ob_source[1].capture_ms:g_ob_source[1].capture_ms-g_ob_source[0].capture_ms;
        pair=valid[0]&&delta<=25&&g_ob_source[0].epoch==g_ob_source[1].epoch &&
             (uint32_t)(g_ob_source[1].serial-g_ob_source[0].serial)==1 &&
             observer_pose_pair(&g_ob_source[0].raw,&g_ob_source[1].raw);
    }
    mode=pair?2:1;
    if(!observer_layout(&g_ob_source[0].fov,&g_ob_source[1].fov,mode,&layout)) {
        mode=1;pair=0;
        if(!observer_layout(&g_ob_source[0].fov,&g_ob_source[1].fov,mode,&layout))goto done;
    }
    /* Avoid rapid re-entry flashes after a rejected pair. The central/right
       image never blends eyes. Re-enable the left strip only after 4 good
       pairs, then fade it in over 6 right-eye frames. */
    if(pair){if(g_ob.good_pairs<4)g_ob.good_pairs++;if(g_ob.good_pairs>=4)g_ob.left_weight=fminf(1,g_ob.left_weight+1.0f/6.0f);}
    else {g_ob.good_pairs=0;g_ob.left_weight=0;}
    layout.options[2]=g_ob.left_weight;
    if(pair)observer_reprojection(&layout,&g_ob_source[0].raw,&g_ob_source[1].raw);
    layout.dimensions[0]=(float)td.Width;layout.dimensions[1]=(float)td.Height;
    layout.dimensions[2]=(float)(td.Width<256?td.Width/4:64);
    layout.dimensions[3]=(float)(td.Height<144?td.Height:144);
    if(!observer_init())goto done;
    if(!g_ob.desktop) {
        sd=td;sd.Usage=D3D11_USAGE_DEFAULT;sd.BindFlags=0;sd.CPUAccessFlags=0;sd.MiscFlags=0;
        if(FAILED(g_dev->lpVtbl->CreateTexture2D(g_dev,&sd,NULL,&g_ob.desktop)))goto done;
    }
    if(!g_ob.field||!g_ob.field_rt||!g_ob.field_srv) {
        OB_RELEASE(g_ob.field_srv);OB_RELEASE(g_ob.field_rt);OB_RELEASE(g_ob.field);
        memset(&sd,0,sizeof sd);sd.Width=(UINT)layout.dimensions[2];sd.Height=(UINT)layout.dimensions[3];
        sd.MipLevels=sd.ArraySize=sd.SampleDesc.Count=1;sd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.Usage=D3D11_USAGE_DEFAULT;sd.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(g_dev->lpVtbl->CreateTexture2D(g_dev,&sd,NULL,&g_ob.field)) ||
           FAILED(g_dev->lpVtbl->CreateRenderTargetView(g_dev,(ID3D11Resource*)g_ob.field,NULL,&g_ob.field_rt)) ||
           FAILED(g_dev->lpVtbl->CreateShaderResourceView(g_dev,(ID3D11Resource*)g_ob.field,NULL,&g_ob.field_srv)))goto done;
    }
    use[0]=mode==2?0:mode;use[1]=mode==2?1:mode;
    for(i=0;i<2;i++)if(FAILED(g_dev->lpVtbl->CreateShaderResourceView(g_dev,(ID3D11Resource*)g_ob_source[use[i]].texture,NULL,&srv[i])))goto done;
    if(FAILED(g_dev->lpVtbl->CreateRenderTargetView(g_dev,(ID3D11Resource*)back,NULL,&rt)))goto done;
    memset(&vp,0,sizeof vp);vp.Width=(float)td.Width;vp.Height=vp.Width*9.0f/16.0f;
    if(vp.Height>(float)td.Height){vp.Height=(float)td.Height;vp.Width=vp.Height*16.0f/9.0f;}
    vp.TopLeftX=((float)td.Width-vp.Width)*.5f;vp.TopLeftY=((float)td.Height-vp.Height)*.5f;vp.MaxDepth=1;
    c=g_ob.dc;c->lpVtbl->ClearState(c);
    c->lpVtbl->UpdateSubresource(c,(ID3D11Resource*)g_ob.cb,0,NULL,&layout,0,0);
    c->lpVtbl->OMSetRenderTargets(c,1,&rt,NULL);
    { const float black[4]={0,0,0,1};c->lpVtbl->ClearRenderTargetView(c,rt,black); }
    c->lpVtbl->RSSetViewports(c,1,&vp);c->lpVtbl->RSSetState(c,g_ob.rs);
    c->lpVtbl->IASetPrimitiveTopology(c,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->lpVtbl->VSSetShader(c,g_ob.vs,NULL,0);c->lpVtbl->PSSetShader(c,g_ob.ps,NULL,0);
    c->lpVtbl->PSSetConstantBuffers(c,0,1,&g_ob.cb);c->lpVtbl->PSSetShaderResources(c,0,2,srv);
    c->lpVtbl->PSSetSamplers(c,0,1,&g_ob.sampler);
    if(mode==2&&layout.options[2]>0) {
        D3D11_VIEWPORT mv={0};mv.Width=layout.dimensions[2];mv.Height=layout.dimensions[3];mv.MaxDepth=1;
        c->lpVtbl->OMSetRenderTargets(c,1,&g_ob.field_rt,NULL);c->lpVtbl->RSSetViewports(c,1,&mv);
        c->lpVtbl->PSSetShader(c,g_ob.match,NULL,0);c->lpVtbl->Draw(c,3,0);
        c->lpVtbl->OMSetRenderTargets(c,1,&rt,NULL);c->lpVtbl->RSSetViewports(c,1,&vp);
        c->lpVtbl->PSSetShader(c,g_ob.ps,NULL,0);c->lpVtbl->PSSetShaderResources(c,2,1,&g_ob.field_srv);
    }
    c->lpVtbl->Draw(c,3,0);
    if(SUCCEEDED(c->lpVtbl->FinishCommandList(c,FALSE,&list))&&list) {
        g_ctx->lpVtbl->ExecuteCommandList(g_ctx,list,TRUE);
        g_ctx->lpVtbl->CopyResource(g_ctx,(ID3D11Resource*)g_ob.desktop,(ID3D11Resource*)back);
        g_ob.have_frame=1;g_ob.desktop_ms=g_ob_source[1].capture_ms;g_ob.serial=g_ob_source[1].serial;
        g_ob.epoch=g_ob_source[1].epoch;g_ob.mode=requested;ok=1;
    }else { observer_free();g_ob.failed=1; }
done:
    OB_RELEASE(list);OB_RELEASE(rt);OB_RELEASE(srv[0]);OB_RELEASE(srv[1]);
    LeaveCriticalSection(&g_cs);OB_RELEASE(back);return ok;
}
#undef OB_RELEASE
