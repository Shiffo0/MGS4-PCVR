/* Scoped weapons in VR first person. Two independent rules, each behind its own
 * token in mgs4vr_scope.on (re-read every 250 ms; an unknown token turns
 * everything off):
 *
 * "nomask"  No full-screen scope mask. Measured with the d3d probe (runs 32716
 *   and A/B 4DB8EA19, M82A2): in scope view the game paints the black mask as
 *   234 non-indexed Draw(6) quads per frame, each with a 16x16 texture on PS
 *   slot 0; in third person there are none. They are counted per present frame
 *   and skipped only while the previous frame held at least SCOPE_ON_FRAME of
 *   them and the VR first-person hold is applied. A lone 16x16 quad (fade, fill)
 *   always draws; the first frame of a scope view still shows the mask once.
 *   Seam, executor's non-indexed branch (submit 0x793470):
 *     7951BA  45 33 C0   xor r8d,r8d        <- only entry of the block
 *     7951BD  41 8B D2   mov edx,r10d
 *     7951C0  FF 50 68   call [rax+0x68]    ID3D11DeviceContext::Draw, left intact
 *   The first six bytes become CALL stub + NOP.
 *
 * "arms"    Snake's arms stay visible in scope view. Measured with the arms mask
 *   probe (run 30880): the player model's part mask for the view is 0xC in
 *   third person, 0x1E in normal first person (everything but part id 5, the
 *   arms) and 0x3E in scope view (arms hidden too), while the arm IK keeps
 *   writing. The mask is picked per model right before part processing:
 *     118EF9  48 8B 8C C3 80 00 00 00   mov rcx,[rbx+rax*8+0x80]   rbx=model, eax=view
 *     118F01  48 89 8E 00 02 00 00      mov [rsi+0x200],rcx        rsi=context
 *     118F0B  E8 ..                     call 0x118010              part processing
 *   The first instruction becomes CALL stub + NOP; the stub asks scope_parts()
 *   for the mask. For a 115-bone model with mask 0x3E, while the VR first-person
 *   hold is applied, the answer is 0x1E. The model's own mask is never written.
 *
 * "weapon"  The weapon stays visible in scope view. Model log run 11512: the
 *   models whose mask goes to 3 in scope view are Snake's head and face (they
 *   carry 3 in normal first person too); the rifle itself (5 bones, 2 parts)
 *   reaches the mask seam in third person every frame but in scope view only
 *   while reloading, and never with the arms rule off. It is dropped earlier:
 *     1186F5  E8 ..         call 0x114d00     bounds classification (context+0x270 = view
 *                                             matrix, model+0x190/+0x1A0 = local bounds)
 *     1186FA  41 89 07      mov [r15],eax
 *     1186FD  A8 01         test al,1         bit 0 = culled: copy the record, skip the parts
 *   The scope camera's narrow view rejects the rifle in the hands. The CALL now
 *   goes to a stub that calls the classification, then scope_cull(): in VR first
 *   person a culled model whose world position (model+0x50, matrix row 3) lies
 *   within SCOPE_NEAR of the player model's position seen in the last two
 *   frames gets bit 0 cleared. Nothing in the model is written.
 *   A second test follows (118896..118976): the model's world bounds against the
 *   context's visibility volumes (count context+0xD9B8, boxes +0xD9D8); no hit =
 *     11896E  B8 01 00 00 00   mov eax,1
 *     118973  41 89 07         mov [r15],eax
 *     118976  E9 ..            jmp 118701       the same early route
 *   The MOV becomes CALL stub: scope_cull2() applies the same near-player rule
 *   and the stub then either plays the MOV (stay culled) or returns past the
 *   JMP to 11897B (keep the model). xmm0-5 are saved around the call.
 *
 * "scopeflag"  The game's own scope-view switch on the held weapon. External
 *   measurement 2026-10-07 (tools/scope_actor_watch.py, M82A2 = weapon id 40):
 *   entering scope view sets weapon+0x16C 7->9 (state) and weapon+0x3BC 0->1,
 *   with weapon+0x3C8 = 3.0 (magnification). Forcing +0x3BC back to 0 from
 *   outside while in scope view made the rifle appear (tools/scope_actor_poke.py,
 *   test C); the game does not write it back until the view changes. Rule, on the
 *   worker every tick: while the VR first-person hold is applied and the held
 *   weapon (identity chain of mgs4vr_weapon_identity.h, no id filter) is in
 *   state 9 with +0x3BC == 1, write 0 and remember it; when the hold ends or the
 *   state leaves 9, put 1 back once if it is still 0 and the state is still 9.
 *
 * "showgun"  Keeps the held weapon drawn in scope view without fighting the game.
 *   Hardware watchpoint on weapon+0x3BC (2026-10-07): a BYTE, touched once per
 *   frame from the component loop 0x904040: written at 97BFF3 (the scope
 *   component, every frame), read for camera/zoom at 97BEF6/97BF6F/97BFA0, at
 *   982DA0, and at
 *     982F79  0F B6 8B BC 03 00 00   movzx ecx,byte [rbx+0x3BC]
 *     982F80  02 C9 ...              bit 1 of [[r15+0x40]+0x60] = hide this model
 *   Only that last read is redirected: CALL stub + 2-byte NOP. The stub does the
 *   original load, then scope_hide() returns 0 for the weapon the player holds
 *   (resolved on the worker tick) while the VR first-person hold is applied.
 *   The game's byte, camera and zoom are untouched.
 *
 * "keepgun"  The rifle stays in the hands in scope view (2026-10-08). The scope
 *   component (0x97BB80, chained to 0x97C024) runs every frame for the weapon
 *   (rdi). On the transition into scope view it calls
 *     97BF9B  E8 -> 0xD46810(controller=[rdi+0x60], 0, [rdi+0x3B0])
 *   0xD46810 is the controller message dispatcher: message 0 looks up an object
 *   by index (0xD48090), attaches it to the controller (0x740D30, controller+0x98)
 *   and sends it 0x200; message 1 detaches controller+0x98 (sets it to 0) and
 *   sends 0x202. Measured (scope_actor_watch runs 1 and 2): in scope view
 *   controller+0x98 and the rifle model's parent (model_actor+0x40) are the
 *   same new object; during a real reload both go back (0 / the controller) and
 *   the rifle is visible. Index changes call the pair at 97BFCB (1) / 97BFE0 (0).
 *   Rule: on these three CALLs only, message 0 for the weapon the player holds
 *   while the VR first-person hold is applied is not delivered (returns 0, as
 *   0xD46810 does for an unknown handle). Detach, zoom and the scope byte are
 *   the game's. Nothing is written per frame.
 *
 * "showrifle"  The rifle stays drawn in scope view (2026-10-08). Measured
 *: the rifle's render model
 *   (model_actor+0x3B8, link from mgs4vr_muzzle_contract.h) has +0x78 = 0 in
 *   third person and while reloading, 7 in scope view; 0x1185DA skips a model
 *   whose +0x78 has the bit of the current view. Watchpoint: the only writer is
 *   the per-frame visibility pass 0x14DB4A0 (rcx = model actor, twice a frame):
 *     r8d = 1 if (actor+0xC0 & 3) == 3 and (object of handle actor+0xA0)+0xC0 & 3 == 3
 *     14DB507  41 8B 49 78   mov ecx,[r9+0x78]
 *     14DB50B  8B C1         mov eax,ecx
 *     ...      or eax,7 / and ecx,~7 / cmp r8d,1 / cmovne ecx,eax
 *     14DB51A  41 89 49 78   mov [r9+0x78],ecx
 *   The first six bytes become CALL stub + NOP (no branch lands inside them).
 *   The stub asks scope_vis_gate(actor, r8d) for the decision, then performs the
 *   two replaced loads. For the held weapon's model actor while the VR
 *   first-person hold is applied the answer is 1 (visible); otherwise the
 *   game's own r8d. Same thread, same frame: nothing is written against the game.
 *
 * "log=<label>"  Measurement on the same seam: every distinct model that passes
 *   is recorded once per label, keyed by its part table (+0xF0) because the
 *   record slots (rbx) change model from frame to frame, with its 0x1E0-byte
 *   record as first seen and a histogram of the masks it carried; removing the
 *   token writes mgs4vr_scope_models_<pid>.csv.
 *
 * D3D11 rewrites its own context vtable entries at run time, so a vtable hook
 * does not hold; a breakpoint per call would cost an exception each time. Both
 * seams are patched once, with every other thread suspended, and stay; without
 * their token the stub's C function answers "unchanged". */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include "mgs4vr_scope.h"

