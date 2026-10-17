#include "mgs4vr_gpu_timer.h"
/* mgs4vr_xr.c - see mgs4vr_xr.h. */
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

#include "mgs4vr_closegate.h"
#include "mgs4vr_theater_rect.h"
#include "mgs4vr_xr.h"
#include "mgs4vr_menu.h"
#include "mgs4vr_wrist_api.h"
#include "mgs4vr_lens.h"
#include "mgs4vr_lens_round.h"

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

/* Optional entry points for motion controllers. Missing ones only disable input. */
static struct {
    XrResult (XRAPI_PTR *StringToPath)(XrInstance, const char *, XrPath *);
    XrResult (XRAPI_PTR *CreateActionSet)(XrInstance, const XrActionSetCreateInfo *, XrActionSet *);
    XrResult (XRAPI_PTR *DestroyActionSet)(XrActionSet);
    XrResult (XRAPI_PTR *CreateAction)(XrActionSet, const XrActionCreateInfo *, XrAction *);
    XrResult (XRAPI_PTR *SuggestInteractionProfileBindings)(XrInstance, const XrInteractionProfileSuggestedBinding *);
    XrResult (XRAPI_PTR *AttachSessionActionSets)(XrSession, const XrSessionActionSetsAttachInfo *);
    XrResult (XRAPI_PTR *CreateActionSpace)(XrSession, const XrActionSpaceCreateInfo *, XrSpace *);
    XrResult (XRAPI_PTR *SyncActions)(XrSession, const XrActionsSyncInfo *);
    XrResult (XRAPI_PTR *GetActionStatePose)(XrSession, const XrActionStateGetInfo *, XrActionStatePose *);
    XrResult (XRAPI_PTR *GetActionStateFloat)(XrSession, const XrActionStateGetInfo *, XrActionStateFloat *);
    XrResult (XRAPI_PTR *GetActionStateBoolean)(XrSession, const XrActionStateGetInfo *, XrActionStateBoolean *);
    XrResult (XRAPI_PTR *GetActionStateVector2f)(XrSession, const XrActionStateGetInfo *, XrActionStateVector2f *);
} xi;

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
static int g_running;                       /* between xrBeginSession and xrEndSession */

static CRITICAL_SECTION g_cs;               /* guards the capture store and g_dev/g_ctx hand-over */
static LONG g_cs_state;
static ID3D11Device *g_dev;                 /* the game's device, AddRef'd */
static ID3D11DeviceContext *g_ctx;
static ID3D11Texture2D *g_store;            /* copy of the last presented back buffer */
static ID3D11Texture2D *g_eye_store[2];
static MGS4VR_EYE_PACKET g_eye_packet[2];
static ULONGLONG g_eye_tick[2];
static void clear_eyes(void){int e;for(e=0;e<2;++e){if(g_eye_store[e])ID3D11Texture2D_Release(g_eye_store[e]);g_eye_store[e]=NULL;}memset(g_eye_packet,0,sizeof(g_eye_packet));}
static int pair_ready(XrTime time){
 int e;if(!g_eye_store[0] || !g_eye_store[1] || !mgs4vr_packet_pair(&g_eye_packet[0],&g_eye_packet[1]))return 0;
 for(e=0;e<2;++e)if(GetTickCount64()-g_eye_tick[e]>150 || time-g_eye_packet[e].display_time>150000000 || g_eye_packet[e].display_time-time>50000000)return 0;
 return 1;
}
static unsigned g_store_w, g_store_h;
static unsigned g_eye_w;                    /* width of one eye image in the stereo stores: 0 = the whole back buffer (AFR); W/2 in two-eye mode */
static DXGI_FORMAT g_store_format;
static int last_projection;
static volatile LONG g_store_valid;
static volatile LONG g_refused_logged;

static MGS4VR_XR_CONFIG g_cfg[2];
static volatile LONG g_cfg_idx;
#include "mgs4vr_observer.inl"
/* Caller holds capture lock. All native eye copies precede the desktop write. */
static void observer_present(void *sc) {
    MGS4VR_XR_CONFIG cfg=g_cfg[g_cfg_idx&1];int e,mode=cfg.observer;
    if(!cfg.enabled||!cfg.stereo||cfg.menu_open||!mode){observer_reset();return;}
    for(e=0;e<2;e++) {
        const MGS4VR_EYE_PACKET *p=&g_eye_packet[e];
        g_ob_source[e].texture=g_eye_store[e];g_ob_source[e].valid=p->valid;
        g_ob_source[e].capture_ms=g_eye_tick[e];g_ob_source[e].serial=p->camera_serial;g_ob_source[e].epoch=p->epoch;
        g_ob_source[e].fov.left=p->fov[0];g_ob_source[e].fov.right=p->fov[1];
        g_ob_source[e].fov.up=p->fov[2];g_ob_source[e].fov.down=p->fov[3];
        g_ob_source[e].raw.qx=p->quat[0];g_ob_source[e].raw.qy=p->quat[1];
        g_ob_source[e].raw.qz=p->quat[2];g_ob_source[e].raw.qw=p->quat[3];
        g_ob_source[e].raw.px=p->pos[0];g_ob_source[e].raw.py=p->pos[1];g_ob_source[e].raw.pz=p->pos[2];
    }
    observer_draw(sc,mode);
}
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

/* ---------------------------------------------------------------- math --- */

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
    /* Heading from the forward vector; looking straight up or down leaves no
       horizontal forward component, then the right vector carries the heading. */
    if (f[0] * f[0] + f[2] * f[2] > 0.04f) yaw = (float)atan2(-f[0], -f[2]);
    else { quat_rotate(q, right0, r); yaw = (float)atan2(-r[2], r[0]); }
    out_quat[0] = 0; out_quat[1] = (float)sin(yaw * 0.5f); out_quat[2] = 0; out_quat[3] = (float)cos(yaw * 0.5f);
    out_pos[0] = head_pos[0] - (float)sin(yaw) * dist_m;
    out_pos[1] = head_pos[1] + height_offset_m;
    out_pos[2] = head_pos[2] - (float)cos(yaw) * dist_m;
}

/* -------------------------------------------------------------- loader --- */

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

/* --------------------------------------------------------------- input --- */

enum { A_GRIP, A_AIM, A_TRIGGER, A_SQUEEZE, A_STICK, A_STICK_CLICK, A_PRIMARY, A_SECONDARY, A_MENU, A_COUNT };
static XrActionSet g_aset;
static XrAction g_act[A_COUNT];
static XrPath g_hand_path[2];
static XrSpace g_grip_space[2], g_aim_space[2];
static volatile LONG g_input_state;         /* 1 attached, 0 not yet, -1 unavailable for this session */
static void (*volatile g_hands_cb)(const MGS4VR_XR_HANDS *hands);
static unsigned long g_hands_seq;
typedef struct { int action, hand; const char *path; } BIND;

