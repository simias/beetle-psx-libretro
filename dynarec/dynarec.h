#ifndef __DYNAREC_H__
#define __DYNAREC_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>

#include "rbtree.h"

#include "psx-instruction.h"

#ifdef __cplusplus
extern "C" {
#endif
#if 0
/* Just to tell emacs to stop indenting everything because of the
   block above */
}
#endif

/* Abort execution when a BREAK is encountered, useful for
   debugging/testing. The break code is returned. */
#define DYNAREC_OPT_EXIT_ON_BREAK  0x1U

/* Recompilation exit codes
 *
 * Several conditions can lead to the recompiling code to return
 * control to the caller, bits [31:28] of the return value contains
 * the EXIT code. The meaning of the remaining 28 bits is
 * code-dependent.
 */
enum dynarec_exit {
   /* Counter exhausted */
   DYNAREC_EXIT_COUNTER = 0,
   /* Cache flush necessary */
   DYNAREC_CACHE_FLUSH = 1,
   /* Unimplemented feature encountered. */
   DYNAREC_EXIT_UNIMPLEMENTED = 0xc,
   /* A SYSCALL instruction was encountered. The low 20 bits contain the
      break code. */
   DYNAREC_EXIT_SYSCALL = 0xd,
   /* A BREAK instruction was encountered. The low 20 bits contain the
      break code. */
   DYNAREC_EXIT_BREAK = 0xe,
};

/* PSX RAM size in bytes: 2MB */
#define PSX_RAM_SIZE               0x200000U
/* BIOS ROM size in bytes: 512kB */
#define PSX_BIOS_SIZE              0x80000U
/* Base address for the BIOS ROM */
#define PSX_BIOS_BASE              0x1FC00000U
/* Scratchpad size in bytes: 1kB */
#define PSX_SCRATCHPAD_SIZE        1024U
/* Base address for the scratchpad */
#define PSX_SCRATCHPAD_BASE        0x1F800000U

/* For now I assume every instruction takes exactly 4 cycles to
   execute. It's rather optimistic (the average in practice is
   closer to 5 cycles) but obviously in practice it varies a lot
   depending on the instruction, the icache, memory latency
   etc... */
#define PSX_CYCLES_PER_INSTRUCTION 4

/* Maximum number of instructions in a recompiled block.
 *
 * If a stretch of instruction goes uninterrupted by an unconditional
 * branch for longer than this it'll automatically be split into
 * multiple blocks. */
#define DYNAREC_MAX_BLOCK_INSTRUCTIONS 128U
#define DYNAREC_MAX_BLOCK_SIZE         (DYNAREC_MAX_BLOCK_INSTRUCTIONS * 4)

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

#ifndef CONTAINER_OF
# define CONTAINER_OF(ptr, type, member)                 \
   ((type *)((char *)ptr - offsetof(type, member)))
#endif

#ifdef DYNAREC_LOG_DEBUG
#define DYNAREC_LOG(...)     \
   fprintf(stderr, "[DYNAREC]: " __VA_ARGS__)
#else
#define DYNAREC_LOG(...) do {} while (0)
#endif

#define DYNAREC_FATAL(...) do {                    \
      fprintf(stderr, "[DYNAREC]: " __VA_ARGS__);  \
      abort();                                     \
   } while(0)

static size_t dynarec_align(size_t n, size_t align) {
   size_t mask = align - 1;

   /* Aligns to powers of two only */
   assert((align & mask) == 0);

   return (n + mask) & ~mask;
}

static const uint32_t dynarec_region_mask[8] = {
   0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, /* KUSEG: 2048MB */
   0x7fffffff,                                     /* KSEG0:  512MB */
   0x1fffffff,                                     /* KSEG1:  512MB */
   0xffffffff, 0xffffffff,                         /* KSEG2: 1024MB */
};

/* Mask "addr" to remove the region bits and return a "canonical"
   address. */
static uint32_t dynarec_mask_address(uint32_t addr) {
   return addr & dynarec_region_mask[addr >> 29];
}

