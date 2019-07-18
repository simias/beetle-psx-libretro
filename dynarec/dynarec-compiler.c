#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include "dynarec-compiler.h"
#include "dynarec-jit-debugger.h"
#include "psx-instruction.h"

enum optype {
   /* Instruction with no effect */
   OP_NOP,
   /* Anything that doesn't fit any of the other types */
   OP_SIMPLE,
   /* Unconditional branch jump (we're shure that the control will
      leave the block) */
   OP_BRANCH_ALWAYS,
   /* Conditional branch: may or may not be taken at runtime */
   OP_BRANCH_COND,
   /* Exception: no delay slot but execution leaves the block */
   OP_EXCEPTION,
   /* Load instruction, followed by a load delay slot. */
   OP_LOAD,
   /* Load instruction that combines with the previous load if we're
      in a delay slot (for lwl/lwr) */
   OP_LOAD_COMBINE,
   /* For SWL/SWR, unaligned store instructions */
   OP_STORE_NOALIGN,
};

struct opdesc {
   uint32_t instruction;
   enum optype type;
   enum PSX_REG target;
   enum PSX_REG op0;
   enum PSX_REG op1;
   union {
      int32_t isigned;
      uint32_t  iunsigned;
   } imm;
};

static void emit_branch_or_jump(struct dynarec_compiler *compiler,
                                uint32_t target,
                                enum PSX_REG reg_a,
                                enum PSX_REG reg_b,
                                enum dynarec_jump_cond cond) {
   struct dynarec_block *b;
   bool needs_patch;
   void *link;

   if (target == compiler->block->base_address) {
      /* This is a jump back to ourselves */
      b = compiler->block;
   } else {
      b = dynarec_find_block(compiler->state, target);
   }

#ifdef DYNAREC_NO_PATCH
   b = NULL;
#endif

   if (b) {
      /* The target has already been recompiled, we can link it directly */
      needs_patch = false;
      link = dynarec_block_code(b);
   } else {
      /* We don't know the target, use a placeholder */
      needs_patch = true;
      link = compiler->state->link_trampoline;
   }

   if (cond == DYNAREC_JUMP_ALWAYS) {
      dynasm_emit_jump_imm(compiler, target, link, needs_patch);
   } else {
      dynasm_emit_jump_imm_cond(compiler,
                                target,
                                link,
                                needs_patch,
                                reg_a,
                                reg_b,
                                cond);
   }
}

static void emit_jump(struct dynarec_compiler *compiler,
                      uint32_t target) {

   emit_branch_or_jump(compiler, target, 0, 0, DYNAREC_JUMP_ALWAYS);
}

static void emit_j(struct dynarec_compiler *compiler,
                   struct opdesc *op) {
   uint32_t imm_jump = op->imm.iunsigned;
   uint32_t target;

   target = compiler->pc & 0xf0000000;
   target |= imm_jump;

   emit_jump(compiler, target);
}

static void emit_jal(struct dynarec_compiler *compiler,
                     struct opdesc *op) {
   /* Store return address in RA */
   dynasm_emit_li(compiler, PSX_REG_RA, compiler->pc + 8);
   emit_j(compiler, op);
}

static void emit_jalr(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_link) {
   if (reg_target == PSX_REG_R0) {
      if (reg_link != PSX_REG_R0) {
         dynasm_emit_li(compiler, PSX_REG_RA, compiler->pc + 8);
      }
      emit_jump(compiler, 0);
   } else {
      dynasm_emit_jump_reg(compiler,
                           reg_target,
                           reg_link,
                           compiler->state->link_trampoline);
   }
}

static void emit_branch(struct dynarec_compiler *compiler,
                        int16_t offset,
                        enum PSX_REG reg_a,
                        enum PSX_REG reg_b,
                        enum dynarec_jump_cond cond) {
   /* Offset is always in words (or instructions) */
   uint32_t off = ((int32_t)offset) << 2;

   /* Offset is relative to the next instruction (the branch delay
      slot) so we need to add 4 bytes */
   uint32_t target = compiler->pc + 4 + off;

   emit_branch_or_jump(compiler, target, reg_a, reg_b, cond);
}

static void emit_bxx(struct dynarec_compiler *compiler,
                     struct opdesc *op) {
   enum dynarec_jump_cond cond;
   int16_t offset = op->imm.isigned;
   enum PSX_REG reg_link = op->target;
   enum PSX_REG reg_op = op->op0;
   bool is_bgez = (op->instruction >> 16) & 1;

   if (reg_link != PSX_REG_R0) {
      /* Store return address. This is done unconditionally even if
         the branch is not taken. */
      dynasm_emit_li(compiler, reg_link, compiler->pc + 8);
   }

   if (is_bgez) {
      cond = DYNAREC_JUMP_GE;
   } else {
      cond = DYNAREC_JUMP_LT;
   }

   emit_branch(compiler, offset, PSX_REG_R0, reg_op, cond);
}

static void emit_beq(struct dynarec_compiler *compiler,
                     int16_t offset,
                     enum PSX_REG reg_a,
                     enum PSX_REG reg_b) {
   enum dynarec_jump_cond cond = DYNAREC_JUMP_EQ;

   if (reg_a == reg_b) {
      cond = DYNAREC_JUMP_ALWAYS;
   }

   emit_branch(compiler, offset, reg_a, reg_b, cond);
}

static void emit_bne(struct dynarec_compiler *compiler,
                     int16_t offset,
                     enum PSX_REG reg_a,
                     enum PSX_REG reg_b) {
   if (reg_a == reg_b) {
      /* NOP */
      return;
   }

   emit_branch(compiler, offset, reg_a, reg_b, DYNAREC_JUMP_NE);
}

static void emit_blez(struct dynarec_compiler *compiler,
                      int16_t offset,
                      enum PSX_REG reg_op) {
   enum dynarec_jump_cond cond;

   if (reg_op == PSX_REG_R0) {
      cond = DYNAREC_JUMP_ALWAYS;
   } else {
      cond = DYNAREC_JUMP_GE;
   }

   emit_branch(compiler, offset, reg_op, PSX_REG_R0, cond);
}

static void emit_bgtz(struct dynarec_compiler *compiler,
                      int16_t offset,
                      enum PSX_REG reg_op) {
   enum dynarec_jump_cond cond;

   if (reg_op == PSX_REG_R0) {
      /* NOP */
      return;
   } else {
      cond = DYNAREC_JUMP_LT;
   }

   emit_branch(compiler, offset, reg_op, PSX_REG_R0, cond);
}

typedef void (*shift_imm_emit_fn_t)(struct dynarec_compiler *compiler,
                                    enum PSX_REG reg_target,
                                    enum PSX_REG reg_source,
                                    uint8_t shift);

static void emit_shift_imm(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           enum PSX_REG reg_source,
                           uint8_t shift,
                           shift_imm_emit_fn_t emit_fn) {
   if (reg_target == 0 || (reg_target == reg_source && shift == 0)) {
      /* NOP */
      return;
   }

   if (reg_source == 0) {
      dynasm_emit_li(compiler, reg_target, 0);
      return;
   }

   if (shift == 0) {
      dynasm_emit_mov(compiler, reg_target, reg_source);
      return;
   }

   emit_fn(compiler, reg_target, reg_source, shift);
}