static int resolve_opt(const char *name, void *slot) {
    PFN_xrVoidFunction fn = NULL;
    if (XR_FAILED(g_gipa(g_inst, name, &fn)) || !fn) return 0;
    *(PFN_xrVoidFunction *)slot = fn;
    return 1;
}
static void destroy_input(void) {
    int h;
    for (h = 0; h < 2; ++h) {
        if (g_grip_space[h] != XR_NULL_HANDLE) { xr.DestroySpace(g_grip_space[h]); g_grip_space[h] = XR_NULL_HANDLE; }
        if (g_aim_space[h] != XR_NULL_HANDLE) { xr.DestroySpace(g_aim_space[h]); g_aim_space[h] = XR_NULL_HANDLE; }
    }
    if (g_aset != XR_NULL_HANDLE) { xi.DestroyActionSet(g_aset); g_aset = XR_NULL_HANDLE; }   /* destroys its actions */
    memset(g_act, 0, sizeof(g_act));
    InterlockedExchange(&g_input_state, 0);
}
static int make_action(int i, const char *name, const char *label, XrActionType type) {
    XrActionCreateInfo ci;
    memset(&ci, 0, sizeof(ci)); ci.type = XR_TYPE_ACTION_CREATE_INFO;
    strcpy_s(ci.actionName, sizeof(ci.actionName), name);
    strcpy_s(ci.localizedActionName, sizeof(ci.localizedActionName), label);
    ci.actionType = type; ci.countSubactionPaths = 2; ci.subactionPaths = g_hand_path;
    return XR_SUCCEEDED(xi.CreateAction(g_aset, &ci, &g_act[i]));
}
/* One profile per call: a runtime that rejects one profile keeps the others. */
static int suggest(const char *profile, const BIND *b, int n) {
    XrActionSuggestedBinding s[24];
    XrInteractionProfileSuggestedBinding ps;
    char path[128];
    int i; XrResult r;
    if (n > 24) return 0;
    memset(&ps, 0, sizeof(ps)); ps.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    for (i = 0; i < n; ++i) {
        sprintf_s(path, sizeof(path), "/user/hand/%s%s", b[i].hand ? "right" : "left", b[i].path);
        s[i].action = g_act[b[i].action];
        if (XR_FAILED(xi.StringToPath(g_inst, path, &s[i].binding))) return 0;
    }
    if (XR_FAILED(xi.StringToPath(g_inst, profile, &ps.interactionProfile))) return 0;
    ps.countSuggestedBindings = (uint32_t)n; ps.suggestedBindings = s;
    r = xi.SuggestInteractionProfileBindings(g_inst, &ps);
    if (XR_FAILED(r)) { log_msg("bindings for %s rejected (%d)", profile, (int)r); return 0; }
    return 1;
}
#define BOTH(a, p) { a, 0, p }, { a, 1, p }
static const BIND touch[] = {
    BOTH(A_GRIP, "/input/grip/pose"), BOTH(A_AIM, "/input/aim/pose"), BOTH(A_TRIGGER, "/input/trigger/value"),
    BOTH(A_SQUEEZE, "/input/squeeze/value"), BOTH(A_STICK, "/input/thumbstick"), BOTH(A_STICK_CLICK, "/input/thumbstick/click"),
    { A_PRIMARY, 0, "/input/x/click" }, { A_PRIMARY, 1, "/input/a/click" },
    { A_SECONDARY, 0, "/input/y/click" }, { A_SECONDARY, 1, "/input/b/click" }, { A_MENU, 0, "/input/menu/click" } };
static const BIND index_ctl[] = {
    BOTH(A_GRIP, "/input/grip/pose"), BOTH(A_AIM, "/input/aim/pose"), BOTH(A_TRIGGER, "/input/trigger/value"),
    BOTH(A_SQUEEZE, "/input/squeeze/value"), BOTH(A_STICK, "/input/thumbstick"), BOTH(A_STICK_CLICK, "/input/thumbstick/click"),
    BOTH(A_PRIMARY, "/input/a/click"), BOTH(A_SECONDARY, "/input/b/click") };
static const BIND simple[] = {
    BOTH(A_GRIP, "/input/grip/pose"), BOTH(A_AIM, "/input/aim/pose"), BOTH(A_TRIGGER, "/input/select/click"), BOTH(A_MENU, "/input/menu/click") };
#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void setup_input(void) {
    XrActionSetCreateInfo si;
    XrSessionActionSetsAttachInfo ai;
    int h, profiles = 0;
    InterlockedExchange(&g_input_state, 0);
    if (!(resolve_opt("xrStringToPath", &xi.StringToPath) && resolve_opt("xrCreateActionSet", &xi.CreateActionSet) &&
          resolve_opt("xrDestroyActionSet", &xi.DestroyActionSet) && resolve_opt("xrCreateAction", &xi.CreateAction) &&
          resolve_opt("xrSuggestInteractionProfileBindings", &xi.SuggestInteractionProfileBindings) &&
          resolve_opt("xrAttachSessionActionSets", &xi.AttachSessionActionSets) && resolve_opt("xrCreateActionSpace", &xi.CreateActionSpace) &&
          resolve_opt("xrSyncActions", &xi.SyncActions) && resolve_opt("xrGetActionStatePose", &xi.GetActionStatePose) &&
          resolve_opt("xrGetActionStateFloat", &xi.GetActionStateFloat) && resolve_opt("xrGetActionStateBoolean", &xi.GetActionStateBoolean) &&
          resolve_opt("xrGetActionStateVector2f", &xi.GetActionStateVector2f))) {
        log_msg("runtime lacks the action entry points - continuing without controllers");
        InterlockedExchange(&g_input_state, -1); return;
    }
    if (XR_FAILED(xi.StringToPath(g_inst, "/user/hand/left", &g_hand_path[0])) || XR_FAILED(xi.StringToPath(g_inst, "/user/hand/right", &g_hand_path[1]))) goto fail;
    memset(&si, 0, sizeof(si)); si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy_s(si.actionSetName, sizeof(si.actionSetName), "mgs4vr");
    strcpy_s(si.localizedActionSetName, sizeof(si.localizedActionSetName), "MGS4 VR");
    if (XR_FAILED(xi.CreateActionSet(g_inst, &si, &g_aset))) { g_aset = XR_NULL_HANDLE; goto fail; }
    if (!(make_action(A_GRIP, "grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT) && make_action(A_AIM, "aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT) &&
          make_action(A_TRIGGER, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT) && make_action(A_SQUEEZE, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT) &&
          make_action(A_STICK, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT) && make_action(A_STICK_CLICK, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT) &&
          make_action(A_PRIMARY, "primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT) && make_action(A_SECONDARY, "secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT) &&
          make_action(A_MENU, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT))) goto fail;
    profiles += suggest("/interaction_profiles/oculus/touch_controller", touch, COUNT_OF(touch));
    profiles += suggest("/interaction_profiles/valve/index_controller", index_ctl, COUNT_OF(index_ctl));
    profiles += suggest("/interaction_profiles/khr/simple_controller", simple, COUNT_OF(simple));
    if (!profiles) goto fail;
    memset(&ai, 0, sizeof(ai)); ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO; ai.countActionSets = 1; ai.actionSets = &g_aset;
    if (XR_FAILED(xi.AttachSessionActionSets(g_sess, &ai))) goto fail;
    for (h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sc;
        memset(&sc, 0, sizeof(sc)); sc.type = XR_TYPE_ACTION_SPACE_CREATE_INFO; sc.poseInActionSpace.orientation.w = 1.0f; sc.subactionPath = g_hand_path[h];
        sc.action = g_act[A_GRIP];
        if (XR_FAILED(xi.CreateActionSpace(g_sess, &sc, &g_grip_space[h]))) { g_grip_space[h] = XR_NULL_HANDLE; goto fail; }
        sc.action = g_act[A_AIM];
        if (XR_FAILED(xi.CreateActionSpace(g_sess, &sc, &g_aim_space[h]))) { g_aim_space[h] = XR_NULL_HANDLE; goto fail; }
    }
    InterlockedExchange(&g_input_state, 1);
    log_msg("controllers: %d interaction profile(s) suggested, actions attached", profiles);
    return;
fail:
    log_msg("controller input unavailable - continuing without it");
    destroy_input();
    InterlockedExchange(&g_input_state, -1);
}

/* Absolute pose of an action space in LOCAL; flags 0 when not locatable or not finite. */
static void locate_pose(XrSpace space, XrTime time, unsigned *flags, float q[4], float p[3]) {
    XrSpaceLocation l;
    int k;
    memset(&l, 0, sizeof(l)); l.type = XR_TYPE_SPACE_LOCATION;
    *flags = 0;
    if (space == XR_NULL_HANDLE || XR_FAILED(xr.LocateSpace(space, g_local, time, &l))) return;
    q[0] = l.pose.orientation.x; q[1] = l.pose.orientation.y; q[2] = l.pose.orientation.z; q[3] = l.pose.orientation.w;
    p[0] = l.pose.position.x; p[1] = l.pose.position.y; p[2] = l.pose.position.z;
    for (k = 0; k < 4; ++k) if (!isfinite(q[k])) return;
    for (k = 0; k < 3; ++k) if (!isfinite(p[k])) return;
    *flags = (unsigned)l.locationFlags;
}

