/* mgs4vr_xr.h - W05: OpenXR session + image transport for MGS4 (D3D11).
 *
 * Own OpenXR session/swapchain
 * implementation:
 *
 *   render thread (the game's Present)      XR thread (ours)
 *   ---------------------------------      --------------------------------
 *   mgs4vr_xr_on_present(swapchain)         instance / system / session
 *     adopt the game's D3D11 device         event pump, session begin / end
 *     copy back buffer -> capture store     xrWaitFrame / xrBeginFrame
 *                                           copy capture store -> XR swapchain
 *                                           xrEndFrame with ONE quad layer
 *
 * Stage W05a is the THEATER: the flat game picture on a screen in the room,
 * anchored in front of the head when the session (re)starts or on recentre.
 * Nothing here touches game memory. The game keeps running flat on the monitor
 * and is fully playable when no runtime or no headset is present; a headset
 * that is taken off and put back on must come back without a restart (the
 * STOPPING -> IDLE -> READY is a normal cycle).
 *
 * Thread safety: two threads drive one D3D11 immediate context, so the device
 * must NOT have been created with D3D11_CREATE_DEVICE_SINGLETHREADED (bgfx asks
 * for it; mgs4vr_gfx strips the flag when XR is requested) and the context is
 * switched to multithread-protected. If the flag is still present the module
 * refuses to submit anything. */
#ifndef MGS4VR_XR_H
#define MGS4VR_XR_H

typedef struct {
    int   enabled;          /* 0: keep the session but submit no layers */
    float dist_m;           /* theater distance in front of the head, metres */
    float width_m;          /* theater width, metres (height follows the picture's aspect) */
    float height_offset_m;  /* screen centre relative to eye height */
    long  recenter;         /* change the value to re-anchor the screen */
    int stereo; /* alternating-eye projection; theater while menu is open */
    int observer; /* 0 off, 1 right eye, 2 both eyes; desktop only */
    int menu_open,menu_row,follow_head,head_available,menu_stereo,stereo_available;
    int twohanded,smoothing_ms;
} MGS4VR_XR_CONFIG;

typedef struct {
    long state;                     /* last XrSessionState */
    long sessions, session_begins, session_ends, reinit;
    long frames, frames_with_layer, frames_without_layer;
    long stereo_layers, theater_layers;
    long captures, capture_failures, copies;
    long errors;
    long no_hmd_polls;
    long poses;                     /* head poses published to the callback */
    long headlocked_layers;         /* stereo layers submitted head-locked (scope view) */
    long wrist_layers;              /* wrist panel quads submitted */
    long twoeye_frames;
    long lens_captures, lens_layers, reticle_layers, lens_inscope;    /* lens scope: squares cut at Present, quad layers submitted, reticle quads */             /* presents captured as two eyes from one frame (mgs4vr_twoeye) */
    long hands, input_errors;       /* controller samples published / xrSyncActions failures */
    long device_singlethreaded;     /* 1 = refused: device was created SINGLETHREADED */
    unsigned width, height;         /* capture / swapchain size */
    long long format;               /* chosen swapchain format (DXGI_FORMAT) */
    float anchor_pos[3], anchor_quat[4];
    float last_head_pos[3];
    long theater_w;   /* sub-image width of the last theater quad (16 Oct: half in two-eye mode = half menus) */
} MGS4VR_XR_STATS;

/* loader_path: full path of openxr_loader.dll, or NULL for the default search.
   get_instance_proc_addr: NULL in production; a desk test passes the entry point
   of an in-process fake runtime here (then no DLL is loaded). */
int  mgs4vr_xr_start(void (*log)(const char *fmt, ...), const char *loader_path, void *get_instance_proc_addr);
void mgs4vr_xr_stop(void);
void mgs4vr_xr_configure(const MGS4VR_XR_CONFIG *cfg);
/* Call from the game's Present, BEFORE the real Present, with the IDXGISwapChain.
   device_flags: the flags the game device was created with (0 if unknown). */
void mgs4vr_xr_on_present(void *dxgi_swapchain, unsigned device_flags);
void mgs4vr_xr_get_stats(MGS4VR_XR_STATS *out);
/* Called on the XR thread once per frame with the head pose predicted for the
   display time, in LOCAL space (metres, quaternion x,y,z,w). Set before start,
   or NULL to stop. Runs whether or not the theater layer is being submitted, so
   head tracking keeps working with the picture switched off. */
void mgs4vr_xr_set_pose_callback(void (*cb)(const float quat[4], const float pos[3]));

/* W06 prerequisite: predicted stereo views in LOCAL space. Callback runs on
   the XR thread; copy synchronously, never retain the pointer. valid=0 revokes
   previous data (tracking/visibility/session loss). This is NOT a render-frame
   identity: the bgfx camera-to-image association must be established separately. */
typedef struct {
    int valid;
    unsigned long sequence;
    long long display_time;
    float quat[2][4], pos[2][3];
    float fov[2][4]; /* left, right, up, down; radians */
} MGS4VR_XR_VIEWS;
void mgs4vr_xr_set_views_callback(void (*cb)(const MGS4VR_XR_VIEWS *views));

/* Pure math, desk-testable: where the theater goes for a given head pose.
   OpenXR convention: right-handed, +y up, -z forward; quaternions are x,y,z,w.
   The screen faces the user, keeps only the head's YAW (a tilted head must not
   tilt the screen) and sits dist_m in front of the head at eye height + offset. */
void mgs4vr_xr_screen_pose(const float head_quat[4], const float head_pos[3], float dist_m, float height_offset_m,
                           float out_quat[4], float out_pos[3]);

/* Motion controllers, LOCAL space, predicted for display_time. Poses are
   ABSOLUTE per frame straight from the runtime (never integrated: drift
   lesson). flags are XrSpaceLocationFlags (bit0 orientation valid, bit1 position
   valid, bit2 orientation tracked, bit3 position tracked). active = the runtime
   has a bound, active pose source for that hand. buttons: bit0 stick click,
   bit1 primary (A/X), bit2 secondary (B/Y), bit3 menu. Callback runs on the XR
   thread; copy synchronously. Controllers failing to initialise never affect
   the session: then the callback is simply never called. */
typedef struct {
    int active;
    unsigned grip_flags, aim_flags;
    float grip_quat[4], grip_pos[3];
    float aim_quat[4], aim_pos[3];
    float trigger, squeeze, stick[2];
    unsigned buttons;
} MGS4VR_XR_HAND;
typedef struct {
    unsigned long sequence;
    long long display_time;
    int focused;                    /* xrSyncActions reported a focused session */
    int head_valid;
    float head_quat[4], head_pos[3];
    MGS4VR_XR_HAND hand[2];         /* 0 = left, 1 = right */
} MGS4VR_XR_HANDS;
void mgs4vr_xr_set_hands_callback(void (*cb)(const MGS4VR_XR_HANDS *hands));
/* Input bring-up status: 1 actions attached, 0 not (yet), -1 unavailable. */
int mgs4vr_xr_input_state(void);

#include "mgs4vr_packet.h"
void mgs4vr_xr_on_present_eye(void *swapchain,unsigned flags,const MGS4VR_EYE_PACKET *packet);
/* Two-eye mode: left half of the back buffer = left eye, right half = right eye (same frame). */
/* Diagnostic: the next stereo present writes both eye images to <prefix>0.raw and <prefix>1.raw. */
void mgs4vr_xr_request_eye_dump(const char *prefix);
void mgs4vr_xr_on_present_two(void *swapchain,unsigned flags,const MGS4VR_EYE_PACKET *left,const MGS4VR_EYE_PACKET *right);
#endif