#define SCOPE_RVA 0x7951bau
#define PARTS_RVA 0x118ef9u
#define CULL_RVA 0x1186f5u
#define CULL2_RVA 0x11896eu
#define HIDE_RVA 0x982f79u
#define ATTACH1_RVA 0x97bf9bu
#define ATTACH2_RVA 0x97bfcbu
#define ATTACH3_RVA 0x97bfe0u
#define MSG_FN_RVA 0xd46810u
#define VIS_RVA 0x14db507u
#define CULL_FN_RVA 0x114d00u
#define SCOPE_NEAR 1500.f   /* game units (mm): the held weapon is well inside */
#define SCOPE_ON_FRAME 100u   /* mask quads in the previous frame that mean scope view (measured 234, third person 0) */
#define SCOPE_MASK_VIEW 0x3eull   /* player part mask in scope view */
#define SCOPE_MASK_FPS 0x1eull    /* ... in normal first person: arms (part id 5) shown */
#define SCOPE_PLAYER_BONES 115u
#define SCOPE_POLL_MS 250u
#define SCOPE_REPORT_MS 1000u
#define SCOPE_THREADS 1024
#define SCOPE_STUB_IMM 24     /* offset of the C function imm64 in the draw and parts stubs */
#define SCOPE_LOG_ROWS 1536
#define SCOPE_LOG_LABELS 8
#define SCOPE_MODEL_BYTES 0x1e0
#define F_MASK 1u
#define F_ARMS 2u
#define F_LOG 4u
#define F_WEAPON 8u
#define F_FLAG 16u
#define F_GUN 32u
#define F_KEEP 64u
#define F_VIS 128u
#define SCOPE_SLOT_RVA 0x23eb5150u
#define SCOPE_OWNERS_RVA 0x23eb5160u
#define SCOPE_WEAPON_SCOPE_STATE 9u
#define SCOPE_PLAYER_FLAGS 0x10u
#define SCOPE_CTX_FRAMES 2u
#define SCOPE_LOG_MASKS 4
static const BYTE draw_sig[9]={0x45,0x33,0xc0,0x41,0x8b,0xd2,0xff,0x50,0x68};
static const BYTE parts_sig[19]={0x48,0x8b,0x8c,0xc3,0x80,0,0,0,0x48,0x89,0x8e,0x00,0x02,0,0,0x48,0x8b,0xce,0xe8};
static const BYTE cull_sig[12]={0xe8,0x06,0xc6,0xff,0xff,0x41,0x89,0x07,0xa8,0x01,0x74,0x45};
static const BYTE cull2_sig[13]={0xb8,0x01,0,0,0,0x41,0x89,0x07,0xe9,0x86,0xfd,0xff,0xff};
static const BYTE hide_sig[11]={0x0f,0xb6,0x8b,0xbc,0x03,0,0,0x02,0xc9,0x0f,0xb6};
static const BYTE attach1_sig[12]={0xe8,0x70,0xa8,0x3c,0x00,0x80,0xbf,0xbc,0x03,0,0,0};    /* call d46810 / cmp byte [rdi+3BC],0 */
static const BYTE attach2_sig[12]={0xe8,0x40,0xa8,0x3c,0x00,0x4c,0x63,0x87,0xb0,0x03,0,0}; /* call d46810 / movsxd r8,[rdi+3B0] */
static const BYTE vis_sig[19]={0x41,0x8b,0x49,0x78,0x8b,0xc1,0x83,0xc8,0x07,0x83,0xe1,0xf8,0x41,0x83,0xf8,0x01,0x0f,0x45,0xc8};
static const BYTE attach3_sig[11]={0xe8,0x2b,0xa8,0x3c,0x00,0x8b,0x87,0xb0,0x03,0,0};      /* call d46810 / mov eax,[rdi+3B0] */
static const BYTE draw_stub[]={
 0x45,0x33,0xc0,                 /* xor r8d,r8d                 the two instructions the CALL replaced */
 0x41,0x8b,0xd2,                 /* mov edx,r10d */
 0x83,0xfa,0x06,                 /* cmp edx,6 */
 0x75,0x2b,                      /* jne ret */
 0x50,0x51,0x52,0x41,0x50,0x41,0x52, /* push rax,rcx,rdx,r8,r10   five pushes re-align the stack */
 0x48,0x83,0xec,0x20,            /* sub rsp,20h */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_skip */
 0xff,0xd0,                      /* call rax                    rcx=context, edx=6 */
 0x48,0x83,0xc4,0x20,            /* add rsp,20h */
 0x85,0xc0,                      /* test eax,eax */
 0x41,0x5a,0x41,0x58,0x5a,0x59,0x58, /* pop r10,r8,rdx,rcx,rax    flags survive */
 0x74,0x05,                      /* jz ret */
 0x48,0x83,0x04,0x24,0x04,       /* add qword [rsp],4           skip NOP + CALL [rax+68h] */
 0xc3};                          /* ret */
static const BYTE parts_stub[]={
 0x50,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53, /* push rax,rdx,r8,r9,r10,r11 */
 0x48,0x83,0xec,0x28,            /* sub rsp,28h                 six pushes + 28h re-align the stack */
 0x48,0x8b,0xcb,                 /* mov rcx,rbx                 model */
 0x8b,0xd0,                      /* mov edx,eax                 view index */
 0x4c,0x8b,0xc6,                 /* mov r8,rsi                  context */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_parts */
 0xff,0xd0,                      /* call rax */
 0x48,0x8b,0xc8,                 /* mov rcx,rax                 the mask, as the replaced MOV left it */
 0x48,0x83,0xc4,0x28,            /* add rsp,28h */
 0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x58, /* pop r11,r10,r9,r8,rdx,rax */
 0xc3};                          /* ret */
#define CULL_STUB_ORIG 6          /* imm64 of the game's classification function */
#define CULL_STUB_FN 27           /* imm64 of scope_cull */
static const BYTE cull_stub[]={
 0x48,0x83,0xec,0x28,            /* sub rsp,28h                 shadow space, stack re-aligned */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,classification */
 0xff,0xd0,                      /* call rax                    rcx,rdx,r8 as the game set them */
 0x44,0x8b,0xc0,                 /* mov r8d,eax                 its result */
 0x48,0x8b,0xcb,                 /* mov rcx,rbx                 model */
 0x48,0x8b,0xd6,                 /* mov rdx,rsi                 context */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_cull */
 0xff,0xd0,                      /* call rax                    returns the result to keep */
 0x48,0x83,0xc4,0x28,            /* add rsp,28h */
 0xc3};                          /* ret */
