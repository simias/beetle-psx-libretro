#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>

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
int32_t dynarec_find_page_index(struct dynarec_state *state,
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

uint8_t *dynarec_page_start(struct dynarec_state *state,
                            uint32_t page_index) {
   return state->map + (dynarec_max_page_size() * page_index);
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
