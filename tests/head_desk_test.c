/* head_desk_test.c - desk acceptance for mgs4vr_head: head pose -> camera offset.
 *
 * Oracle: NOT a copy of the production quaternion path. The expected answer is
 * derived with rotation MATRICES: the head's relative rotation is built in the
 * OpenXR frame from explicit axis rotations, carried into the game's view frame
 * by the basis change (x right, y up, z back) -> (x right, y down, z forward),
 * and the camera axes are then rotated by it directly. The offset under test is
 * turned back into camera axes with the documented composition
 * Rz(roll) * Rx(pitch) * Ry(yaw), built here from basis vectors rather than
 * copied from the implementation. The two paths must agree.
 * Exit 0 = pass. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/mgs4vr_head.h"

#define DEG (3.14159265358979323846 / 180.0)

/* ------------------------------------------------- oracle (matrix path) --- */

/* column-vector rotation matrices in the OpenXR frame */
static void rot_x(double a, double m[9]) { double c = cos(a), s = sin(a); m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = c; m[5] = -s; m[6] = 0; m[7] = s; m[8] = c; }
static void rot_y(double a, double m[9]) { double c = cos(a), s = sin(a); m[0] = c; m[1] = 0; m[2] = s; m[3] = 0; m[4] = 1; m[5] = 0; m[6] = -s; m[7] = 0; m[8] = c; }
static void rot_z(double a, double m[9]) { double c = cos(a), s = sin(a); m[0] = c; m[1] = -s; m[2] = 0; m[3] = s; m[4] = c; m[5] = 0; m[6] = 0; m[7] = 0; m[8] = 1; }
static void mat_mul(const double a[9], const double b[9], double o[9]) {
    int r, c, k; double t[9];
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { t[r * 3 + c] = 0; for (k = 0; k < 3; ++k) t[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c]; }
    memcpy(o, t, sizeof(t));
}
static void mat_to_quat(const double m[9], float q[4]) {     /* column-vector matrix -> (x,y,z,w) */
    double tr = m[0] + m[4] + m[8], s;
    if (tr > 0) { s = sqrt(tr + 1.0) * 2; q[3] = (float)(0.25 * s); q[0] = (float)((m[7] - m[5]) / s); q[1] = (float)((m[2] - m[6]) / s); q[2] = (float)((m[3] - m[1]) / s); }
    else if (m[0] > m[4] && m[0] > m[8]) { s = sqrt(1.0 + m[0] - m[4] - m[8]) * 2; q[3] = (float)((m[7] - m[5]) / s); q[0] = (float)(0.25 * s); q[1] = (float)((m[1] + m[3]) / s); q[2] = (float)((m[2] + m[6]) / s); }
    else if (m[4] > m[8]) { s = sqrt(1.0 + m[4] - m[0] - m[8]) * 2; q[3] = (float)((m[2] - m[6]) / s); q[0] = (float)((m[1] + m[3]) / s); q[1] = (float)(0.25 * s); q[2] = (float)((m[5] + m[7]) / s); }
    else { s = sqrt(1.0 + m[8] - m[0] - m[4]) * 2; q[3] = (float)((m[3] - m[1]) / s); q[0] = (float)((m[2] + m[6]) / s); q[1] = (float)((m[5] + m[7]) / s); q[2] = (float)(0.25 * s); }
}

/* The camera axes after the head rotation, derived independently of the module:
   basis change D = diag(1,-1,-1) (a 180 degree turn about x, determinant +1),
   game_rotation = D * xr_rotation * D. Rows of the result are the images of the
   camera's right / down / forward axes. */
static void oracle_axes(const double xr[9], double out[9]) {
    double d[9] = { 1, 0, 0, 0, -1, 0, 0, 0, -1 }, t[9];
    mat_mul(d, xr, t); mat_mul(t, d, t);
    /* rows = images of the axes = transpose of the column-vector matrix */
    out[0] = t[0]; out[1] = t[3]; out[2] = t[6];
    out[3] = t[1]; out[4] = t[4]; out[5] = t[7];
    out[6] = t[2]; out[7] = t[5]; out[8] = t[8];
}

/* The camera axes that the offset under test asks for, built from basis vectors
   with the documented composition, independently of the implementation. */
static void axes_from_offset(double yaw, double pitch, double roll, double out[9]) {
    /* game view frame: x right, y down, z forward. yaw about "up" = -y,
       pitch about +x, roll about +z, applied to the camera's own axes. */
    double ry[9], rx[9], rz[9], r[9];
    rot_y(yaw, ry);           /* forward tilts toward +x: look right */
    rot_x(pitch, rx);         /* forward tilts toward -y, and y is down: look up */
    rot_z(roll, rz);          /* right tilts toward +y = down: clockwise */
    mat_mul(ry, rx, r);       /* yaw outermost, then pitch, then roll */
    mat_mul(r, rz, r);
    /* rows = images of the axes */
    out[0] = r[0]; out[1] = r[3]; out[2] = r[6];
    out[3] = r[1]; out[4] = r[4]; out[5] = r[7];
    out[6] = r[2]; out[7] = r[5]; out[8] = r[8];
}