#define CULL2_STUB_FN 56
static const BYTE cull2_stub[]={
 0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53, /* push rax,rcx,rdx,r8,r9,r10,r11   seven pushes re-align the stack */
 0x48,0x81,0xec,0x80,0,0,0,      /* sub rsp,80h                 shadow space + six xmm slots */
 0x0f,0x11,0x44,0x24,0x20,0x0f,0x11,0x4c,0x24,0x30,0x0f,0x11,0x54,0x24,0x40, /* movups [rsp+20h..],xmm0..2 */
 0x0f,0x11,0x5c,0x24,0x50,0x0f,0x11,0x64,0x24,0x60,0x0f,0x11,0x6c,0x24,0x70, /* movups [rsp+50h..],xmm3..5 */
 0x48,0x8b,0xcb,                 /* mov rcx,rbx                 model */
 0x48,0x8b,0xd6,                 /* mov rdx,rsi                 context */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_cull2 */
 0xff,0xd0,                      /* call rax                    0 = stay culled, 1 = keep the model */
 0x0f,0x10,0x44,0x24,0x20,0x0f,0x10,0x4c,0x24,0x30,0x0f,0x10,0x54,0x24,0x40, /* movups xmm0..2,[rsp+20h..] */
 0x0f,0x10,0x5c,0x24,0x50,0x0f,0x10,0x64,0x24,0x60,0x0f,0x10,0x6c,0x24,0x70, /* movups xmm3..5,[rsp+50h..] */
 0x48,0x81,0xc4,0x80,0,0,0,      /* add rsp,80h */
 0x85,0xc0,                      /* test eax,eax */
 0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58, /* pop r11,r10,r9,r8,rdx,rcx,rax   flags survive */
 0x74,0x06,                      /* jz keep */
 0x48,0x83,0x04,0x24,0x08,       /* add qword [rsp],8           return past mov [r15],eax / jmp: to 11897B */
 0xc3,                           /* ret */
 0xb8,0x01,0,0,0,                /* keep: mov eax,1             the replaced instruction */
 0xc3};                          /* ret                         back to 118973 */
#define GATE_STUB_FN 0x0f
#define GATE_STUB_ORIG 0x29
static const BYTE gate_stub[]={
 0x51,0x52,0x41,0x50,0x41,0x51,  /* push rcx,rdx,r8,r9          the message arguments */
 0x48,0x83,0xec,0x28,            /* sub rsp,28h                 four pushes + 28h re-align the stack */
 0x48,0x8b,0xcf,                 /* mov rcx,rdi                 the weapon (scope component); edx = message */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_attach_gate */
 0xff,0xd0,                      /* call rax */
 0x48,0x83,0xc4,0x28,            /* add rsp,28h */
 0x41,0x59,0x41,0x58,0x5a,0x59,  /* pop r9,r8,rdx,rcx */
 0x85,0xc0,                      /* test eax,eax */
 0x75,0x0c,                      /* jnz skip */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,0xD46810 */
 0xff,0xe0,                      /* jmp rax                     deliver the message as the game would */
 0x33,0xc0,                      /* skip: xor eax,eax           what 0xD46810 returns for nothing done */
 0xc3};                          /* ret */
#define VIS_STUB_FN 0x13
static const BYTE vis_stub[]={
 0x52,0x41,0x51,0x41,0x52,0x41,0x53, /* push rdx,r9,r10,r11          rdx = model actor, r9 = render model */
 0x48,0x83,0xec,0x28,            /* sub rsp,28h                 four pushes + 28h re-align the stack */
 0x48,0x8b,0xca,                 /* mov rcx,rdx                 model actor */
 0x44,0x89,0xc2,                 /* mov edx,r8d                 the game's decision (1 = visible) */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_vis_gate */
 0xff,0xd0,                      /* call rax */
 0x48,0x83,0xc4,0x28,            /* add rsp,28h */
 0x41,0x5b,0x41,0x5a,0x41,0x59,0x5a, /* pop r11,r10,r9,rdx */
 0x44,0x8b,0xc0,                 /* mov r8d,eax                 the decision to use */
 0x41,0x8b,0x49,0x78,            /* mov ecx,[r9+0x78]           the two replaced instructions */
 0x8b,0xc1,                      /* mov eax,ecx */
 0xc3};                          /* ret                         to the NOP, then or eax,7 */
#define HIDE_STUB_FN 26
static const BYTE hide_stub[]={
 0x0f,0xb6,0x8b,0xbc,0x03,0,0,   /* movzx ecx,byte [rbx+0x3BC]  the replaced instruction */
 0x50,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53, /* push rax,rdx,r8,r9,r10,r11 */
 0x48,0x83,0xec,0x28,            /* sub rsp,28h                 six pushes + 28h re-align the stack */
 0x48,0x8b,0xd3,                 /* mov rdx,rbx                 weapon; ecx = the byte */
 0x48,0xb8,0,0,0,0,0,0,0,0,      /* mov rax,scope_hide */
 0xff,0xd0,                      /* call rax */
 0x8b,0xc8,                      /* mov ecx,eax */
 0x48,0x83,0xc4,0x28,            /* add rsp,28h */
 0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x58, /* pop r11,r10,r9,r8,rdx,rax */
 0xc3};                          /* ret                         to the NOP, then add cl,cl */
typedef struct {unsigned rva,len,sig_len;const BYTE *sig;const BYTE *stub_code;unsigned stub_len,stub_at,fn_at,orig_at,orig_rva;void *fn;const char *name;
 BYTE *site,*stub;BYTE patch[8];volatile LONG installed;int rejected,busy_retries;} SEAM;
static int scope_skip(ID3D11DeviceContext *c,UINT count);
static ULONG64 scope_parts(BYTE *model,unsigned view,BYTE *context);
static unsigned long long (*volatile parts_hook)(const unsigned char *model,unsigned bones,unsigned long long mask);
void mgs4vr_scope_set_parts_hook(unsigned long long (*fn)(const unsigned char *model,unsigned bones,unsigned long long mask)){parts_hook=fn;}
static unsigned scope_cull(BYTE *model,BYTE *context,unsigned result);
static int scope_cull2(BYTE *model,BYTE *context);
static int near_player(BYTE *model,unsigned frame);
static unsigned scope_hide(unsigned value,BYTE *weapon);
static int scope_attach_gate(BYTE *weapon,unsigned msg);
static unsigned scope_vis_gate(BYTE *actor,unsigned decision);
#define SEAMS 9
static SEAM seams[SEAMS]={
 {SCOPE_RVA,6,sizeof(draw_sig),draw_sig,draw_stub,sizeof(draw_stub),0x00,SCOPE_STUB_IMM,0,0,(void *)scope_skip,"draw"},
 {PARTS_RVA,8,sizeof(parts_sig),parts_sig,parts_stub,sizeof(parts_stub),0x80,SCOPE_STUB_IMM,0,0,(void *)scope_parts,"parts"},
 {CULL_RVA,5,sizeof(cull_sig),cull_sig,cull_stub,sizeof(cull_stub),0x100,CULL_STUB_FN,CULL_STUB_ORIG,CULL_FN_RVA,(void *)scope_cull,"cull"},
 {CULL2_RVA,5,sizeof(cull2_sig),cull2_sig,cull2_stub,sizeof(cull2_stub),0x180,CULL2_STUB_FN,0,0,(void *)scope_cull2,"cull2"},
 {HIDE_RVA,7,sizeof(hide_sig),hide_sig,hide_stub,sizeof(hide_stub),0x280,HIDE_STUB_FN,0,0,(void *)scope_hide,"hide"},
 {ATTACH1_RVA,5,sizeof(attach1_sig),attach1_sig,gate_stub,sizeof(gate_stub),0x300,GATE_STUB_FN,GATE_STUB_ORIG,MSG_FN_RVA,(void *)scope_attach_gate,"attach1"},
 {ATTACH2_RVA,5,sizeof(attach2_sig),attach2_sig,gate_stub,sizeof(gate_stub),0x340,GATE_STUB_FN,GATE_STUB_ORIG,MSG_FN_RVA,(void *)scope_attach_gate,"attach2"},
 {ATTACH3_RVA,5,sizeof(attach3_sig),attach3_sig,gate_stub,sizeof(gate_stub),0x380,GATE_STUB_FN,GATE_STUB_ORIG,MSG_FN_RVA,(void *)scope_attach_gate,"attach3"},
 {VIS_RVA,6,sizeof(vis_sig),vis_sig,vis_stub,sizeof(vis_stub),0x3c0,VIS_STUB_FN,0,0,(void *)scope_vis_gate,"vis"}};
