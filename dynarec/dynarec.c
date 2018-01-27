#include <string.h>
#include <stdio.h>

#include "dynarec.h"

static int dynarec_page_is_valid(const struct dynarec_page *p) {
   return p->map != NULL;
}

struct dynarec_state *dynarec_init(void) {
   struct dynarec_state *state;
   unsigned i;

   state = malloc(sizeof(*state));
   if (state == NULL) {
      return NULL;
   }

   state->next_event_cycle = 0;
   memset(state->regs, 0, sizeof(state->regs));

   /* Initialize all pages as unmapped/invalid */
   for (i = 0; i < ARRAY_SIZE(state->pages); i++) {
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

void dynarec_run(struct dynarec_state *state) {
   printf("magic!\n");
   for (;;) {}
}
