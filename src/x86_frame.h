#ifndef FICC_X86_FRAME_H
#define FICC_X86_FRAME_H

#include "ir.h"
#include "regalloc.h"
#include "target.h"
#include "util/arena.h"
#include "util/bytebuf.h"
#include "util/types.h"

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
    u32 off_push; /* byte just past `push rbp` */
    u32 off_mov;  /* byte just past `mov rbp, rsp` */
    u32 off_sub;  /* byte just past `sub rsp, N` (prologue end) */
    bool omit_fp; /* no %rbp frame: saved registers ride `push`/`pop` alone */
} LinearFrame;

/* Shift spill slots past the pushed registers and size the frame.  Mutates the
   allocation's slot table so loc_of resolves against the final %rbp layout.
   With `omit_fp` the frame has no memory slots (the allocator spilled nothing)
   and %rbp is not used as the base. */
void x86_frame_plan(RegAllocation *alloc, IrFunction *f, const TargetDesc *target, bool debug,
                    bool omit_fp, LinearFrame *out);

/* Whether the frame pointer can be dropped (no debug frame, no variadic/alloca/
   phi, every parameter in a register); the allocator must also spill nothing. */
bool x86_frame_can_omit_fp(IrFunction *f, bool debug);

void x86_frame_emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             LinearFrame *frame, bool debug);
void x86_frame_restore_callee(ByteBuf *buf, const LinearFrame *frame);

/* Per-parameter stage-slot offset below %rbp; 0 when the parameter stages nowhere. */
void x86_frame_param_stages(IrFunction *f, const LinearFrame *frame, bool debug, u32 *out);

#endif
