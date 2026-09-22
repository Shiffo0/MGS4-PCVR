/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef MGS4VR_HEAD_H
#define MGS4VR_HEAD_H

#include "mgs4vr_cam.h"

typedef struct {
    int   rotation;        
    int   position;        
    float units_per_m;     
    float yaw_sign, pitch_sign, roll_sign;   
    float max_offset_units;                  
    float trim_yaw, trim_pitch, trim_roll;   
} MGS4VR_HEAD_CFG;

void mgs4vr_head_cfg_defaults(MGS4VR_HEAD_CFG *cfg);

int mgs4vr_head_owner_publishes(int head_on_before, int head_on_now, int marker_changed);

void mgs4vr_head_to_offset(const MGS4VR_HEAD_CFG *cfg,
                           const float ref_quat[4], const float ref_pos[3],
                           const float cur_quat[4], const float cur_pos[3],
                           MGS4VR_CAM_ADJUST *out);

#endif
