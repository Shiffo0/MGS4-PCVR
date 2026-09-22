/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_XR_H
#define MGS4VR_XR_H

typedef struct {
    int   enabled;          
    float dist_m;           
    float width_m;          
    float height_offset_m;  
    long  recenter;         
    int menu_open,menu_row,follow_head,head_available;
} MGS4VR_XR_CONFIG;

typedef struct {
    long state;                     
    long sessions, session_begins, session_ends, reinit;
    long frames, frames_with_layer, frames_without_layer;
    long stereo_layers, theater_layers;
    long captures, capture_failures, copies;
    long errors;
    long no_hmd_polls;
    long poses;                     
    long device_singlethreaded;     
    unsigned width, height;         
    long long format;               
    float anchor_pos[3], anchor_quat[4];
    float last_head_pos[3];
} MGS4VR_XR_STATS;

int  mgs4vr_xr_start(void (*log)(const char *fmt, ...), const char *loader_path, void *get_instance_proc_addr);
void mgs4vr_xr_stop(void);
void mgs4vr_xr_configure(const MGS4VR_XR_CONFIG *cfg);

void mgs4vr_xr_on_present(void *dxgi_swapchain, unsigned device_flags);
void mgs4vr_xr_get_stats(MGS4VR_XR_STATS *out);

void mgs4vr_xr_set_pose_callback(void (*cb)(const float quat[4], const float pos[3]));

typedef struct {
    int valid;
    unsigned long sequence;
    long long display_time;
    float quat[2][4], pos[2][3];
    float fov[2][4]; 
} MGS4VR_XR_VIEWS;
void mgs4vr_xr_set_views_callback(void (*cb)(const MGS4VR_XR_VIEWS *views));

void mgs4vr_xr_screen_pose(const float head_quat[4], const float head_pos[3], float dist_m, float height_offset_m,
                           float out_quat[4], float out_pos[3]);

#endif
