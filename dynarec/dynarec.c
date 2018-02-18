#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "dynarec.h"
#include "dynarec-compiler.h"

static const uint32_t region_mask[8] = {
   0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, /* KUSEG: 2048MB */
   0x7fffffff,                                     /* KSEG0:  512MB */
   0x1fffffff,                                     /* KSEG1:  512MB */
   0xffffffff, 0xffffffff,                         /* KSEG2: 1024MB */
};

/* Mask "addr" to remove the region bits and return a "canonical"
   address. */
static uint32_t dynarec_mask_address(uint32_t addr) {
   return addr & region_mask[addr >> 29];
}

/* Returns the maximum size a recompiled page can take, in bytes. */
static uint32_t dynarec_max_page_size(void) {
   uint32_t s;

   /* We may end up duplicating instructions in the load delay slots,
      and we need an additional pseudo-instruction at the end to jump
      to the next page. The typical page size will be a fraction of
      that but we'll rely on the kernel's lazy memory allocation to
      avoid wasting memory. */
   s = (DYNAREC_PAGE_INSTRUCTIONS * 2 + 1) * DYNAREC_INSTRUCTION_MAX_LEN;

   /* Align to a real hardware page size to avoid having a dynarec
      page starting at the end of a hardware page. Assume that pages
      are always 4096B long. */
   s = (s + 4095) & ~4095U;

   return s;
}

struct dynarec_state *dynarec_init(uint32_t *ram,
                                   uint32_t *scratchpad,
                                   const uint32_t *bios) {
   struct dynarec_state *state;

   state = calloc(1, sizeof(*state));
   if (state == NULL) {
      return NULL;
   }

   state->ram = ram;
   state->scratchpad = scratchpad;
   state->bios = bios;

   state->map_len = dynarec_max_page_size() * DYNAREC_TOTAL_PAGES;
   state->map = mmap(NULL,
                     state->map_len,
#ifdef DYNAREC_DEBUG
                     PROT_READ |
#endif
                     PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1,
                     0);

   if (state->map == MAP_FAILED) {
      free(state);
      return NULL;
   }

   memcpy(state->region_mask, region_mask, sizeof(region_mask));

   return state;
}

void dynarec_delete(struct dynarec_state *state) {
   struct dynarec_page *page;
   unsigned i;

   munmap(state->map, state->map_len);
   free(state);
}

void dynarec_set_pc(struct dynarec_state *state, uint32_t pc) {
   state->pc = pc;
}

/* Find the offset of the page containing `addr`. Returns -1 if no
   page is found. */
static int32_t dynarec_find_page_index(struct dynarec_state *state,
                                       uint32_t addr) {
   addr = dynarec_mask_address(addr);

   /* RAM is mirrored 4 times */
   if (addr < (PSX_RAM_SIZE * 4)) {
      addr = addr % PSX_RAM_SIZE;

      return addr / DYNAREC_PAGE_SIZE;
   }

   if (addr >= PSX_BIOS_BASE && addr < (PSX_BIOS_BASE + PSX_BIOS_SIZE)) {
      addr -= PSX_BIOS_BASE;

      /* BIOS pages follow the RAM's */
      return (addr / DYNAREC_PAGE_SIZE) + DYNAREC_RAM_PAGES;
   }

   /* Unhandled address. TODO: add Expansion 1 @ 0x1f000000 */
   return -1;
}

static uint8_t *dynarec_page_start(struct dynarec_state *state,
                                   uint32_t page_index) {
   return state->map + (dynarec_max_page_size() * page_index);
}

static void add_local_patch(struct dynarec_compiler *compiler,
                            uint8_t *patch_loc,
                            uint32_t target) {

   uint32_t pos = compiler->local_patch_len;

   assert(pos < DYNAREC_PAGE_INSTRUCTIONS);
   compiler->local_patch[pos].patch_loc = patch_loc;
   compiler->local_patch[pos].target = target;
   compiler->local_patch_len++;
}

static void emit_jump(struct dynarec_compiler *compiler,
                      uint32_t instruction) {
   uint32_t imm_jump = (instruction & 0x3ffffff) << 2;
   uint32_t target;
   int32_t target_page;

   target = compiler->pc & 0xf0000000;
   target |= imm_jump;

   /* Test if the target is in the current page */
   target_page = dynarec_find_page_index(compiler->state, target);
   if (target_page < 0) {
      printf("Dynarec: jump to unhandled address 0x%08x", target);
   }

   if (target_page == compiler->page_index) {
      /* We're aiming at the current page, we don't have to worry
         about the target being invalidated and we can hardcode the
         jump target */
      uint32_t pc_index = compiler->pc % DYNAREC_PAGE_SIZE;
      uint32_t target_index = target % DYNAREC_PAGE_SIZE;

      /* Convert from bytes to instructions */
      pc_index >>= 2;
      target_index >>= 2;

      if (target_index <= pc_index) {
         /* We're jumping backwards, we already know the offset of the
            target instruction */
         printf("Patch backward call\n");
         abort();
      } else {
         /* We're jumping forward, we don't know where we're going (do
            we ever?). Add placeholder code and patch the right
            address later. */
         /* As a hint we compute the maximum possible offset */
         int32_t max_offset =
            (target_index - pc_index) * DYNAREC_INSTRUCTION_MAX_LEN;

         uint8_t *patch_pos = compiler->map;

         dynasm_emit_page_local_jump(compiler,
                                     max_offset,
                                     true);
         add_local_patch(compiler, patch_pos, target);
      }
   } else {
      printf("Encountered unimplemented non-local jump\n");
      abort();
   }
}

