/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "mgs4vr_xr.h"
#include "mgs4vr_menu.h"

typedef XrResult (XRAPI_PTR *GIPA_FN)(XrInstance, const char *, PFN_xrVoidFunction *);

static struct {
    PFN_xrLocateViews LocateViews;
    XrResult (XRAPI_PTR *EnumerateInstanceExtensionProperties)(const char *, uint32_t, uint32_t *, XrExtensionProperties *);
    XrResult (XRAPI_PTR *CreateInstance)(const XrInstanceCreateInfo *, XrInstance *);
    XrResult (XRAPI_PTR *DestroyInstance)(XrInstance);
    XrResult (XRAPI_PTR *GetInstanceProperties)(XrInstance, XrInstanceProperties *);
    XrResult (XRAPI_PTR *GetSystem)(XrInstance, const XrSystemGetInfo *, XrSystemId *);
    XrResult (XRAPI_PTR *GetSystemProperties)(XrInstance, XrSystemId, XrSystemProperties *);
    XrResult (XRAPI_PTR *GetD3D11GraphicsRequirementsKHR)(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR *);
    XrResult (XRAPI_PTR *CreateSession)(XrInstance, const XrSessionCreateInfo *, XrSession *);
    XrResult (XRAPI_PTR *DestroySession)(XrSession);
    XrResult (XRAPI_PTR *CreateReferenceSpace)(XrSession, const XrReferenceSpaceCreateInfo *, XrSpace *);
    XrResult (XRAPI_PTR *DestroySpace)(XrSpace);
    XrResult (XRAPI_PTR *LocateSpace)(XrSpace, XrSpace, XrTime, XrSpaceLocation *);
    XrResult (XRAPI_PTR *PollEvent)(XrInstance, XrEventDataBuffer *);
    XrResult (XRAPI_PTR *BeginSession)(XrSession, const XrSessionBeginInfo *);
    XrResult (XRAPI_PTR *EndSession)(XrSession);
    XrResult (XRAPI_PTR *WaitFrame)(XrSession, const XrFrameWaitInfo *, XrFrameState *);
    XrResult (XRAPI_PTR *BeginFrame)(XrSession, const XrFrameBeginInfo *);
    XrResult (XRAPI_PTR *EndFrame)(XrSession, const XrFrameEndInfo *);
    XrResult (XRAPI_PTR *EnumerateSwapchainFormats)(XrSession, uint32_t, uint32_t *, int64_t *);
    XrResult (XRAPI_PTR *CreateSwapchain)(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *);
    XrResult (XRAPI_PTR *DestroySwapchain)(XrSwapchain);
    XrResult (XRAPI_PTR *EnumerateSwapchainImages)(XrSwapchain, uint32_t, uint32_t *, XrSwapchainImageBaseHeader *);
    XrResult (XRAPI_PTR *AcquireSwapchainImage)(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *);
    XrResult (XRAPI_PTR *WaitSwapchainImage)(XrSwapchain, const XrSwapchainImageWaitInfo *);
    XrResult (XRAPI_PTR *ReleaseSwapchainImage)(XrSwapchain, const XrSwapchainImageReleaseInfo *);
} xr;

#define MAX_IMAGES 8

static void (*volatile g_log)(const char *fmt, ...);
static HANDLE g_thread;
static volatile LONG g_stop, g_started;
static HMODULE g_loader;
static GIPA_FN g_gipa;
static char g_loader_path[MAX_PATH];

static XrInstance g_inst;
static XrSystemId g_sys;
static XrSession g_sess;
static XrSpace g_local, g_view;
static XrSwapchain g_swap;
static XrSwapchainImageD3D11KHR g_images[MAX_IMAGES];
static uint32_t g_image_count;
static unsigned g_swap_w, g_swap_h;
static int g_running;                       

static CRITICAL_SECTION g_cs;               
static LONG g_cs_state;
static ID3D11Device *g_dev;                 
static ID3D11DeviceContext *g_ctx;
static ID3D11Texture2D *g_store;            
static unsigned g_store_w, g_store_h;
static DXGI_FORMAT g_store_format;
static volatile LONG g_store_valid;
static volatile LONG g_refused_logged;

