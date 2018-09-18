#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>

#include "dynarec.h"
#include "dynarec-compiler.h"
#include "dynarec-jit-debugger.h"

struct dynarec_state *dynarec_init(uint8_t *ram,
                                   uint8_t *scratchpad,
                                   const uint8_t *bios) {
   struct dynarec_state *state;

   dyndebug_deregister_all();

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
                     /* Needs to be readable since we keep block
                        metadata (tree pointers etc...) in a block
                        header */
                     PROT_READ | PROT_WRITE | PROT_EXEC,
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
   dyndebug_deregister_all();

   munmap(state->map, state->map_len);
   free(state->dummy_ram);
   free(state);
}

/* Flush all recompiled code, restarting from scratch */
void dynarec_flush_cache(struct dynarec_state *state) {
   DYNAREC_LOG("Cache flush\n");

   state->free_map = state->map;
   rbt_init(&state->blocks);
   dynarec_compiler_init(state);
}

void dynarec_set_pc(struct dynarec_state *state, uint32_t pc) {
   state->pc = pc;
}

struct dynarec_block *dynarec_find_or_compile_block(struct dynarec_state *state,
                                                    uint32_t addr) {
   struct dynarec_block *block;

   block = dynarec_find_block(state, addr);

   if (block == NULL) {
      /* Recompile */
      block = dynarec_recompile(state, addr);
      assert(block != NULL);
      rbt_insert(&state->blocks, &block->tree_node, dynarec_block_compare);
      DYNAREC_LOG("Number of blocks: %lu\n", rbt_size(&state->blocks));
   }

   return block;
}

static void dynarec_exception(struct dynarec_state *state,
                              enum psx_cpu_exception e) {

   uint32_t sr = state->sr;
   uint32_t mode;

   /* Shift bits [5:0] of `SR` two places to the left. Those bits
      are three pairs of Interrupt Enable/User Mode bits behaving
      like a stack 3 entries deep. */
   mode = (sr & 0x3f);
   sr &= ~(0x3fU);
   sr |= (mode << 2) & 0x3f;
   state->sr = sr;

   /* Update `CAUSE` register with the exception code (bits [6:2]) */
   state->cause &= !0x7c;
   state->cause |= ((uint32_t)e) << 2;

   /* Store execution PC, used in RFE */
   state->epc = state->pc;

   /* Address of exception handler depends on the value of bit 22 of SR */
   if (sr & (1U << 22)) {
      state->pc = 0xbfc00180;
   } else {
      state->pc = 0x80000080;
   }

   DYNAREC_LOG("Exception! code: %d PC: 0x%08x CAUSE: 0x%08x SR: 0x%08x\n",
               e, state->pc, state->cause, state->sr);
}

static void dynarec_check_for_interrupt(struct dynarec_state *state) {
   /* Check if an interruption is pending */
   uint32_t sr = state->sr;

   if ((sr & 1) == 0) {
      /* Bit 0 of SR is the global IRQ enable, if it's zero there
         can't be an active interrupt */
      return;
   }

   abort();

   /* Check if one of the enabled IRQs in SR is active in CAUSE */
   if (sr & state->cause & 0xff00) {
      /* An interrupt is active! */
      dynarec_exception(state, PSX_EXCEPTION_INTERRUPT);
   }
}

struct dynarec_ret dynarec_run(struct dynarec_state *state, int32_t cycles_to_run) {
   struct dynarec_ret ret;

   ret.val.code = DYNAREC_EXIT_COUNTER;
   ret.val.param = 0;
   ret.counter = cycles_to_run;

   while (ret.counter > 0) {
      struct dynarec_block *block;

      dynarec_check_for_interrupt(state);

      DYNAREC_LOG("dynarec_run(0x%08x, %d, %08x, %08x)\n", state->pc, ret.counter, state->sr, state->cause);

      block = dynarec_find_or_compile_block(state, state->pc);

      dynarec_fn_t f = dynarec_block_code(block);

      ret = dynasm_execute(state, f, ret.counter);

      switch (ret.val.code) {
      case DYNAREC_EXIT_UNIMPLEMENTED:
         printf("Dynarec encountered unimplemented construct on line %u\n",
                ret.val.param);
         abort();
         break;
      case DYNAREC_CACHE_FLUSH:
         /* Our recompiled code cache might be outdated, flush
            everything */
         dynarec_flush_cache(state);
         /* Now we can continue the execution with a clean cache */
         ret.val.code = DYNAREC_EXIT_COUNTER;
         ret.val.param = 0;
         break;
      case DYNAREC_EXIT_COUNTER:
         /* Ran for at least `cycles_to_run` */
         return ret;
      case DYNAREC_EXIT_SYSCALL:
         dynarec_exception(state, PSX_EXCEPTION_SYSCALL);
         break;
      case DYNAREC_EXIT_BREAK:
         /* Encountered BREAK instructions */
         if (state->options & DYNAREC_OPT_EXIT_ON_BREAK) {
            return ret;
         } else {
            dynarec_exception(state, PSX_EXCEPTION_BREAK);
         }
         break;
      default:
         printf("Unsupported return value %u %u\n",
                ret.val.code,
                ret.val.param);
         abort();
      }
   }

   return ret;
}

/* Helper functions called by the recompiled code. Returns 1 if cache
   should be flushed. */
void dynarec_set_cache_isolation(struct dynarec_state *state,
                                 int enabled) {
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