static uint32_t dynarec_canonical_address(uint32_t addr) {
   addr = dynarec_mask_address(addr);

   /* RAM is mirrored 4 times */
   if (addr < (PSX_RAM_SIZE * 4)) {
      addr = addr % PSX_RAM_SIZE;
   }

   return addr;
}

/* Expected length of a cacheline in bytes. Must be a power of two
   otherwise alignment calculations */
#define CACHE_LINE_SIZE  64U

/* Structure representing one block of recompiled code. Recompiled
   code follows directly after this structure in memory. */
struct dynarec_block {
   /* Entry in the Red Black tree. The start address of the block in
      PSX memory is the tree key */
   struct rbt_node tree_node;
   /* Address of the first instruction of the block */
   uint32_t base_address;
   /* Length of the block in bytes */
   unsigned block_len_bytes;
   /* Number of PSX instruction recompiled in this block */
   unsigned psx_instructions;
} __attribute__((aligned(CACHE_LINE_SIZE)));

static struct dynarec_block *dynarec_block_from_node(struct rbt_node *n) {
   return CONTAINER_OF(n, struct dynarec_block, tree_node);
}

static void *dynarec_block_code(struct dynarec_block *b) {
   /* Code follows the block directly */
   assert(b != NULL);
   return (void *)(b + 1);
}

static int dynarec_block_compare(const struct rbt_node *n,
                                 const struct rbt_node *o) {
   const struct dynarec_block *bn =
      CONTAINER_OF(n, struct dynarec_block, tree_node);
   const struct dynarec_block *bo =
      CONTAINER_OF(o, struct dynarec_block, tree_node);

   if (bn->base_address == bo->base_address) {
      return 0;
   } else if (bn->base_address > bo->base_address) {
      return 1;
   } else {
      return -1;
   }
}

static int dynarec_block_compare_key(const struct rbt_node *n,
                                     const void *k) {
   const struct dynarec_block *bn =
      CONTAINER_OF(n, struct dynarec_block, tree_node);
   uint32_t addr = (uintptr_t)k;

   /* Unfortunately we don't support 16bit systems */
   assert(sizeof(uintptr_t) >= sizeof(uint32_t));

   if (bn->base_address == addr) {
      return 0;
   } else if (bn->base_address > addr) {
      return 1;
   } else {
      return -1;
   }
}

struct dynarec_state {
   /* Region mask, it's used heavily in the dynarec'd code so it's
      convenient to have it accessible in this struct. */
   uint32_t region_mask[8];
   /* Current value of the PC */
   uint32_t pc;
   /* Pointer to the PSX RAM */
   uint8_t *ram;
   /* Pointer to the PSX scratchpad */
   uint8_t *scratchpad;
   /* Pointer to the PSX BIOS */
   const uint8_t *bios;
   /* All general purpose CPU registers except R0 */
   uint32_t regs[PSX_REG_TOTAL - 1];
   /* Cop0r11: cause register */
   uint32_t cause;
   /* Cop0r12: status register */
   uint32_t sr;
   /* Cop0r14: Exception PC */
   uint32_t epc;
   /* Executable region of memory containing the dynarec'd code */
   uint8_t *map;
   /* Length of the map */
   uint32_t map_len;
   /* Pointer to unused portion of `map` */
   uint8_t *free_map;
   /* Pointer to the real RAM buffer */
   uint8_t *true_ram;
   /* Pointer to the dummy RAM buffer used when cache isolation is
      active */
   uint8_t *dummy_ram;
   /* Pointer towards the link trampoline which is a small code thunk
      used as a placeholder when the compiler can't statically find
      the target of a jump (because it hasn't been recompiled yet or
      because it's an indirect jump) */
   void *link_trampoline;
   /* Recompilation options (see DYNAREC_OPT_* )*/
   uint32_t options;
   /* Recompiled blocks stored by PSX start address */
   struct rbtree blocks;
};

static struct dynarec_block *dynarec_find_block(struct dynarec_state *state,
                                                uint32_t addr) {
   return dynarec_block_from_node(rbt_find(&state->blocks,
                                           dynarec_block_compare_key,
                                           (void *)(uintptr_t)addr));
}

