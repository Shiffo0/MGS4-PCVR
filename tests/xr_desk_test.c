/* xr_desk_test.c - W05 desk acceptance for mgs4vr_xr: the whole OpenXR path
 * without a headset. This file contains a small in-process FAKE RUNTIME (the
 * two dozen xr* entry points the module uses) working on a REAL D3D11 device:
 * its swapchain images are real textures and xrEndFrame reads the submitted
 * picture back. The test exe plays the game: it renders a colour into a real
 * DXGI swapchain and calls mgs4vr_xr_on_present() before Present, like the hook.
 *
 * Covered: screen-pose math; no headset yet -> polling, game untouched; session
 * on the game's device; sRGB format choice; picture content reaches the layer;
 * quad pose and size; headset off (STOPPING) and on again (READY) without a
 * restart and with a fresh anchor; recentre; layer switched off; back buffer
 * resize; runtime loss -> full re-init; SINGLETHREADED device refused; clean
 * stop with balanced create/destroy and no leaked device references.
 * Exit 0 = pass. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "../src/mgs4vr_xr.h"

/* ------------------------------------------------------- fake runtime --- */

#define FK_IMAGES 3
static struct {
    volatile LONG hmd;                               /* 0: xrGetSystem says FORM_FACTOR_UNAVAILABLE */
    LUID luid;
    ID3D11Device *dev; ID3D11DeviceContext *ctx;
    ID3D11Texture2D *img[FK_IMAGES]; ID3D11Texture2D *staging;
    unsigned w, h; long long format;
    uint32_t next, acquired; int have_acquired, last_released;
    XrSessionState queue[16]; volatile LONG q_head, q_tail;
    volatile LONG visible;
    float head_q[4], head_p[3];
    volatile LONG inst_created, inst_destroyed, sess_created, sess_destroyed, sc_created, sc_destroyed, sp_created, sp_destroyed;
    volatile LONG begin_session, end_session, frames, layer_frames, requirements_asked;
    XrCompositionLayerQuad last_quad; unsigned char last_pixel[4]; volatile LONG pixel_reads;
    volatile LONG stereo_frames;
    XrCompositionLayerProjectionView stereo_views[2];unsigned char eye_pixels[2][4];
    XrTime time;
} fk;

static void fk_push(XrSessionState s) { LONG i = InterlockedIncrement(&fk.q_head) - 1; fk.queue[i & 15] = s; }

