/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_CAM_H
#define MGS4VR_CAM_H

#define MGS4VR_CAM_MAX_SRC  0x200
#define MGS4VR_CAM_MAX_CAM  0x1000

#define MGS4VR_CAM_OK_BUILDER 1
#define MGS4VR_CAM_OK_PROJ    2
#define MGS4VR_CAM_OK_RET0    4
#define MGS4VR_CAM_OK_RET1    8

typedef struct {
    void *builder;  const unsigned char *builder_sig; int builder_sig_len;
    void *proj;     const unsigned char *proj_sig;    int proj_sig_len;
    void *ret[2];                  
    int   require_call_check;      
    void *image_base;              
    int   sample_every;            
    int   cam_bytes, src_bytes;    
    const char *bin_path;          
    unsigned long (*frame_id)(void);
    void (*observe_built)(unsigned long long camera, unsigned long long caller);
    void (*observe_builder)(unsigned long long camera, unsigned long long caller);
    
    void *adjust_callers[2];       
    void *adjust_camera;
    
    void *skip_fn;
    int   stack_log;               
} MGS4VR_CAM_CONFIG;

typedef struct {
    int   enabled;
    float yaw, pitch, roll;
    float x, y, z;
    float sweep_deg, sweep_hz;         
    
    int   cine_auto;
    
    int   blur_off;
} MGS4VR_CAM_ADJUST;

typedef struct {
    long traps, builds, returns, projs, records, dropped, faults, watchdog;
    long adjusted, adjust_same, adjust_invalid, adjust_other_camera;
    long adjust_cinematic, adjust_not_stack, cine_marks;
    long adjust_torn;               
    long skip_calls, skip_applied;
    long threads_armed;
    long long bytes_written;
} MGS4VR_CAM_STATS;

#pragma pack(push, 1)
typedef struct {
    unsigned int  magic;           
    unsigned int  size;            
    long long     qpc;
    unsigned int  frame;
    unsigned int  tid;
    unsigned int  kind;            
    unsigned int  ret_rva;         
    unsigned long long camera, source;
    float scale, p4, p5, aspect;   
    unsigned int  src_len, cam_len;
    unsigned int  seq;             
    unsigned int  reserved;
} MGS4VR_CAM_REC;
#pragma pack(pop)

int  mgs4vr_cam_probe(const MGS4VR_CAM_CONFIG *cfg, void (*log)(const char *fmt, ...));

int  mgs4vr_cam_arm(const MGS4VR_CAM_CONFIG *cfg, void (*log)(const char *fmt, ...));

void mgs4vr_cam_poll(void);
void mgs4vr_cam_disarm(void);
void mgs4vr_cam_get_stats(MGS4VR_CAM_STATS *out);

void mgs4vr_cam_set_adjust(const MGS4VR_CAM_ADJUST *a);

#endif
