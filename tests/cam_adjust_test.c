/* cam_adjust_test.c - desk acceptance for the controlled camera offset (W04),
 * the cutscene context policy and the neutralisable function (motion blur).
 *
 * The test exe plays MGS4's end-of-frame camera update as measured in runs 03
 * and 04: a fresh camera-to-world matrix on the STACK, handed to the builder
 * from a WRAPPER call site (0x0b9ba0 in the game) and then from the OWNER call
 * site (0x0ba3a3). In cutscene frames the wrapper site additionally builds the
 * main camera from another stack matrix just before that pair and from the
 * cinematic camera object on the heap just after it.
 *
 * Oracle: NOT a copy of the production matrix product. Expected axes are
 * derived geometrically in double precision from the definition of the pose
 * (look right = forward turns toward right, look up = forward turns toward
 * -down, roll clockwise = right dips toward down; translation along the
 * camera's own right / up / forward).
 * Exit 0 = pass. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../src/mgs4vr_cam.h"
#include "../src/mgs4vr_eye.h"

typedef struct { float m[16]; float pad[48]; } CAMOBJ;

static void *g_ret[2];
static int g_site;
static float seen_eye_args[4];
static CAMOBJ g_main, g_other;
static volatile unsigned long g_frame;
static float g_after_wrapper[16], g_after_owner[16];
static float g_static_matrix[16];
static float *g_heap_cine;

__declspec(noinline) static void fake_build(CAMOBJ *camera, const float *source, float scale, float p4, float p5, float aspect) {
    g_ret[g_site] = _ReturnAddress();
    if (source != camera->m) memcpy(camera->m, source, 64);
    camera->pad[0] = scale + p4 + p5 + aspect;
    seen_eye_args[0]=scale;seen_eye_args[1]=p4;seen_eye_args[2]=p5;seen_eye_args[3]=aspect;
}

__declspec(noinline) static void fake_build_inplace(CAMOBJ *camera) {
    static void (*volatile p_build)(CAMOBJ *, const float *, float, float, float, float) = fake_build;
    int keep = g_site;
    void *r0 = g_ret[0], *r1 = g_ret[1];
    p_build(camera, camera->m, 1.8f, 0, 0, 1.0f);             /* indirect call: not one of the verified sites */
    g_ret[0] = r0; g_ret[1] = r1; g_site = keep;
}

/* The game's wrapper: ONE call site, used for the seam build and for the cutscene builds. */
__declspec(noinline) static void wrapper_build(CAMOBJ *camera, const float *source) {
    g_site = 0;
    fake_build(camera, source, 1.8f, 0, 0, 1.0f);
}

