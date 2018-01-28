#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>

#include "dynarec.h"

struct dynarec_state *dynarec_init(uint32_t *ram, const uint32_t *bios) {
   struct dynarec_state *state;
   unsigned i;

   state = malloc(sizeof(*state));
   if (state == NULL) {
      return NULL;
   }

   state->next_event_cycle = 0;
   state->ram = ram;
   state->bios = bios;

   memset(state->regs, 0, sizeof(state->regs));

   /* Initialize all pages as unmapped/invalid */
   for (i = 0; i < ARRAY_SIZE(state->pages); i++) {
      state->pages[i].valid = 0;
      state->pages[i].map = NULL;
   }

   return state;
}

void dynarec_set_next_event(struct dynarec_state *state, int32_t cycles) {
   state->next_event_cycle = cycles;
}

void dynarec_set_pc(struct dynarec_state *state, uint32_t pc) {
   state->pc = pc;
}

/* Mask "addr" to remove the region bits and return a "canonical"
   address. */
static uint32_t dynarec_mask_address(uint32_t addr) {
   static const uint32_t region_mask[8] = {
      0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, /* KUSEG: 2048MB */
      0x7fffffff,                                     /* KSEG0:  512MB */
      0x1fffffff,                                     /* KSEG1:  512MB */
      0xffffffff, 0xffffffff,                         /* KSEG2: 1024MB */
   };

   return addr & region_mask[addr >> 29];
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

static int dynarec_map_page(struct dynarec_page *page) {
   if (page->map == NULL) {
      /* Map a new page for execution */
      page->map = mmap(NULL,
                       DYNAREC_MAP_LEN,
                       PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);

      if (page->map == MAP_FAILED) {
         perror("Dynarec mmap failed");
         page->map = NULL;
         return -1;
      }
   }

   return 0;
}

static int dynarec_recompile(struct dynarec_state *state,
                             uint32_t page_index) {
   struct dynarec_page     *page;
   const uint32_t          *emulated_page;
   struct dynarec_compiler  compiler;
   uint8_t                 *map;
   unsigned                 i;

   compiler.state = state;

   page = &state->pages[page_index];
   page->valid = 0;

   if (dynarec_map_page(page) < 0) {
      return -1;
   }

   if (page_index < DYNAREC_RAM_PAGES) {
      emulated_page = state->ram + DYNAREC_PAGE_INSTRUCTIONS * page_index;
      compiler.pc = DYNAREC_PAGE_SIZE * page_index;
   } else {
      uint32_t bios_index = page_index - DYNAREC_RAM_PAGES;
      emulated_page = state->bios + DYNAREC_PAGE_INSTRUCTIONS * bios_index;
      compiler.pc = PSX_BIOS_BASE + DYNAREC_PAGE_SIZE * bios_index;
   }

   compiler.map = page->map;

   /* Helper macros for instruction decoding */
   for (i = 0; i < DYNAREC_PAGE_INSTRUCTIONS; i++) {
      uint32_t instruction = emulated_page[i];

      /* Various decodings of the fields, of course they won't all be
         valid for a given instruction. For now I'll trust the
         compiler to optimize this correctly, otherwise it might be
         better to move them to the instructions where they make
         sense.*/
      uint8_t  reg_t = (instruction >> 16) & 0x1f;
      uint8_t  reg_s = (instruction >> 21) & 0x1f;
      uint16_t imm = instruction & 0xffff;

      printf("Compiling 0x%08x\n", instruction);

      map = compiler.map;

      switch (instruction >> 26) {
      case 0x0d:
         if (reg_t == 0) {
            /* nop */
            break;
         }

         dynarec_emit_ori(&compiler, reg_t, reg_s, imm);

         break;
      case 0x0f:
         if (reg_t == 0) {
            /* nop */
            break;
         }

         dynarec_emit_lui(&compiler, reg_t, imm);
         break;
      default:
         printf("Dynarec encountered unsupported instruction %08x\n",
                instruction);
         abort();
      }

      printf("Emited:");
      for (; map != compiler.map; map++) {
         printf(" %02x", *map);
      }
      printf("\n");
   }

   return 0;
}

void dynarec_run(struct dynarec_state *state) {
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
}