static void poll_input(XrTime time) {
    MGS4VR_XR_HANDS out;
    XrActiveActionSet as;
    XrActionsSyncInfo sy;
    XrSpaceLocation loc;
    XrResult r;
    int h;
    void (*cb)(const MGS4VR_XR_HANDS *) = g_hands_cb;
    if (g_input_state != 1 || !cb) return;
    memset(&out, 0, sizeof(out));
    memset(&as, 0, sizeof(as)); as.actionSet = g_aset; as.subactionPath = XR_NULL_PATH;
    memset(&sy, 0, sizeof(sy)); sy.type = XR_TYPE_ACTIONS_SYNC_INFO; sy.countActiveActionSets = 1; sy.activeActionSets = &as;
    r = xi.SyncActions(g_sess, &sy);
    if (XR_FAILED(r)) { InterlockedIncrement(&g_stats.input_errors); return; }
    out.focused = r == XR_SUCCESS;                          /* XR_SESSION_NOT_FOCUSED is a success code */
    out.sequence = ++g_hands_seq; out.display_time = time;
    memset(&loc, 0, sizeof(loc)); loc.type = XR_TYPE_SPACE_LOCATION;
    if (XR_SUCCEEDED(xr.LocateSpace(g_view, g_local, time, &loc)) &&
        (loc.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) == (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        out.head_valid = 1;
        out.head_quat[0] = loc.pose.orientation.x; out.head_quat[1] = loc.pose.orientation.y; out.head_quat[2] = loc.pose.orientation.z; out.head_quat[3] = loc.pose.orientation.w;
        out.head_pos[0] = loc.pose.position.x; out.head_pos[1] = loc.pose.position.y; out.head_pos[2] = loc.pose.position.z;
    }
    for (h = 0; h < 2; ++h) {
        MGS4VR_XR_HAND *o = &out.hand[h];
        XrActionStateGetInfo gi;
        XrActionStatePose ps; XrActionStateFloat f; XrActionStateBoolean b; XrActionStateVector2f v;
        static const int bits[4] = { A_STICK_CLICK, A_PRIMARY, A_SECONDARY, A_MENU };
        int k;
        memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.subactionPath = g_hand_path[h];
        memset(&ps, 0, sizeof(ps)); ps.type = XR_TYPE_ACTION_STATE_POSE;
        gi.action = g_act[A_GRIP];
        if (XR_SUCCEEDED(xi.GetActionStatePose(g_sess, &gi, &ps))) o->active = ps.isActive ? 1 : 0;
        if (o->active) {
            locate_pose(g_grip_space[h], time, &o->grip_flags, o->grip_quat, o->grip_pos);
            locate_pose(g_aim_space[h], time, &o->aim_flags, o->aim_quat, o->aim_pos);
        }
        memset(&f, 0, sizeof(f)); f.type = XR_TYPE_ACTION_STATE_FLOAT; gi.action = g_act[A_TRIGGER];
        if (XR_SUCCEEDED(xi.GetActionStateFloat(g_sess, &gi, &f)) && f.isActive) o->trigger = f.currentState;
        memset(&f, 0, sizeof(f)); f.type = XR_TYPE_ACTION_STATE_FLOAT; gi.action = g_act[A_SQUEEZE];
        if (XR_SUCCEEDED(xi.GetActionStateFloat(g_sess, &gi, &f)) && f.isActive) o->squeeze = f.currentState;
        memset(&v, 0, sizeof(v)); v.type = XR_TYPE_ACTION_STATE_VECTOR2F; gi.action = g_act[A_STICK];
        if (XR_SUCCEEDED(xi.GetActionStateVector2f(g_sess, &gi, &v)) && v.isActive) { o->stick[0] = v.currentState.x; o->stick[1] = v.currentState.y; }
        for (k = 0; k < 4; ++k) {
            memset(&b, 0, sizeof(b)); b.type = XR_TYPE_ACTION_STATE_BOOLEAN; gi.action = g_act[bits[k]];
            if (XR_SUCCEEDED(xi.GetActionStateBoolean(g_sess, &gi, &b)) && b.isActive && b.currentState) o->buttons |= 1u << k;
        }
    }
    InterlockedIncrement(&g_stats.hands);
    cb(&out);
}

/* ------------------------------------------------------------- session --- */

/* Wrist panels (mgs4vr_wrist.c): two small sRGB quad swapchains, filled from CPU pixels when shown. */
#define WQ_MAX_IMAGES 8
static XrSwapchain g_wq[3];static XrSwapchainImageD3D11KHR g_wq_img[3][WQ_MAX_IMAGES];static uint32_t g_wq_n[3];static int g_wq_w[3],g_wq_h[3];
static unsigned char g_wq_px[3][320*320*4];
static void destroy_wrist(void){int i;for(i=0;i<3;++i){if(g_wq[i]!=XR_NULL_HANDLE)xr.DestroySwapchain(g_wq[i]);g_wq[i]=XR_NULL_HANDLE;g_wq_n[i]=0;}}
static int ensure_wrist(int i,int w,int h){
    XrSwapchainCreateInfo sci;int64_t formats[64],want=0;uint32_t n=0,k;
    if(g_wq[i]!=XR_NULL_HANDLE&&g_wq_w[i]==w&&g_wq_h[i]==h)return 1;
    if(g_wq[i]!=XR_NULL_HANDLE){xr.DestroySwapchain(g_wq[i]);g_wq[i]=XR_NULL_HANDLE;}
    if(w<=0||h<=0||w*h*4>(int)sizeof(g_wq_px[0])||XR_FAILED(xr.EnumerateSwapchainFormats(g_sess,64,&n,formats))||!n)return 0;
    for(k=0;k<n&&!want;++k)if(formats[k]==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)want=formats[k];
    for(k=0;k<n&&!want;++k)if(formats[k]==DXGI_FORMAT_R8G8B8A8_UNORM)want=formats[k];
    if(!want)return 0;
    memset(&sci,0,sizeof(sci));sci.type=XR_TYPE_SWAPCHAIN_CREATE_INFO;sci.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format=want;sci.sampleCount=1;sci.width=w;sci.height=h;sci.faceCount=1;sci.arraySize=1;sci.mipCount=1;
    if(XR_FAILED(xr.CreateSwapchain(g_sess,&sci,&g_wq[i]))){g_wq[i]=XR_NULL_HANDLE;return 0;}
    n=0;if(XR_FAILED(xr.EnumerateSwapchainImages(g_wq[i],0,&n,NULL))||!n||n>WQ_MAX_IMAGES){xr.DestroySwapchain(g_wq[i]);g_wq[i]=XR_NULL_HANDLE;return 0;}
    for(k=0;k<n;++k){memset(&g_wq_img[i][k],0,sizeof(g_wq_img[i][k]));g_wq_img[i][k].type=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;}
    if(XR_FAILED(xr.EnumerateSwapchainImages(g_wq[i],n,&n,(XrSwapchainImageBaseHeader *)g_wq_img[i]))){xr.DestroySwapchain(g_wq[i]);g_wq[i]=XR_NULL_HANDLE;return 0;}
    g_wq_n[i]=n;g_wq_w[i]=w;g_wq_h[i]=h;log_msg("wrist swapchain %d: %dx%d format %lld",i,w,h,(long long)want);return 1;
}
/* Lens scope (mgs4vr_lens.h): the square the game's second view rendered is cut from the back buffer at Present into
   g_lens_tex (and blanked in the eye images); here it goes into its own quad swapchain on the rifle. */
static ID3D11Texture2D *g_lens_tex,*g_lens_black,*g_lens_round;static unsigned g_lens_s;static volatile LONG g_lens_ready;
static XrSwapchain g_lq;static XrSwapchainImageD3D11KHR g_lq_img[WQ_MAX_IMAGES];static uint32_t g_lq_n;static int g_lq_s;static int64_t g_lq_fmt;
static int ensure_lens_swapchain(int s,int64_t fmt){
    XrSwapchainCreateInfo sci;uint32_t n=0,k;
    if(g_lq!=XR_NULL_HANDLE&&g_lq_s==s&&g_lq_fmt==fmt)return 1;
    if(g_lq!=XR_NULL_HANDLE){xr.DestroySwapchain(g_lq);g_lq=XR_NULL_HANDLE;}
    memset(&sci,0,sizeof(sci));sci.type=XR_TYPE_SWAPCHAIN_CREATE_INFO;sci.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format=fmt;sci.sampleCount=1;sci.width=s;sci.height=s;sci.faceCount=1;sci.arraySize=1;sci.mipCount=1;
    if(XR_FAILED(xr.CreateSwapchain(g_sess,&sci,&g_lq))){g_lq=XR_NULL_HANDLE;return 0;}
    if(XR_FAILED(xr.EnumerateSwapchainImages(g_lq,0,&n,NULL))||!n||n>WQ_MAX_IMAGES){xr.DestroySwapchain(g_lq);g_lq=XR_NULL_HANDLE;return 0;}
    for(k=0;k<n;++k){memset(&g_lq_img[k],0,sizeof(g_lq_img[k]));g_lq_img[k].type=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;}
    if(XR_FAILED(xr.EnumerateSwapchainImages(g_lq,n,&n,(XrSwapchainImageBaseHeader *)g_lq_img))){xr.DestroySwapchain(g_lq);g_lq=XR_NULL_HANDLE;return 0;}
    g_lq_n=n;g_lq_s=s;g_lq_fmt=fmt;log_msg("lens swapchain %dx%d format %lld",s,s,(long long)fmt);return 1;
}
/* Present (game thread, under g_cs): cut the lens square out of g_store. */
static void lens_capture(void){
    int x,y,s;D3D11_TEXTURE2D_DESC d;
    if(!g_store||!g_dev||!mgs4vr_lens_rect(&x,&y,&s)||x<0||y<0||(unsigned)(x+s)>g_store_w||(unsigned)(y+s)>g_store_h){InterlockedExchange(&g_lens_ready,0);return;}
    if(g_lens_tex&&g_lens_s!=(unsigned)s){ID3D11Texture2D_Release(g_lens_tex);g_lens_tex=NULL;if(g_lens_black){ID3D11Texture2D_Release(g_lens_black);g_lens_black=NULL;}}
    if(!g_lens_tex){ID3D11Texture2D_GetDesc(g_store,&d);d.Width=d.Height=s;d.MipLevels=1;d.ArraySize=1;d.SampleDesc.Count=1;d.SampleDesc.Quality=0;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=0;d.MiscFlags=0;
        if(FAILED(ID3D11Device_CreateTexture2D(g_dev,&d,NULL,&g_lens_tex)))g_lens_tex=NULL;g_lens_round=NULL;
        if(g_lens_tex){D3D11_SUBRESOURCE_DATA z;void *zero=calloc((size_t)s*s,4);z.pSysMem=zero;z.SysMemPitch=s*4;z.SysMemSlicePitch=0;
            if(!zero||FAILED(ID3D11Device_CreateTexture2D(g_dev,&d,&z,&g_lens_black)))g_lens_black=NULL;free(zero);}
        g_lens_s=s;}
    if(!g_lens_tex){InterlockedExchange(&g_lens_ready,0);return;}
    {D3D11_BOX b;b.left=x;b.top=y;b.right=x+s;b.bottom=y+s;b.front=0;b.back=1;ID3D11DeviceContext_CopySubresourceRegion(g_ctx,(ID3D11Resource *)g_lens_tex,0,0,0,0,(ID3D11Resource *)g_store,0,&b);}
    {MGS4VR_LENS_CFG lc;mgs4vr_lens_config(&lc);g_lens_round=(lc.round&&!lc.inscope)?mgs4vr_lens_round_run(g_dev,g_ctx,g_lens_tex):NULL;}   /* round mask, game thread, own context state */
    InterlockedExchange(&g_lens_ready,1);InterlockedIncrement(&g_stats.lens_captures);
}
/* Blank the lens square in an eye image (it sits in the corner of the frame). */
static void lens_blank(ID3D11Texture2D *eye){int x,y,s;if(eye&&g_lens_black&&mgs4vr_lens_rect(&x,&y,&s)&&(unsigned)s==g_lens_s)ID3D11DeviceContext_CopySubresourceRegion(g_ctx,(ID3D11Resource *)eye,0,x,y,0,(ID3D11Resource *)g_lens_black,0,NULL);}
/* Reticle (crosshair + round vignette, mgs4vr_lens_reticle): its own alpha-blended quad on the lens pose. CPU pixels made
   once; every swapchain image is filled the first time it is acquired. Destroyed with the session like the other quads. */
#define RQ_N 512
static XrSwapchain g_rq;static XrSwapchainImageD3D11KHR g_rq_img[WQ_MAX_IMAGES];static uint32_t g_rq_n;static unsigned char g_rq_filled[WQ_MAX_IMAGES];
static unsigned g_rq_px[RQ_N*RQ_N];static int g_rq_made=-1;   /* -1 = none, else the round flag it was made with */
static void destroy_lens(void){
    if(g_lq!=XR_NULL_HANDLE){xr.DestroySwapchain(g_lq);g_lq=XR_NULL_HANDLE;}g_lq_n=0;g_lq_s=0;
    if(g_rq!=XR_NULL_HANDLE){xr.DestroySwapchain(g_rq);g_rq=XR_NULL_HANDLE;}g_rq_n=0;memset(g_rq_filled,0,sizeof(g_rq_filled));
}
static int reticle_layer(const XrCompositionLayerQuad *lens,XrCompositionLayerQuad *out){
    XrSwapchainImageAcquireInfo ai;XrSwapchainImageWaitInfo wi;XrSwapchainImageReleaseInfo ri;uint32_t idx=0,n=0,k;int ok=0;
    if(g_rq==XR_NULL_HANDLE){
        XrSwapchainCreateInfo sci;int64_t formats[64],want=0;
        if(XR_FAILED(xr.EnumerateSwapchainFormats(g_sess,64,&n,formats))||!n)return 0;
        for(k=0;k<n&&!want;++k)if(formats[k]==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)want=formats[k];
        for(k=0;k<n&&!want;++k)if(formats[k]==DXGI_FORMAT_R8G8B8A8_UNORM)want=formats[k];
        if(!want)return 0;
        memset(&sci,0,sizeof(sci));sci.type=XR_TYPE_SWAPCHAIN_CREATE_INFO;sci.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sci.format=want;sci.sampleCount=1;sci.width=RQ_N;sci.height=RQ_N;sci.faceCount=1;sci.arraySize=1;sci.mipCount=1;
        if(XR_FAILED(xr.CreateSwapchain(g_sess,&sci,&g_rq))){g_rq=XR_NULL_HANDLE;return 0;}
        n=0;if(XR_FAILED(xr.EnumerateSwapchainImages(g_rq,0,&n,NULL))||!n||n>WQ_MAX_IMAGES){xr.DestroySwapchain(g_rq);g_rq=XR_NULL_HANDLE;return 0;}
        for(k=0;k<n;++k){memset(&g_rq_img[k],0,sizeof(g_rq_img[k]));g_rq_img[k].type=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;}
        if(XR_FAILED(xr.EnumerateSwapchainImages(g_rq,n,&n,(XrSwapchainImageBaseHeader *)g_rq_img))){xr.DestroySwapchain(g_rq);g_rq=XR_NULL_HANDLE;return 0;}
        g_rq_n=n;memset(g_rq_filled,0,sizeof(g_rq_filled));log_msg("reticle swapchain %dx%d format %lld",RQ_N,RQ_N,(long long)want);
    }
    {MGS4VR_LENS_CFG lc;mgs4vr_lens_config(&lc);if(g_rq_made!=lc.round){mgs4vr_lens_reticle(g_rq_px,RQ_N,lc.round);g_rq_made=lc.round;memset(g_rq_filled,0,sizeof(g_rq_filled));}}
    memset(&ai,0,sizeof(ai));ai.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;memset(&wi,0,sizeof(wi));wi.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;wi.timeout=XR_INFINITE_DURATION;
    memset(&ri,0,sizeof(ri));ri.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    if(XR_FAILED(xr.AcquireSwapchainImage(g_rq,&ai,&idx))||idx>=g_rq_n)return 0;
    if(XR_SUCCEEDED(xr.WaitSwapchainImage(g_rq,&wi))){
        ok=1;
        if(!g_rq_filled[idx]){EnterCriticalSection(&g_cs);if(g_ctx){ID3D11DeviceContext_UpdateSubresource(g_ctx,(ID3D11Resource *)g_rq_img[idx].texture,0,NULL,g_rq_px,RQ_N*4,0);g_rq_filled[idx]=1;}else ok=0;LeaveCriticalSection(&g_cs);}
    }
    if(XR_FAILED(xr.ReleaseSwapchainImage(g_rq,&ri)))ok=0;
    if(!ok)return 0;
    *out=*lens;out->layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    out->subImage.swapchain=g_rq;out->subImage.imageRect.extent.width=RQ_N;out->subImage.imageRect.extent.height=RQ_N;
    InterlockedIncrement(&g_stats.reticle_layers);return 1;
}
/* Present (game thread, under g_cs): Titanfall 2 VR style, the zoomed picture drawn into the eye image on the ocular. */
static void lens_inscope(ID3D11Texture2D *eye,const MGS4VR_EYE_PACKET *pk){
    MGS4VR_LENS_CFG lc;float clip[16],dot[2];int x,y,s;
    mgs4vr_lens_config(&lc);
    if(!lc.wanted||!lc.inscope||!eye||!pk)return;
    if(mgs4vr_lens_dot()){   /* red dot sight (16 Oct 2026): the 1x lens picture when there is one (dotglass: none), the dot on top */
        ID3D11Texture2D *pic=(g_lens_tex&&g_lens_ready&&mgs4vr_lens_rect(&x,&y,&s)&&(unsigned)s==g_lens_s)?g_lens_tex:NULL;
        if(mgs4vr_lens_ocular_dot(clip,dot,pk->camera_serial,pk->eye)&&mgs4vr_lens_round_draw_dot(g_dev,g_ctx,pic,eye,clip,dot))InterlockedIncrement(&g_stats.lens_inscope);
        return;
    }
    if(!g_lens_tex||!g_lens_ready||!mgs4vr_lens_rect(&x,&y,&s)||(unsigned)s!=g_lens_s)return;
    if(!mgs4vr_lens_ocular(clip,pk->camera_serial,pk->eye))return;
    if(mgs4vr_lens_round_draw(g_dev,g_ctx,g_lens_tex,eye,clip,lc.reticle))InterlockedIncrement(&g_stats.lens_inscope);
}
/* XR thread: lens quad layer; 1 when added. */
static ID3D11Texture2D *g_lens_tex_fwd(void){return g_lens_tex;}
static int lens_layer(XrCompositionLayerQuad *out){
    float q[4],p[3],wm;XrSwapchainImageAcquireInfo ai;XrSwapchainImageWaitInfo wi;XrSwapchainImageReleaseInfo ri;uint32_t idx=0;int ok=0,round=0;D3D11_TEXTURE2D_DESC d;
    {MGS4VR_LENS_CFG lc;mgs4vr_lens_config(&lc);if(lc.inscope)return 0;}   /* drawn into the eye images instead */
    if(!g_lens_ready||!g_lens_tex||!g_ctx||!mgs4vr_lens_quad(q,p,&wm))return 0;
    {ID3D11Texture2D *src;EnterCriticalSection(&g_cs);src=g_lens_round?g_lens_round:g_lens_tex;if(src)ID3D11Texture2D_GetDesc(src,&d);round=g_lens_round!=NULL;LeaveCriticalSection(&g_cs);if(!src)return 0;}
    if(!ensure_lens_swapchain((int)g_lens_s,d.Format==DXGI_FORMAT_R8G8B8A8_UNORM?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:d.Format))return 0;
    memset(&ai,0,sizeof(ai));ai.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;memset(&wi,0,sizeof(wi));wi.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;wi.timeout=XR_INFINITE_DURATION;
    memset(&ri,0,sizeof(ri));ri.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    if(XR_FAILED(xr.AcquireSwapchainImage(g_lq,&ai,&idx))||idx>=g_lq_n)return 0;
    if(XR_SUCCEEDED(xr.WaitSwapchainImage(g_lq,&wi))){EnterCriticalSection(&g_cs);if(g_ctx&&g_lens_tex){ID3D11Texture2D *src=g_lens_round?g_lens_round:g_lens_tex;D3D11_TEXTURE2D_DESC sd;ID3D11Texture2D_GetDesc(src,&sd);
            if(sd.Format==d.Format){ID3D11DeviceContext_CopyResource(g_ctx,(ID3D11Resource *)g_lq_img[idx].texture,(ID3D11Resource *)src);ok=1;}}LeaveCriticalSection(&g_cs);}
    if(XR_FAILED(xr.ReleaseSwapchainImage(g_lq,&ri)))ok=0;
    if(!ok)return 0;
    memset(out,0,sizeof(*out));out->type=XR_TYPE_COMPOSITION_LAYER_QUAD;out->space=g_local;out->eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    if(round)out->layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   /* premultiplied round mask */
    out->subImage.swapchain=g_lq;out->subImage.imageRect.extent.width=g_lq_s;out->subImage.imageRect.extent.height=g_lq_s;
    memcpy(&out->pose.orientation,q,16);memcpy(&out->pose.position,p,12);out->size.width=wm;out->size.height=wm;
    InterlockedIncrement(&g_stats.lens_layers);return 1;
}
/* Fills quad layers for the open wrist panels; returns how many were added to out[]. */
static int wrist_layers(XrCompositionLayerQuad out[3]){
    MGS4VR_XR_QUAD q[3];unsigned char *px[3];int i,n=0;
    px[0]=g_wq_px[0];px[1]=g_wq_px[1];px[2]=g_wq_px[2];
    if(!mgs4vr_wrist_quads(q,px)||!g_ctx)return 0;
    for(i=0;i<3;++i){
        XrSwapchainImageAcquireInfo ai;XrSwapchainImageWaitInfo wi;XrSwapchainImageReleaseInfo ri;uint32_t idx=0;int ok=0;
        if(!q[i].visible||!ensure_wrist(i,q[i].w,q[i].h))continue;
        memset(&ai,0,sizeof(ai));ai.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;memset(&wi,0,sizeof(wi));wi.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;wi.timeout=XR_INFINITE_DURATION;
        memset(&ri,0,sizeof(ri));ri.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        if(XR_FAILED(xr.AcquireSwapchainImage(g_wq[i],&ai,&idx))||idx>=g_wq_n[i])continue;
        if(XR_SUCCEEDED(xr.WaitSwapchainImage(g_wq[i],&wi))){
            EnterCriticalSection(&g_cs);
            if(g_ctx){ID3D11DeviceContext_UpdateSubresource(g_ctx,(ID3D11Resource *)g_wq_img[i][idx].texture,0,NULL,g_wq_px[i],q[i].w*4,0);ok=1;}
            LeaveCriticalSection(&g_cs);
        }
        if(XR_FAILED(xr.ReleaseSwapchainImage(g_wq[i],&ri)))ok=0;
        if(!ok)continue;
        memset(&out[n],0,sizeof(out[n]));out[n].type=XR_TYPE_COMPOSITION_LAYER_QUAD;out[n].space=g_local;out[n].eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        out[n].subImage.swapchain=g_wq[i];out[n].subImage.imageRect.extent.width=q[i].w;out[n].subImage.imageRect.extent.height=q[i].h;
        memcpy(&out[n].pose.orientation,q[i].q,16);memcpy(&out[n].pose.position,q[i].p,12);out[n].size.width=q[i].width;out[n].size.height=q[i].height;
        ++n;InterlockedIncrement(&g_stats.wrist_layers);
    }
    return n;
}
static void destroy_swapchain(void) {
    destroy_wrist(); destroy_lens();   /* lens + reticle quads were not destroyed with the session before 13 Oct */
    if (g_swap != XR_NULL_HANDLE) { xr.DestroySwapchain(g_swap); g_swap = XR_NULL_HANDLE; }
    g_image_count = 0; g_swap_w = g_swap_h = 0;
}

static void destroy_session(void) {
    publish_views(0, 0);
    destroy_swapchain();
    destroy_input();
    if (g_view != XR_NULL_HANDLE) { xr.DestroySpace(g_view); g_view = XR_NULL_HANDLE; }
    if (g_local != XR_NULL_HANDLE) { xr.DestroySpace(g_local); g_local = XR_NULL_HANDLE; }
    if (g_sess != XR_NULL_HANDLE) { xr.DestroySession(g_sess); g_sess = XR_NULL_HANDLE; }
    g_running = 0; g_have_anchor = 0;
}

static void destroy_instance(void) {
    destroy_session();
    if (g_inst != XR_NULL_HANDLE) { xr.DestroyInstance(g_inst); g_inst = XR_NULL_HANDLE; }
}

/* 1 ready, 0 not yet (no headset / no device: retry later), -1 fatal for this instance */
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
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {                 /* runtime alive, headset not connected yet: transient */
        if (InterlockedIncrement(&g_stats.no_hmd_polls) == 1) log_msg("no headset available yet - will keep polling");
        return 0;
    }
    if (XR_FAILED(r)) { log_msg("xrGetSystem failed (%d)", (int)r); return -1; }
    memset(&sp, 0, sizeof(sp)); sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (XR_SUCCEEDED(xr.GetSystemProperties(g_inst, g_sys, &sp))) log_msg("system %s", sp.systemName);

    EnterCriticalSection(&g_cs); dev = g_dev; if (dev) ID3D11Device_AddRef(dev); LeaveCriticalSection(&g_cs);
    if (!dev) return 0;                                           /* the game has not presented yet */
    memset(&req, 0, sizeof(req)); req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
    r = xr.GetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req);  /* mandatory before xrCreateSession */
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
    /* Two threads drive this context from now on. */
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
    setup_input();
    return 1;
}

