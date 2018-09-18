#ifndef __DYNAREC_COMPILER_H__
#define __DYNAREC_COMPILER_H__

#include "dynarec.h"

#ifdef DYNAREC_ARCH_AMD64
# include "dynarec-amd64.h"
#endif

enum dynarec_jump_cond {
   /* Unconditional jump */
   DYNAREC_JUMP_ALWAYS = 0,
   /* Jump if registers aren't equal */
   DYNAREC_JUMP_NE,
   /* Jump if registers are equal */
   DYNAREC_JUMP_EQ,
   /* Jump if register a is greater or equal to register b */
   DYNAREC_JUMP_GE,
   /* Jump if register a is less than register b */
   DYNAREC_JUMP_LT,
};

/* Get the offset of the location of a register within a struct
   dynarec_state. */
#define DYNAREC_STATE_REG_OFFSET(_r)                                    \
   (assert((_r) != PSX_REG_R0),                                         \
    offsetof(struct dynarec_state, regs) + ((_r) - 1) * sizeof(uint32_t))

/* Structure holding the temporary variables during the recompilation
   sequence */
struct dynarec_compiler {
   struct dynarec_state *state;
   /* Pointer to the location where the next recompiled instruction
      will be written. */
   uint8_t *map;
   /* Current value of the PC */
   uint32_t pc;
   /* Current block */
   struct dynarec_block *block;
   /* Cycles spent emulating the current block so far */
   uint32_t spent_cycles;
};

typedef uint32_t (*dynarec_fn_t)(void);

extern int dynarec_compiler_init(struct dynarec_state *state);
extern struct dynarec_block *dynarec_recompile(struct dynarec_state *state,
                                               uint32_t page_index);
extern void *dynarec_recompile_and_patch(struct dynarec_state *state,
                                         uint32_t target,
                                         uint32_t patch_offset);

/* These methods are provided by the various architecture-dependent
   backends */
extern void dynasm_emit_block_prologue(struct dynarec_compiler *compiler);
extern struct dynarec_ret dynasm_execute(struct dynarec_state *state,
                                         dynarec_fn_t target,
                                         int32_t counter);
extern void dynasm_emit_link_trampoline(struct dynarec_compiler *compiler);
extern void dynasm_patch_link(struct dynarec_compiler *compiler,
                              void *link);
extern void dynasm_emit_exception(struct dynarec_compiler *compiler,
                                  enum psx_cpu_exception exception);
extern void dynasm_emit_exit(struct dynarec_compiler *compiler,
                             enum dynarec_exit code,
                             unsigned val);
extern void dynasm_emit_li(struct dynarec_compiler *compiler,
                           enum PSX_REG reg,
                           uint32_t val);
extern void dynasm_emit_mov(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_source);
extern void dynasm_emit_sll(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op,
                            uint8_t shift);
extern void dynasm_emit_srl(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op,
                            uint8_t shift);
extern void dynasm_emit_sra(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op,
                            uint8_t shift);
extern void dynasm_emit_multu(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_op0,
                              enum PSX_REG reg_op1);
extern void dynasm_emit_div(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_n,
                            enum PSX_REG reg_d);
extern void dynasm_emit_divu(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_n,
                             enum PSX_REG reg_d);
extern void dynasm_emit_addi(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s,
                             uint32_t val);
extern void dynasm_emit_addiu(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_t,
                              enum PSX_REG reg_s,
                              uint32_t val);
extern void dynasm_emit_neg(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_REG reg_src);
extern void dynasm_emit_subu(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_REG reg_op0,
                             enum PSX_REG reg_op1);
extern void dynasm_emit_add(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op0,
                            enum PSX_REG reg_op1);
extern void dynasm_emit_addu(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_REG reg_op0,
                             enum PSX_REG reg_op1);
extern void dynasm_emit_or(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           enum PSX_REG reg_op0,
                           enum PSX_REG reg_op1);
extern void dynasm_emit_ori(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_t,
                            enum PSX_REG reg_s,
                            uint32_t val);
extern void dynasm_emit_and(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op0,
                            enum PSX_REG reg_op1);
extern void dynasm_emit_andi(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s,
                             uint32_t val);
extern void dynasm_emit_slt(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op0,
                            enum PSX_REG reg_op1);
extern void dynasm_emit_sltu(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_REG reg_op0,
                             enum PSX_REG reg_op1);
extern void dynasm_emit_slti(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s,
                             int32_t val);
extern void dynasm_emit_sltiu(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_t,
                              enum PSX_REG reg_s,
                              uint32_t val);
extern void dynasm_emit_sw(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_addr,
                           int16_t offset,
                           enum PSX_REG reg_val);
extern void dynasm_emit_sh(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_addr,
                           int16_t offset,
                           enum PSX_REG reg_val);
extern void dynasm_emit_sb(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_addr,
                           int16_t offset,
                           enum PSX_REG reg_val);
extern void dynasm_emit_lb(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           int16_t offset,
                           enum PSX_REG reg_addr);
extern void dynasm_emit_lbu(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            int16_t offset,
                            enum PSX_REG reg_addr);
extern void dynasm_emit_lh(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           int16_t offset,
                           enum PSX_REG reg_addr);
extern void dynasm_emit_lhu(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            int16_t offset,
                            enum PSX_REG reg_addr);
extern void dynasm_emit_lw(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           int16_t offset,
                           enum PSX_REG reg_addr);
extern void dynasm_emit_jump_reg(struct dynarec_compiler *compiler,
                                 enum PSX_REG reg_target,
                                 enum PSX_REG reg_link,
                                 void *link);
extern void dynasm_emit_jump_imm(struct dynarec_compiler *compiler,
                                 uint32_t target,
                                 void *link,
                                 bool needs_patch);
extern void dynasm_emit_jump_imm_cond(struct dynarec_compiler *compiler,
                                      uint32_t target,
                                      void *link,
                                      bool needs_patch,
                                      enum PSX_REG reg_a,
                                      enum PSX_REG reg_b,
                                      enum dynarec_jump_cond cond);
extern void dynasm_emit_mfc0(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_COP0_REG reg_cop0);
extern void dynasm_emit_mtc0(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_source,
                             enum PSX_COP0_REG reg_cop0);

#endif /* __DYNAREC_COMPILER_H__ */
