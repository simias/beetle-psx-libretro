/* Interface with third party debuggers */

#ifndef __DYNAREC_JIT_DEBUGGER_H__
#define __DYNAREC_JIT_DEBUGGER_H__

#include <stdlib.h>

#ifdef DYNAREC_JIT_DEBUGGER
/* Register a symbol with the debugger */
void dyndebug_add_block(void *start, size_t len, uint32_t block_base);
/* Deregistered all block symbols */
void dyndebug_deregister_all(void);
#else
static inline void dyndebug_add_block(void *start,
                                      size_t len,
                                      uint32_t block_base) {
}

static inline void dyndebug_deregister_all(void) {
}
#endif

#endif /* __DYNAREC_JIT_DEBUGGER_H__ */
