#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "dynarec-compiler.h"
#include "dynarec-jit-debugger.h"
#include "psx-instruction.h"

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
                   uint32_t instruction) {
   uint32_t imm_jump = (instruction & 0x3ffffff) << 2;
   uint32_t target;

   target = compiler->pc & 0xf0000000;
   target |= imm_jump;

   emit_jump(compiler, target);
}

static void emit_jal(struct dynarec_compiler *compiler,
                     uint32_t instruction) {
   /* Store return address in RA */
   dynasm_emit_li(compiler, PSX_REG_RA, compiler->pc + 8);
   emit_j(compiler, instruction);
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
                     int16_t offset,
                     enum PSX_REG reg_link,
                     enum PSX_REG reg_op,
                     bool is_bgez) {
   enum dynarec_jump_cond cond;

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

static void emit_rfe(struct dynarec_compiler *compiler,
                     uint32_t instruction) {
   dynasm_emit_rfe(compiler);
}

enum optype {
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
};

/* Gets the general purpose registers referenced by `instruction`. At
 * most any instruction will reference one target and two "operand"
 * registers. For instruction that reference fewer registers the
 * remaining arguments are set to PSX_REG_R0.
 *
 * Returns the number of cycles taken by this instruction to execute.
 */
static enum optype dynarec_instruction_registers(uint32_t instruction,
                                                 enum PSX_REG *reg_target,
                                                 enum PSX_REG *reg_op0,
                                                 enum PSX_REG *reg_op1) {
   uint8_t reg_d = (instruction >> 11) & 0x1f;
   uint8_t reg_t = (instruction >> 16) & 0x1f;
   uint8_t reg_s = (instruction >> 21) & 0x1f;
   enum optype type = OP_SIMPLE;

   *reg_target = PSX_REG_R0;
   *reg_op0    = PSX_REG_R0;
   *reg_op1    = PSX_REG_R0;

   switch (instruction >> 26) {
   case MIPS_OP_FN:
      switch (instruction & 0x3f) {
      case MIPS_FN_SLL:
      case MIPS_FN_SRL:
      case MIPS_FN_SRA:
         *reg_target = reg_d;
         *reg_op0    = reg_t;
         break;
      case MIPS_FN_SLLV:
      case MIPS_FN_SRLV:
      case MIPS_FN_SRAV:
         *reg_target = reg_d;
         *reg_op0 = reg_t;
         *reg_op1 = reg_s;
         break;
      case MIPS_FN_JR:
         *reg_op0 = reg_s;
         type = OP_BRANCH_ALWAYS;
         break;
      case MIPS_FN_JALR:
         *reg_op0 = reg_s;
         *reg_target = reg_d;
         type = OP_BRANCH_ALWAYS;
         break;
      case MIPS_FN_SYSCALL:
      case MIPS_FN_BREAK:
         type = OP_EXCEPTION;
         break;
      case MIPS_FN_MFHI:
         *reg_op0 = PSX_REG_HI;
         *reg_target = reg_d;
         break;
      case MIPS_FN_MTHI:
         *reg_op0 = reg_s;
         *reg_target = PSX_REG_HI;
         break;
      case MIPS_FN_MFLO:
         *reg_op0 = PSX_REG_LO;
         *reg_target = reg_d;
         break;
      case MIPS_FN_MTLO:
         *reg_op0 = reg_s;
         *reg_target = PSX_REG_LO;
         break;
      case MIPS_FN_MULTU:
      case MIPS_FN_DIV:
      case MIPS_FN_DIVU:
        *reg_op0 = reg_s;
        *reg_op1 = reg_t;
        /* XXX It's actually LO and HI, but for the moment we only
           support a single target reg in the logic. That being said
           I don't think it's an issue: HI and LO cannot be addressed
           directly by regular instructions, you have to use
           MTHI/MFHI/MTLO/MFLO to move them to a GPR so I can't think
           of any situation where a data hazard could occur. */
        *reg_target = PSX_REG_LO;
        break;
      case MIPS_FN_ADD:
      case MIPS_FN_ADDU:
      case MIPS_FN_SUBU:
      case MIPS_FN_AND:
      case MIPS_FN_OR:
      case MIPS_FN_SLT:
      case MIPS_FN_SLTU:
         *reg_target = reg_d;
         *reg_op0 = reg_s;
         *reg_op1 = reg_t;
         break;
      case 0x1f:
      case 0x34:
         /* Illegal */
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x (sub: 0x%x)\n",
                instruction,
                instruction & 0x3f);
         abort();
      }
      break;
   case MIPS_OP_BXX:
      if (((instruction >> 17) & 0xf) == 8) {
         /* Link */
         *reg_target = PSX_REG_RA;
      }
      *reg_op0 = reg_s;
      type = OP_BRANCH_COND;
      break;
   case MIPS_OP_J:
      type = OP_BRANCH_ALWAYS;
      break;
   case MIPS_OP_JAL:
      type = OP_BRANCH_ALWAYS;
      *reg_target = PSX_REG_RA;
      break;
   case MIPS_OP_BEQ:
   case MIPS_OP_BNE:
      *reg_op0 = reg_s;
      *reg_op1 = reg_t;
      type = OP_BRANCH_COND;
      break;
   case MIPS_OP_BLEZ:
   case MIPS_OP_BGTZ:
      *reg_op0 = reg_s;
      type = OP_BRANCH_COND;
      break;
   case MIPS_OP_ADDI:
   case MIPS_OP_ADDIU:
   case MIPS_OP_SLTI:
   case MIPS_OP_SLTIU:
   case MIPS_OP_ANDI:
   case MIPS_OP_ORI:
      *reg_target = reg_t;
      *reg_op0    = reg_s;
      break;
   case MIPS_OP_LUI:
      *reg_target = reg_t;
      break;
   case MIPS_OP_COP0:
      switch (reg_s) {
      case MIPS_COP_MFC:
         *reg_target = reg_t;
         break;
      case MIPS_COP_MTC:
         *reg_op0 = reg_t;
         break;
      case MIPS_COP_RFE:
         break;
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case MIPS_OP_LBU:
   case MIPS_OP_LB:
   case MIPS_OP_LHU:
   case MIPS_OP_LH:
   case MIPS_OP_LW:
      *reg_target = reg_t;
      *reg_op0 = reg_s;
      type = OP_LOAD;
      break;
   case 0x28: /* SB */
   case 0x29: /* SH */
   case 0x2b: /* SW */
      *reg_op0 = reg_s;
      *reg_op1 = reg_t;
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
             instruction,
             instruction >> 26);
      abort();
   }

   return type;
}