static void emit_bne(struct dynarec_compiler *compiler,
                     uint32_t instruction) {
   (void)instruction;
   dynasm_emit_exception(compiler, PSX_DYNAREC_UNIMPLEMENTED);
}

static void emit_blez(struct dynarec_compiler *compiler,
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

static void emit_or(struct dynarec_compiler *compiler,
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
         dynasm_emit_or(compiler, reg_target, reg_op0, reg_op1);
      }
   }
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
   case 0x00:
      switch (instruction & 0x3f) {
      case 0x00: /* SLL */
      case 0x03: /* SRA */
         *reg_target = reg_d;
         *reg_op0    = reg_t;
         break;
      case 0x10: /* MFHI */
         *reg_target = reg_d;
         break;
      case 0x13: /* MTLO */
         *reg_op0 = reg_s;
         break;
      case 0x25: /* OR */
         *reg_target = reg_d;
         *reg_op0 = reg_s;
         *reg_op1 = reg_t;
         break;
      case 0x1f:
      case 0x34:
         /* Illegal */
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case 0x02: /* J */
   case 0x05: /* BNE */
      *reg_op0 = reg_s;
      *reg_op1 = reg_t;
      ds = BRANCH_DELAY_SLOT;
      break;
   case 0x06: /* BLEZ */
      *reg_op0 = reg_s;
      ds = BRANCH_DELAY_SLOT;
      break;
   case 0x08: /* ADDI */
   case 0x09: /* ADDIU */
   case 0x0b: /* SLTIU */
   case 0x0c: /* ANDI */
   case 0x0d: /* ORI */
      *reg_target = reg_t;
      *reg_op0    = reg_s;
      break;
   case 0x0f: /* LUI */
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
      printf("Dynarec encountered unsupported instruction %08x\n",
             instruction);
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
   uint32_t imm_se = (int32_t)((int16_t)(instruction & 0xffff));
   uint8_t  shift = (instruction >> 6) & 0x1f;

   switch (instruction >> 26) {
   case 0x00:
      switch (instruction & 0x3f) {
      case 0x00: /* SLL */
         emit_shift_imm(compiler,
                        reg_target,
                        reg_op0,
                        shift,
                        dynasm_emit_sll);
      case 0x03: /* SRA */
         emit_shift_imm(compiler,
                        reg_target,
                        reg_op0,
                        shift,
                        dynasm_emit_sra);
         break;
      case 0x10: /* MFHI */
         dynasm_emit_mfhi(compiler, reg_target);
         break;
      case 0x13: /* MTLO */
         dynasm_emit_mtlo(compiler, reg_op0);
         break;
      case 0x25: /* OR */
         emit_or(compiler,
                 reg_target,
                 reg_op0,
                 reg_op1);
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
   case 0x02: /* J */
      emit_jump(compiler, instruction);
      break;
   case 0x05: /* BNE */
      emit_bne(compiler, instruction);
      break;
   case 0x06: /* BLEZ */
      emit_blez(compiler, instruction);
      break;
   case 0x08: /* ADDI */
      emit_addi(compiler, reg_target, reg_op0, imm_se);
      break;
   case 0x09: /* ADDIU */
      emit_addiu(compiler, reg_target, reg_op0, imm_se);
      break;
   case 0x0b: /* SLTIU */
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
   case 0x0f: /* LUI */
      if (reg_target == 0) {
         /* nop */
         break;
      }

      dynasm_emit_li(compiler, reg_target, ((uint32_t)imm) << 16);
      break;
   case 0x10: /* COP0 */
      switch ((instruction >> 21) & 0x1f) {
      case 0x04: /* MTC0 */
         dynasm_emit_mtc0(compiler, reg_op0, (instruction >> 11) & 0x1f);
         break;
      case 0x00: /* MFC0 */
      case 0x10: /* RFE */
      default:
         printf("Dynarec encountered unsupported COP0 instruction %08x\n",
                instruction);
         abort();
      }
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

static int dynarec_recompile(struct dynarec_state *state,
                             uint32_t page_index) {
   struct dynarec_page     *page;
   const uint32_t          *emulated_page;
   const uint32_t          *next_page;
   uint8_t                 *page_start;
   struct dynarec_compiler  compiler;
   unsigned                 i;

   DYNAREC_LOG("Recompiling page %u\n", page_index);

   state->page_valid[page_index] = 0;

   page_start = dynarec_page_start(state, page_index);

   compiler.state = state;
   compiler.page_index = page_index;
   compiler.local_patch_len = 0;
   compiler.map = page_start;

   if (page_index < DYNAREC_RAM_PAGES) {
      uint32_t ram_index = page_index;

      emulated_page = state->ram + DYNAREC_PAGE_INSTRUCTIONS * ram_index;
      compiler.pc = DYNAREC_PAGE_SIZE * ram_index;

      /* XXX This is not accurate if we're at the very end of the last
         mirror of memory. Not that I expect that it matters much. */
      ram_index = (ram_index + 1) % DYNAREC_RAM_PAGES;
      next_page = state->ram + DYNAREC_PAGE_INSTRUCTIONS * ram_index;
   } else {
      uint32_t bios_index = page_index - DYNAREC_RAM_PAGES;

      emulated_page = state->bios + DYNAREC_PAGE_INSTRUCTIONS * bios_index;
      compiler.pc = PSX_BIOS_BASE + DYNAREC_PAGE_SIZE * bios_index;

      /* XXX This is not accurate if we're at the very end of the
         BIOS. Not that I expect that it matters much. */
      bios_index = (bios_index + 1) % DYNAREC_BIOS_PAGES;
      next_page = state->bios + DYNAREC_PAGE_INSTRUCTIONS * bios_index;
   }

   /* Helper macros for instruction decoding */
   for (i = 0; i < DYNAREC_PAGE_INSTRUCTIONS; i++, compiler.pc += 4) {
      uint32_t instruction = emulated_page[i];

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

      /* For now I assume every instruction takes exactly 5 cycles to
         execute. It's a pretty decent average but obviously in practice
         it varies a lot depending on the instruction, the icache, memory
         latency etc... */
      if (delay_slot == BRANCH_DELAY_SLOT) {
         /* We execute the delay slot *and* the jump */
         cycles = 10;
      } else {
         cycles = 5;
      }

      /* Decrease the event counter before we continue. */
      dynasm_counter_maintenance(&compiler, cycles);

      if (i + 1 < DYNAREC_PAGE_INSTRUCTIONS) {
         ds_instruction = emulated_page[i + 1];
      } else {
         ds_instruction = next_page[0];
      }

      if (delay_slot == LOAD_DELAY_SLOT) {
         DYNAREC_FATAL("Implement LDS\n");
      } else if (delay_slot == BRANCH_DELAY_SLOT &&
                 /* Special case a NOP in the delay slot since it's
                    fairly common. */
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
         uint8_t *patch_pos;

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
            /* This is technically inaccurate but probably
               fine. Remove after running more tests */
            dynasm_emit_exception(&compiler, PSX_DYNAREC_UNIMPLEMENTED);
         }

         if (reg_target) {
            DYNAREC_FATAL("Add check for branch target hazard\n");
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

         /* Emit instruction in branch delay */
         compiler.pc += 4;
         dynarec_emit_instruction(&compiler, ds_instruction,
                                  ds_target, ds_op0, ds_op1);
         compiler.pc -= 4;

         /* Emit branch instruction */
         dynarec_emit_instruction(&compiler, instruction,
                                  reg_target, reg_op0, reg_op1);

         /* In case this is a conditional branch we want to jump over
            the delay slot if it's not taken (otherwise we'll execute
            the instruction twice). */
         patch_pos = compiler.map;
         dynasm_emit_page_local_jump(&compiler,
                                     DYNAREC_INSTRUCTION_MAX_LEN,
                                     true);
         add_local_patch(&compiler, patch_pos, compiler.pc + 8);
      } else {
         /* Boring old instruction, no delay slot involved. */
         dynarec_emit_instruction(&compiler, instruction,
                                  reg_target, reg_op0, reg_op1);
      }

      assert(compiler.map - instruction_start <= DYNAREC_INSTRUCTION_MAX_LEN);

      printf("Emited:");
      for (; instruction_start != compiler.map; instruction_start++) {
         printf(" %02x", *instruction_start);
      }
      printf("\n");

#if 1
      {
         int fd = open("/tmp/dump.amd64", O_WRONLY | O_CREAT| O_TRUNC, 0644);

         write(fd, page_start, compiler.map - page_start);

         close(fd);
      }
#endif
   }

   state->page_valid[page_index] = 1;
   return 0;
}

int32_t dynarec_run(struct dynarec_state *state, int32_t cycles_to_run) {
   struct dynarec_page *page;
   int32_t page_index = dynarec_find_page_index(state, state->pc);

   if (page_index < 0) {
      DYNAREC_FATAL("Unhandled address PC 0x%08x\n", state->pc);
   }

   if (!state->page_valid[page_index]) {
      if (dynarec_recompile(state, page_index) < 0) {
         DYNAREC_FATAL("Recompilation failed\n");
      }
   }

   dynarec_fn_t f = (dynarec_fn_t)dynarec_page_start(state, page_index);

   return dynasm_execute(state, f, cycles_to_run);
}
