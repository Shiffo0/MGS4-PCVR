/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mgs4vr_cam.h"
#include "mgs4vr_eye.h"

#define MAX_THREADS   512
#define MAX_TSTATE    32
#define WATCHDOG_TRAPS_PER_SEC 200000

#define STASH_DEPTH 8
typedef struct {ULONG64 camera,source,ret;MGS4VR_EYE_TICKET eye;int eye_applied;} STASH;
typedef struct {
    int depth;STASH s[STASH_DEPTH];
    volatile LONG tid;
    float last_in[16], last_out[16];
    int have_last;
    ULONG64 seam_src;
    int cine_pending;
    MGS4VR_CAM_ADJUST last_adj;
    int have_adj;
} TSTATE;

static MGS4VR_CAM_CONFIG g_cfg;
static void (*volatile g_log)(const char *fmt, ...);
static PVOID g_veh;
static volatile LONG g_armed;
static ULONG64 g_addr[4];
static ULONG64 g_base;
static ULONG64 g_adj_callers[2];

static volatile LONG g_adj_lock, g_adj_seq;
static MGS4VR_CAM_ADJUST g_adj_live;
static volatile LONG g_adj_blur_off;
static LONGLONG g_qpf, g_qpc0;
static ULONG64 g_skip_fn;

static MGS4VR_CAM_STATS g_stats;
static TSTATE g_tstate[MAX_TSTATE];
static DWORD g_armed_tids[MAX_THREADS];
static int g_armed_tid_count;
static DWORD g_last_summary, g_last_rescan, g_last_watch;
static LONG g_last_traps, g_summary_builds, g_summary_projs;
static unsigned long g_summary_frame;
static void log_msg(const char *fmt, ...) {
    char text[800];
    va_list a;
    void (*log)(const char *f, ...) = g_log;
    if (!log) return;
    va_start(a, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, a);
    va_end(a);
    log("cam: %s", text);
}

