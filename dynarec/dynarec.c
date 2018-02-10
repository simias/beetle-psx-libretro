#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include "dynarec.h"

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

struct dynarec_state *dynarec_init(uint32_t *ram,
                                   uint32_t *scratchpad,
                                   const uint32_t *bios,
                                   dynarec_store_cback memory_sw) {
   struct dynarec_state *state;
   unsigned i;

   state = malloc(sizeof(*state));
   if (state == NULL) {
      return NULL;
   }

   state->ram = ram;
   state->scratchpad = scratchpad;
   state->bios = bios;
   state->memory_sw = memory_sw;

   memcpy(state->region_mask, region_mask, sizeof(region_mask));
   memset(state->regs, 0, sizeof(state->regs));

   /* Initialize all pages as unmapped/invalid */
   for (i = 0; i < ARRAY_SIZE(state->pages); i++) {
      state->pages[i].valid = 0;
      state->pages[i].map = NULL;
      state->pages[i].map_len = 0;
   }

   return state;
}

void dynarec_delete(struct dynarec_state *state) {
   struct dynarec_page *page;
   unsigned i;

   for (i = 0; i < ARRAY_SIZE(state->pages); i++) {
      page = &state->pages[i];

      if (page->map_len > 0) {
         munmap(page->map, page->map_len);
      }
   }

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

/* Make sure that there's enough space after `pos` in `page` to add a
   new instruction. Otherwise reallocate a bigger buffer. Safe to call
   on an empty page.

   Returns `pos` if no reallocation is needed, the same position in
   the new buffer in case of reallocation or NULL if the reallocation
   fails. */
static uint8_t *dynarec_maybe_realloc(struct dynarec_page *page,
                                      uint8_t *pos) {
   uint8_t *new_map;
   uint32_t new_len;
   size_t   used;

   if (page->map_len == 0) {
      used = 0;
      /* Allocate twice the space required for a page where all
         instructions have the average size. */
      new_len = DYNAREC_INSTRUCTION_AVG_LEN * DYNAREC_PAGE_INSTRUCTIONS * 2;
   } else {
      used = pos - page->map;

      if (used + DYNAREC_INSTRUCTION_MAX_LEN < page->map_len) {
         /* We have enough space*/
         return pos;
      }

      /* Double the page size. May be a bit overkill, instead we could
         do something proportional to the number of instructions left
         to recompile ? */
      new_len = page->map_len * 2;
   }

   /* Reallocate */
   new_map = mmap(NULL,
                  new_len,
                  PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1,
                  0);

   if (new_map == MAP_FAILED) {
      perror("Dynarec mmap failed");
      return NULL;
   }

   if (page->map_len != 0) {
      /* Recopy the contents of the old page */
      memcpy(new_map, page->map, used);
      munmap(page->map, page->map_len);
   }
   page->map = new_map;
   page->map_len = new_len;

   return page->map + used;
}

static void add_local_patch(struct dynarec_compiler *compiler,
                            uint32_t page_offset,
                            uint32_t target_index) {

   uint32_t pos = compiler->local_patch_len;

   assert(pos < DYNAREC_PAGE_INSTRUCTIONS);
   compiler->local_patch[pos].page_offset = page_offset;
   compiler->local_patch[pos].target_index = target_index;
   compiler->local_patch_len++;
}

/* Gets the general purpose registers referenced by `instruction`. At
 * most any instruction will reference one target and two "operand"
 * registers. For instruction that reference fewer registers the
 * remaining arguments are set to PSX_REG_R0.
 *
 * Returns the number of cycles taken by this instruction to execute.
 */
static unsigned dynarec_instruction_registers(uint32_t instruction,
                                              enum PSX_REG *reg_target,
                                              enum PSX_REG *reg_op0,
                                              enum PSX_REG *reg_op1) {
   uint8_t reg_d = (instruction >> 11) & 0x1f;
   uint8_t reg_t = (instruction >> 16) & 0x1f;
   uint8_t reg_s = (instruction >> 21) & 0x1f;
   /* For now I assume every instruction takes exactly 5 cycles to
      execute. It's a pretty decent average but obviously in practice
      it varies a lot depending on the instruction, the icache, memory
      latency etc... */
   unsigned cycles = 5;

   *reg_target = PSX_REG_R0;
   *reg_op0    = PSX_REG_R0;
   *reg_op1    = PSX_REG_R0;

   switch (instruction >> 26) {
   case 0x00:
      switch (instruction & 0x3f) {
      case 0x00: /* SLL */
         *reg_target = reg_d;
         *reg_op0    = reg_t;
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x\n",
                instruction);
         abort();
      }
      break;
   case 0x02: /* J */
      /* We execute the delay slot *and* the jump */
      cycles *= 2;
      break;
   case 0x09: /* ADDIU */
   case 0x0d: /* ORI */
      *reg_target = reg_t;
      *reg_op0    = reg_s;
      break;
   case 0x0f: /* LUI */
      *reg_target = reg_t;
      break;
   case 0x2b: /* SW */
      *reg_op0 = reg_s;
      *reg_op1 = reg_t;
      break;
   default:
      printf("Dynarec encountered unsupported instruction %08x\n",
             instruction);
      abort();
   }

   return cycles;
}

