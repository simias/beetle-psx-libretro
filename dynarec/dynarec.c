#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>

#include "dynarec.h"
#include "dynarec-compiler.h"

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

   memcpy(state->region_mask,
          dynarec_region_mask,
          sizeof(dynarec_region_mask));

   if (dynarec_compiler_init(state)) {
      dynarec_delete(state);
      return NULL;
   }

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

static uint32_t dynarec_canonical_address(uint32_t addr) {
   addr = dynarec_mask_address(addr);

   /* RAM is mirrored 4 times */
   if (addr < (PSX_RAM_SIZE * 4)) {
      addr = addr % PSX_RAM_SIZE;
   }

   return addr;
}

struct dynarec_block *dynarec_find_block(struct dynarec_state *state,
                                         uint32_t addr) {
   addr = dynarec_canonical_address(addr);

   return dynarec_block_from_node(rbt_find(&state->blocks, addr));
}

struct dynarec_block *dynarec_find_or_compile_block(struct dynarec_state *state,
                                                    uint32_t addr) {
   struct dynarec_block *block;

   addr = dynarec_canonical_address(addr);

   block = dynarec_block_from_node(rbt_find(&state->blocks, addr));

   if (block == NULL) {
      /* Recompile */
      block = dynarec_recompile(state, addr);
      assert(block != NULL);
      rbt_insert(&state->blocks, &block->tree_node);
      DYNAREC_LOG("Number of blocks: %lu\n", rbt_size(&state->blocks));
   }

   return block;
}

uint32_t dynarec_run(struct dynarec_state *state, int32_t cycles_to_run) {
   struct dynarec_block *block;

   block = dynarec_find_or_compile_block(state, state->pc);

   dynarec_fn_t f = dynarec_block_code(block);

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
