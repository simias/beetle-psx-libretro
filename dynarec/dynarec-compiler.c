#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "dynarec-compiler.h"
#include "psx-instruction.h"

/* Keep track of an unresolved local jump (i.e. within the same page)
   that will have to be patched once we're done recompiling the
   page. */
void dynarec_prepare_patch(struct dynarec_compiler *compiler) {
   uint32_t target = compiler->jump_target;
   uint32_t pos = compiler->local_patch_len;

   /* Jumps should always be 32bit-aligned*/
   assert((target & 3) == 0);

   assert(pos < DYNAREC_PAGE_INSTRUCTIONS);
   compiler->local_patch[pos].patch_loc = compiler->map;
   compiler->local_patch[pos].target = target;
   compiler->local_patch_len++;
}

/* Called when we're done recompiling a page to "patch" the correct
   target addresses */
static void resolve_local_patches(struct dynarec_compiler *compiler) {
   uint32_t i;

   for (i = 0; i < compiler->local_patch_len; i++) {
      uint8_t *patch_loc = compiler->local_patch[i].patch_loc;
      uint32_t target    = compiler->local_patch[i].target;
      uint8_t *target_loc;
      int32_t offset;

      /* We know for sure that the target is within the same page,
         compute the relative offset */
      target = target & (DYNAREC_PAGE_SIZE - 1);

      target_loc = compiler->dynarec_instructions[target >> 2];

      offset = target_loc - patch_loc;

      compiler->map = patch_loc;

      dynasm_patch(compiler, offset);
   }
}

static void emit_branch_or_jump(struct dynarec_compiler *compiler,
                                uint32_t target,
                                enum PSX_REG reg_a,
                                enum PSX_REG reg_b,
                                enum DYNAREC_JUMP_COND cond) {
   int32_t target_page;

   compiler->jump_target = target;

   /* Test if the target is in the current page */
   target_page = dynarec_find_page_index(compiler->state, target);
   if (target_page < 0) {
      DYNAREC_FATAL("Dynarec: jump to unhandled address 0x%08x", target);
   }

   if (target_page == compiler->page_index) {
      /* We're aiming at the current page, we don't have to worry
         about the target being invalidated and we can hardcode the
         jump target */
      uint32_t pc_index = compiler->pc % DYNAREC_PAGE_SIZE;
      uint32_t target_index = target % DYNAREC_PAGE_SIZE;
      uint8_t *dynarec_target;
      bool placeholder;

      /* Convert from bytes to instructions */
      pc_index >>= 2;
      target_index >>= 2;

      if (target_index <= pc_index) {
         /* We're jumping backwards, we already know the offset of the
            target instruction */
         placeholder = false;

         dynarec_target = compiler->dynarec_instructions[target_index];
      } else {
         uint32_t offset;
         /* We're jumping forward, we don't know where we're going (do
            we ever?). Add placeholder code and patch the right
            address later. */
         placeholder = true;
         /* As a hint we compute the maximum possible offset */
         offset =
            (target_index - pc_index) * DYNAREC_INSTRUCTION_MAX_LEN;

         dynarec_target = compiler->map + offset;
      }

      if (cond == DYNAREC_JUMP_ALWAYS) {
         dynasm_emit_page_local_jump(compiler,
                                     dynarec_target,
                                     placeholder);
      } else {
         dynasm_emit_page_local_jump_cond(compiler,
                                          dynarec_target,
                                          placeholder,
                                          reg_a,
                                          reg_b,
                                          cond);
      }
   } else {
      /* Non-local jump */
      if (cond == DYNAREC_JUMP_ALWAYS) {
         dynasm_emit_long_jump_imm(compiler,
                                   target);
      } else {
         dynasm_emit_long_jump_imm_cond(compiler,
                                        target,
                                        reg_a,
                                        reg_b,
                                        cond);
      }
   }
}

static void emit_jump(struct dynarec_compiler *compiler,
                      uint32_t instruction) {
   uint32_t imm_jump = (instruction & 0x3ffffff) << 2;
   uint32_t target;

   target = compiler->pc & 0xf0000000;
   target |= imm_jump;

   emit_branch_or_jump(compiler, target, 0, 0, DYNAREC_JUMP_ALWAYS);
}