static BYTE *stub_page;
static volatile LONG flags,first_person,active,skipped,masked_frames,peak,arms_shown,weapon_shown,culled_seen,culled2_seen,weapon_shown2,near_parts;
static volatile LONG flag_writes,flag_restores,flag_faults,flag_forced,flag_state,flag_value,flag_weapon_id;static ULONG64 flag_weapon;
static volatile LONG64 held_weapon;static volatile LONG gun_shown,hide_seen;
static volatile LONG attach_seen,attach_skipped,detach_seen;
static volatile LONG64 held_model_actor;static volatile LONG vis_seen,vis_hidden,vis_forced;
#define ATT_MAX 4
static volatile LONG64 held_attach_actor,held_attach[ATT_MAX];static volatile LONG held_attach_code,held_attach_n,att_vis_seen,att_vis_hidden,att_vis_forced;
static int is_held_attach(LONG64 a){int k;if(!a)return 0;for(k=0;k<ATT_MAX;++k)if(held_attach[k]==a)return 1;return 0;}
int mgs4vr_scope_attach_code(void){return (int)InterlockedCompareExchange(&held_attach_code,0,0);}
static float player_pos[3];static volatile LONG player_frame=-100;   /* player model world position (classification seam), frame it was seen */
static unsigned (*frame_source)(void);
static unsigned cur_frame,cur_count,prev_count;   /* render thread only */
static char mark[MAX_PATH],status[MAX_PATH];static DWORD poll_at,report_at;static LONG reported=-1;
/* model log */
typedef struct {unsigned label,view,hits,changed,frame_first,frame_last;ULONG64 model,context,parts,mask;ULONG64 hist_mask[SCOPE_LOG_MASKS];unsigned hist_n[SCOPE_LOG_MASKS];BYTE raw[SCOPE_MODEL_BYTES];} LOG_ROW;
static LOG_ROW rows[SCOPE_LOG_ROWS];static volatile LONG nrows,log_lock,log_dropped,log_busy,log_label;
static LONG label_first[SCOPE_LOG_LABELS+1];static char labels[SCOPE_LOG_LABELS+1][24];static int nlabels;static char want_label[24];

/* Size of the texture bound at PS slot 0, 0x0 when none or not a 2D texture. */
static void slot0_dims(ID3D11DeviceContext *c,UINT *w,UINT *h){
 ID3D11ShaderResourceView *v=NULL;ID3D11Resource *res=NULL;ID3D11Texture2D *t=NULL;D3D11_TEXTURE2D_DESC d;*w=*h=0;
 c->lpVtbl->PSGetShaderResources(c,0,1,&v);if(!v)return;
 v->lpVtbl->GetResource(v,&res);v->lpVtbl->Release(v);if(!res)return;
 if(SUCCEEDED(res->lpVtbl->QueryInterface(res,&IID_ID3D11Texture2D,(void **)&t))){t->lpVtbl->GetDesc(t,&d);*w=d.Width;*h=d.Height;t->lpVtbl->Release(t);}
 res->lpVtbl->Release(res);
}
/* Called from the draw stub for every 6-vertex Draw of the executor. Non-zero = do not draw. */
/* Rifle at the eye (scope gesture, MGS3 recipe): the game's own scope picture is wanted then, so the
   mask, arms and showrifle rules step aside. NULL = never. */
static int (*vis_scope_open)(void);void mgs4vr_scope_set_gesture(int (*fn)(void)){vis_scope_open=fn;}
static int scoped_now(void){return vis_scope_open&&vis_scope_open();}
static int scope_skip(ID3D11DeviceContext *c,UINT count){
 int skip=0;
 InterlockedIncrement(&active);
 if(count==6 && (flags&F_MASK) && c && !scoped_now()){
  unsigned f=frame_source?frame_source():0;UINT w,h;
  if(f!=cur_frame){prev_count=f==cur_frame+1?cur_count:0;cur_count=0;cur_frame=f;}
  slot0_dims(c,&w,&h);
  if(w==16 && h==16){
   if((LONG)++cur_count>peak)InterlockedExchange(&peak,(LONG)cur_count);
   if(first_person && prev_count>=SCOPE_ON_FRAME){if(cur_count==1)InterlockedIncrement(&masked_frames);InterlockedIncrement(&skipped);skip=1;}
  }
 }
 InterlockedDecrement(&active);return skip;
}
/* One row per (label, part table, context, view); the record is kept as first seen. */
static void log_model(BYTE *model,unsigned view,BYTE *context,ULONG64 mask){
 LONG i,n,label=log_label;unsigned f=frame_source?frame_source():0,k;LOG_ROW *r=NULL;ULONG64 parts=*(ULONG64 *)(model+0xf0);
 if(InterlockedCompareExchange(&log_lock,1,0)){InterlockedIncrement(&log_busy);return;}
 n=nrows;
 for(i=label_first[label];i<n;++i)if(rows[i].label==(unsigned)label && rows[i].parts==parts && rows[i].context==(ULONG64)context && rows[i].view==view){r=&rows[i];break;}
 if(!r){
  if(n>=SCOPE_LOG_ROWS){InterlockedIncrement(&log_dropped);InterlockedExchange(&log_lock,0);return;}
  r=&rows[n];memset(r,0,sizeof(*r));r->label=(unsigned)label;r->view=view;r->frame_first=f;r->model=(ULONG64)model;r->context=(ULONG64)context;r->parts=parts;r->mask=mask;
  memcpy(r->raw,model,SCOPE_MODEL_BYTES);InterlockedExchange(&nrows,n+1);
 }
 ++r->hits;r->frame_last=f;if(r->mask!=mask){++r->changed;r->mask=mask;}
 for(k=0;k<SCOPE_LOG_MASKS;++k){if(r->hist_n[k] && r->hist_mask[k]==mask){++r->hist_n[k];break;}if(!r->hist_n[k]){r->hist_mask[k]=mask;r->hist_n[k]=1;break;}}
 InterlockedExchange(&log_lock,0);
}
/* 16 Oct 2026 (user: shoulders clip through the first-person camera): token fpbodyor=<hex> adds hidden parts to Snake's
   own body model (115 bones + player flags) in first person; measurement first, which part bit is the shoulders. */