static int safe_copy(void *dst, const void *src, size_t n) {
    __try { memcpy(dst, src, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static unsigned rva_of(ULONG64 addr) {
    return (addr >= g_base && addr - g_base < 0xFFFFFFFFull) ? (unsigned)(addr - g_base) : 0xFFFFFFFFu;
}

static TSTATE *tstate_get(DWORD tid) {
    int i;
    for (i = 0; i < MAX_TSTATE; ++i) {
        LONG cur = g_tstate[i].tid;
        if (cur == (LONG)tid) return &g_tstate[i];
        if (cur == 0 && InterlockedCompareExchange(&g_tstate[i].tid, (LONG)tid, 0) == 0) return &g_tstate[i];
        if (g_tstate[i].tid == (LONG)tid) return &g_tstate[i];
    }
    return NULL;
}

static void on_builder_entry(CONTEXT *c) {
 ULONG64 ret=0;TSTATE *t=tstate_get(GetCurrentThreadId());
 InterlockedIncrement(&g_stats.builds);
 if(!t || !safe_copy(&ret,(void *)c->Rsp,8))return;
 if(ret==g_addr[1] || ret==g_addr[3]){
  if(t->depth<STASH_DEPTH){STASH *s=&t->s[t->depth];memset(s,0,sizeof(*s));s->camera=c->Rcx;s->source=c->Rdx;s->ret=ret;}
  t->depth++;
 }
}
static void on_builder_return(CONTEXT *c,int which) {
 TSTATE *t=tstate_get(GetCurrentThreadId());STASH st;(void)c;(void)which;
 if(!t || t->depth<=0)return;
 t->depth--;if(t->depth>=STASH_DEPTH)return;st=t->s[t->depth];
 if(g_cfg.finish_eye)g_cfg.finish_eye(st.camera,st.ret,&st.eye,st.eye_applied);
}

static int source_is_camera_matrix(const float *m) {
    int i, j;
    for (i = 0; i < 16; ++i) if (!(m[i] == m[i]) || fabsf(m[i]) > 1e8f) return 0;
    if (fabsf(m[3]) > 1e-4f || fabsf(m[7]) > 1e-4f || fabsf(m[11]) > 1e-4f || fabsf(m[15] - 1.0f) > 1e-4f) return 0;
    for (i = 0; i < 3; ++i) {
        const float *a = m + i * 4;
        if (fabsf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1.0f) > 1e-2f) return 0;
        for (j = i + 1; j < 3; ++j) {
            const float *b = m + j * 4;
            if (fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 1e-2f) return 0;
        }
    }
    return 1;
}

static void head_matrix(const MGS4VR_CAM_ADJUST *a, float yaw_deg, float *h) {
    const float d = 0.017453292519943295f;
    float cy = cosf(yaw_deg * d), sy = sinf(yaw_deg * d);
    float cp = cosf(a->pitch * d), sp = sinf(a->pitch * d);
    float cr = cosf(a->roll * d), sr = sinf(a->roll * d);
    float ry[9] = { cy, 0, -sy,   0, 1, 0,   sy, 0, cy };
    float rx[9] = { 1, 0, 0,   0, cp, sp,   0, -sp, cp };
    float rz[9] = { cr, sr, 0,   -sr, cr, 0,   0, 0, 1 };
    float t1[9], t2[9];
    int r, c, k;
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { t1[r * 3 + c] = 0; for (k = 0; k < 3; ++k) t1[r * 3 + c] += rz[r * 3 + k] * rx[k * 3 + c]; }
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) { t2[r * 3 + c] = 0; for (k = 0; k < 3; ++k) t2[r * 3 + c] += t1[r * 3 + k] * ry[k * 3 + c]; }
    memset(h, 0, 16 * sizeof(float));
    for (r = 0; r < 3; ++r) for (c = 0; c < 3; ++c) h[r * 4 + c] = t2[r * 3 + c];
    h[12] = a->x; h[13] = -a->y; h[14] = a->z; h[15] = 1.0f;
}

static int on_this_stack(ULONG64 p) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    return p >= (ULONG64)tib->StackLimit && p + 64 <= (ULONG64)tib->StackBase;
}

static void note_chain(const CONTEXT *c, ULONG64 ret, int cine, float scale) {
    (void)c;(void)ret;(void)cine;(void)scale;
}

static int adjust_get(MGS4VR_CAM_ADJUST *out);