static void emit_jal(struct dynarec_compiler *compiler,
                     uint32_t instruction) {
   /* Store return address in RA */
   dynasm_emit_li(compiler, PSX_REG_RA, compiler->pc + 8);
   emit_jump(compiler, instruction);
}

static void emit_branch(struct dynarec_compiler *compiler,
                        int16_t offset,
                        enum PSX_REG reg_a,
                        enum PSX_REG reg_b,
                        enum DYNAREC_JUMP_COND cond) {
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
   enum DYNAREC_JUMP_COND cond;

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

   emit_branch(compiler, offset, reg_op, PSX_REG_R0, cond);
}

static void emit_beq(struct dynarec_compiler *compiler,
                     int16_t offset,
                     enum PSX_REG reg_a,
                     enum PSX_REG reg_b) {
   enum DYNAREC_JUMP_COND cond = DYNAREC_JUMP_EQ;

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
                      uint32_t instruction) {
   (void)instruction;
   dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
}

static void emit_bgtz(struct dynarec_compiler *compiler,
                      uint32_t instruction) {
   (void)instruction;
   dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
}

typedef void (*shift_emit_fn_t)(struct dynarec_compiler *compiler,
                                enum PSX_REG reg_target,
                                enum PSX_REG reg_source,
                                uint8_t shift);

static void emit_shift_imm(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           enum PSX_REG reg_source,
                           uint8_t shift,
                           shift_emit_fn_t emit_fn) {
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

static void emit_addi(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_target,
                      enum PSX_REG reg_source,
                      uint16_t imm) {
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

   /* Watch out: we have to call this even if reg_target is R0 because
      it might still raise an exception so unlike ADDIU it's not a NOP
      in this case. */
   dynasm_emit_addi(compiler, reg_target, reg_source, imm);
}

static void emit_addiu(struct dynarec_compiler *compiler,
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
      dynasm_emit_mov(compiler, reg_target, reg_op0);
   } else {
      dynasm_emit_or(compiler, reg_target, reg_op0, reg_op1);
   }
}

static void emit_skip_next_instruction(struct dynarec_compiler *compiler) {
   uint32_t cur_target = compiler->jump_target;

   compiler->jump_target = compiler->pc + 8;

   dynasm_emit_page_local_jump(compiler,
                               compiler->map + DYNAREC_INSTRUCTION_MAX_LEN,
                               true);

   compiler->jump_target = cur_target;
}


static void emit_rfe(struct dynarec_compiler *compiler,
                     uint32_t instruction) {
   (void)instruction;
   dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
}

enum delay_slot {
   NO_DELAY = 0,
   BRANCH_DELAY_SLOT,
   LOAD_DELAY_SLOT,
};

/* Gets the general purpose registers referenced by `instruction`. At
 * most any instruction will reference one target and two "operand"
 * registers. For instruction that reference fewer registers the
 * remaining arguments are set to PSX_REG_R0.
 *
 * Returns the number of cycles taken by this instruction to execute.
 */