typedef void (*shift_reg_emit_fn_t)(struct dynarec_compiler *compiler,
                                    enum PSX_REG reg_target,
                                    enum PSX_REG reg_source,
                                    enum PSX_REG reg_shift);


static void emit_shift_reg(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           enum PSX_REG reg_source,
                           enum PSX_REG reg_shift,
                           shift_reg_emit_fn_t emit_fn) {
   if (reg_target == 0 ||
       (reg_target == reg_source && reg_shift == PSX_REG_R0)) {
      /* NOP */
      return;
   }

   if (reg_source == PSX_REG_R0) {
      dynasm_emit_li(compiler, reg_target, 0);
      return;
   }

   if (reg_shift == PSX_REG_R0) {
      dynasm_emit_mov(compiler, reg_target, reg_source);
      return;
   }

   emit_fn(compiler, reg_target, reg_source, reg_shift);
}

static void emit_addi(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_source,
                      uint32_t imm) {
   if (reg_source == 0) {
      if (reg_target != PSX_REG_R0) {
         dynasm_emit_li(compiler, reg_target, imm);
      }
      return;
   }

   if (imm == 0) {
      if (reg_target != reg_source) {
         dynasm_emit_mov(compiler, reg_target, reg_source);
      }
      return;
   }

   /* Watch out: we have to call this even if reg_target is R0 because
      it might still raise an exception so unlike ADDIU it's not a NOP
      in this case. */
   dynasm_emit_addi(compiler, reg_target, reg_source, imm);
}

static void emit_addiu(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_source,
                     uint32_t imm) {
   if (reg_target == 0) {
      /* NOP */
      return;
   }

   if (reg_source == 0) {
      dynasm_emit_li(compiler, reg_target, imm);
      return;
   }

   if (imm == 0) {
      if (reg_target != reg_source) {
         dynasm_emit_mov(compiler, reg_target, reg_source);
      }
      return;
   }

   dynasm_emit_addiu(compiler, reg_target, reg_source, imm);
}

static void emit_andi(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_source,
                     uint16_t imm) {
   if (reg_target == 0) {
      /* NOP */
      return;
   }

   if (imm == 0 || reg_source == 0) {
      dynasm_emit_li(compiler, reg_target, 0);
      return;
   }

   dynasm_emit_andi(compiler, reg_target, reg_source, imm);

}

static void emit_ori(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_source,
                     uint16_t imm) {
   if (reg_target == 0) {
      /* NOP */
      return;
   }

   if (reg_source == 0) {
      dynasm_emit_li(compiler, reg_target, imm);
      return;
   }

   if (imm == 0) {
      if (reg_target != reg_source) {
         dynasm_emit_mov(compiler, reg_target, reg_source);
      }
      return;
   }

   dynasm_emit_ori(compiler, reg_target, reg_source, imm);
}

static void emit_xori(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_source,
                     uint16_t imm) {
   if (reg_target == 0) {
      /* NOP */
      return;
   }

   if (reg_source == 0) {
      dynasm_emit_li(compiler, reg_target, imm);
      return;
   }

   if (imm == 0) {
      if (reg_target != reg_source) {
         dynasm_emit_mov(compiler, reg_target, reg_source);
      }
      return;
   }

   dynasm_emit_xori(compiler, reg_target, reg_source, imm);
}

static void emit_add(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_op0,
                     enum PSX_REG reg_op1) {
   if (reg_target == 0) {
      /* NOP */
      return;
   }

   if (reg_op0 == 0) {
      if (reg_op1 == 0) {
         dynasm_emit_li(compiler, reg_target, 0);
      } else {
         if (reg_target != reg_op1) {
            dynasm_emit_mov(compiler, reg_target, reg_op1);
         }
      }
   } else {
      if (reg_op1 == 0) {
         if (reg_target != reg_op0) {
            dynasm_emit_mov(compiler, reg_target, reg_op0);
         }
      } else {
         dynasm_emit_add(compiler, reg_target, reg_op0, reg_op1);
      }
   }
}

static void emit_addu(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_op0,
                      enum PSX_REG reg_op1) {
   if (reg_target == 0) {
      /* NOP */
      return;
   }

   if (reg_op0 == 0) {
      if (reg_op1 == 0) {
         dynasm_emit_li(compiler, reg_target, 0);
      } else {
         if (reg_target != reg_op1) {
            dynasm_emit_mov(compiler, reg_target, reg_op1);
         }
      }
   } else {
      if (reg_op1 == 0) {
         if (reg_target != reg_op0) {
            dynasm_emit_mov(compiler, reg_target, reg_op0);
         }
      } else {
         dynasm_emit_addu(compiler, reg_target, reg_op0, reg_op1);
      }
   }
}

static void emit_sub(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_op0,
                      enum PSX_REG reg_op1) {
   if (reg_op0 == PSX_REG_R0) {
      if (reg_op1 == PSX_REG_R0) {
         dynasm_emit_li(compiler, reg_target, 0);
      } else {
         /* Sub a, 0, b -> a = -b */
         dynasm_emit_neg(compiler, reg_target, reg_op1);
      }
   } else {
      if (reg_op1 == PSX_REG_R0) {
         if (reg_target != reg_op0) {
            dynasm_emit_mov(compiler, reg_target, reg_op0);
         } else {
            /* NOP: sub a, a, 0 */
            return;
         }
      } else {
         dynasm_emit_sub(compiler, reg_target, reg_op0, reg_op1);
      }
   }
}

static void emit_subu(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_op0,
                      enum PSX_REG reg_op1) {
   if (reg_target == PSX_REG_R0) {
      /* NOP */
      return;
   }

   if (reg_op0 == PSX_REG_R0) {
      if (reg_op1 == PSX_REG_R0) {
         dynasm_emit_li(compiler, reg_target, 0);
      } else {
         /* Sub a, 0, b -> a = -b */
         dynasm_emit_neg(compiler, reg_target, reg_op1);
      }
   } else {
      if (reg_op1 == PSX_REG_R0) {
         if (reg_target != reg_op0) {
            dynasm_emit_mov(compiler, reg_target, reg_op0);
         } else {
            /* NOP: sub a, a, 0 */
            return;
         }
      } else {
         dynasm_emit_subu(compiler, reg_target, reg_op0, reg_op1);
      }
   }
}

static void emit_and(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_op0,
                     enum PSX_REG reg_op1) {
   if (reg_target == PSX_REG_R0) {
      /* NOP */
      return;
   }

   if (reg_op0 == PSX_REG_R0 || reg_op1 == PSX_REG_R0) {
      dynasm_emit_li(compiler, reg_target, 0);
   } else {
      if (reg_op0 == reg_op1) {
         if (reg_op0 == reg_target) {
            /* NOP */
            return;
         } else {
            dynasm_emit_mov(compiler, reg_target, reg_op0);
         }
      } else {
         dynasm_emit_and(compiler, reg_target, reg_op0, reg_op1);
      }
   }
}