static void emit_jump(struct dynarec_compiler *compiler,
                      uint32_t instruction,
                      uint32_t delay_slot) {
   uint32_t imm_jump = (instruction & 0x3ffffff) << 2;
   uint32_t target;
   int32_t target_page;

   if (delay_slot != 0) {
      printf("Unsupported jump delay slot\n");
      abort();
   }

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

         uint32_t page_offset = compiler->map - compiler->page->map;

         dynasm_emit_page_local_jump(compiler,
                                     max_offset,
                                     true);
         add_local_patch(compiler, page_offset, target_index);
      }
   } else {
      printf("Encountered unimplemented non-local jump\n");
      abort();
   }
}

static int dynarec_recompile(struct dynarec_state *state,
                             uint32_t page_index) {
   struct dynarec_page     *page;
   const uint32_t          *emulated_page;
   const uint32_t          *next_page;
   struct dynarec_compiler  compiler;
   unsigned                 i;

   page = &state->pages[page_index];
   page->valid = 0;

   compiler.state = state;
   compiler.page_index = page_index;
   compiler.page = page;
   compiler.local_patch_len = 0;

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
      uint32_t next_instruction;

      if (i + 1 < DYNAREC_PAGE_INSTRUCTIONS) {
         next_instruction = emulated_page[i + 1];
      } else {
         next_instruction = next_page[0];
      }

      /* Various decodings of the fields, of course they won't all be
         valid for a given instruction. For now I'll trust the
         compiler to optimize this correctly, otherwise it might be
         better to move them to the instructions where they make
         sense.*/
      enum PSX_REG reg_target;
      enum PSX_REG reg_op0;
      enum PSX_REG reg_op1;
      uint16_t imm = instruction & 0xffff;
      uint8_t  shift = (instruction >> 6) & 0x1f;
      uint8_t *instruction_start;
      unsigned cycles;

      printf("Compiling 0x%08x\n", instruction);

      cycles =
         dynarec_instruction_registers(instruction,
                                       &reg_target,
                                       &reg_op0,
                                       &reg_op1);

      compiler.map = dynarec_maybe_realloc(page, compiler.map);

      instruction_start = compiler.map;

      if (instruction_start == NULL) {
         return -1;
      }

      /* Decrease the event counter before we continue. */
      dynasm_counter_maintenance(&compiler, cycles);

      switch (instruction >> 26) {
      case 0x00:
         switch (instruction & 0x3f) {
         case 0x00: /* SLL */
            if (reg_target == 0 || (reg_target == reg_op0 && shift == 0)) {
               /* NOP. This is the prefered encoding (full 0s) */
               break;
            }

            if (reg_op0 == 0) {
               dynasm_emit_li(&compiler, reg_target, 0);
               break;
            }

            if (shift == 0) {
               dynasm_emit_mov(&compiler, reg_target, reg_op0);
               break;
            }

            dynasm_emit_sll(&compiler, reg_target, reg_op0, shift);
            break;

         default:
            printf("Dynarec encountered unsupported instruction %08x\n",
                   instruction);
            abort();
         }
         break;
      case 0x02: /* J */
         emit_jump(&compiler, instruction, next_instruction);
         break;
      case 0x09: /* ADDIU */
         if (reg_target == 0 || (reg_target == reg_op0 && imm == 0)) {
            /* NOP */
            break;
         }

         if (reg_op0 == 0) {
            dynasm_emit_li(&compiler, reg_target, imm);
            break;
         }

         if (imm == 0) {
            dynasm_emit_mov(&compiler, reg_target, reg_op0);
            break;
         }

         dynasm_emit_addiu(&compiler, reg_target, reg_op0, imm);
         break;

      case 0x0d: /* ORI */
         if (reg_target == 0 || (reg_target == reg_op0 && imm == 0)) {
            /* NOP */
            break;
         }

         if (reg_op0 == 0) {
            dynasm_emit_li(&compiler, reg_target, imm);
            break;
         }

         if (imm == 0) {
            dynasm_emit_mov(&compiler, reg_target, reg_op0);
            break;
         }

         dynasm_emit_ori(&compiler, reg_target, reg_op0, imm);
         break;
      case 0x0f: /* LUI */
         if (reg_target == 0) {
            /* nop */
            break;
         }

         dynasm_emit_li(&compiler, reg_target, ((uint32_t)imm) << 16);
         break;
      case 0x2b: /* SW */
         dynasm_emit_sw(&compiler, reg_op0, imm, reg_op1);
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x\n",
                instruction);
         return 0;
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

         write(fd, page->map, compiler.map - page->map);

         close(fd);
      }
#endif
   }

   page->valid = 1;
   return 0;
}

int32_t dynarec_run(struct dynarec_state *state, int32_t cycles_to_run) {
   struct dynarec_page *page;
   int32_t page_index = dynarec_find_page_index(state, state->pc);

   if (page_index < 0) {
      printf("Dynarec at unhandled address 0x%08x\n", state->pc);
      abort();
   }

   page = &state->pages[page_index];

   if (!page->valid) {
      if (dynarec_recompile(state, page_index) < 0) {
         printf("Dynarec recompilation failed\n");
         abort();
      }
   }

   dynarec_fn_t f = (dynarec_fn_t)page->map;

   return dynasm_execute(state, f, cycles_to_run);
}
