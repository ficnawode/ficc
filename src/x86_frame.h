#ifndef FICC_X86_FRAME_H
#define FICC_X86_FRAME_H

#include "ir.h"
#include "regalloc.h"
#include "target.h"
#include "util/arena.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "x86_emit.h"

/* Concrete prologue layout for the register-allocating backend.  Spill slots
   sit below the pushed callee registers, and params stage through a scratch
   area so a register home never clobbers an incoming argument register. */
typedef struct
{
    u32 frame_size;    /* bytes reserved by `sub rsp` */
    u32 saved_bytes;   /* bytes of pushed callee-saved registers (16-aligned) */
    u32 stage_base;    /* displacement of the first param staging slot; 0 with no params */
    u32 save_area_off; /* variadic register save area; 0 when not variadic */
    u8 saved_regs[16];
    u8 nsaved;
    u32 off_push;  /* byte just past `push rbp` */
    u32 off_mov;   /* byte just past `mov rbp, rsp` */
    u32 off_sub;   /* byte just past `sub rsp, N` (prologue end) */
    bool omit_fp;  /* no %rbp frame: saved registers ride `push`/`pop` alone */
    i32 disp_bias; /* %rbp displacement plus this, then based on %rsp (omit_fp only) */
} LinearFrame;

/* A frame slot: %rbp-relative normally, %rsp-relative when the frame omits %rbp. */
X86Mem x86_frame_mem(const LinearFrame *frame, i32 disp);

/* Shift spill slots past the pushed registers and size the frame.  Mutates the
   allocation's slot table so loc_of resolves against the final %rbp layout.
   With `omit_fp` the frame is addressed from %rsp instead and holds only what
   the prologue needs (call alignment, phi scratch, static allocas). */
void x86_frame_plan(RegAllocation *alloc, IrFunction *f, const TargetDesc *target, bool debug,
                    bool omit_fp, LinearFrame *out);

/* Whether the frame pointer can be dropped: no debug frame or variadic save
   area, every parameter in a register, and %rsp staying put after the prologue
   (no stack-argument call, no x87 staging). */
bool x86_frame_can_omit_fp(IrModule *mod, IrFunction *f, bool debug);

void x86_frame_emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             LinearFrame *frame, bool debug);
void x86_frame_restore_callee(ByteBuf *buf, const LinearFrame *frame);

/* Per-parameter stage-slot offset below %rbp; 0 when the parameter stages nowhere. */
void x86_frame_param_stages(IrFunction *f, const LinearFrame *frame, bool debug, u32 *out);

#endif
