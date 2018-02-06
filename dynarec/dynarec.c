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

static int dynarec_recompile(struct dynarec_state *state,
                             uint32_t page_index) {
   struct dynarec_page     *page;
   const uint32_t          *emulated_page;
   struct dynarec_compiler  compiler;
   unsigned                 i;

   compiler.state = state;

   page = &state->pages[page_index];
   page->valid = 0;

   if (page_index < DYNAREC_RAM_PAGES) {
      emulated_page = state->ram + DYNAREC_PAGE_INSTRUCTIONS * page_index;
      compiler.pc = DYNAREC_PAGE_SIZE * page_index;
   } else {
      uint32_t bios_index = page_index - DYNAREC_RAM_PAGES;
      emulated_page = state->bios + DYNAREC_PAGE_INSTRUCTIONS * bios_index;
      compiler.pc = PSX_BIOS_BASE + DYNAREC_PAGE_SIZE * bios_index;
   }

   /* Helper macros for instruction decoding */
   for (i = 0; i < DYNAREC_PAGE_INSTRUCTIONS; i++, compiler.pc += 4) {
      uint32_t instruction = emulated_page[i];

      /* Various decodings of the fields, of course they won't all be
         valid for a given instruction. For now I'll trust the
         compiler to optimize this correctly, otherwise it might be
         better to move them to the instructions where they make
         sense.*/
      uint8_t  reg_d  = (instruction >> 11) & 0x1f;
      uint8_t  reg_t  = (instruction >> 16) & 0x1f;
      uint8_t  reg_s  = (instruction >> 21) & 0x1f;
      uint16_t imm    = instruction & 0xffff;
      uint8_t  shift  = (instruction >> 6) & 0x1f;
      uint8_t *instruction_start;

      printf("Compiling 0x%08x\n", instruction);

      compiler.map = dynarec_maybe_realloc(page, compiler.map);

      instruction_start = compiler.map;

      if (instruction_start == NULL) {
         return -1;
      }

      /* Decrease the event counter before we continue */
      dynarec_counter_maintenance(&compiler);

      switch (instruction >> 26) {
      case 0x00: /* ALU */
         switch (instruction & 0x3f) {
         case 0x00: /* SLL */
            if (reg_d == 0 || (reg_t == reg_d && shift == 0)) {
               /* NOP. This is the prefered encoding (full 0s) */
               break;
            }

            if (reg_t == 0) {
               dynarec_emit_li(&compiler, reg_d, 0);
               break;
            }

            if (shift == 0) {
               dynarec_emit_mov(&compiler, reg_d, reg_t);
               break;
            }

            dynarec_emit_sll(&compiler, reg_d, reg_t, shift);
            break;

         default:
            printf("Dynarec encountered unsupported instruction %08x\n",
                   instruction);
            return 0;
         }
      case 0x09: /* ADDIU */
         if (reg_t == 0 || (reg_t == reg_s && imm == 0)) {
            /* NOP */
            break;
         }

         if (reg_s == 0) {
            dynarec_emit_li(&compiler, reg_t, imm);
            break;
         }

         if (imm == 0) {
            dynarec_emit_mov(&compiler, reg_t, reg_s);
            break;
         }

         dynarec_emit_addiu(&compiler, reg_t, reg_s, imm);
         break;

      case 0x0d: /* ORI */
         if (reg_t == 0 || (reg_t == reg_s && imm == 0)) {
            /* NOP */
            break;
         }

         if (reg_s == 0) {
            dynarec_emit_li(&compiler, reg_t, imm);
            break;
         }

         if (imm == 0) {
            dynarec_emit_mov(&compiler, reg_t, reg_s);
            break;
         }

         dynarec_emit_ori(&compiler, reg_t, reg_s, imm);
         break;
      case 0x0f: /* LUI */
         if (reg_t == 0) {
            /* nop */
            break;
         }

         dynarec_emit_li(&compiler, reg_t, ((uint32_t)imm) << 16);
         break;
      case 0x2b: /* SW */
         dynarec_emit_sw(&compiler, reg_s, imm, reg_t);
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

   return dynarec_execute(state, f, cycles_to_run);
}