static volatile LONG64 fp_body_or;static volatile LONG fp_body_masked;
/* Called from the parts stub for every model: the part mask the game is about to use. */
static ULONG64 scope_parts(BYTE *model,unsigned view,BYTE *context){
 ULONG64 mask;LONG f=flags;unsigned bones,frame;
 InterlockedIncrement(&active);
 mask=*(ULONG64 *)(model+0x80+8*(ULONG64)view);bones=*(unsigned *)(model+0xcc);frame=frame_source?frame_source():0;
 if(f&F_LOG)log_model(model,view,context,mask);
 if(first_person && (f&(F_ARMS|F_WEAPON))){
  if(mask==SCOPE_MASK_VIEW && bones==SCOPE_PLAYER_BONES && (f&F_ARMS) && !scoped_now()){InterlockedIncrement(&arms_shown);mask=SCOPE_MASK_FPS;}
  if((f&F_WEAPON) && bones!=SCOPE_PLAYER_BONES && near_player(model,frame))InterlockedIncrement(&near_parts);   /* how many near models get this far */
 }
 if(first_person && fp_body_or && bones==SCOPE_PLAYER_BONES && *(unsigned *)(model+0x78)==SCOPE_PLAYER_FLAGS){mask|=(ULONG64)fp_body_or;InterlockedIncrement(&fp_body_masked);}
 if(parts_hook)mask=parts_hook(model,bones,mask);                 /* CQC first person: hide the head (mgs4vr_cqcfp) */
 InterlockedDecrement(&active);return mask;
}
/* Called from the visibility stub with the game's decision for a model actor (1 = visible). */
static unsigned scope_vis_gate(BYTE *actor,unsigned decision){
 InterlockedIncrement(&active);
 if(is_held_attach((LONG64)(ULONG_PTR)actor)){
  InterlockedIncrement(&att_vis_seen);
  if(decision!=1){InterlockedIncrement(&att_vis_hidden);if((flags&F_VIS) && first_person && !scoped_now()){InterlockedIncrement(&att_vis_forced);decision=1;}}
 }
 if((LONG64)(ULONG_PTR)actor==held_model_actor && held_model_actor){
  InterlockedIncrement(&vis_seen);
  if(decision!=1){InterlockedIncrement(&vis_hidden);if((flags&F_VIS) && first_person && !scoped_now()){InterlockedIncrement(&vis_forced);decision=1;}}
 }
 InterlockedDecrement(&active);return decision;
}
/* Called from the gate stubs before a controller message from the scope component. Non-zero = do not deliver. */
static int scope_attach_gate(BYTE *weapon,unsigned msg){
 int skip=0;
 InterlockedIncrement(&active);
 if((LONG64)(ULONG_PTR)weapon==held_weapon){
  if(msg==1)InterlockedIncrement(&detach_seen);
  else if(msg==0){InterlockedIncrement(&attach_seen);if((flags&F_KEEP) && first_person){InterlockedIncrement(&attach_skipped);skip=1;}}
 }
 InterlockedDecrement(&active);return skip;
}
/* Called from the hide stub with the scope byte of a weapon: the value the hide bit gets. */
static unsigned scope_hide(unsigned value,BYTE *weapon){
 InterlockedIncrement(&active);
 if(value){
  InterlockedIncrement(&hide_seen);
  if((flags&F_GUN) && first_person && (LONG64)(ULONG_PTR)weapon==held_weapon){InterlockedIncrement(&gun_shown);value=0;}
 }
 InterlockedDecrement(&active);return value;
}
/* Within SCOPE_NEAR of the player model seen in the last two frames. */
static int near_player(BYTE *model,unsigned frame){
 const float *pos=(const float *)(model+0x50);float dx,dy,dz;
 if(frame-(unsigned)player_frame>SCOPE_CTX_FRAMES)return 0;
 dx=pos[0]-player_pos[0];dy=pos[1]-player_pos[1];dz=pos[2]-player_pos[2];
 return dx*dx+dy*dy+dz*dz<=SCOPE_NEAR*SCOPE_NEAR;
}
/* Called from the cull stub for every model, right after the game's bounds
   classification: the result to keep. */
static unsigned scope_cull(BYTE *model,BYTE *context,unsigned result){
 LONG f=flags;unsigned bones,frame;const float *pos=(const float *)(model+0x50);(void)context;
 InterlockedIncrement(&active);
 bones=*(unsigned *)(model+0xcc);frame=frame_source?frame_source():0;
 if(bones==SCOPE_PLAYER_BONES && *(unsigned *)(model+0x78)==SCOPE_PLAYER_FLAGS){player_pos[0]=pos[0];player_pos[1]=pos[1];player_pos[2]=pos[2];InterlockedExchange(&player_frame,(LONG)frame);}
 else if((result&1) && (f&F_WEAPON) && first_person){
  InterlockedIncrement(&culled_seen);
  if(near_player(model,frame)){InterlockedIncrement(&weapon_shown);result&=~1u;}
 }
 InterlockedDecrement(&active);return result;
}
/* Called from the second cull stub when the model's world bounds hit none of
   the context's visibility volumes. Non-zero = keep the model anyway. */
