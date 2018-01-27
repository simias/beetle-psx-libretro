#ifndef __DYNAREC_H__
#define __DYNAREC_H__

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
#if 0
/* Just to tell emacs to stop indenting everything because of the
   block above */
}
#endif

#include "constants.h"

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

struct dynarec_page {
   /* Executable portion of memory containing the recompiled code or
      NULL if the map is invalid */
   void               *map;
   /* Offsets into `map` to retreive the location of individual
      instructions. We have one additional pseudo-instruction in the
      end which is used either to cross the boundary to the next page
      or to trigger the recompiler if the next page is not yet
      recompiled. */
   uint32_t            instruction_offsets[DYNAREC_PAGE_INSTRUCTIONS + 1];
   /* Length of the mapped portion of memory (when map != NULL). */
   uint32_t            map_len;
};

struct dynarec_state {
   struct dynarec_page pages[DYNAREC_TOTAL_PAGES];
   /* Time until the next asynchronous event in CPU cycles (*not*
      number of instructions). Can become negative if we've passed the
      deadline. */
   int32_t             next_event_cycle;
   /* Current value of the PC */
   uint32_t            pc;
   /* All CPU registers. R0 is always 0. */
   uint32_t            regs[32];
};

extern struct dynarec_state *dynarec_init(void);
extern void dynarec_set_next_event(struct dynarec_state *state,
                                   int32_t cycles);
extern void dynarec_set_pc(struct dynarec_state *state,
                           uint32_t pc);
extern void dynarec_run(struct dynarec_state *state);

#ifdef __cplusplus
}
#endif

#endif /* __DYNAREC_H__ */