static int ensure_swapchain(unsigned w, unsigned h) {
    XrSwapchainCreateInfo sci;
    int64_t formats[64], want = 0;
    uint32_t n = 0, i;
    if (g_swap != XR_NULL_HANDLE && g_swap_w == w && g_swap_h == h) return 1;
    destroy_swapchain();
    if (XR_FAILED(xr.EnumerateSwapchainFormats(g_sess, 64, &n, formats)) || !n) { log_msg("no swapchain formats"); return 0; }
    /* The game presents R8G8B8A8_UNORM holding sRGB-encoded colours. The _SRGB
       sibling is in the same TYPELESS family (CopyResource stays legal) and tells
       the compositor how to decode it; plain UNORM would be shown too bright. */
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

/* 1 keep going, 0 this session/instance is over */
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
                g_running = 1; g_have_anchor = 0;               /* headset (back) on: re-anchor the screen in front of it */
                InterlockedIncrement(&g_stats.session_begins);
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                /* Normal when the headset comes off. End the session and keep
                   pumping: READY arrives again when it is put back on. */
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
    if (!cb && !g_cfg[g_cfg_idx&1].stereo) return;
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
    if(!out.valid){EnterCriticalSection(&g_cs);memset(g_eye_packet,0,sizeof(g_eye_packet));LeaveCriticalSection(&g_cs);}
    if(cb)cb(&out);
}

