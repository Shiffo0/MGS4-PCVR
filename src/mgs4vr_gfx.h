/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_GFX_H
#define MGS4VR_GFX_H

typedef struct {
    long gpa_calls;            
    long gpa_wrapped;          
    long d3d11_devices;        
    long d3d12_devices;        
    long factories;            
    long swapchains;           
    long presents;             
    long resizes;              
    long iat_repairs;          
    long errors;
    int  backend;              
    unsigned device_flags_asked;   
    unsigned device_flags_used;    
    void *real_get_proc_address;
} MGS4VR_GFX_STATS;

int  mgs4vr_gfx_start(void (*log)(const char *fmt, ...), int stats_period_ms);
void mgs4vr_gfx_stop(void);

int  mgs4vr_gfx_verify(void);
void mgs4vr_gfx_get_stats(MGS4VR_GFX_STATS *out);
unsigned long mgs4vr_gfx_frame(void);

void mgs4vr_gfx_set_options(int strip_singlethreaded);

void mgs4vr_gfx_set_present_callback(void (*cb)(void *dxgi_swapchain, unsigned device_flags));      

#endif