struct dynarec_state *dynarec_init(uint8_t *ram,
                                   uint8_t *scratchpad,
                                   const uint8_t *bios);
void dynarec_flush_cache(struct dynarec_state *state);
struct dynarec_block *dynarec_find_block(struct dynarec_state *state,
                                         uint32_t addr);
struct dynarec_block *dynarec_find_or_compile_block(struct dynarec_state *state,
                                                    uint32_t addr);
void dynarec_delete(struct dynarec_state *state);
void dynarec_set_next_event(struct dynarec_state *state,
                            int32_t cycles);
void dynarec_set_pc(struct dynarec_state *state,
                    uint32_t pc);

struct dynarec_ret {
   struct {
      unsigned param : 28;
      unsigned code : 4;
   } val;
   int32_t counter;
};

struct dynarec_ret dynarec_run(struct dynarec_state *state,
                               int32_t cycles_to_run);

/******************************************************
 * Callbacks that must be implemented by the emulator *
 ******************************************************/

struct dynarec_load_val {
   int32_t counter;
   uint32_t value;
};

/* Callback used by the dynarec to handle GTE MFC2 */
extern int32_t dynarec_gte_mfc2(struct dynarec_state *s,
                           uint32_t reg_target,
                           uint32_t reg_gte,
                           uint32_t instr);

/* Callback used by the dynarec to handle GTE CFC2 */
extern int32_t dynarec_gte_cfc2(struct dynarec_state *s,
                           uint32_t reg_target,
                           uint32_t reg_gte,
                           uint32_t instr);

/* Callback used by the dynarec to handle GTE MTC2 */
extern void dynarec_gte_mtc2(struct dynarec_state *s,
                           uint32_t source,
                           uint32_t reg_gte,
                           uint32_t instr);

/* Callback used by the dynarec to handle GTE MFC2 */
extern void dynarec_gte_ctc2(struct dynarec_state *s,
                           uint32_t source,
                           uint32_t reg_gte,
                           uint32_t instr);

/* Callback used by the dynarec to handle GTE LWC2 */
extern int32_t dynarec_gte_lwc2(struct dynarec_state *s,
                           uint32_t addr,
                           uint32_t instr,
                           int32_t counter);

/* Callback used by the dynarec to handle GTE SWC2 */
extern int32_t dynarec_gte_swc2(struct dynarec_state *s,
                           uint32_t addr,
                           uint32_t instr,
                           int32_t counter);

/* Callback used by the dynarec to handle GTE Instruction */
extern int32_t dynarec_gte_instruction(struct dynarec_state *s,
                           uint32_t instr,
                           int32_t counter);

/* Callback used by the dynarec to handle writes to "miscelanous" COP0
   registers (i.e. not SR nor CAUSE) */
extern void dynarec_set_cop0_misc(struct dynarec_state *s,
                           uint32_t val,
                           uint32_t cop0_reg);

/* Callbacks used by the dynarec to handle device memory writes. Must
   return the new value of the counter. */
extern int32_t dynarec_callback_sw(struct dynarec_state *s,
                                   uint32_t val,
                                   uint32_t addr,
                                   int32_t counter);
extern int32_t dynarec_callback_sh(struct dynarec_state *s,
                                   uint32_t val,
                                   uint32_t addr,
                                   int32_t counter);
extern int32_t dynarec_callback_sb(struct dynarec_state *s,
                                   uint32_t val,
                                   uint32_t addr,
                                   int32_t counter);
extern struct dynarec_load_val dynarec_callback_lb(struct dynarec_state *s,
                                                   uint32_t addr,
                                                   int32_t counter);
extern struct dynarec_load_val dynarec_callback_lh(struct dynarec_state *s,
                                                   uint32_t addr,
                                                   int32_t counter);
extern struct dynarec_load_val dynarec_callback_lw(struct dynarec_state *s,
                                                   uint32_t addr,
                                                   int32_t counter);

#ifdef __cplusplus
}
#endif

#endif /* __DYNAREC_H__ */