static enum delay_slot dynarec_instruction_registers(uint32_t instruction,
                                                     enum PSX_REG *reg_target,
                                                     enum PSX_REG *reg_op0,
                                                     enum PSX_REG *reg_op1) {
   uint8_t reg_d = (instruction >> 11) & 0x1f;
   uint8_t reg_t = (instruction >> 16) & 0x1f;
   uint8_t reg_s = (instruction >> 21) & 0x1f;
   enum delay_slot ds = NO_DELAY;

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
      case 0x08: /* JR */
         *reg_op0 = reg_s;
         ds = BRANCH_DELAY_SLOT;
         break;
      case 0x09: /* JALR */
         *reg_op0 = reg_s;
         *reg_target = reg_d;
         ds = BRANCH_DELAY_SLOT;
         break;
      case MIPS_FN_BREAK:
         break;
      case 0x10: /* MFHI */
         *reg_target = reg_d;
         break;
      case 0x13: /* MTLO */
         *reg_op0 = reg_s;
         break;
      case MIPS_FN_ADD:
      case MIPS_FN_ADDU:
      case 0x23: /* SUBU */
      case 0x24: /* AND */
      case 0x25: /* OR */
      case 0x2b: /* SLTU */
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
   case 0x01: /* BXX */
      if (((instruction >> 17) & 0xf) == 8) {
         /* Link */
         *reg_target = PSX_REG_RA;
      }
      *reg_op0 = reg_s;
      break;
   case 0x02: /* J */
      ds = BRANCH_DELAY_SLOT;
      break;
   case 0x03: /* JAL */
      ds = BRANCH_DELAY_SLOT;
      *reg_target = PSX_REG_RA;
      break;
   case 0x04: /* BEQ */
   case 0x05: /* BNE */
      *reg_op0 = reg_s;
      *reg_op1 = reg_t;
      ds = BRANCH_DELAY_SLOT;
      break;
   case 0x06: /* BLEZ */
      *reg_op0 = reg_s;
      ds = BRANCH_DELAY_SLOT;
      break;
   case 0x07: /* BGTZ */
      *reg_op0 = reg_s;
      ds = BRANCH_DELAY_SLOT;
      break;
   case 0x08: /* ADDI */
   case 0x09: /* ADDIU */
   case 0x0a: /* SLTI */
   case 0x0b: /* SLTIU */
   case 0x0c: /* ANDI */
   case 0x0d: /* ORI */
      *reg_target = reg_t;
      *reg_op0    = reg_s;
      break;
   case MIPS_OP_LUI:
      *reg_target = reg_t;
      break;
   case 0x10: /* COP0 */
      switch ((instruction >> 21) & 0x1f) {
      case 0x00: /* MFC0 */
         *reg_target = reg_t;
         break;
      case 0x04: /* MTC0 */
         *reg_op0 = reg_t;
         break;
      case 0x10: /* RFE */
         break;
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case 0x20: /* LB */
   case 0x23: /* LW */
   case 0x24: /* LBU */
      *reg_target = reg_t;
      *reg_op0 = reg_s;
      ds = LOAD_DELAY_SLOT;
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

   return ds;
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
      case 0x08: /* JR */
         dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
         break;
      case 0x09: /* JALR */
         dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
         break;
      case MIPS_FN_BREAK:
         if (compiler->state->options & DYNAREC_OPT_EXIT_ON_BREAK) {
            dynasm_emit_exit(compiler, DYNAREC_EXIT_BREAK, instruction >> 6);
         } else {
            dynasm_emit_exception(compiler, PSX_EXCEPTION_BREAK);
         }
         break;
      case 0x10: /* MFHI */
         dynasm_emit_mfhi(compiler, reg_target);
         break;
      case 0x13: /* MTLO */
         dynasm_emit_mtlo(compiler, reg_op0);
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
      case 0x23: /* SUBU */
         emit_subu(compiler,
                   reg_target,
                   reg_op0,
                   reg_op1);
         break;
      case 0x24: /* AND */
         emit_and(compiler,
                  reg_target,
                  reg_op0,
                  reg_op1);
         break;
      case 0x25: /* OR */
         emit_or(compiler,
                 reg_target,
                 reg_op0,
                 reg_op1);
         break;
      case 0x2b: /* SLTU */
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
   case 0x01: /* BXX */
      emit_bxx(compiler, simm_se, reg_target, reg_op0, (instruction >> 16) & 1);
      break;
   case 0x02: /* J */
      emit_jump(compiler, instruction);
      break;
   case 0x03: /* JAL */
      emit_jal(compiler, instruction);
      break;
   case 0x04: /* BEQ */
      emit_beq(compiler, simm_se, reg_op0, reg_op1);
      break;
   case 0x05: /* BNE */
      emit_bne(compiler, simm_se, reg_op0, reg_op1);
      break;
   case 0x06: /* BLEZ */
      emit_blez(compiler, instruction);
      break;
   case 0x07: /* BGTZ */
      emit_bgtz(compiler, instruction);
      break;
   case 0x08: /* ADDI */
      emit_addi(compiler, reg_target, reg_op0, imm_se);
      break;
   case 0x09: /* ADDIU */
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
   case 0x0c: /* ANDI */
      emit_andi(compiler, reg_target, reg_op0, imm);
      break;
   case 0x0d: /* ORI */
      emit_ori(compiler, reg_target, reg_op0, imm);
      break;
   case MIPS_OP_LUI:
      if (reg_target == PSX_REG_R0) {
         /* NOP */
         break;
      }

      dynasm_emit_li(compiler, reg_target, ((uint32_t)imm) << 16);
      break;
   case 0x10: /* COP0 */
      switch ((instruction >> 21) & 0x1f) {
      case 0x00: /* MFC0 */
         dynasm_emit_mfc0(compiler, reg_target, (instruction >> 11) & 0x1f);
         break;
      case 0x04: /* MTC0 */
         dynasm_emit_mtc0(compiler, reg_op0, (instruction >> 11) & 0x1f);
         break;
      case 0x10: /* RFE */
         emit_rfe(compiler, instruction);
         break;
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case 0x20: /* LB */
      dynasm_emit_lb(compiler, reg_target, imm, reg_op0);
      break;
   case 0x23: /* LW */
      dynasm_emit_lw(compiler, reg_target, imm, reg_op0);
      break;
   case 0x24: /* LBU */
      dynasm_emit_lbu(compiler, reg_target, imm, reg_op0);
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
   case 0x18:
   case 0x19:
   case 0x1b:
   case 0x1d:
   case 0x1e:
      /* Illegal */
      dynasm_emit_exception(compiler, PSX_EXCEPTION_ILLEGAL_INSTRUCTION);
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

int dynarec_recompile(struct dynarec_state *state,
                      uint32_t page_index) {
   const uint8_t           *emulated_page;
   const uint8_t           *next_page;
   uint8_t                 *page_start;
   struct dynarec_compiler  compiler = { 0 };
   unsigned                 i;

   DYNAREC_LOG("Recompiling page %u\n", page_index);

   state->page_valid[page_index] = 0;

   page_start = dynarec_page_start(state, page_index);

   compiler.state = state;
   compiler.page_index = page_index;
   compiler.local_patch_len = 0;
   compiler.map = page_start;

   /* We'll fill up each individual's instruction address as we
      recompile them */
   compiler.dynarec_instructions =
      &state->dynarec_instructions[page_index * DYNAREC_PAGE_INSTRUCTIONS];

   if (page_index < DYNAREC_RAM_PAGES) {
      uint32_t ram_index = page_index;

      emulated_page = state->ram + DYNAREC_PAGE_SIZE * ram_index;
      compiler.pc = DYNAREC_PAGE_SIZE * ram_index;

      /* XXX This is not accurate if we're at the very end of the last
         mirror of memory. Not that I expect that it matters much. */
      ram_index = (ram_index + 1) % DYNAREC_RAM_PAGES;
      next_page = state->ram + DYNAREC_PAGE_SIZE * ram_index;
   } else {
      uint32_t bios_index = page_index - DYNAREC_RAM_PAGES;

      emulated_page = state->bios + DYNAREC_PAGE_SIZE * bios_index;
      compiler.pc = PSX_BIOS_BASE + DYNAREC_PAGE_SIZE * bios_index;

      /* XXX This is not accurate if we're at the very end of the
         BIOS. Not that I expect that it matters much. */
      bios_index = (bios_index + 1) % DYNAREC_BIOS_PAGES;
      next_page = state->bios + DYNAREC_PAGE_SIZE * bios_index;
   }

   /* Helper macros for instruction decoding */
   for (i = 0; i < DYNAREC_PAGE_INSTRUCTIONS; i++, compiler.pc += 4) {
      unsigned pg_byte_off = i * 4;
      uint32_t instruction = load_le(emulated_page + pg_byte_off);

      /* Various decodings of the fields, of course they won't all be
         valid for a given instruction. For now I'll trust the
         compiler to optimize this correctly, otherwise it might be
         better to move them to the instructions where they make
         sense.*/
      enum PSX_REG reg_target;
      enum PSX_REG reg_op0;
      enum PSX_REG reg_op1;
      enum delay_slot delay_slot;
      uint8_t *instruction_start;
      unsigned cycles;
      uint32_t ds_instruction;

      DYNAREC_LOG("Compiling 0x%08x\n", instruction);

      delay_slot =
         dynarec_instruction_registers(instruction,
                                       &reg_target,
                                       &reg_op0,
                                       &reg_op1);

      instruction_start = compiler.map;
      compiler.dynarec_instructions[i] = instruction_start;

      /* For now I assume every instruction takes exactly 4 cycles to
         execute. It's rather optimistic (the average in practice is
         closer to 5 cycles) but obviously in practice it varies a lot
         depending on the instruction, the icache, memory latency
         etc... */
      cycles = 4;

      if (i + 1 < DYNAREC_PAGE_INSTRUCTIONS) {
         ds_instruction = load_le(emulated_page + pg_byte_off + 4);
      } else {
         ds_instruction = load_le(next_page);
      }

      if (delay_slot == LOAD_DELAY_SLOT &&
          reg_target != PSX_REG_R0 &&
          ds_instruction != 0) {
         /* We have to check if the next instruction references the
            load target */
         enum delay_slot ds_delay_slot;
         enum PSX_REG ds_target;
         enum PSX_REG ds_op0;
         enum PSX_REG ds_op1;

         ds_delay_slot =
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

            dynasm_counter_maintenance(&compiler, cycles);
            dynarec_emit_instruction(&compiler, instruction,
                                     reg_target, reg_op0, reg_op1);

         } else if (reg_target == ds_op0 || reg_target == ds_op1) {
            /* That's a bit trickier, we need to make sure that the
               previous value of `reg_target` is used in the load
               delay. The only way to have this work in all cases
               (consider if we have a branch in the delay slot for
               instance) */

            if (ds_delay_slot != NO_DELAY) {
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

               dynasm_counter_maintenance(&compiler, cycles * 2);

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

               /* Then we want to step over the instruction in the
                  delay slot that's going to be emitted next since
                  we've already executed it here. */
               emit_skip_next_instruction(&compiler);
            }
         } else {
            /* We don't have any hazard, we can emit the load */
            dynasm_counter_maintenance(&compiler, cycles);
            dynarec_emit_instruction(&compiler, instruction,
                                     reg_target, reg_op0, reg_op1);
         }

      } else if (delay_slot == BRANCH_DELAY_SLOT &&
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
         enum delay_slot ds_delay_slot;
         int needs_dt = 0;

         dynasm_counter_maintenance(&compiler, cycles * 2);

         ds_delay_slot =
            dynarec_instruction_registers(ds_instruction,
                                          &ds_target,
                                          &ds_op0,
                                          &ds_op1);

         if (ds_delay_slot == BRANCH_DELAY_SLOT) {
            /* This would be a pain to implement. Let's not the
               average game doesn't require something like that. */
            dynasm_emit_exception(&compiler, PSX_DYNAREC_UNIMPLEMENTED);
         } else if (ds_delay_slot == LOAD_DELAY_SLOT) {
            /* This is technically inaccurate but probably fine the
               vast majority of the time (relying on load delay slot
               behaviour across a jump sounds nasty, but who
               knows). Remove after running more tests. */
            dynasm_emit_exception(&compiler, PSX_DYNAREC_UNIMPLEMENTED);
         }

         if (ds_target != 0) {
            /* Check for data hazard */
            if (ds_target == reg_target) {
               DYNAREC_FATAL("Register race on branch target\n");
            }

            if (ds_target == reg_op0) {
               needs_dt = 1;
               reg_op0 = PSX_REG_DT;
            }

            if (ds_target == reg_op1) {
               needs_dt = 1;
               reg_op0 = PSX_REG_DT;
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

         /* In case this is a conditional branch we want to jump over
            the delay slot if it's not taken (otherwise we'll execute
            the instruction twice).

            XXX We could skip this if the branch/jump is unconditional
            since this code will never be reached. */
         emit_skip_next_instruction(&compiler);
      } else {
         /* Boring old instruction, no delay slot involved. */
         dynasm_counter_maintenance(&compiler, cycles);
         dynarec_emit_instruction(&compiler, instruction,
                                  reg_target, reg_op0, reg_op1);
      }

      assert(compiler.map - instruction_start <= DYNAREC_INSTRUCTION_MAX_LEN);

#ifdef DYNAREC_DEBUG
      DYNAREC_LOG("Emited:");
      for (; instruction_start != compiler.map; instruction_start++) {
         printf(" %02x", *instruction_start);
      }
      printf("\n");
#endif

#if 1
      {
         int fd = open("/tmp/dump.amd64", O_WRONLY | O_CREAT| O_TRUNC, 0644);

         write(fd, page_start, compiler.map - page_start);

         close(fd);
      }
#endif
   }

   resolve_local_patches(&compiler);
   state->page_valid[page_index] = 1;
   return 0;
}