static void adjust_entry(CONTEXT *c) {
    MGS4VR_CAM_ADJUST a;
    MGS4VR_EYE_TICKET ticket;
    int eye=0;
    ULONG64 ret = 0;
    float m[16], h[16], out[16], yaw, scale;
    TSTATE *t;
    int r, col, k, cinematic;
    if (!g_adj_callers[0] && !g_adj_callers[1]) return;
    if (!safe_copy(&ret, (const void *)c->Rsp, sizeof(ret)) || !ret) return;
    if (ret != g_adj_callers[0] && ret != g_adj_callers[1]) return;
    if (g_cfg.adjust_camera && c->Rcx != (ULONG64)g_cfg.adjust_camera) { InterlockedIncrement(&g_stats.adjust_other_camera); return; }
    if (!c->Rdx || c->Rdx == c->Rcx) return;                  /* never the in-place rebuilds */
    t = tstate_get(GetCurrentThreadId());
    if (!t) return;
    /* Context tracking runs whether or not an offset is enabled. The owner
       site always builds from THE seam matrix; a build of the main camera from
       the wrapper site with any OTHER source is the cutscene's own build
       (stack+0x20 just before the seam pair, the cinematic camera object on
       the heap just after it). */
    if (ret == g_adj_callers[1]) t->seam_src = c->Rdx;
    else if (!t->seam_src || c->Rdx != t->seam_src) {
        /* Frame structure, not call counting: the PRE-build uses another stack
           matrix and comes right before the seam pair, so it flags that pair.
           The POST-build (heap object) follows the pair and flags nothing. */
        if (on_this_stack(c->Rdx)) t->cine_pending = 1;
        InterlockedIncrement(&g_stats.cine_marks);
    }
    cinematic = t->cine_pending;
    if (ret == g_adj_callers[1]) t->cine_pending = 0;       /* the owner build closes the pair */
    memcpy(&scale, &c->Xmm2, sizeof(float));

    if (adjust_get(&a)) { t->last_adj = a; t->have_adj = 1; }
    else if (t->have_adj) a = t->last_adj;               /* one frame of the previous offset beats a mixed one */
    else return;
    if (!a.enabled && !g_cfg.prepare_eye) return;
    if (c->Rdx != t->seam_src) return;                        /* only THE seam matrix ... */
    if (!on_this_stack(c->Rdx)) { InterlockedIncrement(&g_stats.adjust_not_stack); return; }   /* ... and only a temporary */
    if (cinematic && (a.cine_auto || g_cfg.prepare_eye)) { InterlockedIncrement(&g_stats.adjust_cinematic); return; }
    if (!safe_copy(m, (const void *)c->Rdx, sizeof(m))) { InterlockedIncrement(&g_stats.faults); return; }
    if (!source_is_camera_matrix(m)) { InterlockedIncrement(&g_stats.adjust_invalid); return; }
    /* Measured in run 04 (2026-09-19): MGS4 refills the stack matrix before
       EACH of the two builds, so every call sees a fresh original and gets the
       offset exactly once. This guard only covers the other possibility - a
       caller that hands the same, already offset matrix to a second build -
       and it must never fire when our write changed nothing: with a zero
       offset out == in, a static camera then looks "already applied" forever
       and a later offset change is ignored (the live stall in run 04). */
    if (t->have_last && memcmp(m, t->last_out, sizeof(m)) == 0 &&
        memcmp(t->last_out, t->last_in, sizeof(m)) != 0) { InterlockedIncrement(&g_stats.adjust_same); return; }
    if(g_cfg.prepare_eye){
        /* Exact reset camera sampled in run09 intro/loading at origin. This is
           a conservative exclusion, not a universal gameplay/menu classifier. */
        static const float reset[16]={-1,0,0,0,0,-1,0,0,0,0,1,0,0,0,0,1};
        int reset_match=1,i;
        for(i=0;i<16;++i)if(m[i]!=reset[i])reset_match=0;

        eye=g_cfg.prepare_eye(c->Rcx,cinematic,&ticket);
        if(eye<0)return;
        if(eye>0 && reset_match)return;
        if(!on_this_stack(c->Rsp+0x28))return;
        if(eye>0)a=ticket.adjust;
    }
    if(!a.enabled)return;
    yaw = a.yaw;
    if (a.sweep_deg != 0.0f && g_qpf) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        yaw += a.sweep_deg * sinf(6.2831853f * a.sweep_hz * (float)((double)(q.QuadPart - g_qpc0) / (double)g_qpf));
    }
    head_matrix(&a, yaw, h);
    for (r = 0; r < 4; ++r) for (col = 0; col < 4; ++col) {
        float s = 0;
        for (k = 0; k < 4; ++k) s += h[r * 4 + k] * m[k * 4 + col];
        out[r * 4 + col] = s;
    }
    out[3] = out[7] = out[11] = 0.0f; out[15] = 1.0f;
    __try {
        memcpy((void *)c->Rdx, out, sizeof(out));
        if(eye>0){
            memcpy((void *)(c->Rsp+0x28),&ticket.args[2],4);
            memcpy((void *)(c->Rsp+0x30),&ticket.args[3],4);
            memcpy(&c->Xmm2,&ticket.args[0],4);memcpy(&c->Xmm3,&ticket.args[1],4);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_stats.faults); return; }
    if(eye>0 && t->depth>0 && t->depth<=STASH_DEPTH){
        STASH *st=&t->s[t->depth-1];
        if(st->camera==c->Rcx && st->source==c->Rdx && st->ret==ret){st->eye=ticket;st->eye_applied=1;}
    }
    memcpy(t->last_in, m, sizeof(m));
    memcpy(t->last_out, out, sizeof(out));
    t->have_last = 1;
    InterlockedIncrement(&g_stats.adjusted);
}