static SRWLOCK config_lock=SRWLOCK_INIT;
static MGS4VR_XR_CONFIG g_cfg[2];
static volatile LONG g_cfg_idx;
static int g_have_anchor;
static long g_anchor_recenter;
static float g_anchor_dist, g_anchor_off;
static XrPosef g_anchor;

static MGS4VR_XR_STATS g_stats;
static void publish_views(XrTime time, int locate);
static void (*volatile g_views_cb)(const MGS4VR_XR_VIEWS *views);
static unsigned long g_views_sequence;
static void (*volatile g_pose_cb)(const float quat[4], const float pos[3]);

static void log_msg(const char *fmt, ...) {
    char text[800];
    va_list a;
    void (*log)(const char *f, ...) = g_log;
    if (!log) return;
    va_start(a, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, a);
    va_end(a);
    log("xr: %s", text);
}

static void cs_init(void) {
    if (InterlockedCompareExchange(&g_cs_state, 1, 0) == 0) { InitializeCriticalSection(&g_cs); InterlockedExchange(&g_cs_state, 2); }
    while (g_cs_state != 2) Sleep(0);
}

static void quat_rotate(const float q[4], const float v[3], float out[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2.0f * (y * v[2] - z * v[1]);
    float ty = 2.0f * (z * v[0] - x * v[2]);
    float tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

void mgs4vr_xr_screen_pose(const float head_quat[4], const float head_pos[3], float dist_m, float height_offset_m,
                           float out_quat[4], float out_pos[3]) {
    const float fwd0[3] = { 0, 0, -1 }, right0[3] = { 1, 0, 0 };
    float f[3], r[3], yaw, n;
    float q[4] = { head_quat[0], head_quat[1], head_quat[2], head_quat[3] };
    n = (float)sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f)) { q[0] = q[1] = q[2] = 0; q[3] = 1; } else { q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n; }
    quat_rotate(q, fwd0, f);
    
    if (f[0] * f[0] + f[2] * f[2] > 0.04f) yaw = (float)atan2(-f[0], -f[2]);
    else { quat_rotate(q, right0, r); yaw = (float)atan2(-r[2], r[0]); }
    out_quat[0] = 0; out_quat[1] = (float)sin(yaw * 0.5f); out_quat[2] = 0; out_quat[3] = (float)cos(yaw * 0.5f);
    out_pos[0] = head_pos[0] - (float)sin(yaw) * dist_m;
    out_pos[1] = head_pos[1] + height_offset_m;
    out_pos[2] = head_pos[2] - (float)cos(yaw) * dist_m;
}

static int resolve(const char *name, void *slot) {
    PFN_xrVoidFunction fn = NULL;
    if (XR_FAILED(g_gipa(g_inst, name, &fn)) || !fn) { log_msg("missing %s", name); return 0; }
    *(PFN_xrVoidFunction *)slot = fn;
    return 1;
}