static void dynarec_emit_instruction(struct dynarec_compiler *compiler,
                                     uint32_t instruction,
                                     enum PSX_REG reg_target,
                                     enum PSX_REG reg_op0,
                                     enum PSX_REG reg_op1) {

   uint16_t imm = instruction & 0xffff;
   int16_t  simm_se = instruction & 0xffff;
   uint32_t imm_se = (int16_t)simm_se;
   uint8_t  shift = (instruction >> 6) & 0x1f;

   DYNAREC_LOG("Emitting 0x%08x\n", instruction);

   switch (instruction >> 26) {
   case MIPS_OP_FN:
      switch (instruction & 0x3f) {
      case MIPS_FN_SLL:
         emit_shift_imm(compiler,
                        reg_target,
                        reg_op0,
                        shift,
                        dynasm_emit_sll);
         break;
      case MIPS_FN_SRL:
         emit_shift_imm(compiler,
                        reg_target,
                        reg_op0,
                        shift,
                        dynasm_emit_srl);
         break;
      case MIPS_FN_SRA:
         emit_shift_imm(compiler,
                        reg_target,
                        reg_op0,
                        shift,
                        dynasm_emit_sra);
         break;
      case MIPS_FN_SLLV:
         emit_shift_reg(compiler,
                        reg_target,
                        reg_op0,
                        reg_op1,
                        dynasm_emit_sllv);
         break;
      case MIPS_FN_SRLV:
         emit_shift_reg(compiler,
                        reg_target,
                        reg_op0,
                        reg_op1,
                        dynasm_emit_srlv);
         break;
      case MIPS_FN_SRAV:
         emit_shift_reg(compiler,
                        reg_target,
                        reg_op0,
                        reg_op1,
                        dynasm_emit_srav);
         break;
      case MIPS_FN_JR:
         emit_jalr(compiler, reg_op0, PSX_REG_R0);
         break;
      case MIPS_FN_JALR:
         emit_jalr(compiler, reg_op0, reg_target);
         break;
      case MIPS_FN_SYSCALL:
         dynasm_emit_exit(compiler, DYNAREC_EXIT_SYSCALL, instruction >> 6);
         break;
      case MIPS_FN_BREAK:
         if (compiler->state->options & DYNAREC_OPT_EXIT_ON_BREAK) {
            dynasm_emit_exit(compiler, DYNAREC_EXIT_BREAK, instruction >> 6);
         } else {
            dynasm_emit_exception(compiler, PSX_EXCEPTION_BREAK);
         }
         break;
      case MIPS_FN_MFHI:
      case MIPS_FN_MTHI:
      case MIPS_FN_MFLO:
      case MIPS_FN_MTLO:
         if (reg_target == PSX_REG_R0) {
            /* nop */
            break;
         }

         if (reg_op0 == PSX_REG_R0) {
            dynasm_emit_li(compiler, reg_target, 0);
            break;
         }

         dynasm_emit_mov(compiler, reg_target, reg_op0);
         break;
      case MIPS_FN_MULTU:
         if (reg_op0 == PSX_REG_R0 || reg_op1 == PSX_REG_R0) {
            // Multiplication by zero yields zero
            dynasm_emit_li(compiler, PSX_REG_LO, 0);
            dynasm_emit_li(compiler, PSX_REG_HI, 0);
         } else {
            dynasm_emit_multu(compiler, reg_op0, reg_op1);
         }
         break;
      case MIPS_FN_DIV:
         dynasm_emit_div(compiler, reg_op0, reg_op1);
         break;
      case MIPS_FN_DIVU:
         dynasm_emit_divu(compiler, reg_op0, reg_op1);
         break;
      case MIPS_FN_ADD:
         emit_add(compiler,
                  reg_target,
                  reg_op0,
                  reg_op1);
         break;
      case MIPS_FN_ADDU:
         emit_addu(compiler,
                   reg_target,
                   reg_op0,
                   reg_op1);
         break;
      case MIPS_FN_SUBU:
         emit_subu(compiler,
                   reg_target,
                   reg_op0,
                   reg_op1);
         break;
      case MIPS_FN_AND:
         emit_and(compiler,
                  reg_target,
                  reg_op0,
                  reg_op1);
         break;
      case MIPS_FN_OR:
         emit_or(compiler,
                 reg_target,
                 reg_op0,
                 reg_op1);
         break;
      case MIPS_FN_SLT:
         if (reg_target == PSX_REG_R0) {
            /* NOP */
            break;
         }

         if (reg_op0 == PSX_REG_R0 && reg_op1 == PSX_REG_R0) {
            /* 0 isn't less than 0 */
            dynasm_emit_li(compiler, reg_target, 0);
            break;
         }

         dynasm_emit_slt(compiler, reg_target, reg_op0, reg_op1);
         break;
      case MIPS_FN_SLTU:
         if (reg_target == PSX_REG_R0) {
            /* NOP */
            break;
         }

         if (reg_op1 == PSX_REG_R0) {
            /* Nothing is less than 0 */
            dynasm_emit_li(compiler, reg_target, 0);
            break;
         }

         dynasm_emit_sltu(compiler, reg_target, reg_op0, reg_op1);
         break;
      case 0x1f:
      case 0x34:
         /* Illegal */
         dynasm_emit_exception(compiler, PSX_EXCEPTION_ILLEGAL_INSTRUCTION);
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case MIPS_OP_BXX:
      emit_bxx(compiler, simm_se, reg_target, reg_op0, (instruction >> 16) & 1);
      break;
   case MIPS_OP_J:
      emit_j(compiler, instruction);
      break;
   case MIPS_OP_JAL:
      emit_jal(compiler, instruction);
      break;
   case MIPS_OP_BEQ:
      emit_beq(compiler, simm_se, reg_op0, reg_op1);
      break;
   case MIPS_OP_BNE:
      emit_bne(compiler, simm_se, reg_op0, reg_op1);
      break;
   case MIPS_OP_BLEZ:
      emit_blez(compiler, simm_se, reg_op0);
      break;
   case MIPS_OP_BGTZ:
      emit_bgtz(compiler, simm_se, reg_op0);
      break;
   case MIPS_OP_ADDI:
      emit_addi(compiler, reg_target, reg_op0, imm_se);
      break;
   case MIPS_OP_ADDIU:
      emit_addiu(compiler, reg_target, reg_op0, imm_se);
      break;
   case 0x0a: /* SLTI */
      if (reg_target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      dynasm_emit_slti(compiler, reg_target, reg_op0, imm_se);
      break;
   case 0x0b: /* SLTIU */
      if (reg_target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      if (imm_se == 0) {
         /* Nothing is less than 0 */
         dynasm_emit_li(compiler, reg_target, 0);
         break;
      }

      dynasm_emit_sltiu(compiler, reg_target, reg_op0, imm_se);
      break;
   case MIPS_OP_ANDI:
      emit_andi(compiler, reg_target, reg_op0, imm);
      break;
   case MIPS_OP_ORI:
      emit_ori(compiler, reg_target, reg_op0, imm);
      break;
   case MIPS_OP_LUI:
      if (reg_target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      dynasm_emit_li(compiler, reg_target, ((uint32_t)imm) << 16);
      break;
   case MIPS_OP_COP0:
      switch ((instruction >> 21) & 0x1f) {
      case MIPS_COP_MFC:
         dynasm_emit_mfc0(compiler, reg_target, (instruction >> 11) & 0x1f);
         break;
      case MIPS_COP_MTC:
         dynasm_emit_mtc0(compiler, reg_op0, (instruction >> 11) & 0x1f);
         break;
      case MIPS_COP_RFE:
         emit_rfe(compiler, instruction);
         break;
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case MIPS_OP_LB:
      dynasm_emit_lb(compiler, reg_target, imm, reg_op0);
      break;
   case MIPS_OP_LBU:
      dynasm_emit_lbu(compiler, reg_target, imm, reg_op0);
      break;
   case MIPS_OP_LH:
      dynasm_emit_lh(compiler, reg_target, imm, reg_op0);
      break;
   case MIPS_OP_LHU:
      dynasm_emit_lhu(compiler, reg_target, imm, reg_op0);
      break;
   case MIPS_OP_LW:
      dynasm_emit_lw(compiler, reg_target, imm, reg_op0);
      break;
   case 0x28: /* SB */
      dynasm_emit_sb(compiler, reg_op0, imm, reg_op1);
      break;
   case 0x29: /* SH */
      dynasm_emit_sh(compiler, reg_op0, imm, reg_op1);
      break;
   case 0x2b: /* SW */
      dynasm_emit_sw(compiler, reg_op0, imm, reg_op1);
      break;
   default:
      printf("Dynarec encountered unsupported instruction %08x\n",
             instruction);
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
   struct dynarec_block    *block;
   const uint8_t           *block_start;
   const uint8_t           *block_end;
   const uint8_t           *block_max;
   const uint8_t           *cur;
   enum optype              optype = OP_SIMPLE;
   uint32_t                 canonical_addr;
   bool                     eob;

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
      uint32_t instruction = load_le(cur);

      /* Various decodings of the fields, of course they won't all be
         valid for a given instruction. For now I'll trust the
         compiler to optimize this correctly, otherwise it might be
         better to move them to the instructions where they make
         sense.*/
      enum PSX_REG reg_target;
      enum PSX_REG reg_op0;
      enum PSX_REG reg_op1;
      uint32_t ds_instruction = 0;
      int has_branch_delay_slot;
      int has_load_delay_slot;
      int has_delay_slot;

      DYNAREC_LOG("Compiling 0x%08x @ 0x%08x\n", instruction, compiler.pc);

      compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;

      optype =
         dynarec_instruction_registers(instruction,
                                       &reg_target,
                                       &reg_op0,
                                       &reg_op1);

      has_branch_delay_slot =
         (optype == OP_BRANCH_ALWAYS) || (optype == OP_BRANCH_COND);
      has_load_delay_slot = (optype == OP_LOAD);
      has_delay_slot = has_branch_delay_slot || has_load_delay_slot;

      if ((optype == OP_BRANCH_ALWAYS) || (optype == OP_EXCEPTION)) {
         /* We are certain that the execution won't continue after
            this instruction (besides potentially the load delay slot
            which is handled below) */
         eob = true;
      }

      if (has_delay_slot && (cur + 4) < block_max) {
         ds_instruction = load_le(cur + 4);
      } else {
         /* Assume the delay slot is a NOP */
         ds_instruction = 0;
      }

      if (has_load_delay_slot &&
          reg_target != PSX_REG_R0 &&
          ds_instruction != 0) {
         /* We have to check if the next instruction conflicts with
            the load target */
         enum optype ds_type;
         enum PSX_REG ds_target;
         enum PSX_REG ds_op0;
         enum PSX_REG ds_op1;

         ds_type =
            dynarec_instruction_registers(ds_instruction,
                                          &ds_target,
                                          &ds_op0,
                                          &ds_op1);

         if (ds_target == reg_target) {
            /* The instruction in the delay slot overwrites the value,
               effectively making the LW useless (or only useful for
               side-effect). Seems odd but easy enough to implement,
               we can just pretend that this load just targets R0
               since it's functionally equivalent. */
            reg_target = PSX_REG_R0;

            dynarec_emit_instruction(&compiler, instruction,
                                     reg_target, reg_op0, reg_op1);

         } else if (reg_target == ds_op0 || reg_target == ds_op1) {
            /* That's a bit trickier, we need to make sure that the
               previous value of `reg_target` is used in the load
               delay. */

            if (ds_type == OP_BRANCH_ALWAYS ||
                ds_type == OP_BRANCH_COND ||
                ds_type == OP_EXCEPTION) {
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

               if (reg_op0 == ds_target) {
                  needs_dt = true;
                  reg_op0 = PSX_REG_DT;
               }

               if (reg_op1 == ds_target) {
                  needs_dt = true;
                  reg_op1 = PSX_REG_DT;
               }

               if (needs_dt) {
                  /* The instruction in the delay slot targets a register
                     used by the branch, we need to keep a copy. */
                  dynasm_emit_mov(&compiler, PSX_REG_DT, ds_target);
               }

               /* Emit instruction in load delay slot */
               compiler.pc += 4;
               dynarec_emit_instruction(&compiler, ds_instruction,
                                           ds_target, ds_op0, ds_op1);
               compiler.pc -= 4;

               /* Emit load instruction */
               dynarec_emit_instruction(&compiler, instruction,
                                        reg_target, reg_op0, reg_op1);

               /* Since we reordered we must jump ahead not to execute
                  the load delay instruction twice */
               cur += 4;
               compiler.pc += 4;
               compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;
            }
         } else {
            /* We don't have any hazard, we can simply emit the load
               normally */
            dynarec_emit_instruction(&compiler, instruction,
                                     reg_target, reg_op0, reg_op1);
         }
      } else if (has_branch_delay_slot &&
                 /* Special case a NOP in the delay slot since it's
                    fairly common. A branch with a NOP in the delay
                    slot behaves like a common instruction. */
                 ds_instruction != 0) {
         /* We have to run the delay slot before the actual
          * jump. First let's make sure that we don't have a data
          * hazard.
          */
         enum PSX_REG ds_target;
         enum PSX_REG ds_op0;
         enum PSX_REG ds_op1;
         enum optype ds_type;
         int needs_dt = 0;

         ds_type =
            dynarec_instruction_registers(ds_instruction,
                                          &ds_target,
                                          &ds_op0,
                                          &ds_op1);

         if (ds_type == OP_BRANCH_ALWAYS ||
             ds_type == OP_BRANCH_COND ||
             ds_type == OP_EXCEPTION) {
            /* Nested branch delay slot or exception in delay
               slot. This would be a pain to implement. Let's hope the
               average game doesn't require something like that. */
            dynasm_emit_exit(&compiler, DYNAREC_EXIT_UNIMPLEMENTED, __LINE__);
         } else if (ds_type == OP_LOAD) {
            /* Emitting this directly is technically inaccurate but
               probably fine the vast majority of the time (relying on
               load delay slot behaviour across a jump sounds nasty,
               but who knows).  */
#if 0
            dynasm_emit_exit(&compiler, DYNAREC_EXIT_UNIMPLEMENTED, __LINE__);
#endif
         }

         if (ds_target != PSX_REG_R0) {
            /* Check for data hazard */
            if (ds_target == reg_target) {
               /* Not sure what happens if the jump and delay slot
                  write to the same register. If the jump wins then we
                  have nothing to do, if it's the instruction we just
                  need to replace the jump with the equivalent
                  instruction that doesn't link. */
               DYNAREC_FATAL("Register race on branch target\n");
            }

            if (ds_target == reg_op0) {
               needs_dt = 1;
               reg_op0 = PSX_REG_DT;
            }

            if (ds_target == reg_op1) {
               needs_dt = 1;
               reg_op1 = PSX_REG_DT;
            }

            if (needs_dt) {
               /* The instruction in the delay slot targets a register
                  used by the branch, we need to keep a copy. */
               dynasm_emit_mov(&compiler, PSX_REG_DT, ds_target);
            }
         }

         /* Emit instruction in branch delay slot */
         compiler.pc += 4;
         dynarec_emit_instruction(&compiler, ds_instruction,
                                  ds_target, ds_op0, ds_op1);
         compiler.pc -= 4;

         /* Emit branch instruction */
         dynarec_emit_instruction(&compiler, instruction,
                                  reg_target, reg_op0, reg_op1);
         /* Move ahead not to emit the same instruction twice */
         cur += 4;
         compiler.pc += 4;
         compiler.spent_cycles += PSX_CYCLES_PER_INSTRUCTION;
      } else {
         /* Boring old instruction, no delay slot involved. */
         dynarec_emit_instruction(&compiler, instruction,
                                  reg_target, reg_op0, reg_op1);
      }

#if 1
      {
         int fd = open("/tmp/dump.amd64", O_WRONLY | O_CREAT| O_TRUNC, 0644);

         write(fd, compiler.block + 1,
               compiler.map - (uint8_t*)(compiler.block + 1));

         close(fd);
      }
#endif
   }

   /* We're done with this block */
   if ((optype != OP_BRANCH_ALWAYS) && (optype != OP_EXCEPTION)) {
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
   patch_offset = NULL;
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