static void emit_or(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_target,
                    enum PSX_REG reg_op0,
                    enum PSX_REG reg_op1) {
   if (reg_target == PSX_REG_R0) {
      /* NOP */
      return;
   }

   if (reg_op0 == PSX_REG_R0) {
      if (reg_op1 == PSX_REG_R0) {
         dynasm_emit_li(compiler, reg_target, 0);
      } else if (reg_target != reg_op1) {
         dynasm_emit_mov(compiler, reg_target, reg_op1);
      } else {
         /* OR a, 0, a -> NOP */
         return;
      }
   } else if (reg_op1 == PSX_REG_R0) {
      if (reg_target != reg_op0) {
         dynasm_emit_mov(compiler, reg_target, reg_op0);
      } else {
         /* OR a, a, 0 -> NOP */
         return;
      }
   } else if (reg_op0 == reg_op1) {
      if (reg_target == reg_op0) {
         /* OR a, a, a -> NOP */
         return;
      } else {
         dynasm_emit_mov(compiler, reg_target, reg_op0);
      }
   } else {
      dynasm_emit_or(compiler, reg_target, reg_op0, reg_op1);
   }
}

static void emit_xor(struct dynarec_compiler *compiler,
                    enum PSX_REG reg_target,
                    enum PSX_REG reg_op0,
                    enum PSX_REG reg_op1) {
   if (reg_target == PSX_REG_R0) {
      /* NOP */
      return;
   }

   if (reg_op0 == PSX_REG_R0) {
      if (reg_op1 == PSX_REG_R0) {
         dynasm_emit_li(compiler, reg_target, 0);
      } else if (reg_target != reg_op1) {
         dynasm_emit_mov(compiler, reg_target, reg_op1);
      } else {
         /* XOR a, 0, a -> NOP */
         return;
      }
   } else if (reg_op1 == PSX_REG_R0) {
      if (reg_target != reg_op0) {
         dynasm_emit_mov(compiler, reg_target, reg_op0);
      } else {
         /* XOR a, a, 0 -> NOP */
         return;
      }
   } else if (reg_op0 == reg_op1) {
      /* XOR t, a, a -> 0 */
      dynasm_emit_li(compiler, reg_target, 0);
   } else {
      dynasm_emit_xor(compiler, reg_target, reg_op0, reg_op1);
   }
}

static void emit_nor(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_target,
                     enum PSX_REG reg_op0,
                     enum PSX_REG reg_op1) {
   if (reg_target == PSX_REG_R0) {
      /* NOP */
      return;
   }

   if (reg_op0 == PSX_REG_R0) {
      if (reg_op1 == PSX_REG_R0) {
         /* NOR x, 0, 0 -> ~0 */
         dynasm_emit_li(compiler, reg_target, 0xffffffff);
      } else {
         /* NOR x, 0, a -> ~a */
         dynasm_emit_not(compiler, reg_target, reg_op1);
      }
   } else if (reg_op1 == PSX_REG_R0) {
      /* NOR x, a, 0 -> ~a */
      dynasm_emit_not(compiler, reg_target, reg_op0);
   } else if (reg_op0 == reg_op1) {
      /* NOR x, a, a -> ~a */
      dynasm_emit_not(compiler, reg_target, reg_op0);
   } else {
      dynasm_emit_nor(compiler, reg_target, reg_op0, reg_op1);
   }
}

/* Attempt to fold lwl/lwr instruction pair for unaligned memory
   load */
static bool try_fold_lwl_lwr(struct dynarec_compiler *compiler,
                             struct opdesc *op1,
                             struct opdesc *op2) {
   struct opdesc *op_lwl;
   struct opdesc *op_lwr;
   unsigned opc1 = op1->instruction >> 26;
   unsigned opc2 = op2->instruction >> 26;

   if (op1->target != op2->target ||
       op1->op0 != op2->op0) {
      /* We don't use the same registers, can't fold */
      return false;
   }

   if (opc1 == MIPS_OP_LWL) {
      op_lwl = op1;
      if (opc2 == MIPS_OP_LWR) {
         op_lwr = op2;
      } else {
         return false;
      }
   } else if (opc1 == MIPS_OP_LWR) {
      op_lwr = op1;

      if (opc2 == MIPS_OP_LWL) {
         op_lwl = op2;
      } else {
         return false;
      }
   } else {
      return false;
   }

   if (op_lwl->imm.iunsigned != op_lwr->imm.iunsigned + 3) {
      /* The offsets don't match */
      return false;
   }

   /* We can fold the two instructions into a single (potentially)
      non-aligned access */
   dynasm_emit_lw_noalign(compiler,
                          op_lwr->target,
                          op_lwr->imm.iunsigned,
                          op_lwr->op0);
   return true;
}

/* Attempt to fold swl/swr instruction pair for unaligned memory
   store */
static bool try_fold_swl_swr(struct dynarec_compiler *compiler,
                             struct opdesc *op1,
                             struct opdesc *op2) {
   struct opdesc *op_swl;
   struct opdesc *op_swr;
   unsigned opc1 = op1->instruction >> 26;
   unsigned opc2 = op2->instruction >> 26;

   if (op1->target != op2->target ||
       op1->op0 != op2->op0) {
      /* We don't use the same registers, can't fold */
      return false;
   }

   if (opc1 == MIPS_OP_SWL) {
      op_swl = op1;
      if (opc2 == MIPS_OP_SWR) {
         op_swr = op2;
      } else {
         return false;
      }
   } else if (opc1 == MIPS_OP_SWR) {
      op_swr = op1;

      if (opc2 == MIPS_OP_SWL) {
         op_swl = op2;
      } else {
         return false;
      }
   } else {
      return false;
   }

   if (op_swl->imm.iunsigned != op_swr->imm.iunsigned + 3) {
      /* The offsets don't match */
      return false;
   }

   /* We can fold the two instructions into a single (potentially)
      non-aligned access */
   dynasm_emit_sw_noalign(compiler,
                          op_swr->target,
                          op_swr->imm.iunsigned,
                          op_swr->op0);
   return true;
}

/* Decode the fields of `instruction`. At most any instruction will
 * reference one target and two "operand" registers (with the
 * exception of DIV/MULT instructions which have two target registers
 * HI/LO, see below). For instruction that reference fewer registers
 * the remaining arguments are set to PSX_REG_R0.
 */