static const char *state_name(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE"; case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED"; case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED"; case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING"; case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

static int create_instance(void) {
    XrInstanceCreateInfo ici;
    XrInstanceProperties ip;
    XrExtensionProperties *ext = NULL;
    uint32_t n = 0, i;
    int have_d3d11 = 0;
    const char *names[1] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    g_inst = XR_NULL_HANDLE;
    if (XR_FAILED(g_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction *)&xr.EnumerateInstanceExtensionProperties)) ||
        XR_FAILED(g_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&xr.CreateInstance))) {
        log_msg("loader does not export the instance entry points"); return 0;
    }
    if (XR_SUCCEEDED(xr.EnumerateInstanceExtensionProperties(NULL, 0, &n, NULL)) && n) {
        ext = (XrExtensionProperties *)calloc(n, sizeof(*ext));
        for (i = 0; ext && i < n; ++i) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        if (ext && XR_SUCCEEDED(xr.EnumerateInstanceExtensionProperties(NULL, n, &n, ext)))
            for (i = 0; i < n; ++i) if (strcmp(ext[i].extensionName, names[0]) == 0) have_d3d11 = 1;
        free(ext);
    }
    if (!have_d3d11) { log_msg("runtime has no %s - no runtime active?", names[0]); return 0; }
    memset(&ici, 0, sizeof(ici));
    ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strcpy_s(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "MGS4 PCVR");
    strcpy_s(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "mgs4vr");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = names;
    { XrResult r = xr.CreateInstance(&ici, &g_inst); if (XR_FAILED(r)) { log_msg("xrCreateInstance failed (%d)", (int)r); g_inst = XR_NULL_HANDLE; return 0; } }
    if (!(resolve("xrDestroyInstance", &xr.DestroyInstance) && resolve("xrGetInstanceProperties", &xr.GetInstanceProperties) &&
          resolve("xrGetSystem", &xr.GetSystem) && resolve("xrGetSystemProperties", &xr.GetSystemProperties) &&
          resolve("xrGetD3D11GraphicsRequirementsKHR", &xr.GetD3D11GraphicsRequirementsKHR) &&
          resolve("xrCreateSession", &xr.CreateSession) && resolve("xrDestroySession", &xr.DestroySession) &&
          resolve("xrCreateReferenceSpace", &xr.CreateReferenceSpace) && resolve("xrDestroySpace", &xr.DestroySpace) &&
          resolve("xrLocateViews", &xr.LocateViews) && resolve("xrLocateSpace", &xr.LocateSpace) && resolve("xrPollEvent", &xr.PollEvent) &&
          resolve("xrBeginSession", &xr.BeginSession) && resolve("xrEndSession", &xr.EndSession) &&
          resolve("xrWaitFrame", &xr.WaitFrame) && resolve("xrBeginFrame", &xr.BeginFrame) && resolve("xrEndFrame", &xr.EndFrame) &&
          resolve("xrEnumerateSwapchainFormats", &xr.EnumerateSwapchainFormats) && resolve("xrCreateSwapchain", &xr.CreateSwapchain) &&
          resolve("xrDestroySwapchain", &xr.DestroySwapchain) && resolve("xrEnumerateSwapchainImages", &xr.EnumerateSwapchainImages) &&
          resolve("xrAcquireSwapchainImage", &xr.AcquireSwapchainImage) && resolve("xrWaitSwapchainImage", &xr.WaitSwapchainImage) &&
          resolve("xrReleaseSwapchainImage", &xr.ReleaseSwapchainImage))) return 0;
    memset(&ip, 0, sizeof(ip)); ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(xr.GetInstanceProperties(g_inst, &ip)))
        log_msg("runtime %s %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
                (unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));
    return 1;
}

static void destroy_swapchain(void) {
    if (g_swap != XR_NULL_HANDLE) { xr.DestroySwapchain(g_swap); g_swap = XR_NULL_HANDLE; }
    g_image_count = 0; g_swap_w = g_swap_h = 0;
}

static void destroy_session(void) {
    publish_views(0, 0);
    destroy_swapchain();
    if (g_view != XR_NULL_HANDLE) { xr.DestroySpace(g_view); g_view = XR_NULL_HANDLE; }
    if (g_local != XR_NULL_HANDLE) { xr.DestroySpace(g_local); g_local = XR_NULL_HANDLE; }
    if (g_sess != XR_NULL_HANDLE) { xr.DestroySession(g_sess); g_sess = XR_NULL_HANDLE; }
    g_running = 0; g_have_anchor = 0;
}

static void destroy_instance(void) {
    destroy_session();
    if (g_inst != XR_NULL_HANDLE) { xr.DestroyInstance(g_inst); g_inst = XR_NULL_HANDLE; }
}