static int frame(void) {
    XrFrameWaitInfo wi; XrFrameState fs; XrFrameBeginInfo bi; XrFrameEndInfo ei;
    XrCompositionLayerQuad quad;
    XrCompositionLayerProjection projection;
    XrCompositionLayerProjectionView pv[2];
    MGS4VR_EYE_PACKET submitted[2];
    const XrCompositionLayerBaseHeader *layers[6];
    XrCompositionLayerQuad wrist[3],lensq,retq;int nwrist=0,nlens=0;
    MGS4VR_XR_CONFIG cfg = g_cfg[g_cfg_idx & 1];
    XrResult r;
    int have_layer = 0,use_stereo=0,head_valid=0;unsigned theater_w=0;
    float frame_hq[4],frame_hp[3];
    unsigned w, h;

    memset(&wi, 0, sizeof(wi)); wi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&fs, 0, sizeof(fs)); fs.type = XR_TYPE_FRAME_STATE;
    r = xr.WaitFrame(g_sess, &wi, &fs);
    if (XR_FAILED(r)) { log_msg("xrWaitFrame failed (%d)", (int)r); return 0; }
    memset(&bi, 0, sizeof(bi)); bi.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = xr.BeginFrame(g_sess, &bi);
    if (XR_FAILED(r)) { log_msg("xrBeginFrame failed (%d)", (int)r); return 0; }
    InterlockedIncrement(&g_stats.frames);
    publish_views(fs.predictedDisplayTime, fs.shouldRender);
    poll_input(fs.predictedDisplayTime);

    if (fs.shouldRender) {
        XrSpaceLocation loc;
        memset(&loc, 0, sizeof(loc)); loc.type = XR_TYPE_SPACE_LOCATION;
        if (XR_SUCCEEDED(xr.LocateSpace(g_view, g_local, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            float hq[4] = { loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w };
            float hp[3] = { loc.pose.position.x, loc.pose.position.y, loc.pose.position.z };
            void (*cb)(const float *, const float *) = g_pose_cb;
            memcpy(frame_hq,hq,sizeof(hq));memcpy(frame_hp,hp,sizeof(hp));head_valid=1;
            g_stats.last_head_pos[0] = hp[0]; g_stats.last_head_pos[1] = hp[1]; g_stats.last_head_pos[2] = hp[2];
            InterlockedIncrement(&g_stats.poses);
            if (cb) cb(hq, hp);                        /* head tracking, independent of the theater layer */
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
        EnterCriticalSection(&g_cs); w = g_store_w; h = g_store_h; if(cfg.stereo)w=(g_eye_w?g_eye_w:g_store_w)*2; LeaveCriticalSection(&g_cs);
        if (!cfg.enabled || !g_store_valid) { w = 0; h = 0; }
        if ((cfg.stereo || g_have_anchor) && w && h && ensure_swapchain(w, h)) {
            XrSwapchainImageAcquireInfo ai; XrSwapchainImageWaitInfo swi; XrSwapchainImageReleaseInfo ri;
            uint32_t idx = 0;
            memset(&ai, 0, sizeof(ai)); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            memset(&swi, 0, sizeof(swi)); swi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; swi.timeout = XR_INFINITE_DURATION;
            memset(&ri, 0, sizeof(ri)); ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            if (XR_SUCCEEDED(xr.AcquireSwapchainImage(g_swap, &ai, &idx)) && idx < g_image_count) {
                if (XR_SUCCEEDED(xr.WaitSwapchainImage(g_swap, &swi))) {
                    EnterCriticalSection(&g_cs);
                    mgs4vr_gpu_xr_begin();
                    if(cfg.stereo && g_ctx && (g_eye_w?g_eye_w:g_store_w)*2==g_swap_w && g_store_h==g_swap_h && pair_ready(fs.predictedDisplayTime)){
                        unsigned ew=g_eye_w?g_eye_w:g_store_w;int e;for(e=0;e<2;++e)ID3D11DeviceContext_CopySubresourceRegion(g_ctx,(ID3D11Resource *)g_images[idx].texture,0,e*ew,0,0,(ID3D11Resource *)g_eye_store[e],0,NULL);
                        memcpy(submitted,g_eye_packet,sizeof(submitted));have_layer=1;use_stereo=1;InterlockedIncrement(&g_stats.copies);
                    } else if(cfg.stereo && g_have_anchor && g_store_valid && g_store && g_ctx && (g_store_w*2==g_swap_w || g_store_w==g_swap_w) && g_store_h==g_swap_h){
                        /* Keep the packed swapchain; quad samples only its left
                           half. No stale right-eye image and no resize churn. */
                        ID3D11DeviceContext_CopySubresourceRegion(g_ctx,(ID3D11Resource *)g_images[idx].texture,0,0,0,0,(ID3D11Resource *)g_store,0,NULL);
                        theater_w=g_store_w;have_layer=1;InterlockedIncrement(&g_stats.copies);
                    } else if (!cfg.stereo && g_store && g_store_w == g_swap_w && g_store_h == g_swap_h && g_ctx) {
                        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_images[idx].texture, (ID3D11Resource *)g_store);
                        if(cfg.menu_open){
                            MGS4VR_MENU menu;int mw=g_swap_w*3/4,mh=g_swap_h*3/4;void *pixels;
                            D3D11_BOX box;
                            mgs4vr_menu_init(&menu,cfg.follow_head,(int)(cfg.dist_m*100+.5f));
                            menu.twohanded=cfg.twohanded;menu.smoothing_ms=cfg.smoothing_ms;menu.observer=cfg.observer;menu.open=1;menu.row=cfg.menu_row;menu.available=cfg.head_available;menu.stereo=cfg.menu_stereo;menu.stereo_available=cfg.stereo_available;
                            pixels=mgs4vr_menu_bitmap(&menu,mw,mh);
                            if(pixels){box.left=(g_swap_w-mw)/2;box.right=box.left+mw;box.top=(g_swap_h-mh)/2;box.bottom=box.top+mh;box.front=0;box.back=1;
                                ID3D11DeviceContext_UpdateSubresource(g_ctx,(ID3D11Resource *)g_images[idx].texture,0,&box,pixels,mw*4,0);}
                        }
                        have_layer = 1;
                        InterlockedIncrement(&g_stats.copies);
                    }
                    mgs4vr_gpu_xr_end();
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
        if(!use_stereo && last_projection && head_valid){
            float q[4],p[3];mgs4vr_xr_screen_pose(frame_hq,frame_hp,cfg.dist_m,cfg.height_offset_m,q,p);
            memcpy(&g_anchor.orientation,q,16);memcpy(&g_anchor.position,p,12);
            memcpy(g_stats.anchor_pos,p,12);memcpy(g_stats.anchor_quat,q,16);
        }
        last_projection=use_stereo;
        memset(&quad, 0, sizeof(quad)); quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.space = g_local; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swap;
        quad.subImage.imageRect.extent.width = (int32_t)mgs4vr_theater_width(cfg.stereo, g_swap_w, theater_w); g_stats.theater_w = quad.subImage.imageRect.extent.width; quad.subImage.imageRect.extent.height = (int32_t)g_swap_h;
        quad.pose = g_anchor;
        quad.size.width = cfg.width_m; quad.size.height = cfg.width_m * (float)g_swap_h / (float)quad.subImage.imageRect.extent.width;
        layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
        if(use_stereo){
            int e;memset(&projection,0,sizeof(projection));memset(pv,0,sizeof(pv));
            int headlocked=mgs4vr_packet_scoped(&submitted[0],&submitted[1]);
            projection.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION;projection.space=headlocked?g_view:g_local;projection.viewCount=2;projection.views=pv;
            for(e=0;e<2;++e){
                pv[e].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                memcpy(&pv[e].pose.orientation,submitted[e].quat,16);memcpy(&pv[e].pose.position,submitted[e].pos,12);
                memcpy(&pv[e].fov,submitted[e].fov,16);
                /* scope view (MGS3 HeadLocked): the magnified picture follows the head, no world reprojection */
                if(headlocked){pv[e].pose.orientation.x=pv[e].pose.orientation.y=pv[e].pose.orientation.z=0;pv[e].pose.orientation.w=1;
                    pv[e].pose.position.x=pv[e].pose.position.y=pv[e].pose.position.z=0;}
                pv[e].subImage.swapchain=g_swap;pv[e].subImage.imageRect.offset.x=e*(int)(g_swap_w/2);
                pv[e].subImage.imageRect.extent.width=g_swap_w/2;pv[e].subImage.imageRect.extent.height=g_swap_h;
            }
            layers[0]=(const XrCompositionLayerBaseHeader *)&projection;
        }
        if(use_stereo&&mgs4vr_packet_scoped(&submitted[0],&submitted[1]))InterlockedIncrement(&g_stats.headlocked_layers);
        if(use_stereo)InterlockedIncrement(&g_stats.stereo_layers);else InterlockedIncrement(&g_stats.theater_layers);
        if(use_stereo){int k;nwrist=wrist_layers(wrist);for(k=0;k<nwrist;++k)layers[1+k]=(const XrCompositionLayerBaseHeader *)&wrist[k];
            nlens=lens_layer(&lensq);if(nlens){MGS4VR_LENS_CFG lc;layers[1+nwrist]=(const XrCompositionLayerBaseHeader *)&lensq;
                mgs4vr_lens_config(&lc);if(lc.reticle&&!mgs4vr_lens_dot()&&reticle_layer(&lensq,&retq)){layers[2+nwrist]=(const XrCompositionLayerBaseHeader *)&retq;++nlens;}}}
        ei.layerCount = 1+nwrist+nlens; ei.layers = layers;
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
        if (!create_instance()) { wait_ms(10000); continue; }      /* no runtime active: look again later */
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

/* ----------------------------------------------------------------- api --- */

/* Eye dump (13 Oct 2026, colour/effects check of the two eyes): mgs4vr_xr_request_eye_dump(prefix) asks the next
   stereo present to write both eye images as <prefix>0.raw / <prefix>1.raw (int32 width, height, rowpitch, DXGI
   format, then rows). Game thread, under the capture lock; diagnostic only. */
static char g_dump_prefix[MAX_PATH]; static volatile LONG g_dump_req;
void mgs4vr_xr_request_eye_dump(const char *prefix) {
    if (!prefix || InterlockedCompareExchange(&g_dump_req, 0, 0)) return;
    strcpy_s(g_dump_prefix, sizeof(g_dump_prefix), prefix); InterlockedExchange(&g_dump_req, 1);
}
static ID3D11Texture2D *g_lens_tex_fwd(void);
static void dump_eyes(void) {
    int e;
    for (e = 0; e < 3; ++e) {
        D3D11_TEXTURE2D_DESC d; ID3D11Texture2D *st = NULL, *src = e < 2 ? g_eye_store[e] : g_lens_tex_fwd(); D3D11_MAPPED_SUBRESOURCE m; char path[MAX_PATH]; FILE *f;
        if (!src) continue;
        ID3D11Texture2D_GetDesc(src, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &st)) || !st) continue;
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            sprintf_s(path, sizeof(path), "%s%d.raw", g_dump_prefix, e);
            if (!fopen_s(&f, path, "wb")) { int hdr[4] = { (int)d.Width, (int)d.Height, (int)m.RowPitch, (int)d.Format }; unsigned y;
                fwrite(hdr, sizeof(hdr), 1, f); for (y = 0; y < d.Height; ++y) fwrite((BYTE *)m.pData + (size_t)y * m.RowPitch, 1, m.RowPitch, f); fclose(f); }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
}
/* Eye store e with the given width (recreated when the size changes between AFR and two-eye mode). */
static ID3D11Texture2D *eye_store(int e, unsigned w) {
    D3D11_TEXTURE2D_DESC ed;
    if (g_eye_store[e]) { ID3D11Texture2D_GetDesc(g_eye_store[e], &ed); if (ed.Width != w || ed.Height != g_store_h) { ID3D11Texture2D_Release(g_eye_store[e]); g_eye_store[e] = NULL; } }
    if (!g_eye_store[e] && g_store) { ID3D11Texture2D_GetDesc(g_store, &ed); ed.Width = w; ed.BindFlags |= D3D11_BIND_RENDER_TARGET;   /* lens scope draws into it */
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &ed, NULL, &g_eye_store[e]))) { ed.BindFlags &= ~D3D11_BIND_RENDER_TARGET; if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &ed, NULL, &g_eye_store[e]))) g_eye_store[e] = NULL; } }
    return g_eye_store[e];
}
static void present_impl(void *dxgi_swapchain, unsigned device_flags, const MGS4VR_EYE_PACKET *packet, const MGS4VR_EYE_PACKET *right);
void mgs4vr_xr_on_present_eye(void *sc, unsigned flags, const MGS4VR_EYE_PACKET *packet) { present_impl(sc, flags, packet, NULL); }
/* Two-eye mode (13 Oct 2026): the back buffer holds the left eye in its left half and the right eye in its right
   half (main view + second native view, same frame); each half becomes one eye image. */