static double axes_err(const double a[9], const double b[9]) {
    int i; double e = 0;
    for (i = 0; i < 9; ++i) { double d = fabs(a[i] - b[i]); if (d > e) e = d; }
    return e;
}

/* ------------------------------------------------------------- helpers --- */

static const float IDENT[4] = { 0, 0, 0, 1 };
static const float ORIGIN[3] = { 0, 0, 0 };

static void head_quat(double yaw, double pitch, double roll, float q[4]) {   /* OpenXR frame, yaw about +y, pitch about +x, roll about +z */
    double m[9], t[9];
    rot_y(yaw, m); rot_x(pitch, t); mat_mul(m, t, m); rot_z(roll, t); mat_mul(m, t, m);
    mat_to_quat(m, q);
}
static void head_matrix(double yaw, double pitch, double roll, double m[9]) {
    double t[9];
    rot_y(yaw, m); rot_x(pitch, t); mat_mul(m, t, m); rot_z(roll, t); mat_mul(m, t, m);
}

#define CHECK(cond, name) do { if (!(cond)) { printf("FAIL: %s\n", name); return 1; } printf("ok: %s\n", name); } while (0)

int main(void) {
    MGS4VR_HEAD_CFG cfg;
    MGS4VR_CAM_ADJUST a;
    float q[4], p[3];
    double got[9], want[9], worst;
    int i;

    mgs4vr_head_cfg_defaults(&cfg);
    memset(&a, 0, sizeof(a));

    /* ---- the offset means nothing until the head moves ---- */
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, IDENT, ORIGIN, &a);
    CHECK(a.yaw == 0 && a.pitch == 0 && a.roll == 0 && a.x == 0 && a.y == 0 && a.z == 0, "head at the reference: offset is exactly zero");
    a.enabled = 1; a.cine_auto = 1; a.sweep_deg = 7.0f;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, IDENT, ORIGIN, &a);
    CHECK(a.enabled == 1 && a.cine_auto == 1 && a.sweep_deg == 7.0f, "the caller's own fields are left alone");
    a.sweep_deg = 0.0f;

    /* ---- the six physical directions, with the defaults ---- */
    head_quat(-30 * DEG, 0, 0, q);                       /* turn the head to the RIGHT */
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
    CHECK(fabs(a.yaw - 30.0f) < 1e-3 && fabs(a.pitch) < 1e-3 && fabs(a.roll) < 1e-3, "head 30 deg right -> camera looks 30 deg right, nothing else");
    head_quat(20 * DEG, 0, 0, q);
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
    CHECK(fabs(a.yaw + 20.0f) < 1e-3, "head 20 deg left -> camera looks 20 deg left");
    head_quat(0, 25 * DEG, 0, q);                        /* look UP */
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
    CHECK(fabs(a.pitch - 25.0f) < 1e-3 && fabs(a.yaw) < 1e-3 && fabs(a.roll) < 1e-3, "head 25 deg up -> camera looks 25 deg up, nothing else");
    head_quat(0, -15 * DEG, 0, q);
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
    CHECK(fabs(a.pitch + 15.0f) < 1e-3, "head 15 deg down -> camera looks 15 deg down");
    head_quat(0, 0, -10 * DEG, q);                       /* tilt the head to the RIGHT (right ear down) */
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
    CHECK(fabs(a.roll - 10.0f) < 1e-3 && fabs(a.yaw) < 1e-3 && fabs(a.pitch) < 1e-3, "head tilted 10 deg right -> horizon rolls 10 deg clockwise, nothing else");
    p[0] = 0.10f; p[1] = 0.05f; p[2] = -0.20f;           /* lean right, up and forward */
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, IDENT, p, &a);
    CHECK(fabs(a.x - 100.0f) < 1e-3 && fabs(a.y - 50.0f) < 1e-3 && fabs(a.z - 200.0f) < 1e-3, "lean 10 cm right / 5 cm up / 20 cm forward -> +100 / +50 / +200 units");

    /* ---- rotation against the matrix oracle, over many combinations ---- */
    worst = 0;
    for (i = 0; i < 1000; ++i) {
        double y = ((i * 37) % 300 - 150) * DEG, pt = ((i * 53) % 150 - 75) * DEG, rl = ((i * 71) % 120 - 60) * DEG;
        double xr[9];
        head_quat(y, pt, rl, q);
        head_matrix(y, pt, rl, xr);
        mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
        oracle_axes(xr, want);
        axes_from_offset(a.yaw * DEG, a.pitch * DEG, a.roll * DEG, got);
        { double e = axes_err(got, want); if (e > worst) worst = e; }
    }
    printf("  worst camera-axis error over 1000 head orientations: %.2e\n", worst);
    CHECK(worst < 1e-5, "every head orientation maps to camera axes that match the matrix oracle");

    /* ---- reference pose: the offset is relative, and zero right after a recentre ---- */
    head_quat(80 * DEG, 12 * DEG, -5 * DEG, q);
    p[0] = 1.3f; p[1] = 1.7f; p[2] = -0.4f;
    mgs4vr_head_to_offset(&cfg, q, p, q, p, &a);
    CHECK(fabs(a.yaw) < 1e-3 && fabs(a.pitch) < 1e-3 && fabs(a.roll) < 1e-3 && fabs(a.x) < 1e-3 && fabs(a.y) < 1e-3 && fabs(a.z) < 1e-3,
          "recentred at an arbitrary pose: the offset is exactly zero (a deliberate reset of the neutral, not image continuity)");
    {
        float q2[4], ref[4];
        double xr_ref[9], xr_cur[9], rel[9], inv[9];
        /* Turning the head 25 degrees right means 25 degrees in the head's OWN
           frame. (Changing the world yaw by 25 while the head is pitched and
           rolled is a different rotation, and would not be a pure yaw here.) */
        head_matrix(80 * DEG, 12 * DEG, -5 * DEG, xr_ref);
        mat_to_quat(xr_ref, ref);
        rot_y(-25 * DEG, rel);
        mat_mul(xr_ref, rel, xr_cur);
        mat_to_quat(xr_cur, q2);
        mgs4vr_head_to_offset(&cfg, ref, p, q2, p, &a);
        CHECK(fabs(a.yaw - 25.0f) < 1e-2 && fabs(a.pitch) < 1e-2 && fabs(a.roll) < 1e-2,
              "25 deg right from a pitched and rolled reference is a pure 25 deg right");
        head_matrix(80 * DEG, 12 * DEG, -5 * DEG, xr_ref);
        head_matrix(-40 * DEG, 30 * DEG, 20 * DEG, xr_cur);
        { int r, c; for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) inv[r * 3 + c] = xr_ref[c * 3 + r]; }
        mat_mul(inv, xr_cur, rel);
        mat_to_quat(xr_cur, q2);
        mgs4vr_head_to_offset(&cfg, ref, p, q2, p, &a);
        oracle_axes(rel, want);
        axes_from_offset(a.yaw * DEG, a.pitch * DEG, a.roll * DEG, got);
        CHECK(axes_err(got, want) < 1e-4, "an arbitrary reference and an arbitrary head pose agree with the oracle");
    }

    /* ---- position uses the heading of the reference, not its tilt ---- */
    {
        float ref[4], pos[3];
        head_quat(90 * DEG, 0, 0, ref);                   /* recentred facing 90 deg left */
        pos[0] = -0.20f; pos[1] = 0.0f; pos[2] = 0.0f;    /* a step along world -x */
        mgs4vr_head_to_offset(&cfg, ref, ORIGIN, ref, pos, &a);
        CHECK(fabs(a.z - 200.0f) < 1e-2 && fabs(a.x) < 1e-2, "facing 90 deg left, a step along world -x is FORWARD for the camera");
        head_quat(90 * DEG, 40 * DEG, 30 * DEG, ref);     /* same heading, head tilted and looking up */
        mgs4vr_head_to_offset(&cfg, ref, ORIGIN, ref, pos, &a);
        CHECK(fabs(a.z - 200.0f) < 1e-2 && fabs(a.x) < 1e-2 && fabs(a.y) < 1e-2, "the same step with a tilted reference head is still exactly forward");
    }

    /* ---- switches ---- */
    head_quat(-30 * DEG, 20 * DEG, -10 * DEG, q);
    p[0] = 0.1f; p[1] = 0.05f; p[2] = -0.2f;
    cfg.position = 0;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, p, &a);
    CHECK(a.x == 0 && a.y == 0 && a.z == 0 && fabs(a.yaw - 30.0f) < 1e-3, "position off: rotation still works, translation is zero");
    cfg.position = 1; cfg.rotation = 0;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, p, &a);
    CHECK(a.yaw == 0 && a.pitch == 0 && a.roll == 0 && fabs(a.x - 100.0f) < 1e-3, "rotation off: translation still works, rotation is zero");
    cfg.rotation = 1;
    cfg.yaw_sign = -1.0f; cfg.pitch_sign = -1.0f; cfg.roll_sign = -1.0f;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, p, &a);
    CHECK(fabs(a.yaw + 30.0f) < 1e-3 && fabs(a.pitch + 20.0f) < 1e-3 && fabs(a.roll + 10.0f) < 1e-3, "the three sign switches flip exactly their own axis");
    cfg.yaw_sign = cfg.pitch_sign = cfg.roll_sign = 1.0f;
    cfg.trim_yaw = 5.0f; cfg.trim_pitch = -2.0f; cfg.trim_roll = 1.0f;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, p, &a);
    CHECK(fabs(a.yaw - 35.0f) < 1e-3 && fabs(a.pitch - 18.0f) < 1e-3 && fabs(a.roll - 11.0f) < 1e-3, "trim is added after the conversion");
    cfg.trim_yaw = cfg.trim_pitch = cfg.trim_roll = 0.0f;
    cfg.units_per_m = 100.0f;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, IDENT, p, &a);
    CHECK(fabs(a.x - 10.0f) < 1e-3 && fabs(a.z - 20.0f) < 1e-3, "a different world scale scales the translation");
    cfg.units_per_m = 1000.0f;
    p[0] = 9.0f; p[1] = -9.0f; p[2] = -9.0f;
    mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, IDENT, p, &a);
    CHECK(fabs(a.x - cfg.max_offset_units) < 1e-3 && fabs(a.y + cfg.max_offset_units) < 1e-3 && fabs(a.z - cfg.max_offset_units) < 1e-3,
          "a tracking glitch 9 m away is clamped to the limit on every axis");

    /* ---- degenerate input ---- */
    {
        float zero[4] = { 0, 0, 0, 0 };
        mgs4vr_head_to_offset(&cfg, zero, ORIGIN, zero, ORIGIN, &a);
        CHECK(fabs(a.yaw) < 1e-3 && fabs(a.pitch) < 1e-3 && fabs(a.roll) < 1e-3, "a zero quaternion is treated as identity, not as garbage");
        head_quat(0, 89.99 * DEG, 0, q);
        mgs4vr_head_to_offset(&cfg, IDENT, ORIGIN, q, ORIGIN, &a);
        CHECK(fabs(a.pitch - 89.99f) < 0.05f && fabs(a.roll) < 1e-3 && a.yaw == a.yaw, "looking straight up: pitch is right, the horizon stays level, no NaN");
        {   /* A runtime sends unit quaternions, but arithmetic drifts; the same
               rotation at a different length must give the same answer. */
            float scaled[4], ref2[4];
            MGS4VR_CAM_ADJUST b;
            int k;
            memset(&b, 0, sizeof(b));
            head_quat(-33 * DEG, 17 * DEG, -8 * DEG, q);
            head_quat(50 * DEG, -10 * DEG, 4 * DEG, ref2);
            mgs4vr_head_to_offset(&cfg, ref2, ORIGIN, q, ORIGIN, &a);
            for (k = 0; k < 4; ++k) scaled[k] = q[k] * 3.0f;
            mgs4vr_head_to_offset(&cfg, ref2, ORIGIN, scaled, ORIGIN, &b);
            CHECK(fabs(a.yaw - b.yaw) < 1e-3 && fabs(a.pitch - b.pitch) < 1e-3 && fabs(a.roll - b.roll) < 1e-3,
                  "a quaternion that is not unit length gives the same rotation");
            for (k = 0; k < 4; ++k) scaled[k] = ref2[k] * 0.25f;
            mgs4vr_head_to_offset(&cfg, scaled, ORIGIN, q, ORIGIN, &b);
            CHECK(fabs(a.yaw - b.yaw) < 1e-3 && fabs(a.pitch - b.pitch) < 1e-3 && fabs(a.roll - b.roll) < 1e-3,
                  "the same holds for the reference quaternion");
        }
    }
    /* ---- who publishes the offset (the 3A98EDAD defect) ---- */
    CHECK(mgs4vr_head_owner_publishes(0, 1, 1) == 0 && mgs4vr_head_owner_publishes(1, 1, 1) == 0,
          "while head tracking is on, the owner never publishes over the pose callback");
    CHECK(mgs4vr_head_owner_publishes(1, 0, 0) == 1,
          "turning head tracking OFF publishes the recovery state even when nothing else changed");
    CHECK(mgs4vr_head_owner_publishes(1, 0, 1) == 1 && mgs4vr_head_owner_publishes(0, 0, 1) == 1,
          "with head tracking off, a marker change is published as before");
    CHECK(mgs4vr_head_owner_publishes(0, 0, 0) == 0, "nothing changed and nothing is driving: no publication");

    printf("PASS: head_desk_test\n");
    return 0;
}
