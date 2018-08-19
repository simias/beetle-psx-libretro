#ifndef __DYNAREC_COMPILER_H__
#define __DYNAREC_COMPILER_H__

#include "dynarec.h"

#ifdef DYNAREC_ARCH_AMD64
# include "dynarec-amd64.h"
#endif

enum DYNAREC_JUMP_COND {
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

struct dynarec_page_local_patch {
   /* Location of the instruction that needs patching */
   uint8_t *patch_loc;
   /* Address of the target PSX instruction */
   uint32_t target;
};

/* Structure holding the temporary variables during the recompilation
   sequence */
struct dynarec_compiler {
   struct dynarec_state *state;
   /* Pointer to the location where the next recompiled instruction
      will be written. */
   uint8_t *map;
   /* Current value of the PC */
   uint32_t pc;
   /* Index of the page currently being recompiled */
   uint32_t page_index;
   /* Pointer towards the dynarec's `dynarec_instruction` array for
      the current page */
   void   **dynarec_instructions;
   /* When recompiling a jump this contains the target address in PSX
      memory. */
   uint32_t jump_target;
   /* Number of entries in local_patch */
   uint32_t local_patch_len;
   /* Contains offset of instructions that need patching */
   struct dynarec_page_local_patch local_patch[DYNAREC_PAGE_INSTRUCTIONS];
};

typedef uint32_t (*dynarec_fn_t)(void);

extern int dynarec_recompile(struct dynarec_state *state,
                             uint32_t page_index);

extern void dynarec_prepare_patch(struct dynarec_compiler *compiler);

/* These methods are provided by the various architecture-dependent
   backends */
extern void dynasm_counter_maintenance(struct dynarec_compiler *compiler,
                                       unsigned cycles);
extern void dynasm_patch(struct dynarec_compiler *compiler, int32_t offset);
extern uint32_t dynasm_execute(struct dynarec_state *state,
                               dynarec_fn_t target,
                               int32_t counter);
extern void dynasm_emit_exception(struct dynarec_compiler *compiler,
                                  enum PSX_CPU_EXCEPTION exception);
extern void dynasm_emit_exit(struct dynarec_compiler *compiler,
                             unsigned code,
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
extern void dynasm_emit_lw(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           int16_t offset,
                           enum PSX_REG reg_addr);
extern void dynasm_emit_lbu(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            int16_t offset,
                            enum PSX_REG reg_addr);
extern void dynasm_emit_page_local_jump(struct dynarec_compiler *compiler,
                                        uint8_t *dynarec_target,
                                        bool placeholder);
extern void dynasm_emit_page_local_jump_cond(struct dynarec_compiler *compiler,
                                             uint8_t *dynarec_target,
                                             bool placeholder,
                                             enum PSX_REG reg_a,
                                             enum PSX_REG reg_b,
                                             enum DYNAREC_JUMP_COND cond);
extern void dynasm_emit_long_jump_imm(struct dynarec_compiler *compiler,
                                      uint32_t target);
extern void dynasm_emit_long_jump_imm_cond(struct dynarec_compiler *compiler,
                                           uint32_t target,
                                           enum PSX_REG reg_a,
                                           enum PSX_REG reg_b,
                                           enum DYNAREC_JUMP_COND cond);
extern void dynasm_emit_mfc0(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_target,
                             enum PSX_COP0_REG reg_cop0);
extern void dynasm_emit_mtc0(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_source,
                             enum PSX_COP0_REG reg_cop0);

#endif /* __DYNAREC_COMPILER_H__ */