void mgs4vr_xr_on_present_two(void *sc, unsigned flags, const MGS4VR_EYE_PACKET *left, const MGS4VR_EYE_PACKET *right) {
    if (!left || !right || !left->valid || !right->valid || !left->twoeye || !right->twoeye) { present_impl(sc, flags, NULL, NULL); return; }
    present_impl(sc, flags, left, right);
}
/* Close gate (16 Oct 2026, MGS3 recipe): the XR session/instance was never destroyed, so the simulator's Operator layer cleaned
   up in its DLL detach after process exit had removed its threads and the exit hung (1-thread mgs4 remnants of ~2 GB, also after
   a clean in-game exit). On the first WM_CLOSE/WM_DESTROY of the game window the XR thread is stopped: it ends the session and
   destroys session + instance (destroy_instance), waited for at most 3 s. */
static void xr_close_gate(const char *how) {
    DWORD w = WAIT_FAILED, t0 = GetTickCount(); int forced = 0; char path[MAX_PATH], *leaf; FILE *f;
    g_stop = 1;
    if (g_thread) w = WaitForSingleObject(g_thread, 3000);
    /* 16 Oct (sim, 2 of 6): a session whose XR thread hangs in xrEndFrame does not finish within 3 s and the remnant came back.
       Then tear the instance down from here (best effort; the process is closing anyway). */
    if (w != WAIT_OBJECT_0 && g_inst != XR_NULL_HANDLE) { __try { destroy_instance(); forced = 1; } __except (EXCEPTION_EXECUTE_HANDLER) { forced = -1; } }
    log_msg("close gate (%s): %s", how, w == WAIT_OBJECT_0 ? "XR session + instance torn down before exit" : forced == 1 ? "XR thread stuck - instance destroyed from the window thread" : "XR thread stuck - teardown failed");
    if (GetModuleFileNameA(NULL, path, MAX_PATH) && (leaf = strrchr(path, '\\')) != NULL) {
        sprintf_s(leaf + 1, MAX_PATH - (leaf + 1 - path), "mgs4vr_closegate_%lu.txt", GetCurrentProcessId());
        if (!fopen_s(&f, path, "w")) { fprintf(f, "closegate=1 how=%s thread=%s forced=%d ms=%lu\n", how, w == WAIT_OBJECT_0 ? "joined" : w == WAIT_TIMEOUT ? "timeout" : "none", forced, GetTickCount() - t0); fclose(f); }
    }
}static void present_impl(void *dxgi_swapchain, unsigned device_flags, const MGS4VR_EYE_PACKET *packet, const MGS4VR_EYE_PACKET *right) {
    IDXGISwapChain *sc = (IDXGISwapChain *)dxgi_swapchain;
    ID3D11Texture2D *back = NULL;
    D3D11_TEXTURE2D_DESC td;
    if (!g_started || g_stop || !sc) return;
    { DXGI_SWAP_CHAIN_DESC scd; static int gate_tried; if (!gate_tried && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &scd)) && scd.OutputWindow) { gate_tried = 1; log_msg("close gate %s", mgs4vr_closegate_install(scd.OutputWindow, xr_close_gate) ? "installed" : "not installed"); } }
    if (device_flags & D3D11_CREATE_DEVICE_SINGLETHREADED) {
        g_stats.device_singlethreaded = 1;
        if (InterlockedCompareExchange(&g_refused_logged, 1, 0) == 0)
            log_msg("REFUSED: the game device was created SINGLETHREADED (flags 0x%x); XR must be requested before the game creates its device", device_flags);
        return;
    }
    EnterCriticalSection(&g_cs);
    g_store_valid=0;
    if(g_cfg[g_cfg_idx&1].stereo && (!packet || !packet->valid))memset(g_eye_packet,0,sizeof(g_eye_packet));
    if (!g_dev) {
        ID3D11Device *dev = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&dev)) && dev) {
            g_dev = dev;                                            /* keeps the reference for the life of the module */
            ID3D11Device_GetImmediateContext(dev, &g_ctx);
        }
    }
    if (g_dev && SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back)) && back) {
        ID3D11Texture2D_GetDesc(back, &td);
        if (g_store && (g_store_w != td.Width || g_store_h != td.Height || g_store_format != td.Format)) {
            clear_eyes();ID3D11Texture2D_Release(g_store); g_store = NULL; InterlockedExchange(&g_store_valid, 0);
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
            lens_capture();
            if(g_cfg[g_cfg_idx&1].stereo && packet && packet->valid && right){
                unsigned hw=(g_store_w/2)&~1u;int e;
                g_eye_w=hw;
                for(e=0;e<2;++e){D3D11_BOX box;box.left=e?g_store_w-hw:0;box.right=box.left+hw;box.top=0;box.bottom=g_store_h;box.front=0;box.back=1;
                    if(eye_store(e,hw)){ID3D11DeviceContext_CopySubresourceRegion(g_ctx,(ID3D11Resource *)g_eye_store[e],0,0,0,0,(ID3D11Resource *)g_store,0,&box);
                        /* 15 Oct: lens on its own view (lens view=2) runs with two-eye; its square sits in the left half's corner */
                        if(!e)lens_blank(g_eye_store[e]);lens_inscope(g_eye_store[e],e?right:packet);
                        g_eye_packet[e]=e?*right:*packet;g_eye_tick[e]=GetTickCount64();}
                    else {memset(g_eye_packet,0,sizeof(g_eye_packet));InterlockedIncrement(&g_stats.capture_failures);break;}}
                InterlockedIncrement(&g_stats.twoeye_frames);
                if (InterlockedCompareExchange(&g_dump_req, 0, 0)) { dump_eyes(); InterlockedExchange(&g_dump_req, 0); }
            } else if(g_cfg[g_cfg_idx&1].stereo && packet && packet->valid && packet->eye>=0 && packet->eye<=1){
                int e=packet->eye;
                if(g_eye_w){g_eye_w=0;memset(g_eye_packet,0,sizeof(g_eye_packet));}   /* back from two-eye mode: full-size stores */
                if(g_eye_packet[1-e].valid && (g_eye_packet[1-e].epoch!=packet->epoch || (unsigned)(packet->camera_serial-g_eye_packet[1-e].camera_serial)!=1))memset(g_eye_packet,0,sizeof(g_eye_packet));
                if(eye_store(e,g_store_w)){ID3D11DeviceContext_CopyResource(g_ctx,(ID3D11Resource *)g_eye_store[e],(ID3D11Resource *)g_store);lens_blank(g_eye_store[e]);lens_inscope(g_eye_store[e],packet);g_eye_packet[e]=*packet;g_eye_tick[e]=GetTickCount64();if(e==1&&InterlockedCompareExchange(&g_dump_req,0,0)){dump_eyes();InterlockedExchange(&g_dump_req,0);}}
                else {memset(g_eye_packet,0,sizeof(g_eye_packet));InterlockedIncrement(&g_stats.capture_failures);}
            }
            InterlockedExchange(&g_store_valid, 1);
            InterlockedIncrement(&g_stats.captures);
        }
        ID3D11Texture2D_Release(back);
    } else if (g_dev) InterlockedIncrement(&g_stats.capture_failures);
    if(!g_store_valid)memset(g_eye_packet,0,sizeof(g_eye_packet));
    observer_present(sc);
    LeaveCriticalSection(&g_cs);
}
void mgs4vr_xr_on_present(void *sc,unsigned flags){mgs4vr_xr_on_present_eye(sc,flags,NULL);}