static void make_camera(float *m, double yaw, double pitch, double px, double py, double pz) {
    /* world Y up; view x right, y down, z forward - the measured MGS4 convention */
    double f[3] = { sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch) };
    double r[3] = { cos(yaw), 0.0, -sin(yaw) };
    double d[3];
    int i;
    /* right x down = forward  =>  down = forward x right */
    d[0] = f[1] * r[2] - f[2] * r[1]; d[1] = f[2] * r[0] - f[0] * r[2]; d[2] = f[0] * r[1] - f[1] * r[0];
    for (i = 0; i < 3; ++i) { m[i] = (float)r[i]; m[4 + i] = (float)d[i]; m[8 + i] = (float)f[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = (float)px; m[13] = (float)py; m[14] = (float)pz; m[15] = 1;
}

/* Fills go through a volatile function pointer: the optimiser cannot know that
   an exception handler rewrote the matrix in between and would otherwise drop
   a second, "redundant" fill. */
static void (*volatile p_make_camera)(float *, double, double, double, double, double) = make_camera;

#define F_REFILL   1   /* MGS4: the matrix is refilled before each of the two builds */
#define F_CORRUPT  2   /* source is not a camera matrix */
#define F_CINE     4   /* cutscene frame: extra wrapper builds before and after the seam pair */
#define F_RESET   16
#define F_STATIC   8   /* the seam source is NOT on the stack (persistent memory) */

/* The four in-place rebuilds MGS4 does at the start of every frame (src == cam, other call sites). */
__declspec(noinline) static void rebuilds(CAMOBJ *cam) {
    int i;
    for (i = 0; i < 4; ++i) { g_site = 1; fake_build_inplace(cam); }
}

/* One game frame. Always the SAME two call sites - the hook only touches builds from verified return addresses. */
__declspec(noinline) static void game_frame(CAMOBJ *cam, double yaw, double pitch, double px, float *original, int flags) {
    float stack_matrix[16], stack_cine[16];
    float *seam = (flags & F_STATIC) ? g_static_matrix : stack_matrix;
    rebuilds(cam);
    p_make_camera(seam, yaw, pitch, px, 1700.0, 30000.0);
    if(flags & F_RESET){static const float reset[16]={-1,0,0,0,0,-1,0,0,0,0,1,0,0,0,0,1};memcpy(seam,reset,64);}
    if (flags & F_CORRUPT) seam[0] = 5.0f;
    memcpy(original, seam, 64);
    if (flags & F_CINE) {
        p_make_camera(stack_cine, yaw, pitch, px, 1700.0, 30000.0);
        wrapper_build(cam, stack_cine);
        if (memcmp(stack_cine, original, 64) != 0) { printf("FAIL: the cutscene's own stack matrix was modified\n"); exit(1); }
    }
    wrapper_build(cam, seam);
    memcpy(g_after_wrapper, cam->m, 64);
    if (flags & F_REFILL) p_make_camera(seam, yaw, pitch, px, 1700.0, 30000.0);
    g_site = 1; fake_build(cam, seam, 1.8f, 0, 0, 1.0f);
    memcpy(g_after_owner, cam->m, 64);
    if (flags & F_CINE) {
        p_make_camera(g_heap_cine, yaw, pitch, px, 1700.0, 30000.0);
        wrapper_build(cam, g_heap_cine);
        if (memcmp(g_heap_cine, original, 64) != 0) { printf("FAIL: the persistent cinematic camera object was modified\n"); exit(1); }
    }
    g_frame++;
}

/* Two threads publish offsets while the game keeps building cameras, which is
   what the XR pose callback (~120 Hz) and the owner's worker thread really do.
   A reader that copies the struct without protection can see half of each. */
static MGS4VR_CAM_ADJUST g_pub[2];
static volatile LONG g_pub_stop;
static DWORD WINAPI publisher(LPVOID p) {
    int which = (int)(INT_PTR)p;
    while (!g_pub_stop) { mgs4vr_cam_set_adjust(&g_pub[which]); }
    return 0;
}

static volatile long long g_blur_value = 7;
__declspec(noinline) static long long fake_blur_query(void) { return g_blur_value; }
static long long (*volatile p_blur)(void) = fake_blur_query;

static unsigned long frame_id(void) { return g_frame; }
static void test_log(const char *fmt, ...) { char t[900]; va_list a; va_start(a, fmt); _vsnprintf_s(t, sizeof(t), _TRUNCATE, fmt, a); va_end(a); printf("  log| %s\n", t); }

static MGS4VR_CAM_CONFIG g_cfg;
static int g_arm_result;
static DWORD WINAPI arm_thread(LPVOID p) { (void)p; g_arm_result = mgs4vr_cam_arm(&g_cfg, test_log); return 0; }
static DWORD WINAPI disarm_thread(LPVOID p) { (void)p; mgs4vr_cam_disarm(); return 0; }
static void run_thread(LPTHREAD_START_ROUTINE fn) { HANDLE h = CreateThread(NULL, 0, fn, NULL, 0, NULL); WaitForSingleObject(h, 20000); CloseHandle(h); }

static double err3(const float *got, const double *want) {
    double e = 0; int i;
    for (i = 0; i < 3; ++i) { double d = fabs((double)got[i] - want[i]); if (d > e) e = d; }
    return e;
}

/* geometric expectation from the ORIGINAL matrix */
static double check_pose(const float *o, const float *got, double yaw, double pitch, double roll, double x, double y, double z) {
    const double k = 3.14159265358979323846 / 180.0;
    double R[3], D[3], F[3], r1[3], d1[3], f1[3], r2[3], d2[3], f2[3], r3[3], d3[3], f3[3], P[3], e, worst = 0;
    int i;
    for (i = 0; i < 3; ++i) { R[i] = o[i]; D[i] = o[4 + i]; F[i] = o[8 + i]; }
    /* yaw is outermost (about the game camera's own vertical), then pitch, then roll innermost */
    for (i = 0; i < 3; ++i) { f1[i] = cos(yaw * k) * F[i] + sin(yaw * k) * R[i]; r1[i] = cos(yaw * k) * R[i] - sin(yaw * k) * F[i]; d1[i] = D[i]; }
    for (i = 0; i < 3; ++i) { f2[i] = cos(pitch * k) * f1[i] - sin(pitch * k) * d1[i]; d2[i] = cos(pitch * k) * d1[i] + sin(pitch * k) * f1[i]; r2[i] = r1[i]; }
    for (i = 0; i < 3; ++i) { r3[i] = cos(roll * k) * r2[i] + sin(roll * k) * d2[i]; d3[i] = cos(roll * k) * d2[i] - sin(roll * k) * r2[i]; f3[i] = f2[i]; }
    for (i = 0; i < 3; ++i) P[i] = o[12 + i] + x * R[i] - y * D[i] + z * F[i];
    e = err3(got, r3); if (e > worst) worst = e;
    e = err3(got + 4, d3); if (e > worst) worst = e;
    e = err3(got + 8, f3); if (e > worst) worst = e;
    e = err3(got + 12, P) / 1000.0; if (e > worst) worst = e;      /* position tolerance scaled: 1e-4 -> 0.1 unit */
    return worst;
}

static int same_values(const float *a, const float *b) {      /* numeric: -0.0 == +0.0 */
    int k; for (k = 0; k < 16; ++k) if (a[k] != b[k]) return 0;
    return 1;
}

#define CHECK(cond, name) do { if (!(cond)) { printf("FAIL: %s\n", name); return 1; } printf("ok: %s\n", name); } while (0)

static int eye_applied_count,eye_rejected_count,eye_fail;
static int test_prepare_eye(unsigned long long camera,int cine,MGS4VR_EYE_TICKET *t){
 (void)camera;(void)cine;memset(t,0,sizeof(*t));if(eye_fail==2)return 0;if(eye_fail)return -1;
 t->packet.valid=1;t->packet.pose_sequence=123;t->adjust.enabled=1;t->adjust.x=32;
 t->args[0]=2;t->args[1]=.1f;t->args[2]=.2f;t->args[3]=1.3f;return 1;
}
static void test_finish_eye(unsigned long long camera,unsigned long long caller,const MGS4VR_EYE_TICKET *t,int applied){
 (void)camera;if(caller!=(unsigned long long)g_ret[1])return;
 if(applied && t->packet.pose_sequence==123)++eye_applied_count;else ++eye_rejected_count;
}
int main(void) {
    float orig[16];
    unsigned char sig[6];
    MGS4VR_CAM_ADJUST a;
    MGS4VR_CAM_STATS s, s0;
    double worst;
    int i, bad;

    g_heap_cine = (float *)malloc(64 * sizeof(float));
    game_frame(&g_main, 0.3, 0.1, -14000.0, orig, 0);              /* learn both return sites, unobserved */
    CHECK(g_ret[0] && g_ret[1] && g_ret[0] != g_ret[1], "two distinct builder call sites learned (wrapper, owner)");
    CHECK(memcmp(g_main.m, orig, 64) == 0, "baseline: camera equals the game's matrix");
    memcpy(sig, (void *)fake_build, sizeof(sig));
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.image_base = GetModuleHandleA(NULL);
    g_cfg.builder = (void *)fake_build; g_cfg.builder_sig = sig; g_cfg.builder_sig_len = 6;
    g_cfg.require_call_check = 1;
    g_cfg.sample_every = 1000000; g_cfg.cam_bytes = 64; g_cfg.src_bytes = 64;
    g_cfg.frame_id = frame_id;
    g_cfg.adjust_callers[0] = g_ret[0]; g_cfg.adjust_callers[1] = g_ret[1];
    g_cfg.adjust_camera = &g_main;
    g_cfg.skip_fn = (void *)fake_blur_query;
    g_cfg.stack_log = 1;
    run_thread(arm_thread);
    CHECK(g_arm_result >= 1, "armed");

    game_frame(&g_main, 0.3, 0.1, -14000.0, orig, 0);
    CHECK(memcmp(g_main.m, orig, 64) == 0, "armed but offset off: camera untouched");

    memset(&a, 0, sizeof(a)); a.enabled = 1; a.yaw = 10.0f; a.x = 100.0f; a.y = 50.0f; a.z = -200.0f;
    mgs4vr_cam_set_adjust(&a);
    mgs4vr_cam_get_stats(&s0);
    worst = 0;
    for (i = 0; i < 200; ++i) {
        double e;
        game_frame(&g_main, 0.3 + i * 0.05, 0.4 * sin(i * 0.1), -14000.0 + i * 37.0, orig, 0);
        e = check_pose(orig, g_main.m, 10, 0, 0, 100, 50, -200);
        if (e > worst) worst = e;
    }
    printf("  yaw+translation worst error %.2e\n", worst);
    CHECK(worst < 1e-4, "yaw 10 + translation matches the geometric oracle over 200 moving frames");
    mgs4vr_cam_get_stats(&s);
    CHECK(s.adjusted - s0.adjusted == 200 && s.adjust_same - s0.adjust_same == 200, "un-refilled matrix: applied once, the second build of the frame skipped");

    for (i = 0; i < 50; ++i) game_frame(&g_main, 1.0, 0.0, -14000.0, orig, 0);      /* perfectly static game camera */
    mgs4vr_cam_get_stats(&s);
    CHECK(s.adjusted - s0.adjusted == 250 && s.adjust_same - s0.adjust_same == 250, "static camera: still once per frame, never twice, never zero");
    CHECK(check_pose(orig, g_main.m, 10, 0, 0, 100, 50, -200) < 1e-4, "static camera: no accumulation (no drift)");

    memset(&a, 0, sizeof(a)); a.enabled = 1; a.pitch = 20.0f; a.roll = -15.0f; a.yaw = -35.0f;
    mgs4vr_cam_set_adjust(&a);
    game_frame(&g_main, 2.2, -0.3, 5000.0, orig, 0);
    worst = check_pose(orig, g_main.m, -35, 20, -15, 0, 0, 0);
    printf("  yaw/pitch/roll worst error %.2e\n", worst);
    CHECK(worst < 1e-4, "combined yaw -35, pitch 20, roll -15 matches the oracle");
    {
        const float *m = g_main.m; double dot = 0, len = 0; int k;
        for (k = 0; k < 3; ++k) { dot += m[k] * m[4 + k]; len += m[8 + k] * m[8 + k]; }
        CHECK(fabs(dot) < 1e-5 && fabs(len - 1.0) < 1e-5, "result stays orthonormal");
    }

    /* Live stall of run 04: zero offset on a STATIC camera, then a change.
       pitch 0.2: no -0.0 entries, so a zero offset writes back the SAME BYTES. */
    memset(&a, 0, sizeof(a)); a.enabled = 1;
    mgs4vr_cam_set_adjust(&a);
    for (i = 0; i < 20; ++i) game_frame(&g_main, 1.0, 0.2, -14000.0, orig, 0);
    CHECK(memcmp(g_main.m, orig, 64) == 0, "precondition: zero offset reproduces the game's matrix bit for bit");
    a.yaw = 15.0f; mgs4vr_cam_set_adjust(&a);
    game_frame(&g_main, 1.0, 0.2, -14000.0, orig, 0);
    CHECK(check_pose(orig, g_main.m, 15, 0, 0, 0, 0, 0) < 1e-4, "offset change after a zero offset takes effect on a camera that never moved");
    bad = 0;
    for (i = 0; i < 100; ++i) {
        game_frame(&g_main, 1.0 + (i < 50 ? 0.0 : i * 0.01), 0.0, -14000.0, orig, F_REFILL);
        if (check_pose(orig, g_after_wrapper, 15, 0, 0, 0, 0, 0) > 1e-4) bad++;
        if (check_pose(orig, g_main.m, 15, 0, 0, 0, 0, 0) > 1e-4) bad++;
    }
    CHECK(bad == 0, "MGS4 pattern (matrix refilled before each build): both builds get the offset exactly once, static and moving");

    /* ---- context policy: cutscene frames ---- */
    a.cine_auto = 1; mgs4vr_cam_set_adjust(&a);
    mgs4vr_cam_get_stats(&s0);
    bad = 0;
    for (i = 0; i < 50; ++i) {
        game_frame(&g_main, 0.7 + i * 0.02, 0.1, 9000.0, orig, F_REFILL | F_CINE);
        if (!same_values(g_after_wrapper, orig) || !same_values(g_after_owner, orig) || !same_values(g_main.m, orig)) bad++;
    }
    mgs4vr_cam_get_stats(&s);
    CHECK(bad == 0, "cutscene frames with cam_cine on: every build keeps the authored camera");
    CHECK(s.adjusted == s0.adjusted && s.adjust_cinematic - s0.adjust_cinematic == 100, "offset withheld for both seam builds of all 50 cutscene frames");
    game_frame(&g_main, 0.7, 0.1, 9000.0, orig, F_REFILL);
    CHECK(check_pose(orig, g_main.m, 15, 0, 0, 0, 0, 0) < 1e-4 && check_pose(orig, g_after_wrapper, 15, 0, 0, 0, 0, 0) < 1e-4,
          "first gameplay frame after the cutscene has the offset again, in both builds");
    game_frame(&g_main, 0.7, 0.1, 9000.0, orig, F_REFILL | F_CINE);
    CHECK(same_values(g_after_owner, orig), "first cutscene frame after gameplay is already left alone");
    a.cine_auto = 0; mgs4vr_cam_set_adjust(&a);
    game_frame(&g_main, 0.7, 0.1, 9000.0, orig, F_REFILL | F_CINE);
    CHECK(check_pose(orig, g_after_owner, 15, 0, 0, 0, 0, 0) < 1e-4, "cam_cine off: the seam build of a cutscene frame gets the offset (run 04 behaviour)");
    CHECK(same_values(g_main.m, orig), "cam_cine off: the cutscene's own sources are still never written (checked inside the frame too)");

    /* ---- only temporaries ---- */
    mgs4vr_cam_get_stats(&s0);
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, F_STATIC);
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, F_STATIC);
    mgs4vr_cam_get_stats(&s);
    CHECK(same_values(g_main.m, orig) && s.adjust_not_stack - s0.adjust_not_stack >= 2, "a seam source in persistent memory (not on the stack) is never written");
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, 0);                /* back to the stack: relearn */
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, 0);
    CHECK(check_pose(orig, g_main.m, 15, 0, 0, 0, 0, 0) < 1e-4, "back on the stack: offset applies again");

    mgs4vr_cam_get_stats(&s0);
    game_frame(&g_other, 0.5, 0.0, 100.0, orig, 0);
    CHECK(memcmp(g_other.m, orig, 64) == 0, "a different camera object is never touched");
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, F_CORRUPT);
    CHECK(memcmp(g_main.m, orig, 64) == 0, "a source that is not a camera matrix is never touched");
    mgs4vr_cam_get_stats(&s);
    CHECK(s.adjust_other_camera - s0.adjust_other_camera == 2 && s.adjust_invalid - s0.adjust_invalid == 2 && s.faults == 0, "refusals counted, no faults");

    a.enabled = 0; mgs4vr_cam_set_adjust(&a);
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, 0);
    CHECK(memcmp(g_main.m, orig, 64) == 0, "offset off: the very next frame is the game's own camera");

    /* ---- neutralisable function (motion blur query) ---- */
    CHECK(p_blur() == 7, "blur_off = 0: the function runs untouched and returns its own value");
    a.blur_off = 1; mgs4vr_cam_set_adjust(&a);
    CHECK(p_blur() == 0 && p_blur() == 0, "blur_off = 1: the call returns 0 without running");
    g_blur_value = 9;
    a.blur_off = 0; mgs4vr_cam_set_adjust(&a);
    CHECK(p_blur() == 9, "blur_off back to 0: untouched again, live");
    mgs4vr_cam_get_stats(&s);
    CHECK(s.skip_calls == 4 && s.skip_applied == 2, "4 calls seen, 2 neutralised");

    {   /* Every frame must come out as ONE of the two published offsets. */
        HANDLE th[2];
        int mixed = 0, seen_a = 0, seen_b = 0;
        MGS4VR_CAM_STATS s2;
        memset(&g_pub[0], 0, sizeof(g_pub[0])); memset(&g_pub[1], 0, sizeof(g_pub[1]));
        g_pub[0].enabled = 1; g_pub[0].yaw = 30.0f; g_pub[0].pitch = 20.0f; g_pub[0].roll = 10.0f;
        g_pub[0].x = 100.0f; g_pub[0].y = 200.0f; g_pub[0].z = 300.0f;
        g_pub[1].enabled = 1; g_pub[1].yaw = -30.0f; g_pub[1].pitch = -20.0f; g_pub[1].roll = -10.0f;
        g_pub[1].x = -100.0f; g_pub[1].y = -200.0f; g_pub[1].z = -300.0f;
        g_pub_stop = 0;
        th[0] = CreateThread(NULL, 0, publisher, (LPVOID)(INT_PTR)0, 0, NULL);
        th[1] = CreateThread(NULL, 0, publisher, (LPVOID)(INT_PTR)1, 0, NULL);
        for (i = 0; i < 4000; ++i) {
            game_frame(&g_main, 0.4 + i * 0.001, 0.1, -14000.0, orig, F_REFILL);
            if (check_pose(orig, g_main.m, 30, 20, 10, 100, 200, 300) < 1e-4) seen_a = 1;
            else if (check_pose(orig, g_main.m, -30, -20, -10, -100, -200, -300) < 1e-4) seen_b = 1;
            else if (!same_values(g_main.m, orig)) mixed++;     /* untouched is fine; a blend is not */
        }
        g_pub_stop = 1;
        WaitForSingleObject(th[0], 5000); WaitForSingleObject(th[1], 5000);
        CloseHandle(th[0]); CloseHandle(th[1]);
        mgs4vr_cam_get_stats(&s2);
        printf("  two publishers: saw A %d, saw B %d, mixed %d, torn reads %ld\n", seen_a, seen_b, mixed, s2.adjust_torn);
        CHECK(seen_a && seen_b, "both publishers reached the camera (the race really was exercised)");
        CHECK(mixed == 0, "two publishers at once never produce a half-and-half offset");
        CHECK(s2.faults == 0, "no faults under concurrent publication");
    }
    memset(&a, 0, sizeof(a)); a.enabled = 1; a.sweep_deg = 20.0f; a.sweep_hz = 5.0f;
    mgs4vr_cam_set_adjust(&a);
    {
        double lo = 1e9, hi = -1e9;
        for (i = 0; i < 60; ++i) {
            double yaw;
            game_frame(&g_main, 0.0, 0.0, 0.0, orig, 0);
            yaw = atan2(g_main.m[8], g_main.m[10]) * 57.29577951;
            if (yaw < lo) lo = yaw; if (yaw > hi) hi = yaw;
            Sleep(5);
        }
        printf("  sweep yaw range %.1f .. %.1f deg\n", lo, hi);
        CHECK(lo < -15.0 && hi > 15.0 && lo >= -20.01 && hi <= 20.01, "sweep oscillates within +-20 deg");
    }
    mgs4vr_cam_poll();                                           /* logs the seam call chains */
    run_thread(disarm_thread);
    game_frame(&g_main, 0.5, 0.0, 100.0, orig, 0);
    CHECK(memcmp(g_main.m, orig, 64) == 0, "disarmed: camera untouched");
    CHECK(p_blur() == 9, "disarmed: function untouched");
    g_cfg.ret[0]=g_ret[1];g_cfg.prepare_eye=test_prepare_eye;g_cfg.finish_eye=test_finish_eye;
    run_thread(arm_thread);CHECK(g_arm_result>0,"stereo camera hooks armed");
    game_frame(&g_main,.5,0,100,orig,F_REFILL); /* learn seam after rearm */
    eye_applied_count=eye_rejected_count=0;
    game_frame(&g_main,.5,0,100,orig,F_REFILL);
    CHECK(eye_applied_count==1 && check_pose(orig,g_main.m,0,0,0,32,0,0)<1e-4,"matching return acknowledges applied eye pose");
    CHECK(seen_eye_args[0]==2 && seen_eye_args[1]==.1f && seen_eye_args[2]==.2f && seen_eye_args[3]==1.3f,"all four eye projection arguments reach native builder");
    game_frame(&g_main,.5,0,100,orig,F_CORRUPT);
    CHECK(eye_applied_count==1 && eye_rejected_count==1 && fabsf(g_main.pad[0]-2.8f)<1e-5,"invalid source leaves projection unchanged and rejects ticket");
    eye_fail=1;game_frame(&g_main,.5,0,100,orig,F_REFILL);
    CHECK(eye_rejected_count==2 && same_values(orig,g_main.m),"failed preparation cannot publish old ticket");
    eye_fail=0;game_frame(&g_main,.5,0,100,orig,F_REFILL|F_CINE);
    CHECK(eye_applied_count==1 && eye_rejected_count==3,"cinematic owner return rejects eye ticket");
    game_frame(&g_main,.5,0,100,orig,F_REFILL);
    CHECK(eye_applied_count==2,"eye application recovers after cinematic");
    game_frame(&g_main,.5,0,100,orig,F_RESET);
    CHECK(eye_applied_count==2 && eye_rejected_count==4 && same_values(orig,g_main.m),"recorded intro reset camera remains native and rejects stereo ticket");
    CHECK(seen_eye_args[0]==1.8f && seen_eye_args[1]==0 && seen_eye_args[2]==0 && seen_eye_args[3]==1,"intro keeps native projection arguments");
    game_frame(&g_main,.5,0,100,orig,F_REFILL);
    CHECK(eye_applied_count==3,"gameplay resumes eye transforms after intro reset");
    eye_fail=2;memset(&a,0,sizeof(a));a.enabled=1;a.yaw=20;a.cine_auto=1;mgs4vr_cam_set_adjust(&a);
    game_frame(&g_main,.5,0,100,orig,F_REFILL);
    CHECK(check_pose(orig,g_main.m,20,0,0,0,0,0)<1e-4,"stereo off callback preserves mono head-follow");
    a.enabled=0;mgs4vr_cam_set_adjust(&a);game_frame(&g_main,.5,0,100,orig,F_REFILL);
    CHECK(same_values(orig,g_main.m),"stereo off and head-follow off leaves camera native");

    run_thread(disarm_thread);
    printf("PASS: cam_adjust_test\n");
    return 0;
}