static int create_session(void) {
    XrSystemGetInfo sgi;
    XrSystemProperties sp;
    XrGraphicsRequirementsD3D11KHR req;
    XrGraphicsBindingD3D11KHR bind;
    XrSessionCreateInfo sci;
    XrReferenceSpaceCreateInfo rsci;
    ID3D11Device *dev;
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC ad;
    ID3D11Multithread *mt = NULL;
    XrResult r;
    int same = 0;

    memset(&sgi, 0, sizeof(sgi)); sgi.type = XR_TYPE_SYSTEM_GET_INFO; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xr.GetSystem(g_inst, &sgi, &g_sys);
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {                 
        if (InterlockedIncrement(&g_stats.no_hmd_polls) == 1) log_msg("no headset available yet - will keep polling");
        return 0;
    }
    if (XR_FAILED(r)) { log_msg("xrGetSystem failed (%d)", (int)r); return -1; }
    memset(&sp, 0, sizeof(sp)); sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (XR_SUCCEEDED(xr.GetSystemProperties(g_inst, g_sys, &sp))) log_msg("system %s", sp.systemName);

    EnterCriticalSection(&g_cs); dev = g_dev; if (dev) ID3D11Device_AddRef(dev); LeaveCriticalSection(&g_cs);
    if (!dev) return 0;                                           
    memset(&req, 0, sizeof(req)); req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    r = xr.GetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req);  
    if (XR_FAILED(r)) { log_msg("xrGetD3D11GraphicsRequirementsKHR failed (%d)", (int)r); ID3D11Device_Release(dev); return -1; }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi)) && dxgi) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && adapter) {
            if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad)))
                same = ad.AdapterLuid.LowPart == req.adapterLuid.LowPart && ad.AdapterLuid.HighPart == req.adapterLuid.HighPart;
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi);
    }
    if (!same) { log_msg("the game renders on a different adapter than the headset - cannot share textures"); ID3D11Device_Release(dev); return -1; }
    if (ID3D11Device_GetFeatureLevel(dev) < req.minFeatureLevel) { log_msg("game device feature level too low for the runtime"); ID3D11Device_Release(dev); return -1; }
    
    if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(g_ctx, &IID_ID3D11Multithread, (void **)&mt)) && mt) {
        ID3D11Multithread_SetMultithreadProtected(mt, TRUE);
        if (!ID3D11Multithread_GetMultithreadProtected(mt)) log_msg("WARNING: multithread protection did not switch on");
        ID3D11Multithread_Release(mt);
    } else log_msg("WARNING: ID3D11Multithread unavailable");

    memset(&bind, 0, sizeof(bind)); bind.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR; bind.device = dev;
    memset(&sci, 0, sizeof(sci)); sci.type = XR_TYPE_SESSION_CREATE_INFO; sci.next = &bind; sci.systemId = g_sys;
    r = xr.CreateSession(g_inst, &sci, &g_sess);
    ID3D11Device_Release(dev);
    if (XR_FAILED(r)) { log_msg("xrCreateSession failed (%d)", (int)r); g_sess = XR_NULL_HANDLE; return -1; }
    InterlockedIncrement(&g_stats.sessions);
    memset(&rsci, 0, sizeof(rsci)); rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO; rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xr.CreateReferenceSpace(g_sess, &rsci, &g_local))) { log_msg("LOCAL space failed"); return -1; }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xr.CreateReferenceSpace(g_sess, &rsci, &g_view))) { log_msg("VIEW space failed"); return -1; }
    log_msg("session created on the game's D3D11 device (adapter \"%ls\")", ad.Description);
    return 1;
}

static int ensure_swapchain(unsigned w, unsigned h) {
    XrSwapchainCreateInfo sci;
    int64_t formats[64], want = 0;
    uint32_t n = 0, i;
    if (g_swap != XR_NULL_HANDLE && g_swap_w == w && g_swap_h == h) return 1;
    destroy_swapchain();
    if (XR_FAILED(xr.EnumerateSwapchainFormats(g_sess, 64, &n, formats)) || !n) { log_msg("no swapchain formats"); return 0; }
    
    for (i = 0; i < n && !want; ++i) if (formats[i] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) want = formats[i];
    for (i = 0; i < n && !want; ++i) if (formats[i] == DXGI_FORMAT_R8G8B8A8_UNORM) want = formats[i];
    if (!want) { log_msg("runtime offers no RGBA8 swapchain format (first is %lld)", (long long)formats[0]); return 0; }
    memset(&sci, 0, sizeof(sci)); sci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.format = want; sci.sampleCount = 1; sci.width = w; sci.height = h; sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
    if (XR_FAILED(xr.CreateSwapchain(g_sess, &sci, &g_swap))) { g_swap = XR_NULL_HANDLE; log_msg("xrCreateSwapchain %ux%u failed", w, h); return 0; }
    n = 0;
    if (XR_FAILED(xr.EnumerateSwapchainImages(g_swap, 0, &n, NULL)) || !n || n > MAX_IMAGES) { log_msg("unexpected swapchain image count %u", n); destroy_swapchain(); return 0; }
    for (i = 0; i < n; ++i) { memset(&g_images[i], 0, sizeof(g_images[i])); g_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR; }
    if (XR_FAILED(xr.EnumerateSwapchainImages(g_swap, n, &n, (XrSwapchainImageBaseHeader *)g_images))) { destroy_swapchain(); return 0; }
    g_image_count = n; g_swap_w = w; g_swap_h = h;
    g_stats.width = w; g_stats.height = h; g_stats.format = want;
    log_msg("image swapchain %ux%u format %lld, %u images", w, h, (long long)want, n);
    return 1;
}

