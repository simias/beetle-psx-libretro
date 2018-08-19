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

struct dynarec_state *dynarec_init(uint8_t *ram,
                                   uint8_t *scratchpad,
                                   const uint8_t *bios) {
   struct dynarec_state *state;

   state = calloc(1, sizeof(*state));
   if (state == NULL) {
      return NULL;
   }

   /* Allocate dummy RAM buffer used when cache isolation is active */
   state->dummy_ram = calloc(1, PSX_RAM_SIZE);
   if (state->dummy_ram == NULL) {
      free(state);
      return NULL;
   }

   state->true_ram = ram;
   state->ram = ram;
   state->scratchpad = scratchpad;
   state->bios = bios;
   rbt_init(&state->blocks);

   /* For now let's be greedy and allocate a huge buffer. Untouched
      pages shouldn't take any resident memory so it shouldn't be too
      bad. Later it might make more sense to allocate smaller buffers
      and free or reuse them when they're no longer referenced. */
   state->map_len = 256 * 1024 * 1024;
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
      free(state->dummy_ram);
      free(state);
      return NULL;
   }

   state->free_map = state->map;

   memcpy(state->region_mask, region_mask, sizeof(region_mask));

   return state;
}

void dynarec_delete(struct dynarec_state *state) {
   munmap(state->map, state->map_len);
   free(state->dummy_ram);
   free(state);
}

void dynarec_set_pc(struct dynarec_state *state, uint32_t pc) {
   state->pc = pc;
}

uint32_t dynarec_run(struct dynarec_state *state, int32_t cycles_to_run) {
   uint32_t addr;
   struct dynarec_block *block;

   addr = dynarec_mask_address(state->pc);

   /* RAM is mirrored 4 times */
   if (addr < (PSX_RAM_SIZE * 4)) {
      addr = addr % PSX_RAM_SIZE;
   }

   block = TO_DYNAREC_BLOCK(rbt_find(&state->blocks, addr));

   if (block == NULL) {
      /* Recompile */
      block = dynarec_recompile(state, addr);
      assert(block != NULL);
      rbt_insert(&state->blocks, &block->tree_node);
   }

   dynarec_fn_t f = (void *)&block->code;

   return dynasm_execute(state, f, cycles_to_run);
}

/* Helper functions called by the recompiled code */
void dynarec_set_cache_isolation(struct dynarec_state *state, int enabled) {
   DYNAREC_LOG("set cache isolation %d\n", enabled);

   /* This is not completely accurate, I think when the cache is
      isolated you can't access *anything* (RAM, scratchpad, device
      memory...). That being said as far as I know the only thing this
      is used for is for flushing the cache in which case the code
      will write to very low addresses that would normally end up in
      RAM. For this reason swapping the RAM buffer away might be
      sufficient in the vast majority of the cases. */
   if (enabled) {
      state->ram = state->dummy_ram;
   } else {
      state->ram = state->true_ram;
   }
}