static void dynarec_decode_instruction(struct opdesc *op) {
   uint8_t reg_d = (op->instruction >> 11) & 0x1f;
   uint8_t reg_t = (op->instruction >> 16) & 0x1f;
   uint8_t reg_s = (op->instruction >> 21) & 0x1f;
   uint16_t imm = op->instruction & 0xffff;
   int16_t  simm_se = op->instruction & 0xffff;
   uint32_t imm_se = (int16_t)simm_se;
   uint32_t sysbrk_code = op->instruction >> 6;
   uint8_t  shift = (op->instruction >> 6) & 0x1f;
   uint32_t j_target = (op->instruction & 0x3ffffff) << 2;
   uint32_t imm25 = op->instruction & 0x1ffffff;

   op->type = OP_SIMPLE;
   op->target = PSX_REG_R0;
   op->op0 = PSX_REG_R0;
   op->op1 = PSX_REG_R0;
   op->imm.iunsigned = 0;

   switch (op->instruction >> 26) {
   case MIPS_OP_FN:
      switch (op->instruction & 0x3f) {
      case MIPS_FN_SLL:
      case MIPS_FN_SRL:
      case MIPS_FN_SRA:
         op->target = reg_d;
         op->op0 = reg_t;
         op->imm.iunsigned = shift;
         if (op->target == PSX_REG_R0) {
            op->type = OP_NOP;
         }
         break;
      case MIPS_FN_SLLV:
      case MIPS_FN_SRLV:
      case MIPS_FN_SRAV:
         op->target = reg_d;
         op->op0 = reg_t;
         op->op1 = reg_s;
         if (op->target == PSX_REG_R0) {
            op->type = OP_NOP;
         }
         break;
      case MIPS_FN_JR:
         op->op0 = reg_s;
         op->type = OP_BRANCH_ALWAYS;
         break;
      case MIPS_FN_JALR:
         op->op0 = reg_s;
         op->target = reg_d;
         op->type = OP_BRANCH_ALWAYS;
         break;
      case MIPS_FN_SYSCALL:
      case MIPS_FN_BREAK:
         op->imm.iunsigned = sysbrk_code;
         op->type = OP_EXCEPTION;
         break;
      case MIPS_FN_MFHI:
         op->op0 = PSX_REG_HI;
         op->target = reg_d;
         break;
      case MIPS_FN_MTHI:
         op->op0 = reg_s;
         op->target = PSX_REG_HI;
         break;
      case MIPS_FN_MFLO:
         op->op0 = PSX_REG_LO;
         op->target = reg_d;
         break;
      case MIPS_FN_MTLO:
         op->op0 = reg_s;
         op->target = PSX_REG_LO;
         break;
      case MIPS_FN_MULT:
      case MIPS_FN_MULTU:
      case MIPS_FN_DIV:
      case MIPS_FN_DIVU:
        op->op0 = reg_s;
        op->op1 = reg_t;
        /* XXX It's actually LO and HI, but for the moment we only
           support a single target reg in the logic. That being said
           I don't think it's an issue: HI and LO cannot be addressed
           directly by regular instructions, you have to use
           MTHI/MFHI/MTLO/MFLO to move them to a GPR so I can't think
           of any situation where a data hazard could occur. */
        op->target = PSX_REG_LO;
        break;
      case MIPS_FN_ADD:
      case MIPS_FN_ADDU:
      case MIPS_FN_SUB:
      case MIPS_FN_SUBU:
      case MIPS_FN_AND:
      case MIPS_FN_OR:
      case MIPS_FN_XOR:
      case MIPS_FN_NOR:
      case MIPS_FN_SLT:
      case MIPS_FN_SLTU:
         op->target = reg_d;
         op->op0 = reg_s;
         op->op1 = reg_t;
         if (op->target == PSX_REG_R0) {
            op->type = OP_NOP;
         }
         break;
      case 0x1f:
      case 0x34:
         /* Illegal */
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x (sub: 0x%x)\n",
                op->instruction,
                op->instruction & 0x3f);
         abort();
      }
      break;
   case MIPS_OP_BXX:
      if (((op->instruction >> 17) & 0xf) == 8) {
         /* Link */
         op->target = PSX_REG_RA;
      }
      op->op0 = reg_s;
      op->imm.isigned = simm_se;
      op->type = OP_BRANCH_COND;
      break;
   case MIPS_OP_J:
      op->imm.iunsigned = j_target;
      op->type = OP_BRANCH_ALWAYS;
      break;
   case MIPS_OP_JAL:
      op->imm.iunsigned = j_target;
      op->type = OP_BRANCH_ALWAYS;
      op->target = PSX_REG_RA;
      break;
   case MIPS_OP_BEQ:
      op->op0 = reg_s;
      op->op1 = reg_t;
      op->imm.isigned = simm_se;
      op->type = OP_BRANCH_COND;
      if (op->op0 == op->op1) {
         op->type = OP_BRANCH_ALWAYS;
      }
      break;
   case MIPS_OP_BNE:
      op->op0 = reg_s;
      op->op1 = reg_t;
      op->imm.isigned = simm_se;
      op->type = OP_BRANCH_COND;
      if (op->op0 == op->op1) {
         op->type = OP_NOP;
      }
      break;
   case MIPS_OP_BLEZ:
   case MIPS_OP_BGTZ:
      op->op0 = reg_s;
      op->imm.isigned = simm_se;
      op->type = OP_BRANCH_COND;
      break;
   case MIPS_OP_ADDI:
   case MIPS_OP_ADDIU:
   case MIPS_OP_SLTI:
   case MIPS_OP_SLTIU:
      op->target = reg_t;
      op->op0    = reg_s;
      op->imm.iunsigned = imm_se;
      if (op->target == PSX_REG_R0) {
         op->type = OP_NOP;
      }
      break;
   case MIPS_OP_ANDI:
   case MIPS_OP_ORI:
   case MIPS_OP_XORI:
      op->target = reg_t;
      op->op0    = reg_s;
      op->imm.iunsigned = imm;
      if (op->target == PSX_REG_R0) {
         op->type = OP_NOP;
      }
      break;
   case MIPS_OP_LUI:
      op->target = reg_t;
      op->imm.iunsigned = imm << 16;
      break;
   case MIPS_OP_COP0:
      switch (reg_s) {
      case MIPS_COP_MFC:
         op->target = reg_t;
         op->op0 = reg_d;
         op->type = OP_LOAD;
         break;
      case MIPS_COP_MTC:
         op->target = reg_d;
         op->op0 = reg_t;
         break;
      case MIPS_COP_RFE:
         break;
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x (%x)\n",
                op->instruction, reg_s);
         abort();
      }
      break;
   case MIPS_OP_COP2:
      switch (reg_s) {
      case MIPS_GTE_MFC2:
         op->target = reg_t;
         op->op0 = reg_d;
         op->type = OP_LOAD;
         break;
      case MIPS_GTE_CFC2:
         op->target = reg_t;
         op->op0 = reg_d;
         op->type = OP_LOAD;
         break;
      case MIPS_GTE_MTC2:
         op->target = reg_d;
         op->op0 = reg_t;
         break;
      case MIPS_GTE_CTC2:
         op->target = reg_d;
         op->op0 = reg_t;
         break;
      case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
      case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
         op->imm.iunsigned = imm25;
         break;
      default:
         printf("Dynarec encountered unsupported GTE instruction %08x (%x)\n",
                op->instruction, reg_s);
         abort();
      }
      break;
   case MIPS_OP_LBU:
   case MIPS_OP_LB:
   case MIPS_OP_LHU:
   case MIPS_OP_LH:
   case MIPS_OP_LW:
      op->target = reg_t;
      op->op0 = reg_s;
      op->imm.iunsigned = imm;
      op->type = OP_LOAD;
      break;
   case MIPS_OP_LWL:
   case MIPS_OP_LWR:
      op->target = reg_t;
      op->op0 = reg_s;
      op->imm.iunsigned = imm;
      op->type = OP_LOAD_COMBINE;
      break;
   case MIPS_OP_SB: /* SB */
   case MIPS_OP_SH: /* SH */
   case MIPS_OP_SW: /* SW */
      op->op0 = reg_s;
      op->op1 = reg_t;
      op->imm.iunsigned = imm;
      break;
   case MIPS_OP_SWL:
   case MIPS_OP_SWR:
      op->op0 = reg_s;
      op->op1 = reg_t;
      op->imm.iunsigned = imm;
      op->type = OP_STORE_NOALIGN;
      break;
   case MIPS_OP_LWC2:
      op->op0 = reg_s;
      op->op1 = reg_t;
      op->imm.iunsigned = imm;
      op->type = OP_LOAD;
      break;
   case MIPS_OP_SWC2:
      op->op0 = reg_s;
      op->op1 = reg_t;
      op->imm.iunsigned = imm;
      break;
   case 0x18:
   case 0x19:
   case 0x1b:
   case 0x1d:
   case 0x1e:
         /* Illegal */
         break;
   default:
      printf("Dynarec encountered unsupported instruction %08x (fn: 0x%x)\n",
             op->instruction,
             op->instruction >> 26);
      abort();
   }
}

