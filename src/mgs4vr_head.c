/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#include <math.h>
#include <string.h>
#include "mgs4vr_head.h"

#define RAD2DEG 57.295779513082320876798154814105

void mgs4vr_head_cfg_defaults(MGS4VR_HEAD_CFG *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->rotation = 1;
    cfg->position = 1;
    cfg->units_per_m = 1000.0f;
    cfg->yaw_sign = cfg->pitch_sign = cfg->roll_sign = 1.0f;
    cfg->max_offset_units = 1500.0f;      
}

int mgs4vr_head_owner_publishes(int head_on_before, int head_on_now, int marker_changed) {
    if (head_on_now) return 0;              
    if (head_on_before) return 1;           
    return marker_changed ? 1 : 0;
}

static void q_normalise(float q[4]) {
    double n = sqrt((double)q[0] * q[0] + (double)q[1] * q[1] + (double)q[2] * q[2] + (double)q[3] * q[3]);
    if (n > 1e-9) { q[0] = (float)(q[0] / n); q[1] = (float)(q[1] / n); q[2] = (float)(q[2] / n); q[3] = (float)(q[3] / n); }
    else { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
}

static void q_mul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void q_conj(const float a[4], float o[4]) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }

static void q_rotate(const float q[4], const float v[3], float o[3]) {
    float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    o[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    o[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    o[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

static void q_yaw_only(const float q[4], float o[4]) {
    const float fwd0[3] = { 0, 0, -1 }, right0[3] = { 1, 0, 0 };
    float f[3], r[3];
    double yaw;
    q_rotate(q, fwd0, f);
    if (f[0] * f[0] + f[2] * f[2] > 0.04f) yaw = atan2(-(double)f[0], -(double)f[2]);
    else { q_rotate(q, right0, r); yaw = atan2(-(double)r[2], (double)r[0]); }
    o[0] = 0.0f; o[1] = (float)sin(yaw * 0.5); o[2] = 0.0f; o[3] = (float)cos(yaw * 0.5);
}

static float clampf(float v, float lim) { if (v > lim) return lim; if (v < -lim) return -lim; return v; }

void mgs4vr_head_to_offset(const MGS4VR_HEAD_CFG *cfg,
                           const float ref_quat[4], const float ref_pos[3],
                           const float cur_quat[4], const float cur_pos[3],
                           MGS4VR_CAM_ADJUST *out) {
    float rq[4], cq[4], inv[4], rel[4], game[4];
    double m[9], yaw = 0.0, pitch = 0.0, roll = 0.0, scale;
    float lim;

    memcpy(rq, ref_quat, sizeof(rq)); q_normalise(rq);
    memcpy(cq, cur_quat, sizeof(cq)); q_normalise(cq);

    if (cfg->rotation) {
        
        q_conj(rq, inv);
        q_mul(inv, cq, rel);
        
        game[0] = rel[0]; game[1] = -rel[1]; game[2] = -rel[2]; game[3] = rel[3];
        {   
            double x = game[0], y = game[1], z = game[2], w = game[3];
            m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w);     m[2] = 2 * (x * z - y * w);
            m[3] = 2 * (x * y - z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + x * w);
            m[6] = 2 * (x * z + y * w);     m[7] = 2 * (y * z - x * w);     m[8] = 1 - 2 * (x * x + y * y);
        }
        
        if (m[7] > 1.0) m[7] = 1.0; else if (m[7] < -1.0) m[7] = -1.0;
        pitch = asin(-m[7]);
        if (fabs(m[7]) < 0.9999) {
            yaw = atan2(m[6], m[8]);
            roll = atan2(m[1], m[4]);
        } else {
            
            yaw = atan2(-m[3], m[0]);
            roll = 0.0;
        }
        out->yaw = (float)(yaw * RAD2DEG) * cfg->yaw_sign + cfg->trim_yaw;
        out->pitch = (float)(pitch * RAD2DEG) * cfg->pitch_sign + cfg->trim_pitch;
        out->roll = (float)(roll * RAD2DEG) * cfg->roll_sign + cfg->trim_roll;
    } else {
        out->yaw = cfg->trim_yaw; out->pitch = cfg->trim_pitch; out->roll = cfg->trim_roll;
    }

    if (cfg->position) {
        float head_ref[4], head_inv[4], d[3], local[3];
        q_yaw_only(rq, head_ref);            
        q_conj(head_ref, head_inv);
        d[0] = cur_pos[0] - ref_pos[0];
        d[1] = cur_pos[1] - ref_pos[1];
        d[2] = cur_pos[2] - ref_pos[2];
        q_rotate(head_inv, d, local);
        scale = cfg->units_per_m;
        lim = cfg->max_offset_units > 0.0f ? cfg->max_offset_units : 1e9f;
        out->x = clampf((float)(local[0] * scale), lim);
        out->y = clampf((float)(local[1] * scale), lim);
        out->z = clampf((float)(-local[2] * scale), lim);   
    } else {
        out->x = out->y = out->z = 0.0f;
    }
}