static int pump_events(void) {
    XrEventDataBuffer ev;
    for (;;) {
        XrResult r;
        memset(&ev, 0, sizeof(ev)); ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        r = xr.PollEvent(g_inst, &ev);
        if (r == XR_EVENT_UNAVAILABLE) return 1;
        if (XR_FAILED(r)) { log_msg("xrPollEvent failed (%d)", (int)r); return 0; }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *s = (const XrEventDataSessionStateChanged *)&ev;
            log_msg("session state -> %s", state_name(s->state));
            g_stats.state = (long)s->state;
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi;
                memset(&bi, 0, sizeof(bi)); bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_FAILED(xr.BeginSession(g_sess, &bi))) { log_msg("xrBeginSession failed"); return 0; }
                g_running = 1; g_have_anchor = 0;               
                InterlockedIncrement(&g_stats.session_begins);
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                
                publish_views(0, 0);
                if (g_running && XR_FAILED(xr.EndSession(g_sess))) { log_msg("xrEndSession failed"); return 0; }
                g_running = 0;
                InterlockedIncrement(&g_stats.session_ends);
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) return 0;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) { log_msg("instance loss pending"); return 0; }
    }
}

static void publish_views(XrTime time, int locate) {
    MGS4VR_XR_VIEWS out = {0};
    void (*cb)(const MGS4VR_XR_VIEWS *) = g_views_cb;
    XrViewLocateInfo info = {XR_TYPE_VIEW_LOCATE_INFO};
    XrViewState state = {XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    int e, j;
    if (!cb) return;
    out.sequence = ++g_views_sequence; out.display_time = time;
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time; info.space = g_local;
    if (locate && XR_SUCCEEDED(xr.LocateViews(g_sess, &info, &state, 2, &count, views)) && count == 2 &&
        (state.viewStateFlags & (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT)) ==
        (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT)) {
        out.valid = 1;
        for (e = 0; e < 2; ++e) {
            float norm = 0;
            memcpy(out.quat[e], &views[e].pose.orientation, sizeof(out.quat[e]));
            memcpy(out.pos[e], &views[e].pose.position, sizeof(out.pos[e]));
            out.fov[e][0] = views[e].fov.angleLeft; out.fov[e][1] = views[e].fov.angleRight;
            out.fov[e][2] = views[e].fov.angleUp; out.fov[e][3] = views[e].fov.angleDown;
            for (j=0;j<4;++j) {
                if (!isfinite(out.quat[e][j]) || !isfinite(out.fov[e][j]) || fabsf(out.fov[e][j]) >= 1.5707f) out.valid=0;
                norm += out.quat[e][j]*out.quat[e][j];
            }
            for (j=0;j<3;++j) if (!isfinite(out.pos[e][j])) out.valid=0;
            if (fabsf(norm-1.0f)>0.01f || out.fov[e][0]>=out.fov[e][1] || out.fov[e][3]>=out.fov[e][2]) out.valid=0;
        }
    }
    if (!out.valid) { memset(out.quat,0,sizeof(out.quat)); memset(out.pos,0,sizeof(out.pos)); memset(out.fov,0,sizeof(out.fov)); }
    if(cb)cb(&out);
}

static int frame(void) {
    XrFrameWaitInfo wi; XrFrameState fs; XrFrameBeginInfo bi; XrFrameEndInfo ei;
    XrCompositionLayerQuad quad;
    const XrCompositionLayerBaseHeader *layers[1];
    MGS4VR_XR_CONFIG cfg;
    XrResult r;
    int have_layer = 0;
    unsigned w, h;

    AcquireSRWLockShared(&config_lock);cfg=g_cfg[g_cfg_idx&1];ReleaseSRWLockShared(&config_lock);
    memset(&wi, 0, sizeof(wi)); wi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&fs, 0, sizeof(fs)); fs.type = XR_TYPE_FRAME_STATE;
    r = xr.WaitFrame(g_sess, &wi, &fs);
    if (XR_FAILED(r)) { log_msg("xrWaitFrame failed (%d)", (int)r); return 0; }
    memset(&bi, 0, sizeof(bi)); bi.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = xr.BeginFrame(g_sess, &bi);
    if (XR_FAILED(r)) { log_msg("xrBeginFrame failed (%d)", (int)r); return 0; }
    InterlockedIncrement(&g_stats.frames);
    publish_views(fs.predictedDisplayTime, fs.shouldRender);

    if (fs.shouldRender) {
        XrSpaceLocation loc;
        memset(&loc, 0, sizeof(loc)); loc.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(xr.LocateSpace(g_view, g_local, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            float hq[4] = { loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w };
            float hp[3] = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            void (*cb)(const float *, const float *) = g_pose_cb;
            g_stats.last_head_pos[0] = hp[0]; g_stats.last_head_pos[1] = hp[1]; g_stats.last_head_pos[2] = hp[2];
            InterlockedIncrement(&g_stats.poses);
            if (cb) cb(hq, hp);                        
            if (cfg.enabled && g_store_valid &&
                (!g_have_anchor || g_anchor_recenter != cfg.recenter || g_anchor_dist != cfg.dist_m || g_anchor_off != cfg.height_offset_m)) {
                float q[4], p[3];
                mgs4vr_xr_screen_pose(hq, hp, cfg.dist_m, cfg.height_offset_m, q, p);
                g_anchor.orientation.x = q[0]; g_anchor.orientation.y = q[1]; g_anchor.orientation.z = q[2]; g_anchor.orientation.w = q[3];
                g_anchor.position.x = p[0]; g_anchor.position.y = p[1]; g_anchor.position.z = p[2];
                g_have_anchor = 1; g_anchor_recenter = cfg.recenter; g_anchor_dist = cfg.dist_m; g_anchor_off = cfg.height_offset_m;
                memcpy(g_stats.anchor_pos, p, sizeof(p)); memcpy(g_stats.anchor_quat, q, sizeof(q));
                log_msg("theater anchored at (%.2f, %.2f, %.2f) m, %.2f m from the head, %.2f m wide", p[0], p[1], p[2], cfg.dist_m, cfg.width_m);
            }
        }
        EnterCriticalSection(&g_cs); w = g_store_w; h = g_store_h; LeaveCriticalSection(&g_cs);
        if (!cfg.enabled || !g_store_valid) { w = 0; h = 0; }
        if (g_have_anchor && w && h && ensure_swapchain(w, h)) {
            XrSwapchainImageAcquireInfo ai; XrSwapchainImageWaitInfo swi; XrSwapchainImageReleaseInfo ri;
            uint32_t idx = 0;
            memset(&ai, 0, sizeof(ai)); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            memset(&swi, 0, sizeof(swi)); swi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; swi.timeout = XR_INFINITE_DURATION;
            memset(&ri, 0, sizeof(ri)); ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            if (XR_SUCCEEDED(xr.AcquireSwapchainImage(g_swap, &ai, &idx)) && idx < g_image_count) {
                if (XR_SUCCEEDED(xr.WaitSwapchainImage(g_swap, &swi))) {
                    EnterCriticalSection(&g_cs);
                    if (g_store && g_store_w == g_swap_w && g_store_h == g_swap_h && g_ctx) {
                        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_images[idx].texture, (ID3D11Resource *)g_store);
                        if(cfg.menu_open){
                            MGS4VR_MENU menu;int mw=g_swap_w*3/4,mh=g_swap_h*3/4;void *pixels;
                            D3D11_BOX box;
                            mgs4vr_menu_init(&menu,cfg.follow_head,(int)(cfg.dist_m*100+.5f));
                            menu.open=1;menu.row=cfg.menu_row;menu.available=cfg.head_available;
                            pixels=mgs4vr_menu_bitmap(&menu,mw,mh);
                            if(pixels){box.left=(g_swap_w-mw)/2;box.right=box.left+mw;box.top=(g_swap_h-mh)/2;box.bottom=box.top+mh;box.front=0;box.back=1;
                                ID3D11DeviceContext_UpdateSubresource(g_ctx,(ID3D11Resource *)g_images[idx].texture,0,&box,pixels,mw*4,0);}
                        }
                        have_layer = 1;
                        InterlockedIncrement(&g_stats.copies);
                    }
                    LeaveCriticalSection(&g_cs);
                }
                if(XR_FAILED(xr.ReleaseSwapchainImage(g_swap, &ri)))have_layer=0;
            }
        }
    }
    memset(&ei, 0, sizeof(ei)); ei.type = XR_TYPE_FRAME_END_INFO;
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (have_layer) {
        memset(&quad, 0, sizeof(quad)); quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.space = g_local; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swap;
        quad.subImage.imageRect.extent.width = (int32_t)g_swap_w; quad.subImage.imageRect.extent.height = (int32_t)g_swap_h;
        quad.pose = g_anchor;
        quad.size.width = cfg.width_m; quad.size.height = cfg.width_m * (float)g_swap_h / (float)quad.subImage.imageRect.extent.width;
        layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
        ei.layerCount = 1; ei.layers = layers;
        InterlockedIncrement(&g_stats.frames_with_layer);
    } else InterlockedIncrement(&g_stats.frames_without_layer);
    r = xr.EndFrame(g_sess, &ei);
    if (XR_FAILED(r)) { log_msg("xrEndFrame failed (%d)", (int)r); InterlockedIncrement(&g_stats.errors); return 0; }
    return 1;
}