static int scope_cull2(BYTE *model,BYTE *context){
 LONG f=flags;int keep=0;unsigned frame;(void)context;
 InterlockedIncrement(&active);
 frame=frame_source?frame_source():0;
 if((f&F_WEAPON) && first_person && *(unsigned *)(model+0xcc)!=SCOPE_PLAYER_BONES){
  InterlockedIncrement(&culled2_seen);
  if(near_player(model,frame)){InterlockedIncrement(&weapon_shown2);keep=1;}
 }
 InterlockedDecrement(&active);return keep;
}
/* 0 original, 1 ours, 2 original but the d3d probe holds its breakpoint on the draw CALL, -1 anything else. */
static int seam_state(const SEAM *s){
 __try{
  if(!s->site)return -1;
  if(memcmp(s->site+s->len,s->sig+s->len,s->sig_len-s->len)){
   if(s!=&seams[0] || s->site[6]!=0xcc || s->site[7]!=s->sig[7] || s->site[8]!=s->sig[8])return -1;
   if(s->stub && !memcmp(s->site,s->patch,s->len))return 1;
   return memcmp(s->site,s->sig,s->len)?-1:2;
  }
  if(s->stub && !memcmp(s->site,s->patch,s->len))return 1;
  return memcmp(s->site,s->sig,s->len)?-1:0;
 }__except(EXCEPTION_EXECUTE_HANDLER){return -1;}
}
/* A page the CALL rel32 can reach. */
static BYTE *alloc_near(BYTE *target){
 SYSTEM_INFO si;MEMORY_BASIC_INFORMATION m;ULONG_PTR t=(ULONG_PTR)target,lo,hi,a;GetSystemInfo(&si);
 lo=t>0x70000000u?t-0x70000000u:(ULONG_PTR)si.lpMinimumApplicationAddress;hi=t+0x70000000u;
 if(lo<(ULONG_PTR)si.lpMinimumApplicationAddress)lo=(ULONG_PTR)si.lpMinimumApplicationAddress;
 for(a=lo;a<hi;){
  if(!VirtualQuery((void *)a,&m,sizeof(m)))break;
  if(m.State==MEM_FREE){
   ULONG_PTR p=((ULONG_PTR)m.BaseAddress+0xffff)&~(ULONG_PTR)0xffff;
   if(p>=lo && p+0x1000<=(ULONG_PTR)m.BaseAddress+m.RegionSize && p+0x1000<=hi){void *r=VirtualAlloc((void *)p,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(r)return (BYTE *)r;}
  }
  a=(ULONG_PTR)m.BaseAddress+m.RegionSize;
 }
 return NULL;
}
/* All stubs live in one page, written once. */
static int make_stubs(BYTE *base){
 BYTE *p;DWORD old;int i;LONG64 rel[SEAMS];
 if(stub_page)return 1;
 for(i=0;i<SEAMS;++i){int k;if(seams[i].stub_at+seams[i].stub_len>0x1000)return 0;for(k=0;k<SEAMS;++k)if(k!=i && seams[i].stub_at<seams[k].stub_at+seams[k].stub_len && seams[k].stub_at<seams[i].stub_at+seams[i].stub_len)return 0;}   /* stubs must not overlap */
 p=alloc_near(base+SCOPE_RVA);if(!p)return 0;
 for(i=0;i<SEAMS;++i){
  SEAM *s=&seams[i];BYTE *st=p+s->stub_at;
  memcpy(st,s->stub_code,s->stub_len);memcpy(st+s->fn_at,&s->fn,8);
  if(s->orig_rva){void *orig=base+s->orig_rva;memcpy(st+s->orig_at,&orig,8);}
  rel[i]=(LONG64)(ULONG_PTR)st-(LONG64)(ULONG_PTR)(base+s->rva+5);
  if(rel[i]>0x7fffffffLL || rel[i]<-0x7fffffffLL-1){VirtualFree(p,0,MEM_RELEASE);return 0;}
 }
 if(!VirtualProtect(p,0x1000,PAGE_EXECUTE_READ,&old)){VirtualFree(p,0,MEM_RELEASE);return 0;}
 FlushInstructionCache(GetCurrentProcess(),p,0x1000);
 for(i=0;i<SEAMS;++i){
  SEAM *s=&seams[i];int r32=(int)rel[i];static const BYTE nop3[3]={0x0f,0x1f,0x00};
  s->patch[0]=0xe8;memcpy(s->patch+1,&r32,4);
  if(s->len==6)s->patch[5]=0x90;else if(s->len==7){s->patch[5]=0x66;s->patch[6]=0x90;}else if(s->len==8)memcpy(s->patch+5,nop3,3);
  s->stub=p+s->stub_at;
 }
 stub_page=p;return 1;
}
/* Rewrites the seam's first bytes with every other thread suspended and none of
   them inside the replaced bytes or the stub page. 1 done, 0 busy (try again), -1 failed.
   Nothing here may take a lock a suspended thread could hold (no heap, no CRT I/O). */
static int rewrite(SEAM *s,const BYTE *bytes){
 static HANDLE th[SCOPE_THREADS];HANDLE snap;THREADENTRY32 te;DWORD pid=GetCurrentProcessId(),self=GetCurrentThreadId(),old,tmp;int n=0,i,busy=0,r=-1,overflow=0;
 snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snap==INVALID_HANDLE_VALUE)return -1;
 te.dwSize=sizeof(te);
 if(Thread32First(snap,&te))do{
  if(te.th32OwnerProcessID!=pid || te.th32ThreadID==self)continue;
  if(n==SCOPE_THREADS){overflow=1;break;}
  th[n]=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,te.th32ThreadID);if(th[n])++n;
 }while(Thread32Next(snap,&te));
 CloseHandle(snap);
 if(overflow){for(i=0;i<n;++i)CloseHandle(th[i]);return -1;}
 for(i=0;i<n;++i)if(SuspendThread(th[i])==(DWORD)-1){CloseHandle(th[i]);th[i]=NULL;}
 for(i=0;i<n;++i)if(th[i]){
  CONTEXT c;ULONG_PTR ip;c.ContextFlags=CONTEXT_CONTROL;
  if(!GetThreadContext(th[i],&c)){busy=1;continue;}
  ip=(ULONG_PTR)c.Rip;
  if((ip>(ULONG_PTR)s->site && ip<(ULONG_PTR)s->site+s->len+3) || (stub_page && ip>=(ULONG_PTR)stub_page && ip<(ULONG_PTR)stub_page+0x1000))busy=1;
 }
 if(active)busy=1;
 if(busy)r=0;
 else if(VirtualProtect(s->site,s->len,PAGE_EXECUTE_READWRITE,&old)){memcpy(s->site,bytes,s->len);VirtualProtect(s->site,s->len,old,&tmp);FlushInstructionCache(GetCurrentProcess(),s->site,s->len);r=1;}
 for(i=0;i<n;++i)if(th[i]){ResumeThread(th[i]);CloseHandle(th[i]);}
 return r;
}
/* Worker tick for "scopeflag": the held weapon through the identity chain. All reads are guarded. */
static int flag_read(ULONG64 a,void *out,unsigned n){
 MEMORY_BASIC_INFORMATION m;DWORD p;
 if(a<0x10000 || a+n<a || a+n>=0x800000000000ULL || !VirtualQuery((void *)(ULONG_PTR)a,&m,sizeof(m)))return 0;
 p=m.Protect&0xff;if(m.State!=MEM_COMMIT || (m.Protect&PAGE_GUARD) || a+n>(ULONG64)(ULONG_PTR)m.BaseAddress+m.RegionSize)return 0;
 if(!(p==PAGE_READONLY || p==PAGE_READWRITE || p==PAGE_WRITECOPY || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY))return 0;
 __try{memcpy(out,(void *)(ULONG_PTR)a,n);return 1;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int flag_write(ULONG64 a,unsigned v){
 MEMORY_BASIC_INFORMATION m;DWORD p;
 if(a<0x10000 || !VirtualQuery((void *)(ULONG_PTR)a,&m,sizeof(m)))return 0;
 p=m.Protect&0xff;if(m.State!=MEM_COMMIT || (m.Protect&PAGE_GUARD) || !(p==PAGE_READWRITE || p==PAGE_EXECUTE_READWRITE))return 0;
 __try{*(volatile unsigned *)(ULONG_PTR)a=v;return 1;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
static int flag_handle(ULONG64 h,ULONG64 *p){ULONG64 check;*p=h&0xfffffffffffffULL;return h && flag_read(*p+0x30,&check,8) && check==h;}
static ULONG64 flag_resolve(BYTE *base){
 int slot;ULONG64 owner,actor,data,weapon;
 if(!flag_read((ULONG64)(ULONG_PTR)base+SCOPE_SLOT_RVA,&slot,4) || slot< -1 || slot>23)return 0;
 if(!flag_read((ULONG64)(ULONG_PTR)base+SCOPE_OWNERS_RVA+(ULONG64)(slot+1)*0x80,&owner,8) || !flag_handle(owner,&actor))return 0;
 if(!flag_read(actor+0x90,&data,8) || !data || !flag_read(data+0xc8,&weapon,8) || !weapon)return 0;
 return weapon;
}
static void flag_tick(BYTE *base,int on){
 ULONG64 weapon=flag_resolve(base);unsigned state=0,value=0,id=0;
 if(!weapon){InterlockedExchange(&flag_state,-1);return;}   /* chain unreadable this tick: keep what we know */
 if(!flag_read(weapon+0x16c,&state,4) || !flag_read(weapon+0x3bc,&value,4) || !flag_read(weapon+0x70,&id,4)){InterlockedIncrement(&flag_faults);return;}
 InterlockedExchange64(&held_weapon,(LONG64)weapon);
 {ULONG64 mh=0,ma=0,ah=0,aa=0;unsigned short code=0;
  if(flag_read(weapon+0x68,&mh,8) && flag_handle(mh,&ma))InterlockedExchange64(&held_model_actor,(LONG64)ma);else InterlockedExchange64(&held_model_actor,0);
  /* 17 Oct 2026 (sim, Crescent Meridian save, M4 Custom via Weapons > Customize): the attachment is its own model actor,
     handle at weapon model actor +0x98 (none = 0), part code u16 at attachment +0xAA (DOT SIGHT 0x55, Operator suppressor
     0x4D, AK102 GP30 0x4C). The game drops a scope attachment before part processing in first person (model log: no
     attachment model at all), like the scoped rifle before showrifle: the visibility gate forces it too. */
  /* headset 17 Oct (swap via Customize: the game's own dot sight came back, the VR dot not): the attachments form a chain,
     each actor +0x98 -> the next (sim: rail/iron-sight piece code 1 first, then the dot 0x55). Walk it (at most
     ATT_MAX); the reported code is a sight (0x55 dot, else 0x54 scope) when there is one, else the first. */
  {ULONG64 cur=ma;int k,nat=0;unsigned short best=0;ULONG64 got[ATT_MAX]={0};
   for(k=0;k<ATT_MAX && cur;++k){ah=aa=0;code=0;if(!flag_read(cur+0x98,&ah,8) || !flag_handle(ah,&aa) || !flag_read(aa+0xaa,&code,2))break;
    got[nat++]=aa;if(code==0x55 || (code==0x54 && best!=0x55) || !best)best=code;cur=aa;}
   for(k=0;k<ATT_MAX;++k)InterlockedExchange64(&held_attach[k],(LONG64)got[k]);
   InterlockedExchange64(&held_attach_actor,(LONG64)got[0]);InterlockedExchange(&held_attach_code,best);InterlockedExchange(&held_attach_n,nat);}}
 if(weapon!=flag_weapon){if(flag_forced)InterlockedExchange(&flag_forced,0);flag_weapon=weapon;}   /* another weapon: the old one is the game's again */
 InterlockedExchange(&flag_state,(LONG)state);InterlockedExchange(&flag_value,(LONG)value);InterlockedExchange(&flag_weapon_id,(LONG)id);
 if(on && first_person && state==SCOPE_WEAPON_SCOPE_STATE){
  if(value==1){if(flag_write(weapon+0x3bc,0)){InterlockedIncrement(&flag_writes);InterlockedExchange(&flag_forced,1);}else InterlockedIncrement(&flag_faults);}
 }else if(flag_forced){
  if(value==0 && state==SCOPE_WEAPON_SCOPE_STATE){if(flag_write(weapon+0x3bc,1))InterlockedIncrement(&flag_restores);else InterlockedIncrement(&flag_faults);}
  InterlockedExchange(&flag_forced,0);
 }
}
static void report(const char *event){
 FILE *f;LONG fl=flags;if(!status[0] || fopen_s(&f,status,"w"))return;
 fprintf(f,"scope_version=7 event=%s mask=%d arms=%d weapon=%d scopeflag=%d log=%d draw_seam=%d parts_seam=%d cull_seam=%d cull2_seam=%d first_person=%ld skipped=%ld masked_frames=%ld peak_quads_per_frame=%ld threshold=%u arms_shown=%ld culled_seen=%ld weapon_shown=%ld culled2_seen=%ld weapon_shown2=%ld near_parts=%ld near=%.0f flag_weapon_id=%ld flag_state=%ld flag_value=%ld flag_forced=%ld flag_writes=%ld flag_restores=%ld flag_faults=%ld showgun=%d hide_seam=%d hide_seen=%ld gun_shown=%ld keepgun=%d attach_seams=%d,%d,%d attach_seen=%ld attach_skipped=%ld detach_seen=%ld showrifle=%d vis_seam=%d vis_seen=%ld vis_hidden=%ld vis_forced=%ld log_rows=%ld log_dropped=%ld log_busy=%ld busy_retries=%d,%d,%d,%d\n",
  event,!!(fl&F_MASK),!!(fl&F_ARMS),!!(fl&F_WEAPON),!!(fl&F_FLAG),!!(fl&F_LOG),seam_state(&seams[0]),seam_state(&seams[1]),seam_state(&seams[2]),seam_state(&seams[3]),first_person,skipped,masked_frames,peak,SCOPE_ON_FRAME,arms_shown,culled_seen,weapon_shown,culled2_seen,weapon_shown2,near_parts,SCOPE_NEAR,flag_weapon_id,flag_state,flag_value,flag_forced,flag_writes,flag_restores,flag_faults,!!(fl&F_GUN),seam_state(&seams[4]),hide_seen,gun_shown,!!(fl&F_KEEP),seam_state(&seams[5]),seam_state(&seams[6]),seam_state(&seams[7]),attach_seen,attach_skipped,detach_seen,!!(fl&F_VIS),seam_state(&seams[8]),vis_seen,vis_hidden,vis_forced,nrows,log_dropped,log_busy,seams[0].busy_retries,seams[1].busy_retries,seams[2].busy_retries,seams[3].busy_retries);
 fprintf(f,"attach_n=%ld ",held_attach_n);fprintf(f,"attach_code=%#lx attach_actor=%d att_vis_seen=%ld att_vis_hidden=%ld att_vis_forced=%ld\n",held_attach_code,held_attach_actor!=0,att_vis_seen,att_vis_hidden,att_vis_forced);
 fclose(f);
}
/* Exact tokens only; anything unknown turns every rule off. */
static unsigned read_marker(void){
 FILE *f;char b[96]={0},*tok,*ctx=NULL;unsigned e=0;size_t n;ULONG64 bor=0;want_label[0]=0;
 if(fopen_s(&f,mark,"rb")){InterlockedExchange64(&fp_body_or,0);return 0;}n=fread(b,1,sizeof(b)-1,f);fclose(f);b[n]=0;
 InterlockedExchange64(&fp_body_or,0);
 for(tok=strtok_s(b," \t\r\n",&ctx);tok;tok=strtok_s(NULL," \t\r\n",&ctx)){
  if(!strncmp(tok,"fpbodyor=",9)){bor=_strtoui64(tok+9,NULL,16);InterlockedExchange64(&fp_body_or,(LONG64)bor);continue;}   /* measurement: extra hidden parts on Snake in first person */
  if(!strcmp(tok,"nomask"))e|=F_MASK;else if(!strcmp(tok,"arms"))e|=F_ARMS;else if(!strcmp(tok,"weapon"))e|=F_WEAPON;else if(!strcmp(tok,"scopeflag"))e|=F_FLAG;else if(!strcmp(tok,"showgun"))e|=F_GUN;else if(!strcmp(tok,"keepgun"))e|=F_KEEP;else if(!strcmp(tok,"showrifle"))e|=F_VIS;
  else if(!strncmp(tok,"log=",4) && tok[4] && strlen(tok+4)<sizeof(want_label)){
   const char *p;for(p=tok+4;*p;++p)if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || *p=='_'))return 0;
   strcpy_s(want_label,sizeof(want_label),tok+4);e|=F_LOG;
  }else return 0;
 }
 return e;
}
static void dump_models(void){
 FILE *f;char path[MAX_PATH];LONG i,n;unsigned k;int spins=0;
 while(InterlockedCompareExchange(&log_lock,1,0)){if(++spins>2000)return;Sleep(1);}
 strcpy_s(path,sizeof(path),status);sprintf_s(strrchr(path,'\\')+1,64,"mgs4vr_scope_models_%lu.csv",GetCurrentProcessId());
 if(!fopen_s(&f,path,"w")){
  fputs("label,name,model,context,parts,view,mask,hits,changed,frame_first,frame_last,bones,flags78,flagsc0,state_dc,part_count,masks,raw\n",f);
  for(i=0,n=nrows;i<n;++i){LOG_ROW *r=&rows[i];
   fprintf(f,"%u,%s,%llx,%llx,%llx,%u,%llx,%u,%u,%u,%u,%u,%x,%x,%x,%u,",r->label,labels[r->label],r->model,r->context,r->parts,r->view,r->mask,r->hits,r->changed,r->frame_first,r->frame_last,
    *(unsigned *)(r->raw+0xcc),*(unsigned *)(r->raw+0x78),*(unsigned *)(r->raw+0xc0),*(unsigned *)(r->raw+0xdc),*(unsigned *)(r->raw+0xc8));
   for(k=0;k<SCOPE_LOG_MASKS && r->hist_n[k];++k)fprintf(f,"%s%llx:%u",k?" ":"",r->hist_mask[k],r->hist_n[k]);fputc(',',f);
   for(k=0;k<SCOPE_MODEL_BYTES;++k)fprintf(f,"%02x",r->raw[k]);
   fputc('\n',f);
  }
  fclose(f);
 }
 InterlockedExchange(&log_lock,0);
}
/* Label of the running log window; a label used before continues its own rows. */
static int select_label(void){
 int i;for(i=1;i<=nlabels;++i)if(!strcmp(labels[i],want_label))break;
 if(i>nlabels){if(nlabels>=SCOPE_LOG_LABELS)return 0;i=++nlabels;strcpy_s(labels[i],sizeof(labels[i]),want_label);label_first[i]=nrows;}
 InterlockedExchange(&log_label,i);return 1;
}
static int install(SEAM *s,BYTE *base){
 int st,r;
 s->site=base+s->rva;st=seam_state(s);
 if(st==2)return 0;                                                 /* the probe owns the draw CALL right now; later */
 if(st!=0 || !make_stubs(base)){s->rejected=1;report(st!=0?"reject_signature":"reject_stub");return 0;}
 r=rewrite(s,s->patch);
 if(!r){if(++s->busy_retries>=200){s->rejected=1;report("reject_busy");}return 0;}
 if(r<0 || seam_state(s)!=1){s->rejected=1;report("reject_patch");return 0;}
 InterlockedExchange(&s->installed,1);return 1;
}
void mgs4vr_scope_poll(void *base,const char *exe,int fp){
 DWORD now=GetTickCount();char *leaf;unsigned want,have=0;LONG old=flags;int fresh=0;
 InterlockedExchange(&first_person,!!fp);
 if(!mark[0]){
  strcpy_s(mark,sizeof(mark),exe);leaf=strrchr(mark,'\\');if(!leaf){mark[0]=0;return;}
  strcpy_s(leaf+1,sizeof(mark)-(size_t)(leaf+1-mark),"mgs4vr_scope.on");
  strcpy_s(status,sizeof(status),mark);sprintf_s(strrchr(status,'\\')+1,64,"mgs4vr_scope_%lu.txt",GetCurrentProcessId());
 }else if(now-poll_at<SCOPE_POLL_MS){if(base)flag_tick((BYTE *)base,!!(flags&F_FLAG));return;}   /* the flag rule runs every worker tick */
 poll_at=now;want=read_marker();
 if(!base)return;
 have|=want&F_FLAG;
 if((want&F_MASK) && !seams[0].installed && !seams[0].rejected)fresh|=install(&seams[0],(BYTE *)base);
 if((want&(F_ARMS|F_LOG)) && !seams[1].installed && !seams[1].rejected)fresh|=install(&seams[1],(BYTE *)base);
 if((want&F_WEAPON) && !seams[2].installed && !seams[2].rejected)fresh|=install(&seams[2],(BYTE *)base);
 if((want&F_WEAPON) && !seams[3].installed && !seams[3].rejected)fresh|=install(&seams[3],(BYTE *)base);
 if((want&F_GUN) && !seams[4].installed && !seams[4].rejected)fresh|=install(&seams[4],(BYTE *)base);
 if(seams[4].installed)have|=want&F_GUN;
 {int k;for(k=5;k<8;++k)if((want&F_KEEP) && !seams[k].installed && !seams[k].rejected)fresh|=install(&seams[k],(BYTE *)base);}
 if(seams[5].installed && seams[6].installed && seams[7].installed)have|=want&F_KEEP;
 if((want&F_VIS) && !seams[8].installed && !seams[8].rejected)fresh|=install(&seams[8],(BYTE *)base);
 if(seams[8].installed)have|=want&F_VIS;
 if(seams[0].installed)have|=want&F_MASK;
 if(seams[2].installed && seams[3].installed)have|=want&F_WEAPON;
 if(seams[1].installed){have|=want&F_ARMS;if((want&F_LOG) && (((old&F_LOG) && !strcmp(labels[log_label],want_label)) || (!(old&F_LOG) && select_label())))have|=F_LOG;}
 if((LONG)have!=old || fresh){
  InterlockedExchange(&flags,(LONG)have);
  if((old&F_LOG) && !(have&F_LOG))dump_models();
  report(fresh?"installed":have?"enabled":"disabled");report_at=now;reported=skipped+arms_shown+weapon_shown+weapon_shown2+flag_writes+flag_restores+gun_shown+attach_seen+detach_seen+vis_seen;flag_tick((BYTE *)base,!!(have&F_FLAG));return;
 }
 flag_tick((BYTE *)base,!!(have&F_FLAG));
 if(have && now-report_at>=SCOPE_REPORT_MS && reported!=skipped+arms_shown+weapon_shown+weapon_shown2+flag_writes+flag_restores+gun_shown+attach_seen+detach_seen+vis_seen){report_at=now;reported=skipped+arms_shown+weapon_shown+weapon_shown2+flag_writes+flag_restores+gun_shown+attach_seen+detach_seen+vis_seen;report("active");}
}
void mgs4vr_scope_set_frame_source(unsigned (*fn)(void)){frame_source=fn;}
/* Desk tests only: the game never takes a seam out again. A thread between a
   stub's CALL and the C function's first instruction is not seen by rewrite(). */
/* Game zoom of the held weapon while it is in scope view (weapon+0x3BC scope byte = 1, weapon+0x3C8 zoom,
   3.0 for the M82A2/DSR-1/XM25/Javelin in the 12 Oct sim); 0 otherwise. Worker thread, guarded read. */
void *mgs4vr_scope_held_weapon(void){return (void *)(ULONG_PTR)held_weapon;}
float mgs4vr_scope_zoom(void){
 BYTE *w=(BYTE *)(ULONG_PTR)held_weapon;float z=0;BYTE b=0;
 if(!w)return 0;
 __try{b=w[0x3bc];z=*(float *)(w+0x3c8);}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
 return (b==1&&isfinite(z)&&z>=1.5f&&z<=40.f)?z:0;
}
int mgs4vr_scope_stop(void){
 int i,k,r;LONG old=InterlockedExchange(&flags,0);
 if(old&F_LOG)dump_models();
 if(flag_forced && flag_weapon){unsigned v=0,st=0;if(flag_read(flag_weapon+0x3bc,&v,4) && flag_read(flag_weapon+0x16c,&st,4) && v==0 && st==SCOPE_WEAPON_SCOPE_STATE && flag_write(flag_weapon+0x3bc,1))InterlockedIncrement(&flag_restores);InterlockedExchange(&flag_forced,0);}
 for(k=0;k<SEAMS;++k)if(seams[k].installed){r=0;for(i=0;i<50 && (r=rewrite(&seams[k],seams[k].sig))==0;++i)Sleep(1);if(r!=1)return 0;InterlockedExchange(&seams[k].installed,0);}
 report("stopped");return 1;
}