void mgs4vr_cam_set_adjust(const MGS4VR_CAM_ADJUST *a) {
    int spins = 0;
    while (InterlockedCompareExchange(&g_adj_lock, 1, 0) != 0) {
        if (++spins > 64) Sleep(0); else YieldProcessor();
    }
    InterlockedIncrement(&g_adj_seq);
    g_adj_live = *a;
    InterlockedIncrement(&g_adj_seq);
    InterlockedExchange(&g_adj_blur_off, a->blur_off ? 1 : 0);
    InterlockedExchange(&g_adj_lock, 0);
}

static int adjust_get(MGS4VR_CAM_ADJUST *out) {
    int i;
    for (i = 0; i < 32; ++i) {
        LONG s = g_adj_seq;
        if (s & 1) { YieldProcessor(); continue; }
        *out = g_adj_live;
        MemoryBarrier();
        if (g_adj_seq == s) return 1;
    }
    InterlockedIncrement(&g_stats.adjust_torn);
    return 0;
}

static void on_proj(CONTEXT *c) {
    (void)c;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep) {
    CONTEXT *c;
    ULONG64 rip;
    int i, which = -1;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    c = ep->ContextRecord;
    rip = c->Rip;

    for (i = 0; i < 4; ++i)
        if (g_addr[i] && rip == g_addr[i] && (c->Dr6 & (1ull << i))) { which = i; break; }
    if (which < 0) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_stats.traps);
    if (g_armed) {
        if (which == 0) {
            on_builder_entry(c);
            adjust_entry(c);
        } else if (which == 2 && g_skip_fn) {
            InterlockedIncrement(&g_stats.skip_calls);
            if (g_adj_blur_off) {

                ULONG64 ret = 0;
                if (safe_copy(&ret, (const void *)c->Rsp, sizeof(ret)) && ret) {
                    c->Rax = 0; c->Rip = ret; c->Rsp += 8;
                    InterlockedIncrement(&g_stats.skip_applied);
                    c->Dr6 = 0;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        } else if (which == 2) on_proj(c);
        else on_builder_return(c, which);
    }

    c->EFlags |= 0x10000;
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static ULONG64 dr7_value(void) {
    ULONG64 v = 0x400;
    int i;
    for (i = 0; i < 4; ++i) if (g_addr[i]) v |= 1ull << (i * 2);
    return v;
}

static int set_dr(HANDLE th, int arm) {
    CONTEXT c;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return 0;
    if (arm && (c.Dr7 & 0xFF) && c.Dr0 != g_addr[0]) return -1;
    c.Dr0 = arm ? g_addr[0] : 0; c.Dr1 = arm ? g_addr[1] : 0;
    c.Dr2 = arm ? g_addr[2] : 0; c.Dr3 = arm ? g_addr[3] : 0;
    c.Dr6 = 0;
    c.Dr7 = arm ? dr7_value() : 0;
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    return SetThreadContext(th, &c) ? 1 : 0;
}

static int tid_known(DWORD tid) {
    int i;
    for (i = 0; i < g_armed_tid_count; ++i) if (g_armed_tids[i] == tid) return 1;
    return 0;
}

static int sweep_threads(int arm) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int n = 0, foreign = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            HANDLE th;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            if (arm && tid_known(te.th32ThreadID)) continue;
            if (!arm && !tid_known(te.th32ThreadID)) continue;
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                int r = set_dr(th, arm);
                ResumeThread(th);
                if (r == 1) { n++; if (arm && g_armed_tid_count < MAX_THREADS) g_armed_tids[g_armed_tid_count++] = te.th32ThreadID; }
                else if (r == -1) { foreign++; if (arm && g_armed_tid_count < MAX_THREADS) g_armed_tids[g_armed_tid_count++] = te.th32ThreadID; }
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (foreign) log_msg("WARNING: %d thread(s) already had debug registers in use - left alone", foreign);
    if (!arm) g_armed_tid_count = 0;
    return n;
}

static void hex_bytes(char *out, size_t cap, const unsigned char *b, int n) {
    int i; size_t len = 0;
    out[0] = 0;
    for (i = 0; i < n && len + 4 < cap; ++i) { _snprintf_s(out + len, cap - len, _TRUNCATE, "%02x ", b[i]); len += 3; }
}

static int check_sig(const char *name, void *addr, const unsigned char *sig, int sig_len) {
    unsigned char got[16];
    char hex[64];
    int ok;
    if (!addr) { log_msg("probe %s: no address", name); return 0; }
    if (!safe_copy(got, addr, sizeof(got))) { log_msg("probe %s @%p: unreadable", name, addr); return 0; }
    hex_bytes(hex, sizeof(hex), got, 16);
    ok = sig && sig_len > 0 && sig_len <= 16 && memcmp(got, sig, (size_t)sig_len) == 0;
    log_msg("probe %s rva 0x%x: %s-> prologue %s", name, rva_of((ULONG64)addr), hex, ok ? "MATCH" : "MISMATCH");
    return ok;
}

static int check_call_site(const char *name, void *ret, void *target) {
    unsigned char b[5];
    LONG rel;
    ULONG64 dest;
    if (!ret) return 0;
    if (!safe_copy(b, (unsigned char *)ret - 5, 5)) { log_msg("probe %s: unreadable", name); return 0; }
    if (b[0] != 0xE8) { log_msg("probe %s rva 0x%x: byte before is %02x, not a direct call", name, rva_of((ULONG64)ret), b[0]); return 0; }
    memcpy(&rel, b + 1, 4);
    dest = (ULONG64)ret + (LONG64)rel;
    log_msg("probe %s rva 0x%x: call -> rva 0x%x %s", name, rva_of((ULONG64)ret), rva_of(dest),
            dest == (ULONG64)target ? "= builder, MATCH" : "NOT the builder");
    return dest == (ULONG64)target;
}

int mgs4vr_cam_probe(const MGS4VR_CAM_CONFIG *cfg, void (*log)(const char *fmt, ...)) {
    int r = 0;
    g_log = log;
    g_base = (ULONG64)cfg->image_base;
    if (check_sig("camera builder", cfg->builder, cfg->builder_sig, cfg->builder_sig_len)) r |= MGS4VR_CAM_OK_BUILDER;
    if (check_sig("projection setter", cfg->proj, cfg->proj_sig, cfg->proj_sig_len)) r |= MGS4VR_CAM_OK_PROJ;
    if (check_call_site("return site 0", cfg->ret[0], cfg->builder)) r |= MGS4VR_CAM_OK_RET0;
    if (check_call_site("return site 1", cfg->ret[1], cfg->builder)) r |= MGS4VR_CAM_OK_RET1;
    return r;
}

static void resolve_adjust_callers(const MGS4VR_CAM_CONFIG *cfg) {
    int i;
    g_adj_callers[0] = g_adj_callers[1] = 0;
    for (i = 0; i < 2; ++i) {
        char name[32];
        if (!cfg->adjust_callers[i]) continue;
        _snprintf_s(name, sizeof(name), _TRUNCATE, "adjust caller %d", i);
        if (check_call_site(name, cfg->adjust_callers[i], cfg->builder) || !cfg->require_call_check)
            g_adj_callers[i] = (ULONG64)cfg->adjust_callers[i];
    }
}

int mgs4vr_cam_arm(const MGS4VR_CAM_CONFIG *cfg, void (*log)(const char *fmt, ...)) {
    int ok, n;
    if (g_armed) return (int)g_stats.threads_armed;
    ok = mgs4vr_cam_probe(cfg, log);
    g_cfg = *cfg;
    if (g_cfg.cam_bytes > MGS4VR_CAM_MAX_CAM) g_cfg.cam_bytes = MGS4VR_CAM_MAX_CAM;
    if (g_cfg.src_bytes > MGS4VR_CAM_MAX_SRC) g_cfg.src_bytes = MGS4VR_CAM_MAX_SRC;
    memset(g_addr, 0, sizeof(g_addr));
    if (ok & MGS4VR_CAM_OK_BUILDER) g_addr[0] = (ULONG64)cfg->builder;
    if (ok & MGS4VR_CAM_OK_PROJ)    g_addr[2] = (ULONG64)cfg->proj;
    g_skip_fn = (ULONG64)cfg->skip_fn;
    if (g_skip_fn) g_addr[2] = g_skip_fn;
    if (g_addr[0]) {
        if ((ok & MGS4VR_CAM_OK_RET0) || (!cfg->require_call_check && cfg->ret[0])) g_addr[1] = (ULONG64)cfg->ret[0];
        if ((ok & MGS4VR_CAM_OK_RET1) || (!cfg->require_call_check && cfg->ret[1])) g_addr[3] = (ULONG64)cfg->ret[1];
    }
    if (!g_addr[0] && !g_addr[2]) { log_msg("nothing verified - NOT arming"); return 0; }
    if (g_addr[0]) resolve_adjust_callers(cfg);
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); g_qpf = q.QuadPart; QueryPerformanceCounter(&q); g_qpc0 = q.QuadPart; }
    log_msg("camera offset seam: caller0 %s caller1 %s, camera filter %p",
            g_adj_callers[0] ? "verified" : "off", g_adj_callers[1] ? "verified" : "off", cfg->adjust_camera);
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_tstate, 0, sizeof(g_tstate));
    g_veh = AddVectoredExceptionHandler(1, veh);
    if (!g_veh) { log_msg("ERROR: AddVectoredExceptionHandler failed"); return 0; }
    InterlockedExchange(&g_armed, 1);
    n = sweep_threads(1);
    g_stats.threads_armed = n;
    g_last_summary = g_last_rescan = g_last_watch = GetTickCount();
    log_msg("armed %d threads: DR0 builder %s, DR1 ret0 %s, DR2 proj %s, DR3 ret1 %s; sampling every %d frame(s), cam %d src %d bytes",
            n, g_addr[0] ? "on" : "off", g_addr[1] ? "on" : "off", g_addr[2] ? "on" : "off", g_addr[3] ? "on" : "off",
            g_cfg.sample_every, g_cfg.cam_bytes, g_cfg.src_bytes);
    return n;
}

static void drain(void) {

}

void mgs4vr_cam_poll(void) {
    DWORD now=GetTickCount();
    if(!g_veh)return;
    if(g_armed && now-g_last_watch>=1000){
        LONG traps=g_stats.traps;
        if((traps-g_last_traps)*1000ll/(LONG)(now-g_last_watch)>WATCHDOG_TRAPS_PER_SEC){mgs4vr_cam_disarm();return;}
        g_last_traps=traps;g_last_watch=now;
    }
    if(g_armed && now-g_last_rescan>=2000){g_stats.threads_armed+=sweep_threads(1);g_last_rescan=now;}
}

void mgs4vr_cam_disarm(void) {
    if (!g_veh) return;
    InterlockedExchange(&g_armed, 0);
    sweep_threads(0);
    Sleep(50);
    RemoveVectoredExceptionHandler(g_veh);
    g_veh = NULL;
    drain();
    log_msg("disarmed: traps %ld builds %ld returns %ld projs %ld records %ld dropped %ld faults %ld",
            g_stats.traps, g_stats.builds, g_stats.returns, g_stats.projs, g_stats.records, g_stats.dropped, g_stats.faults);
}

void mgs4vr_cam_get_stats(MGS4VR_CAM_STATS *out) { *out = g_stats; }