static void wait_ms(DWORD ms) { DWORD t; for (t = 0; t < ms && !g_stop; t += 50) Sleep(50); }

static DWORD WINAPI xr_thread(LPVOID unused) {
    (void)unused;
    if (!g_gipa) {
        g_loader = LoadLibraryA(g_loader_path[0] ? g_loader_path : "openxr_loader.dll");
        if (!g_loader) { log_msg("openxr_loader.dll not found (%s) - staying flat", g_loader_path[0] ? g_loader_path : "default search"); return 0; }
        g_gipa = (GIPA_FN)GetProcAddress(g_loader, "xrGetInstanceProcAddr");
        if (!g_gipa) { log_msg("loader has no xrGetInstanceProcAddr"); return 0; }
    }
    while (!g_stop) {
        int s;
        if (!create_instance()) { wait_ms(10000); continue; }      
        for (s = 0; !g_stop && s == 0; ) { s = create_session(); if (s == 0) wait_ms(2000); }
        if (s == 1) {
            while (!g_stop) {
                if (!pump_events()) break;
                if (g_running) { if (!frame()) break; }
                else Sleep(50);
            }
            if (g_running && g_sess != XR_NULL_HANDLE) { publish_views(0, 0); xr.EndSession(g_sess); }
        }
        destroy_instance();
        if (!g_stop) { InterlockedIncrement(&g_stats.reinit); log_msg("session over - starting again in 3 s"); wait_ms(3000); }
    }
    publish_views(0, 0);
    log_msg("stopped: %ld frames (%ld with a layer), %ld captures, %ld errors", g_stats.frames, g_stats.frames_with_layer, g_stats.captures, g_stats.errors);
    return 0;
}

