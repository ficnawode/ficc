#ifndef FICC_X86_FRAME_H
#define FICC_X86_FRAME_H

#include "ir.h"
#include "regalloc.h"
#include "target.h"
#include "util/arena.h"
#include "util/bytebuf.h"
#include "util/types.h"
#include "x86_emit.h"

typedef struct
{
    u32 frame_size;
    u32 saved_bytes;
    u32 stage_base;
    u32 save_area_off;
    u8 saved_regs[16];
    u8 nsaved;
    u32 off_push;
    u32 off_mov;
    u32 off_sub;
    bool omit_fp;
    i32 disp_bias;
} LinearFrame;

X86Mem x86_frame_mem(const LinearFrame *frame, i32 disp);

void x86_frame_plan(RegAllocation *alloc, IrFunction *f, const TargetDesc *target, bool debug,
                    bool omit_fp, LinearFrame *out);

bool x86_frame_can_omit_fp(IrModule *mod, IrFunction *f, bool debug);

void x86_frame_emit_prologue(ByteBuf *buf, IrFunction *f, IrModule *mod, const RegAllocation *alloc,
                             LinearFrame *frame, bool debug);
void x86_frame_restore_callee(ByteBuf *buf, const LinearFrame *frame);

void x86_frame_param_stages(IrFunction *f, const LinearFrame *frame, bool debug, u32 *out);

#endif