static void dynarec_emit_instruction(struct dynarec_compiler *compiler,
                                     struct opdesc *op) {
   switch (op->instruction >> 26) {
   case MIPS_OP_FN:
      switch (op->instruction & 0x3f) {
      case MIPS_FN_SLL:
         DYNAREC_LOG("Emitting MIPS_FN_SLL 0x%08x\n", op->instruction);
         emit_shift_imm(compiler,
                        op->target,
                        op->op0,
                        op->imm.iunsigned,
                        dynasm_emit_sll);
         break;
      case MIPS_FN_SRL:
         DYNAREC_LOG("Emitting MIPS_FN_SRL 0x%08x\n", op->instruction);
         emit_shift_imm(compiler,
                        op->target,
                        op->op0,
                        op->imm.iunsigned,
                        dynasm_emit_srl);
         break;
      case MIPS_FN_SRA:
         DYNAREC_LOG("Emitting MIPS_FN_SRA 0x%08x\n", op->instruction);
         emit_shift_imm(compiler,
                        op->target,
                        op->op0,
                        op->imm.iunsigned,
                        dynasm_emit_sra);
         break;
      case MIPS_FN_SLLV:
         DYNAREC_LOG("Emitting MIPS_FN_SLLV 0x%08x\n", op->instruction);
         emit_shift_reg(compiler,
                        op->target,
                        op->op0,
                        op->op1,
                        dynasm_emit_sllv);
         break;
      case MIPS_FN_SRLV:
         DYNAREC_LOG("Emitting MIPS_FN_SRLV 0x%08x\n", op->instruction);
         emit_shift_reg(compiler,
                        op->target,
                        op->op0,
                        op->op1,
                        dynasm_emit_srlv);
         break;
      case MIPS_FN_SRAV:
         DYNAREC_LOG("Emitting MIPS_FN_SRAV 0x%08x\n", op->instruction);
         emit_shift_reg(compiler,
                        op->target,
                        op->op0,
                        op->op1,
                        dynasm_emit_srav);
         break;
      case MIPS_FN_JR:
         DYNAREC_LOG("Emitting MIPS_FN_JR 0x%08x\n", op->instruction);
         emit_jalr(compiler, op->op0, PSX_REG_R0);
         break;
      case MIPS_FN_JALR:
         DYNAREC_LOG("Emitting MIPS_FN_JALR 0x%08x\n", op->instruction);
         emit_jalr(compiler, op->op0, op->target);
         break;
      case MIPS_FN_SYSCALL:
         DYNAREC_LOG("Emitting MIPS_FN_SYSCALL 0x%08x\n", op->instruction);
         dynasm_emit_exit(compiler, DYNAREC_EXIT_SYSCALL, op->imm.iunsigned);
         break;
      case MIPS_FN_BREAK:
         DYNAREC_LOG("Emitting MIPS_FN_BREAK 0x%08x\n", op->instruction);
         if (compiler->state->options & DYNAREC_OPT_EXIT_ON_BREAK) {
            dynasm_emit_exit(compiler, DYNAREC_EXIT_BREAK, op->imm.iunsigned);
         } else {
            dynasm_emit_exception(compiler, PSX_EXCEPTION_BREAK);
         }
         break;
      case MIPS_FN_MFHI:
      case MIPS_FN_MTHI:
      case MIPS_FN_MFLO:
      case MIPS_FN_MTLO:
         DYNAREC_LOG("Emitting MIPS_FN_M[F/T][HI/LO] 0x%08x\n", op->instruction);
         if (op->target == PSX_REG_R0) {
            /* nop */
            break;
         }

         if (op->op0 == PSX_REG_R0) {
            dynasm_emit_li(compiler, op->target, 0);
            break;
         }

         dynasm_emit_mov(compiler, op->target, op->op0);
         break;
      case MIPS_FN_MULT:
         DYNAREC_LOG("Emitting MIPS_FN_MULT 0x%08x\n", op->instruction);
         if (op->op0 == PSX_REG_R0 || op->op1 == PSX_REG_R0) {
            // Multiplication by zero yields zero
            dynasm_emit_li(compiler, PSX_REG_LO, 0);
            dynasm_emit_li(compiler, PSX_REG_HI, 0);
         } else {
            dynasm_emit_mult(compiler, op->op0, op->op1);
         }
         break;
      case MIPS_FN_MULTU:
         DYNAREC_LOG("Emitting MIPS_FN_MULTU 0x%08x\n", op->instruction);
         if (op->op0 == PSX_REG_R0 || op->op1 == PSX_REG_R0) {
            // Multiplication by zero yields zero
            dynasm_emit_li(compiler, PSX_REG_LO, 0);
            dynasm_emit_li(compiler, PSX_REG_HI, 0);
         } else {
            dynasm_emit_multu(compiler, op->op0, op->op1);
         }
         break;
      case MIPS_FN_DIV:
         DYNAREC_LOG("Emitting MIPS_FN_DIV 0x%08x\n", op->instruction);
         dynasm_emit_div(compiler, op->op0, op->op1);
         break;
      case MIPS_FN_DIVU:
         DYNAREC_LOG("Emitting MIPS_FN_DIVU 0x%08x\n", op->instruction);
         dynasm_emit_divu(compiler, op->op0, op->op1);
         break;
      case MIPS_FN_ADD:
         DYNAREC_LOG("Emitting MIPS_FN_ADD 0x%08x\n", op->instruction);
         emit_add(compiler,
                  op->target,
                  op->op0,
                  op->op1);
         break;
      case MIPS_FN_ADDU:
         DYNAREC_LOG("Emitting MIPS_FN_ADDU 0x%08x\n", op->instruction);
         emit_addu(compiler,
                   op->target,
                   op->op0,
                   op->op1);
         break;
      case MIPS_FN_SUB:
         DYNAREC_LOG("Emitting MIPS_FN_SUB 0x%08x\n", op->instruction);
         emit_sub(compiler,
                   op->target,
                   op->op0,
                   op->op1);
         break;
      case MIPS_FN_SUBU:
         DYNAREC_LOG("Emitting MIPS_FN_SUBU 0x%08x\n", op->instruction);
         emit_subu(compiler,
                   op->target,
                   op->op0,
                   op->op1);
         break;
      case MIPS_FN_AND:
         DYNAREC_LOG("Emitting MIPS_FN_AND 0x%08x\n", op->instruction);
         emit_and(compiler,
                  op->target,
                  op->op0,
                  op->op1);
         break;
      case MIPS_FN_OR:
         DYNAREC_LOG("Emitting MIPS_FN_OR 0x%08x\n", op->instruction);
         emit_or(compiler,
                 op->target,
                 op->op0,
                 op->op1);
         break;
      case MIPS_FN_XOR:
         DYNAREC_LOG("Emitting MIPS_FN_XOR 0x%08x\n", op->instruction);
         emit_xor(compiler,
                 op->target,
                 op->op0,
                 op->op1);
         break;
      case MIPS_FN_NOR:
         DYNAREC_LOG("Emitting MIPS_FN_NOR 0x%08x\n", op->instruction);
         emit_nor(compiler,
                 op->target,
                 op->op0,
                 op->op1);
         break;
      case MIPS_FN_SLT:
         DYNAREC_LOG("Emitting MIPS_FN_SLT 0x%08x\n", op->instruction);
         if (op->target == PSX_REG_R0) {
            /* NOP */
            break;
         }

         if (op->op0 == PSX_REG_R0 && op->op1 == PSX_REG_R0) {
            /* 0 isn't less than 0 */
            dynasm_emit_li(compiler, op->target, 0);
            break;
         }

         dynasm_emit_slt(compiler, op->target, op->op0, op->op1);
         break;
      case MIPS_FN_SLTU:
         DYNAREC_LOG("Emitting MIPS_FN_SLTU 0x%08x\n", op->instruction);
         if (op->target == PSX_REG_R0) {
            /* NOP */
            break;
         }

         if (op->op1 == PSX_REG_R0) {
            /* Nothing is less than 0 */
            dynasm_emit_li(compiler, op->target, 0);
            break;
         }

         dynasm_emit_sltu(compiler, op->target, op->op0, op->op1);
         break;
      case 0x1f:
      case 0x34:
         DYNAREC_LOG("Emitting exception illegal instruction 0x%08x\n", op->instruction);
         /* Illegal */
         dynasm_emit_exception(compiler, PSX_EXCEPTION_ILLEGAL_INSTRUCTION);
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x\n",
                op->instruction);
         abort();
      }
      break;
   case MIPS_OP_BXX:
      DYNAREC_LOG("Emitting MIPS_OP_BXX 0x%08x\n", op->instruction);
      emit_bxx(compiler, op);
      break;
   case MIPS_OP_J:
      DYNAREC_LOG("Emitting MIPS_OP_J 0x%08x\n", op->instruction);
      emit_j(compiler, op);
      break;
   case MIPS_OP_JAL:
      DYNAREC_LOG("Emitting MIPS_OP_JAL 0x%08x\n", op->instruction);
      emit_jal(compiler, op);
      break;
   case MIPS_OP_BEQ:
      DYNAREC_LOG("Emitting MIPS_OP_BEQ 0x%08x\n", op->instruction);
      emit_beq(compiler, op->imm.isigned, op->op0, op->op1);
      break;
   case MIPS_OP_BNE:
      DYNAREC_LOG("Emitting MIPS_OP_BNE 0x%08x\n", op->instruction);
      emit_bne(compiler, op->imm.isigned, op->op0, op->op1);
      break;
   case MIPS_OP_BLEZ:
      DYNAREC_LOG("Emitting MIPS_OP_BLEZ 0x%08x\n", op->instruction);
      emit_blez(compiler, op->imm.isigned, op->op0);
      break;
   case MIPS_OP_BGTZ:
      DYNAREC_LOG("Emitting MIPS_OP_BGTZ 0x%08x\n", op->instruction);
      emit_bgtz(compiler, op->imm.isigned, op->op0);
      break;
   case MIPS_OP_ADDI:
      DYNAREC_LOG("Emitting MIPS_OP_ADDI 0x%08x\n", op->instruction);
      emit_addi(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_ADDIU:
      DYNAREC_LOG("Emitting MIPS_OP_ADDIU 0x%08x\n", op->instruction);
      emit_addiu(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_SLTI: /* SLTI */
      DYNAREC_LOG("Emitting MIPS_OP_SLTI 0x%08x\n", op->instruction);
      if (op->target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      dynasm_emit_slti(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_SLTIU: /* SLTIU */
      DYNAREC_LOG("Emitting MIPS_OP_SLTIU 0x%08x\n", op->instruction);
      if (op->target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      if (op->imm.iunsigned == 0) {
         /* Nothing is less than 0 */
         dynasm_emit_li(compiler, op->target, 0);
         break;
      }

      dynasm_emit_sltiu(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_ANDI:
      DYNAREC_LOG("Emitting MIPS_OP_ANDI 0x%08x\n", op->instruction);
      emit_andi(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_ORI:
      DYNAREC_LOG("Emitting MIPS_OP_ORI 0x%08x\n", op->instruction);
      emit_ori(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_XORI:
      DYNAREC_LOG("Emitting MIPS_OP_XORI 0x%08x\n", op->instruction);
      emit_xori(compiler, op->target, op->op0, op->imm.iunsigned);
      break;
   case MIPS_OP_LUI:
      DYNAREC_LOG("Emitting MIPS_OP_LUI 0x%08x\n", op->instruction);
      if (op->target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      dynasm_emit_li(compiler, op->target, op->imm.iunsigned);
      break;
   case MIPS_OP_COP0:
      switch ((op->instruction >> 21) & 0x1f) {
      case MIPS_COP_MFC:
         DYNAREC_LOG("Emitting MIPS_COP_MFC 0x%08x\n", op->instruction);
         dynasm_emit_mfc0(compiler, op->target, op->op0);
         break;
      case MIPS_COP_MTC:
         DYNAREC_LOG("Emitting MIPS_COP_MTC 0x%08x\n", op->instruction);
         dynasm_emit_mtc0(compiler, op->op0, op->target);
         break;
      case MIPS_COP_RFE:
         DYNAREC_LOG("Emitting MIPS_COP_RFE 0x%08x\n", op->instruction);
         dynasm_emit_rfe(compiler);
         break;
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x\n",
                op->instruction);
         abort();
      }
      break;
   case MIPS_OP_COP2:
      switch ((op->instruction >> 21) & 0x1f) {
      case MIPS_GTE_MFC2:
         DYNAREC_LOG("Emitting MIPS_GTE_MFC2 0x%08x\n", op->instruction);
         dynasm_emit_mfc2(compiler, op->target, op->op0, op->instruction);
         break;
      case MIPS_GTE_CFC2:
         DYNAREC_LOG("Emitting MIPS_GTE_CFC2 0x%08x\n", op->instruction);
         dynasm_emit_cfc2(compiler, op->target, op->op0, op->instruction);
         break;
      case MIPS_GTE_MTC2:
         DYNAREC_LOG("Emitting MIPS_GTE_MTC2 0x%08x\n", op->instruction);
         dynasm_emit_mtc2(compiler, op->op0, op->target, op->instruction);
         break;
      case MIPS_GTE_CTC2:
         DYNAREC_LOG("Emitting MIPS_GTE_CTC2 0x%08x\n", op->instruction);
         dynasm_emit_ctc2(compiler, op->op0, op->target, op->instruction);
         break;
      case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
      case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
         DYNAREC_LOG("Emitting GTE Instruction 0x%08x\n", op->instruction);
         dynasm_emit_gte_instruction(compiler, op->imm.iunsigned);
         break;
      default:
         printf("Dynarec encountered unsupported GTE instruction %08x\n",
                op->instruction);
         abort();
      }
      break;
   case MIPS_OP_LB:
      DYNAREC_LOG("Emitting MIPS_OP_LB 0x%08x\n", op->instruction);
      dynasm_emit_lb(compiler, op->target, op->imm.iunsigned, op->op0);
      break;
   case MIPS_OP_LBU:
      DYNAREC_LOG("Emitting MIPS_OP_LBU 0x%08x\n", op->instruction);
      dynasm_emit_lbu(compiler, op->target, op->imm.iunsigned, op->op0);
      break;
   case MIPS_OP_LH:
      DYNAREC_LOG("Emitting MIPS_OP_LH 0x%08x\n", op->instruction);
      dynasm_emit_lh(compiler, op->target, op->imm.iunsigned, op->op0);
      break;
   case MIPS_OP_LHU:
      DYNAREC_LOG("Emitting MIPS_OP_LHU 0x%08x\n", op->instruction);
      dynasm_emit_lhu(compiler, op->target, op->imm.iunsigned, op->op0);
      break;
   case MIPS_OP_LW:
      DYNAREC_LOG("Emitting MIPS_OP_LW 0x%08x\n", op->instruction);
      dynasm_emit_lw(compiler, op->target, op->imm.iunsigned, op->op0);
      break;
   case MIPS_OP_SB: /* SB */
      DYNAREC_LOG("Emitting MIPS_OP_SB 0x%08x\n", op->instruction);
      dynasm_emit_sb(compiler, op->op0, op->imm.iunsigned, op->op1);
      break;
   case MIPS_OP_SH: /* SH */
      DYNAREC_LOG("Emitting MIPS_OP_SH 0x%08x\n", op->instruction);
      dynasm_emit_sh(compiler, op->op0, op->imm.iunsigned, op->op1);
      break;
   case MIPS_OP_SW: /* SW */
      DYNAREC_LOG("Emitting MIPS_OP_SW 0x%08x\n", op->instruction);
      dynasm_emit_sw(compiler, op->op0, op->imm.iunsigned, op->op1);
      break;
   case MIPS_OP_LWC2:
      DYNAREC_LOG("Emitting MIPS_OP_LWC2 0x%08x\n", op->instruction);
      dynasm_emit_lwc2(compiler, op->instruction);
      break;
   case MIPS_OP_SWC2:
      DYNAREC_LOG("Emitting MIPS_OP_SWC2 0x%08x\n", op->instruction);
      dynasm_emit_swc2(compiler, op->instruction);
      break;
   default:
      printf("Dynarec encountered unsupported instruction %08x\n",
             op->instruction);
      abort();
   }
}

static uint32_t load_le(const uint8_t *p) {
   return
      ((uint32_t)p[0]) |
      (((uint32_t)p[1]) << 8) |
      (((uint32_t)p[2]) << 16) |
      (((uint32_t)p[3]) << 24);
}

struct dynarec_block *dynarec_recompile(struct dynarec_state *state,
                                        uint32_t addr) {
   struct dynarec_compiler  compiler = { 0 };
   struct dynarec_block *block;
   const uint8_t *block_start;
   const uint8_t *block_end;
   const uint8_t *block_max;
   const uint8_t *cur;
   uint32_t canonical_addr;
   bool eob;
   struct opdesc op = { .type = OP_SIMPLE };
   struct opdesc ds_op = { .type = OP_SIMPLE };

   DYNAREC_LOG("Recompiling block starting at 0x%08x\n", addr);

   assert((addr & 3) == 0);

   /* Some memory regions are aliased several time in the memory map */
   canonical_addr = dynarec_canonical_address(addr);

   if (canonical_addr < PSX_RAM_SIZE) {
      block_start = state->ram + canonical_addr;
      block_max = state->ram + PSX_RAM_SIZE;
   } else if (canonical_addr >= PSX_BIOS_BASE &&
              canonical_addr < (PSX_BIOS_BASE + PSX_BIOS_SIZE)){
      block_start = state->bios + (canonical_addr - PSX_BIOS_BASE);
      block_max = state->bios + PSX_RAM_SIZE;
   } else {
      /* What are we trying to recompile here exactly ? */
      assert("Recompiling unknown address" == NULL);
      return NULL;
   }

   if ((block_max - block_start) > DYNAREC_MAX_BLOCK_SIZE) {
      block_end = block_start + DYNAREC_MAX_BLOCK_SIZE;
   } else {
      block_end = block_max;
   }

   /* Make sure that we're not running out of free space */
   assert(state->map_len - (state->free_map - state->map) > (1 * 1024 * 1024));

   block = (struct dynarec_block *)state->free_map;
   block->base_address = addr;

   compiler.state = state;
   compiler.block = block;
   compiler.map = dynarec_block_code(block);
   compiler.pc = addr;
   compiler.spent_cycles = 0;

   dynasm_emit_block_prologue(&compiler);

   for (eob = false, cur = block_start;
        !eob && cur < block_end;
        cur += 4, compiler.pc += 4) {
      int has_branch_delay_slot;
      int has_load_delay_slot;
      int has_delay_slot;

      op.instruction = load_le(cur);

      DYNAREC_LOG("Compiling 0x%08x @ 0x%08x\n", op.instruction, compiler.pc);

      compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;

      dynarec_decode_instruction(&op);

      has_branch_delay_slot =
         (op.type == OP_BRANCH_ALWAYS) || (op.type == OP_BRANCH_COND);
      has_load_delay_slot = (op.type == OP_LOAD || op.type == OP_LOAD_COMBINE);
      has_delay_slot = has_branch_delay_slot || has_load_delay_slot;

      if ((op.type == OP_BRANCH_ALWAYS) || (op.type == OP_EXCEPTION)) {
         /* We are certain that the execution won't continue after
            this instruction (besides potentially the load delay slot
            which is handled below) */
         eob = true;
      }

      if ((has_delay_slot || op.type == OP_STORE_NOALIGN) &&
          (cur + 4) < block_max) {
         ds_op.instruction = load_le(cur + 4);
         dynarec_decode_instruction(&ds_op);
      } else {
         /* Pretend the delay slot is a NOP */
         memset(&ds_op, 0, sizeof(ds_op));
      }

      if (op.type == OP_STORE_NOALIGN &&
          ds_op.type == OP_STORE_NOALIGN &&
          try_fold_swl_swr(&compiler, &op, &ds_op)) {
         /* We've folded both instructions, skip ahead */
         cur += 4;
         compiler.pc += 4;
         compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;
         continue;
      }

      if (op.type == OP_LOAD_COMBINE &&
          ds_op.type == OP_LOAD_COMBINE &&
          try_fold_lwl_lwr(&compiler, &op, &ds_op)) {
         /* We've folded both instructions, skip ahead */
         cur += 4;
         compiler.pc += 4;
         compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;
         continue;
      }

      if (has_load_delay_slot &&
          op.target != PSX_REG_R0 &&
          ds_op.type != OP_NOP) {

         /* We have to check if the next instruction conflicts with
            the load target */
         if (ds_op.type == OP_LOAD_COMBINE) {
            /* Next instruction bypasses the load delay, we don't have
               to worry about it */
            dynarec_emit_instruction(&compiler, &op);
         } else if (ds_op.target == op.target) {
            /* The instruction in the delay slot overwrites the value,
               effectively making the LW useless (or only useful for
               side-effect). Seems odd but easy enough to implement,
               we can just pretend that this load just targets R0
               since it's functionally equivalent. */
            op.target = PSX_REG_R0;

            dynarec_emit_instruction(&compiler, &op);

         } else if (op.target == ds_op.op0 || op.target == ds_op.op1) {
            /* That's a bit trickier, we need to make sure that the
               previous value of `op.target` is used in the load
               delay. */

            if (ds_op.type == OP_BRANCH_ALWAYS ||
                ds_op.type == OP_BRANCH_COND ||
                ds_op.type == OP_EXCEPTION) {
               /* If the instruction in the delay slot is a branch we
                  can't reorder (otherwise we'll jump away before we
                  have a chance to execute the load). If this needs
                  implementing we'll have to be clever. */
               DYNAREC_FATAL("Nested delay slot in load delay slot\n");
            } else {
               /* We can simply swap the order of the instructions
                  (while keeping the old value in a temporary register,
                  like branch delay slots). We need to be careful however
                  if the load references the target as operand. */
               bool needs_dt = false;

               if (op.op0 == ds_op.target) {
                  needs_dt = true;
                  op.op0 = PSX_REG_DT;
               }

               if (op.op1 == ds_op.target) {
                  needs_dt = true;
                  op.op1 = PSX_REG_DT;
               }

               if (needs_dt) {
                  /* The instruction in the delay slot targets a register
                     used by the branch, we need to keep a copy. */
                  dynasm_emit_mov(&compiler, PSX_REG_DT, ds_op.target);
               }

               /* Emit instruction in load delay slot */
               compiler.pc += 4;
               dynarec_emit_instruction(&compiler, &ds_op);
               compiler.pc -= 4;

               /* Emit load instruction */
               dynarec_emit_instruction(&compiler, &op);

               /* Since we reordered we must jump ahead not to execute
                  the load delay instruction twice */
               cur += 4;
               compiler.pc += 4;
               compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;
            }
         } else {
            /* We don't have any hazard, we can simply emit the load
               normally */
            dynarec_emit_instruction(&compiler, &op);
         }
      } else if (has_branch_delay_slot &&
                 /* Special case a NOP in the delay slot since it's
                    fairly common. A branch with a NOP in the delay
                    slot behaves like a common instruction. */
                 ds_op.instruction != 0) {
         /* We have to run the delay slot before the actual
          * jump. First let's make sure that we don't have a data
          * hazard.
          */
         int needs_dt = 0;

         if (ds_op.type == OP_BRANCH_ALWAYS ||
             ds_op.type == OP_BRANCH_COND ||
             ds_op.type == OP_EXCEPTION) {
            /* Nested branch delay slot or exception in delay
               slot. This would be a pain to implement. Let's hope the
               average game doesn't require something like that. */
            dynasm_emit_exit(&compiler, DYNAREC_EXIT_UNIMPLEMENTED, __LINE__);
         } else if (ds_op.type == OP_LOAD || ds_op.type == OP_LOAD_COMBINE) {
            /* Emitting this directly is technically inaccurate but
               probably fine the vast majority of the time (relying on
               load delay slot behaviour across a jump sounds nasty,
               but who knows).  */
#if 0
            dynasm_emit_exit(&compiler, DYNAREC_EXIT_UNIMPLEMENTED, __LINE__);
#endif
         }

         if (ds_op.target != PSX_REG_R0) {
            /* Check for data hazard */
            if (ds_op.target == op.target) {
               /* Not sure what happens if the jump and delay slot
                  write to the same register. If the jump wins then we
                  have nothing to do, if it's the instruction we just
                  need to replace the jump with the equivalent
                  instruction that doesn't link. */
               DYNAREC_FATAL("Register race on branch target\n");
            }

            if (ds_op.target == op.op0) {
               needs_dt = 1;
               op.op0 = PSX_REG_DT;
            }

            if (ds_op.target == op.op1) {
               needs_dt = 1;
               op.op1 = PSX_REG_DT;
            }

            if (needs_dt) {
               /* The instruction in the delay slot targets a register
                  used by the branch, we need to keep a copy. */
               dynasm_emit_mov(&compiler, PSX_REG_DT, ds_op.target);
            }
         }

         /* Emit instruction in branch delay slot */
         compiler.pc += 4;
         dynarec_emit_instruction(&compiler, &ds_op);
         compiler.pc -= 4;

         /* Emit branch instruction */
         dynarec_emit_instruction(&compiler, &op);
         /* Move ahead not to emit the same instruction twice */
         cur += 4;
         compiler.pc += 4;
         compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;
      } else if (op.type != OP_NOP) {
         /* Boring old instruction, no delay slot involved. */
         dynarec_emit_instruction(&compiler, &op);
      }

#if 0
      {
         int fd = open("/tmp/dump.amd64", O_WRONLY | O_CREAT| O_TRUNC, 0644);

         write(fd, compiler.block + 1,
               compiler.map - (uint8_t*)(compiler.block + 1));

         close(fd);
      }
#endif
   }

   /* We're done with this block */
   if ((op.type != OP_BRANCH_ALWAYS) && (op.type != OP_EXCEPTION)) {
      /* Execution continues after this block, we need to link it to
         the next one */
      emit_jump(&compiler, compiler.pc);
   }

   block->block_len_bytes = compiler.map - (uint8_t *)compiler.block;

   dyndebug_add_block(dynarec_block_code(block),
                      block->block_len_bytes,
                      block->base_address);

   block->block_len_bytes = dynarec_align(block->block_len_bytes,
                                          CACHE_LINE_SIZE);
   block->psx_instructions = (compiler.pc - addr) / 4;

   DYNAREC_LOG("Block len: %uB\n", block->block_len_bytes);
   DYNAREC_LOG("Number of PSX instructions: %u\n", block->psx_instructions);

   state->free_map += block->block_len_bytes;

   return compiler.block;
}

/* Called by the recompiled code when a target needs to be
   resolved. Should patch the caller if `patch_offset` isn't 0 and
   return the target location. */
void *dynarec_recompile_and_patch(struct dynarec_state *state,
                                  uint32_t target,
                                  uint32_t patch_offset) {
   struct dynarec_block *b;
   void *link;

   DYNAREC_LOG("dynarec_recompile_and_patch(0x%08x, 0x%08x)\n",
               target, patch_offset);

   b = dynarec_find_or_compile_block(state, target);

   link = dynarec_block_code(b);

#ifdef DYNAREC_NO_PATCH
   patch_offset = 0;
#endif

   if (patch_offset != 0) {
      /* Patch the caller */
      struct dynarec_compiler compiler = { 0 };

      compiler.state = state;
      compiler.map = state->map + patch_offset;

      dynasm_patch_link(&compiler, link);
   }

   return link;
}

int dynarec_compiler_init(struct dynarec_state *state) {
   /* Generate the trampoline at the beginning of the map */
   struct dynarec_compiler compiler = { 0 };
   size_t len;

   compiler.state = state;
   compiler.map = state->free_map;

   state->link_trampoline = compiler.map;
   dynasm_emit_link_trampoline(&compiler);

   len = compiler.map - state->free_map;
   len = dynarec_align(len, CACHE_LINE_SIZE);
   state->free_map += len;

   return 0;
}