void mgs4vr_xr_on_present(void *dxgi_swapchain, unsigned device_flags) {
    IDXGISwapChain *sc = (IDXGISwapChain *)dxgi_swapchain;
    ID3D11Texture2D *back = NULL;
    D3D11_TEXTURE2D_DESC td;
    if (!g_started || g_stop || !sc) return;
    if (device_flags & D3D11_CREATE_DEVICE_SINGLETHREADED) {
        g_stats.device_singlethreaded = 1;
        if (InterlockedCompareExchange(&g_refused_logged, 1, 0) == 0)
            log_msg("REFUSED: the game device was created SINGLETHREADED (flags 0x%x); XR must be requested before the game creates its device", device_flags);
        return;
    }
    EnterCriticalSection(&g_cs);
    g_store_valid=0;
    if (!g_dev) {
        ID3D11Device *dev = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&dev)) && dev) {
            g_dev = dev;                                            
            ID3D11Device_GetImmediateContext(dev, &g_ctx);
        }
    }
    if (g_dev && SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back)) && back) {
        ID3D11Texture2D_GetDesc(back, &td);
        if (g_store && (g_store_w != td.Width || g_store_h != td.Height || g_store_format != td.Format)) {
            ID3D11Texture2D_Release(g_store); g_store = NULL; InterlockedExchange(&g_store_valid, 0);
        }
        if (!g_store) {
            D3D11_TEXTURE2D_DESC cd = td;
            cd.Usage = D3D11_USAGE_DEFAULT; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.CPUAccessFlags = 0; cd.MiscFlags = 0;
            cd.SampleDesc.Count = 1; cd.SampleDesc.Quality = 0; cd.MipLevels = 1; cd.ArraySize = 1;
            if (td.SampleDesc.Count != 1 || FAILED(ID3D11Device_CreateTexture2D(g_dev, &cd, NULL, &g_store))) { g_store = NULL; InterlockedIncrement(&g_stats.capture_failures); }
            else { g_store_w = td.Width; g_store_h = td.Height; g_store_format = td.Format; }
        }
        if (g_store) {
            ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_store, (ID3D11Resource *)back);
            InterlockedExchange(&g_store_valid, 1);
            InterlockedIncrement(&g_stats.captures);
        }
        ID3D11Texture2D_Release(back);
    } else if (g_dev) InterlockedIncrement(&g_stats.capture_failures);
    LeaveCriticalSection(&g_cs);
}