static XrResult XRAPI_CALL fk_EnumExt(const char *layer, uint32_t cap, uint32_t *n, XrExtensionProperties *p) {
    (void)layer; *n = 1;
    if (cap >= 1) { strcpy_s(p[0].extensionName, sizeof(p[0].extensionName), XR_KHR_D3D11_ENABLE_EXTENSION_NAME); p[0].extensionVersion = 9; }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL fk_CreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out) {
    if (ci->enabledExtensionCount != 1 || strcmp(ci->enabledExtensionNames[0], XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) return XR_ERROR_EXTENSION_NOT_PRESENT;
    InterlockedIncrement(&fk.inst_created); *out = (XrInstance)(size_t)0x1001; fk.q_head = fk.q_tail = 0; return XR_SUCCESS;
}
static XrResult XRAPI_CALL fk_DestroyInstance(XrInstance i) { (void)i; InterlockedIncrement(&fk.inst_destroyed); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_GetInstanceProperties(XrInstance i, XrInstanceProperties *p) { (void)i; strcpy_s(p->runtimeName, sizeof(p->runtimeName), "mgs4vr fake runtime"); p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_GetSystem(XrInstance i, const XrSystemGetInfo *gi, XrSystemId *out) { (void)i; (void)gi; if (!fk.hmd) return XR_ERROR_FORM_FACTOR_UNAVAILABLE; *out = 77; return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_GetSystemProperties(XrInstance i, XrSystemId s, XrSystemProperties *p) { (void)i; (void)s; strcpy_s(p->systemName, sizeof(p->systemName), "Fake HMD"); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_GetReq(XrInstance i, XrSystemId s, XrGraphicsRequirementsD3D11KHR *r) { (void)i; (void)s; r->adapterLuid = fk.luid; r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0; InterlockedIncrement(&fk.requirements_asked); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_CreateSession(XrInstance i, const XrSessionCreateInfo *ci, XrSession *out) {
    const XrGraphicsBindingD3D11KHR *b = (const XrGraphicsBindingD3D11KHR *)ci->next;
    (void)i;
    if (!fk.requirements_asked) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    if (!b || b->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || !b->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    fk.dev = b->device; ID3D11Device_AddRef(fk.dev); ID3D11Device_GetImmediateContext(fk.dev, &fk.ctx);
    InterlockedIncrement(&fk.sess_created); *out = (XrSession)(size_t)0x2001;
    fk_push(XR_SESSION_STATE_IDLE); fk_push(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}
static void fk_free_images(void) { int k; for (k = 0; k < FK_IMAGES; ++k) if (fk.img[k]) { ID3D11Texture2D_Release(fk.img[k]); fk.img[k] = NULL; } if (fk.staging) { ID3D11Texture2D_Release(fk.staging); fk.staging = NULL; } }
static XrResult XRAPI_CALL fk_DestroySession(XrSession s) { (void)s; fk_free_images(); if (fk.ctx) { ID3D11DeviceContext_Release(fk.ctx); fk.ctx = NULL; } if (fk.dev) { ID3D11Device_Release(fk.dev); fk.dev = NULL; } InterlockedIncrement(&fk.sess_destroyed); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_CreateSpace(XrSession s, const XrReferenceSpaceCreateInfo *ci, XrSpace *out) { (void)s; *out = (XrSpace)(size_t)(0x3000 + ci->referenceSpaceType); InterlockedIncrement(&fk.sp_created); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_DestroySpace(XrSpace s) { (void)s; InterlockedIncrement(&fk.sp_destroyed); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_LocateSpace(XrSpace a, XrSpace b, XrTime t, XrSpaceLocation *loc) {
    (void)t;
    if (a != (XrSpace)(size_t)(0x3000 + XR_REFERENCE_SPACE_TYPE_VIEW) || b != (XrSpace)(size_t)(0x3000 + XR_REFERENCE_SPACE_TYPE_LOCAL)) return XR_ERROR_HANDLE_INVALID;
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    loc->pose.orientation.x = fk.head_q[0]; loc->pose.orientation.y = fk.head_q[1]; loc->pose.orientation.z = fk.head_q[2]; loc->pose.orientation.w = fk.head_q[3];
    loc->pose.position.x = fk.head_p[0]; loc->pose.position.y = fk.head_p[1]; loc->pose.position.z = fk.head_p[2];
    return XR_SUCCESS;
}
static volatile LONG view_mode, view_valid, view_invalid, view_bad;
static XrResult XRAPI_CALL fk_LocateViews(XrSession session, const XrViewLocateInfo *info, XrViewState *state,
                                        uint32_t cap, uint32_t *count, XrView *views) {
    int e; LONG mode = view_mode;
    (void)session;
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO || info->displayTime != fk.time ||
        info->space != (XrSpace)(size_t)(0x3000 + XR_REFERENCE_SPACE_TYPE_LOCAL) || cap != 2) return XR_ERROR_VALIDATION_FAILURE;
    if (mode == 1) return XR_ERROR_RUNTIME_FAILURE;
    *count = mode == 2 ? 1 : 2;
    state->viewStateFlags = mode == 3 ? XR_VIEW_STATE_ORIENTATION_VALID_BIT :
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    for (e=0;e<2;++e) {
        views[e].pose.orientation.w=1;
        views[e].pose.position.x=e ? .032f : -.032f;
        views[e].fov.angleLeft= e ? -.7f : -.9f;
        views[e].fov.angleRight= e ? .9f : .7f;
        views[e].fov.angleUp=.8f; views[e].fov.angleDown=-.75f;
    }
    if (mode == 4) views[1].fov.angleRight = (float)NAN;
    return XR_SUCCESS;
}
static void observe_views(const MGS4VR_XR_VIEWS *v) {
    static unsigned long last;
    if (v->sequence <= last) InterlockedIncrement(&view_bad);
    last=v->sequence;
    if (!v->valid) { InterlockedIncrement(&view_invalid); return; }
    if (v->display_time != fk.time || fabsf(v->pos[0][0]+.032f)>1e-6f || fabsf(v->pos[1][0]-.032f)>1e-6f ||
        fabsf(v->fov[0][0]+.9f)>1e-6f || fabsf(v->fov[1][0]+.7f)>1e-6f) InterlockedIncrement(&view_bad);
    InterlockedIncrement(&view_valid);
}
static XrResult XRAPI_CALL fk_PollEvent(XrInstance i, XrEventDataBuffer *ev) {
    XrEventDataSessionStateChanged *s = (XrEventDataSessionStateChanged *)ev;
    (void)i;
    if (fk.q_tail >= fk.q_head) return XR_EVENT_UNAVAILABLE;
    s->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED; s->session = (XrSession)(size_t)0x2001; s->state = fk.queue[fk.q_tail & 15]; fk.q_tail++;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL fk_BeginSession(XrSession s, const XrSessionBeginInfo *bi) { (void)s; if (bi->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED; InterlockedIncrement(&fk.begin_session); fk_push(XR_SESSION_STATE_SYNCHRONIZED); fk_push(XR_SESSION_STATE_VISIBLE); fk_push(XR_SESSION_STATE_FOCUSED); InterlockedExchange(&fk.visible, 1); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_EndSession(XrSession s) { (void)s; InterlockedIncrement(&fk.end_session); InterlockedExchange(&fk.visible, 0); fk_push(XR_SESSION_STATE_IDLE); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_WaitFrame(XrSession s, const XrFrameWaitInfo *wi, XrFrameState *fs) { (void)s; (void)wi; Sleep(11); fs->predictedDisplayTime = ++fk.time; fs->predictedDisplayPeriod = 11111111; fs->shouldRender = fk.visible ? XR_TRUE : XR_FALSE; return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_BeginFrame(XrSession s, const XrFrameBeginInfo *bi) { (void)s; (void)bi; return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_EndFrame(XrSession s, const XrFrameEndInfo *ei) {
    (void)s;
    InterlockedIncrement(&fk.frames);
    if (ei->displayTime != fk.time) return XR_ERROR_TIME_INVALID;
    if (ei->layerCount == 1 && ei->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)ei->layers[0];
        D3D11_MAPPED_SUBRESOURCE map;
        if (fk.have_acquired) return XR_ERROR_LAYER_INVALID;               /* image must be released before EndFrame */
        fk.last_quad = *q;
        ID3D11DeviceContext_CopyResource(fk.ctx, (ID3D11Resource *)fk.staging, (ID3D11Resource *)fk.img[fk.last_released]);
        if (SUCCEEDED(ID3D11DeviceContext_Map(fk.ctx, (ID3D11Resource *)fk.staging, 0, D3D11_MAP_READ, 0, &map))) {
            memcpy(fk.last_pixel, (unsigned char *)map.pData + (fk.h / 2) * map.RowPitch + (q->subImage.imageRect.offset.x+q->subImage.imageRect.extent.width/2) * 4, 4);
            ID3D11DeviceContext_Unmap(fk.ctx, (ID3D11Resource *)fk.staging, 0);
            InterlockedIncrement(&fk.pixel_reads);
        }
        InterlockedIncrement(&fk.layer_frames);
    } else if(ei->layerCount==1 && ei->layers[0]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION){
        const XrCompositionLayerProjection *p=(const XrCompositionLayerProjection *)ei->layers[0];
        D3D11_MAPPED_SUBRESOURCE map;int e;
        if(fk.have_acquired || p->viewCount!=2)return XR_ERROR_LAYER_INVALID;
        for(e=0;e<2;++e){
            if(p->views[e].subImage.imageRect.offset.x!=(int)(e*fk.w/2) || p->views[e].subImage.imageRect.extent.width!=(int)fk.w/2)return XR_ERROR_LAYER_INVALID;
            fk.stereo_views[e]=p->views[e];
        }
        ID3D11DeviceContext_CopyResource(fk.ctx,(ID3D11Resource *)fk.staging,(ID3D11Resource *)fk.img[fk.last_released]);
        if(FAILED(ID3D11DeviceContext_Map(fk.ctx,(ID3D11Resource *)fk.staging,0,D3D11_MAP_READ,0,&map)))return XR_ERROR_RUNTIME_FAILURE;
        for(e=0;e<2;++e)memcpy(fk.eye_pixels[e],(BYTE *)map.pData+(fk.h/2)*map.RowPitch+(fk.w/4+e*fk.w/2)*4,4);
        ID3D11DeviceContext_Unmap(fk.ctx,(ID3D11Resource *)fk.staging,0);InterlockedIncrement(&fk.stereo_frames);
    } else if (ei->layerCount != 0) return XR_ERROR_LAYER_INVALID;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL fk_EnumFormats(XrSession s, uint32_t cap, uint32_t *n, int64_t *f) { (void)s; *n = 3; if (cap >= 3) { f[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; f[1] = DXGI_FORMAT_R8G8B8A8_UNORM; f[2] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; } return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_CreateSwapchain(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    D3D11_TEXTURE2D_DESC d; int k;
    (void)s;
    fk_free_images();
    memset(&d, 0, sizeof(d)); d.Width = ci->width; d.Height = ci->height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = (DXGI_FORMAT)ci->format; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (k = 0; k < FK_IMAGES; ++k) if (FAILED(ID3D11Device_CreateTexture2D(fk.dev, &d, NULL, &fk.img[k]))) return XR_ERROR_RUNTIME_FAILURE;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(fk.dev, &d, NULL, &fk.staging))) return XR_ERROR_RUNTIME_FAILURE;
    fk.w = ci->width; fk.h = ci->height; fk.format = ci->format; fk.next = 0; fk.have_acquired = 0;
    InterlockedIncrement(&fk.sc_created); *out = (XrSwapchain)(size_t)0x4001; return XR_SUCCESS;
}
static XrResult XRAPI_CALL fk_DestroySwapchain(XrSwapchain s) { (void)s; InterlockedIncrement(&fk.sc_destroyed); return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_EnumImages(XrSwapchain s, uint32_t cap, uint32_t *n, XrSwapchainImageBaseHeader *im) {
    XrSwapchainImageD3D11KHR *d = (XrSwapchainImageD3D11KHR *)im; uint32_t k;
    (void)s; *n = FK_IMAGES;
    for (k = 0; k < cap && k < FK_IMAGES; ++k) { if (d[k].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE; d[k].texture = fk.img[k]; }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL fk_Acquire(XrSwapchain s, const XrSwapchainImageAcquireInfo *ai, uint32_t *idx) { (void)s; (void)ai; if (fk.have_acquired) return XR_ERROR_CALL_ORDER_INVALID; fk.acquired = fk.next; fk.next = (fk.next + 1) % FK_IMAGES; fk.have_acquired = 1; *idx = fk.acquired; return XR_SUCCESS; }
static XrResult XRAPI_CALL fk_WaitImage(XrSwapchain s, const XrSwapchainImageWaitInfo *wi) { (void)s; (void)wi; return fk.have_acquired ? XR_SUCCESS : XR_ERROR_CALL_ORDER_INVALID; }
static XrResult XRAPI_CALL fk_Release(XrSwapchain s, const XrSwapchainImageReleaseInfo *ri) { (void)s; (void)ri; if (!fk.have_acquired) return XR_ERROR_CALL_ORDER_INVALID; fk.last_released = (int)fk.acquired; fk.have_acquired = 0; return XR_SUCCESS; }

static XrResult XRAPI_CALL fk_gipa(XrInstance inst, const char *name, PFN_xrVoidFunction *out) {
    static const struct { const char *n; void *f; } t[] = {
        { "xrEnumerateInstanceExtensionProperties", fk_EnumExt }, { "xrCreateInstance", fk_CreateInstance }, { "xrDestroyInstance", fk_DestroyInstance },
        { "xrGetInstanceProperties", fk_GetInstanceProperties }, { "xrGetSystem", fk_GetSystem }, { "xrGetSystemProperties", fk_GetSystemProperties },
        { "xrGetD3D11GraphicsRequirementsKHR", fk_GetReq }, { "xrCreateSession", fk_CreateSession }, { "xrDestroySession", fk_DestroySession },
        { "xrLocateViews", fk_LocateViews }, { "xrCreateReferenceSpace", fk_CreateSpace }, { "xrDestroySpace", fk_DestroySpace }, { "xrLocateSpace", fk_LocateSpace }, { "xrPollEvent", fk_PollEvent },
        { "xrBeginSession", fk_BeginSession }, { "xrEndSession", fk_EndSession }, { "xrWaitFrame", fk_WaitFrame }, { "xrBeginFrame", fk_BeginFrame }, { "xrEndFrame", fk_EndFrame },
        { "xrEnumerateSwapchainFormats", fk_EnumFormats }, { "xrCreateSwapchain", fk_CreateSwapchain }, { "xrDestroySwapchain", fk_DestroySwapchain },
        { "xrEnumerateSwapchainImages", fk_EnumImages }, { "xrAcquireSwapchainImage", fk_Acquire }, { "xrWaitSwapchainImage", fk_WaitImage }, { "xrReleaseSwapchainImage", fk_Release } };
    size_t k;
    (void)inst;
    for (k = 0; k < sizeof(t) / sizeof(t[0]); ++k) if (strcmp(t[k].n, name) == 0) { *out = (PFN_xrVoidFunction)t[k].f; return XR_SUCCESS; }
    *out = NULL; return XR_ERROR_FUNCTION_UNSUPPORTED;
}

/* --------------------------------------------------------------- game --- */

static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx; static IDXGISwapChain *g_sc;
static float g_color[4];
static int stereo_test,missing_packet;static unsigned eye_serial=100;

static void test_log(const char *fmt, ...) { char t[900]; va_list a; va_start(a, fmt); _vsnprintf_s(t, sizeof(t), _TRUNCATE, fmt, a); va_end(a); printf("  log| %s\n", t); }
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int game_present(unsigned flags) {
    ID3D11Texture2D *back = NULL; ID3D11RenderTargetView *rtv = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void **)&back))) return 0;
    if (FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &rtv))) { ID3D11Texture2D_Release(back); return 0; }
    if(stereo_test){g_color[0]=(eye_serial&1)?0:1;g_color[1]=(eye_serial&1)?1:0;g_color[2]=0;}
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, rtv, g_color);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(back);
    if(stereo_test){
        MGS4VR_EYE_PACKET p={0};p.valid=!missing_packet;p.epoch=7;p.camera_serial=eye_serial++;p.eye=p.camera_serial&1;
        p.pose_sequence=50;p.display_time=fk.time;p.quat[3]=1;p.pos[0]=p.eye?.123f:-.234f;
        p.fov[0]=-.9f;p.fov[1]=.7f;p.fov[2]=.8f;p.fov[3]=-.75f;
        mgs4vr_xr_on_present_eye(g_sc,flags,&p);
    }else mgs4vr_xr_on_present(g_sc, flags);
    return SUCCEEDED(IDXGISwapChain_Present(g_sc, 0, 0));
}

/* present frames until cond() holds or the timeout expires */
static int pump_until(int (*cond)(void), DWORD timeout_ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeout_ms) { if (!game_present(0)) return 0; if (cond()) return 1; Sleep(8); }
    return cond();
}

static MGS4VR_XR_STATS st;
static long want_layers, want_begins, want_ends, want_reinit;
static unsigned char want_px[4];
static int c_layers(void) { mgs4vr_xr_get_stats(&st); return st.frames_with_layer >= want_layers; }
static int c_begins(void) { mgs4vr_xr_get_stats(&st); return st.session_begins >= want_begins && st.frames_with_layer >= want_layers; }
static int c_ends(void) { mgs4vr_xr_get_stats(&st); return st.session_ends >= want_ends; }
static int c_pixel(void) { return fk.pixel_reads > 0 && memcmp(fk.last_pixel, want_px, 4) == 0; }
static int c_size(void) { return fk.w == 800 && fk.h == 450 && fk.pixel_reads > 0 && memcmp(fk.last_pixel, want_px, 4) == 0; }
static int c_reinit(void) { mgs4vr_xr_get_stats(&st); return st.reinit >= want_reinit && st.frames_with_layer >= want_layers; }
static int c_nolayer(void) { mgs4vr_xr_get_stats(&st); return st.frames_without_layer >= want_layers; }

static void yaw_pitch_quat(double yaw, double pitch, float q[4]) {       /* OpenXR: yaw about +y, pitch about +x, q = qy * qx */
    double cy = cos(yaw / 2), sy = sin(yaw / 2), cp = cos(pitch / 2), sp = sin(pitch / 2);
    q[0] = (float)(cy * sp); q[1] = (float)(sy * cp); q[2] = (float)(-sy * sp); q[3] = (float)(cy * cp);
}
static double pose_err(const float q[4], const float p[3], double yaw, double px, double py, double pz) {
    double e = 0, d;
    /* screen normal (its +z) must point back along the heading; q and -q are the same rotation */
    double nz_x = 2.0 * (q[0] * q[2] + q[3] * q[1]), nz_z = 1.0 - 2.0 * (q[0] * q[0] + q[1] * q[1]);
    d = fabs(nz_x - sin(yaw)); if (d > e) e = d;
    d = fabs(nz_z - cos(yaw)); if (d > e) e = d;
    d = fabs(q[0]) + fabs(q[2]); if (d > e) e = d;                         /* yaw only: no tilt */
    d = fabs(p[0] - px); if (d > e) e = d; d = fabs(p[1] - py); if (d > e) e = d; d = fabs(p[2] - pz); if (d > e) e = d;
    return e;
}

#define CHECK(cond, name) do { if (!(cond)) { printf("FAIL: %s\n", name); return 1; } printf("ok: %s\n", name); } while (0)

int main(void) {
    WNDCLASSA wc = { 0 }; HWND hwnd; DXGI_SWAP_CHAIN_DESC sd = { 0 };
    IDXGIDevice *dxgi = NULL; IDXGIAdapter *adapter = NULL; DXGI_ADAPTER_DESC ad;
    MGS4VR_XR_CONFIG cfg = { 1, 2.5f, 3.2f, 0.0f, 0 };
    float q[4], p[3], hq[4], hp[3];
    ULONG ref_before, ref_after;
    int i;

    /* ---- pure math ---- */
    yaw_pitch_quat(0.0, 0.0, hq); hp[0] = 0; hp[1] = 1.6f; hp[2] = 0;
    mgs4vr_xr_screen_pose(hq, hp, 2.0f, 0.0f, q, p);
    CHECK(pose_err(q, p, 0.0, 0.0, 1.6, -2.0) < 1e-5, "looking straight ahead: screen 2 m down -z, facing the user");
    yaw_pitch_quat(0.5235987756, 0.35, hq); hp[0] = 0.1f; hp[1] = 1.55f; hp[2] = -0.2f;      /* 30 deg left, looking up 20 deg */
    mgs4vr_xr_screen_pose(hq, hp, 2.5f, -0.1f, q, p);
    CHECK(pose_err(q, p, 0.5235987756, 0.1 - sin(0.5235987756) * 2.5, 1.45, -0.2 - cos(0.5235987756) * 2.5) < 1e-5, "yaw 30 + pitch 20: only the heading is used, height offset applied");
    yaw_pitch_quat(-2.0, 1.55, hq);
    mgs4vr_xr_screen_pose(hq, hp, 1.0f, 0.0f, q, p);
    CHECK(pose_err(q, p, -2.0, 0.1 - sin(-2.0), 1.55, -0.2 - cos(-2.0)) < 1e-3, "looking almost straight up: heading still correct (right-vector fallback)");
    { float z[4] = { 0, 0, 0, 0 }; mgs4vr_xr_screen_pose(z, hp, 1.0f, 0.0f, q, p); CHECK(pose_err(q, p, 0.0, 0.1, 1.55, -1.2) < 1e-5, "degenerate quaternion treated as identity"); }

    /* ---- the game ---- */
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mgs4vr_xr_desk"; RegisterClassA(&wc);
    hwnd = CreateWindowA("mgs4vr_xr_desk", "xr desk", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, NULL, NULL, wc.hInstance, NULL);
    sd.BufferDesc.Width = 640; sd.BufferDesc.Height = 360; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.OutputWindow = hwnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    CHECK(SUCCEEDED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, NULL, &g_ctx)), "game device + swapchain (not SINGLETHREADED)");
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxgi); IDXGIDevice_GetAdapter(dxgi, &adapter); IDXGIAdapter_GetDesc(adapter, &ad);
    fk.luid = ad.AdapterLuid; IDXGIAdapter_Release(adapter); IDXGIDevice_Release(dxgi);
    ID3D11Device_AddRef(g_dev); ref_before = ID3D11Device_Release(g_dev);
    yaw_pitch_quat(0.5235987756, 0.35, fk.head_q); fk.head_p[0] = 0.1f; fk.head_p[1] = 1.55f; fk.head_p[2] = -0.2f;
    g_color[0] = 1.0f; g_color[1] = 0.2f; g_color[2] = 0.4f; g_color[3] = 1.0f;   /* 255, 51, 102: exact in 8 bit */

    mgs4vr_xr_configure(&cfg);
    mgs4vr_xr_set_views_callback(observe_views);
    CHECK(mgs4vr_xr_start(test_log, NULL, (void *)fk_gipa), "module started against the fake runtime");
    for (i = 0; i < 20; ++i) { game_present(D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT); Sleep(5); }
    mgs4vr_xr_get_stats(&st);
    CHECK(st.captures == 0 && st.device_singlethreaded == 1 && fk.sess_created == 0, "SINGLETHREADED game device: nothing captured, no session");
    for (i = 0; i < 60; ++i) { game_present(0); Sleep(5); }
    mgs4vr_xr_get_stats(&st);
    CHECK(st.captures >= 60 && st.no_hmd_polls >= 1 && fk.sess_created == 0 && st.frames == 0, "no headset yet: game keeps presenting, module polls, no session");

    InterlockedExchange(&fk.hmd, 1);
    want_layers = 5; want_begins = 1;
    CHECK(pump_until(c_begins, 15000), "headset appears: session begins and theater frames are submitted");
    CHECK(view_valid >= 5 && view_bad == 0, "stereo views: left/right poses, asymmetric FOV, predicted time and monotonic identity");
    for (i=1;i<=4;++i) {
        LONG invalid0=view_invalid, valid0;
        InterlockedExchange(&view_mode,i);
        mgs4vr_xr_get_stats(&st); want_layers=st.frames_with_layer+5;
        CHECK(pump_until(c_layers,5000), "theater survives unusable stereo views");
        valid0=view_valid;
        mgs4vr_xr_get_stats(&st); want_layers=st.frames_with_layer+5;
        CHECK(pump_until(c_layers,5000) && view_invalid>invalid0 && view_valid==valid0, "bad result/count/tracking/NaN revokes stereo data");
    }
    InterlockedExchange(&view_mode,0);
    mgs4vr_xr_get_stats(&st); want_layers=st.frames_with_layer+5;
    CHECK(pump_until(c_layers,5000) && view_bad==0, "valid views recover without recreating theater");
    CHECK(fk.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && fk.w == 640 && fk.h == 360, "swapchain: sRGB sibling chosen, size of the back buffer");
    want_px[0] = 255; want_px[1] = 51; want_px[2] = 102; want_px[3] = 255;
    i = pump_until(c_pixel, 5000);
    printf("  centre pixel read back: %u,%u,%u,%u after %ld reads\n", fk.last_pixel[0], fk.last_pixel[1], fk.last_pixel[2], fk.last_pixel[3], fk.pixel_reads);
    CHECK(i, "the game's picture arrives in the submitted layer (centre pixel 255,51,102)");
    {
        const XrCompositionLayerQuad *lq = &fk.last_quad;
        float lqq[4] = { lq->pose.orientation.x, lq->pose.orientation.y, lq->pose.orientation.z, lq->pose.orientation.w };
        float lqp[3] = { lq->pose.position.x, lq->pose.position.y, lq->pose.position.z };
        CHECK(pose_err(lqq, lqp, 0.5235987756, 0.1 - sin(0.5235987756) * 2.5, 1.55, -0.2 - cos(0.5235987756) * 2.5) < 1e-4, "quad anchored 2.5 m in front of the head's heading, upright");
        CHECK(fabs(lq->size.width - 3.2f) < 1e-5 && fabs(lq->size.height - 1.8f) < 1e-5 && lq->eyeVisibility == XR_EYE_VISIBILITY_BOTH &&
              lq->subImage.imageRect.extent.width == 640 && lq->subImage.imageRect.extent.height == 360, "quad 3.2 x 1.8 m (16:9), both eyes, full image rect");
    }
    g_color[0] = 0.0f; g_color[1] = 1.0f; g_color[2] = 0.0f;
    want_px[0] = 0; want_px[1] = 255; want_px[2] = 0;
    CHECK(pump_until(c_pixel, 5000), "a new game frame replaces the picture (0,255,0)");

    /* head moves: the anchor must NOT follow (a screen in the room, not glued to the face) */
    yaw_pitch_quat(-1.0, 0.0, fk.head_q); fk.head_p[0] = 1.0f;
    mgs4vr_xr_get_stats(&st); want_layers = st.frames_with_layer + 10; pump_until(c_layers, 5000);
    CHECK(fabs(fk.last_quad.pose.position.x - (0.1f - (float)sin(0.5235987756) * 2.5f)) < 1e-4, "head turns and moves: the screen stays where it was anchored");
    cfg.recenter = 1; mgs4vr_xr_configure(&cfg);
    mgs4vr_xr_get_stats(&st); want_layers = st.frames_with_layer + 10; pump_until(c_layers, 5000);
    {
        float lqq[4] = { fk.last_quad.pose.orientation.x, fk.last_quad.pose.orientation.y, fk.last_quad.pose.orientation.z, fk.last_quad.pose.orientation.w };
        float lqp[3] = { fk.last_quad.pose.position.x, fk.last_quad.pose.position.y, fk.last_quad.pose.position.z };
        CHECK(pose_err(lqq, lqp, -1.0, 1.0 - sin(-1.0) * 2.5, 1.55, -0.2 - cos(-1.0) * 2.5) < 1e-4, "recentre: re-anchored in front of the new head pose");
    }

    /* headset off and on again - without a restart (MGS2 run 15 lesson) */
    InterlockedExchange(&fk.visible, 0); fk_push(XR_SESSION_STATE_STOPPING);
    want_ends = 1; CHECK(pump_until(c_ends, 5000) && fk.end_session == 1, "headset off: STOPPING answered with xrEndSession");
    { long f0; Sleep(200); f0 = fk.frames; for (i = 0; i < 30; ++i) { game_present(0); Sleep(8); } CHECK(fk.frames == f0, "while off: no xr frames, the game keeps presenting"); }
    yaw_pitch_quat(2.0, 0.0, fk.head_q);
    fk_push(XR_SESSION_STATE_READY);
    mgs4vr_xr_get_stats(&st); want_begins = 2; want_layers = st.frames_with_layer + 5;
    CHECK(pump_until(c_begins, 8000) && fk.sess_created == 1, "headset on again: same session begins again, layers resume");
    {
        float lqq[4] = { fk.last_quad.pose.orientation.x, fk.last_quad.pose.orientation.y, fk.last_quad.pose.orientation.z, fk.last_quad.pose.orientation.w };
        float lqp[3] = { fk.last_quad.pose.position.x, fk.last_quad.pose.position.y, fk.last_quad.pose.position.z };
        CHECK(pose_err(lqq, lqp, 2.0, 1.0 - sin(2.0) * 2.5, 1.55, -0.2 - cos(2.0) * 2.5) < 1e-4, "after putting it back on the screen is anchored in front of the user again");
    }

    cfg.enabled = 0; mgs4vr_xr_configure(&cfg);
    mgs4vr_xr_get_stats(&st); { long w0 = st.frames_with_layer; want_layers = st.frames_without_layer + 10; CHECK(pump_until(c_nolayer, 5000), "layer switched off: frames continue with zero layers"); mgs4vr_xr_get_stats(&st); CHECK(st.frames_with_layer <= w0 + 1, "no theater layer while switched off"); }
    cfg.enabled = 1; mgs4vr_xr_configure(&cfg);

    /* the game changes resolution */
    CHECK(SUCCEEDED(IDXGISwapChain_ResizeBuffers(g_sc, 0, 800, 450, DXGI_FORMAT_UNKNOWN, 0)), "ResizeBuffers succeeds (module holds no back buffer reference)");
    g_color[0] = 0.0f; g_color[1] = 0.0f; g_color[2] = 1.0f; want_px[0] = 0; want_px[1] = 0; want_px[2] = 255;
    CHECK(pump_until(c_size, 8000), "after the resize: new 800x450 swapchain carries the new picture");

    /* runtime goes away (Link restart): full re-init */
    fk_push(XR_SESSION_STATE_LOSS_PENDING);
    mgs4vr_xr_get_stats(&st); want_reinit = 1; want_layers = st.frames_with_layer + 5;
    CHECK(pump_until(c_reinit, 15000) && fk.inst_created == 2 && fk.sess_created == 2, "session loss: instance and session rebuilt, layers resume");

    cfg.stereo=1;mgs4vr_xr_configure(&cfg);stereo_test=1;
    for(i=0;i<80 && fk.stereo_frames<3;++i){game_present(0);Sleep(8);}
    CHECK(fk.stereo_frames>=3 && fk.w==1600 && fk.h==450,"stereo projection layer packs two 800x450 eye images");
    CHECK(fk.eye_pixels[0][0]==255 && fk.eye_pixels[0][1]==0 && fk.eye_pixels[1][0]==0 && fk.eye_pixels[1][1]==255,"left red and right green remain distinct in actual D3D11 textures");
    CHECK(fabsf(fk.stereo_views[0].pose.position.x+.234f)<1e-6 && fabsf(fk.stereo_views[1].pose.position.x-.123f)<1e-6 && fabsf(fk.stereo_views[0].fov.angleLeft+.9f)<1e-6,"projection uses captured packet pose and asymmetric FOV, not latest located views");
    missing_packet=1;game_present(0);Sleep(60);
    {LONG n=fk.stereo_frames,q=fk.layer_frames;for(i=0;i<8;++i){game_present(0);Sleep(12);}CHECK(fk.stereo_frames==n && fk.layer_frames>q,"missing association switches to visible theater, no stale stereo pair");
      CHECK(fk.last_quad.subImage.imageRect.extent.width==800 && fk.last_quad.subImage.imageRect.extent.height==450 && fabsf(fk.last_quad.size.height/cfg.width_m-450.0f/800)<1e-6,"fallback quad crops one full image with native aspect");
      CHECK(fk.last_pixel[0]==255 || fk.last_pixel[1]==255,"fallback transports current backbuffer pixels");}
    missing_packet=0;
    {LONG n=fk.stereo_frames;for(i=0;i<15;++i){game_present(0);Sleep(12);}CHECK(fk.stereo_frames>n,"fresh adjacent eye pair recovers");}
    view_mode=3;
    for(i=0;i<5;++i){game_present(0);Sleep(12);}
    {LONG n=fk.stereo_frames;for(i=0;i<10;++i){game_present(0);Sleep(12);}CHECK(fk.stereo_frames==n,"invalid XR tracking revokes captured stereo pair");}
    view_mode=0;
    {LONG n=fk.stereo_frames;for(i=0;i<15;++i){game_present(0);Sleep(12);}CHECK(fk.stereo_frames>n,"valid tracking and fresh images recover stereo");}
    Sleep(180);
    {LONG n=fk.stereo_frames;Sleep(50);CHECK(fk.stereo_frames==n,"stalled capture expires stereo pair");}
    CHECK(SUCCEEDED(IDXGISwapChain_ResizeBuffers(g_sc,0,640,360,DXGI_FORMAT_UNKNOWN,0)),"stereo resize releases backbuffer");
    for(i=0;i<20;++i){game_present(0);Sleep(12);}
    CHECK(fk.w==1280 && fk.h==360,"stereo resize reconstructs both eye textures and packed swapchain");
    cfg.stereo=0;cfg.menu_open=1;cfg.menu_row=3;cfg.menu_stereo=1;cfg.stereo_available=1;cfg.head_available=1;mgs4vr_xr_configure(&cfg);
    for(i=0;i<20;++i){game_present(0);Sleep(12);}
    CHECK(fk.w==640 && fk.h==360,"Home menu switches packed stereo to mono theater");
    {LONG n=fk.stereo_frames;for(i=0;i<10;++i){game_present(0);Sleep(12);}CHECK(fk.stereo_frames==n,"Home menu remains a mono layer");}
    CHECK(memcmp(fk.last_pixel,want_px,4)!=0,"Home menu pixels replace the scene inside the panel");
    cfg.menu_open=0;cfg.stereo=1;mgs4vr_xr_configure(&cfg);
    {LONG n=fk.stereo_frames;for(i=0;i<20;++i){game_present(0);Sleep(12);}CHECK(fk.stereo_frames>n && fk.w==1280,"closing Home menu recovers fresh stereo");}
    cfg.stereo=0;mgs4vr_xr_configure(&cfg);for(i=0;i<15;++i){game_present(0);Sleep(12);}
    CHECK(fk.w==640,"stereo off restores mono dimensions");
    mgs4vr_xr_stop();
    CHECK(fk.inst_created == fk.inst_destroyed && fk.sess_created == fk.sess_destroyed && fk.sc_created == fk.sc_destroyed && fk.sp_created == fk.sp_destroyed,
          "stop: every instance, session, swapchain and space destroyed");
    for (i = 0; i < 5; ++i) game_present(0);
    ID3D11Device_AddRef(g_dev); ref_after = ID3D11Device_Release(g_dev);
    printf("  device refcount before %lu after %lu\n", ref_before, ref_after);
    CHECK(ref_after == ref_before, "stop: no leaked references on the game's device");
    mgs4vr_xr_get_stats(&st);
    CHECK(st.errors == 0 && st.capture_failures == 0, "no errors, no capture failures");
    printf("PASS: xr_desk_test\n");
    return 0;
}