void mgs4vr_xr_configure(const MGS4VR_XR_CONFIG *cfg) {
    LONG next = (g_cfg_idx + 1) & 1;
    MGS4VR_XR_CONFIG c = *cfg;
    if (!(c.dist_m >= 0.5f && c.dist_m <= 10.0f)) c.dist_m = 2.5f;
    if(c.observer<0||c.observer>2)c.observer=2;
    if (!(c.width_m >= 0.5f && c.width_m <= 12.0f)) c.width_m = 3.2f;
    if (!(c.height_offset_m >= -2.0f && c.height_offset_m <= 2.0f)) c.height_offset_m = 0.0f;
    cs_init();EnterCriticalSection(&g_cs);memset(g_eye_packet,0,sizeof(g_eye_packet));observer_reset();LeaveCriticalSection(&g_cs);
    g_cfg[next] = c;
    InterlockedExchange(&g_cfg_idx, next);
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
    observer_free();
    clear_eyes();
    if (g_store) { ID3D11Texture2D_Release(g_store); g_store = NULL; }
    g_store_valid = 0; g_store_w = g_store_h = 0;
    if (g_ctx) { ID3D11DeviceContext_Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { ID3D11Device_Release(g_dev); g_dev = NULL; }
    LeaveCriticalSection(&g_cs);
    if (g_loader) { FreeLibrary(g_loader); g_loader = NULL; }
    g_gipa = NULL;
    mgs4vr_menu_free();
    g_started = 0;
}

void mgs4vr_xr_get_stats(MGS4VR_XR_STATS *out) { *out = g_stats; }
void mgs4vr_xr_set_pose_callback(void (*cb)(const float quat[4], const float pos[3])) { g_pose_cb = cb; }

void mgs4vr_xr_set_views_callback(void (*cb)(const MGS4VR_XR_VIEWS *views)) { g_views_cb = cb; }
void mgs4vr_xr_set_hands_callback(void (*cb)(const MGS4VR_XR_HANDS *hands)) { g_hands_cb = cb; }
int mgs4vr_xr_input_state(void) { return (int)g_input_state; }