void mgs4vr_xr_configure(const MGS4VR_XR_CONFIG *cfg) {
    LONG next;
    MGS4VR_XR_CONFIG c = *cfg;
    if (!(c.dist_m >= 0.5f && c.dist_m <= 10.0f)) c.dist_m = 2.5f;
    if (!(c.width_m >= 0.5f && c.width_m <= 12.0f)) c.width_m = 3.2f;
    if (!(c.height_offset_m >= -2.0f && c.height_offset_m <= 2.0f)) c.height_offset_m = 0.0f;
    AcquireSRWLockExclusive(&config_lock);next=(g_cfg_idx+1)&1;g_cfg[next]=c;InterlockedExchange(&g_cfg_idx,next);ReleaseSRWLockExclusive(&config_lock);
}

int mgs4vr_xr_start(void (*log)(const char *fmt, ...), const char *loader_path, void *get_instance_proc_addr) {
    cs_init();
    if (InterlockedCompareExchange(&g_started, 1, 0)) return 1;
    g_log = log; g_stop = 0; g_refused_logged = 0;
    memset(&g_stats, 0, sizeof(g_stats));
    g_gipa = (GIPA_FN)get_instance_proc_addr;
    g_loader_path[0] = 0;
    if (loader_path) strncpy_s(g_loader_path, sizeof(g_loader_path), loader_path, _TRUNCATE);
    if (!g_cfg[g_cfg_idx & 1].dist_m) { MGS4VR_XR_CONFIG c = { 1, 2.5f, 3.2f, 0.0f, 0 }; mgs4vr_xr_configure(&c); }
    g_thread = CreateThread(NULL, 0, xr_thread, NULL, 0, NULL);
    if (!g_thread) { g_started = 0; return 0; }
    return 1;
}

void mgs4vr_xr_stop(void) {
    if (!g_started) return;
    g_stop = 1;
    if (g_thread) { WaitForSingleObject(g_thread, 15000); CloseHandle(g_thread); g_thread = NULL; }
    EnterCriticalSection(&g_cs);
    
    if (g_store) { ID3D11Texture2D_Release(g_store); g_store = NULL; }
    g_store_valid = 0; g_store_w = g_store_h = 0;
    if (g_ctx) { ID3D11DeviceContext_Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { ID3D11Device_Release(g_dev); g_dev = NULL; }
    LeaveCriticalSection(&g_cs);
    if (g_loader) { FreeLibrary(g_loader); g_loader = NULL; }
    mgs4vr_menu_free();
    g_gipa = NULL;
    g_started = 0;
}

void mgs4vr_xr_get_stats(MGS4VR_XR_STATS *out) { *out = g_stats; }
void mgs4vr_xr_set_pose_callback(void (*cb)(const float quat[4], const float pos[3])) { g_pose_cb = cb; }

void mgs4vr_xr_set_views_callback(void (*cb)(const MGS4VR_XR_VIEWS *views)) { g_views_cb = cb; }
