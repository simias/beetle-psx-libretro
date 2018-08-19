#ifndef __DYNAREC_H__
#define __DYNAREC_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>

#include "rbtree.h"

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

/* A BREAK instruction was encountered while DYNAREC_OPT_EXIT_ON_BREAK
   was set. The low 20 bits contain the break code. */
#define DYNAREC_EXIT_BREAK         0xEU

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

#ifdef DYNAREC_DEBUG
#define DYNAREC_LOG(...)     \
   fprintf(stderr, "[DYNAREC]: " __VA_ARGS__)
#else
#define DYNAREC_LOG(...) do {} while (0)
#endif

#define DYNAREC_FATAL(...) do {                    \
      fprintf(stderr, "[DYNAREC]: " __VA_ARGS__);  \
      abort();                                     \
   } while(0)


enum PSX_REG {
   PSX_REG_R0,
   PSX_REG_AT,
   PSX_REG_V0,
   PSX_REG_V1,
   PSX_REG_A0,
   PSX_REG_A1,
   PSX_REG_A2,
   PSX_REG_A3,
   PSX_REG_T0,
   PSX_REG_T1,
   PSX_REG_T2,
   PSX_REG_T3,
   PSX_REG_T4,
   PSX_REG_T5,
   PSX_REG_T6,
   PSX_REG_T7,
   PSX_REG_S0,
   PSX_REG_S1,
   PSX_REG_S2,
   PSX_REG_S3,
   PSX_REG_S4,
   PSX_REG_S5,
   PSX_REG_S6,
   PSX_REG_S7,
   PSX_REG_T8,
   PSX_REG_T9,
   PSX_REG_K0,
   PSX_REG_K1,
   PSX_REG_GP,
   PSX_REG_SP,
   PSX_REG_FP,
   PSX_REG_RA,
   /* Dynarec temporary: not a real hardware register, used by the
      dynarec when it needs to reorder code for delay slots. */
   PSX_REG_DT,
   /* Registers used by MULT/DIV and related opcodes */
   PSX_REG_HI,
   PSX_REG_LO,
   PSX_REG_TOTAL,
};

/* Coprocessor 0 registers (accessed with mtc0/mfc0) */
enum PSX_COP0_REG {
   PSX_COP0_R0 = 0,       /* N/A */
   PSX_COP0_R1 = 1,       /* N/A */
   PSX_COP0_R2 = 2,       /* N/A */
   PSX_COP0_BPC = 3,      /* Breakpoint on execute (RW) */
   PSX_COP0_R4 = 4,       /* N/A */
   PSX_COP0_BDA = 5,      /* Breakpoint on data access (RW) */
   PSX_COP0_JUMPDEST = 6, /* Jump address (RO) */
   PSX_COP0_DCIC = 7,     /* Breakpoint control (RW) */
   PSX_COP0_BADVADDR = 8, /* Bad virtual address (RO) */
   PSX_COP0_BDAM = 9,     /* Data access breakpoint mask (RW) */
   PSX_COP0_R10 = 10,     /* N/A */
   PSX_COP0_BPCM = 11,    /* Execute breakpoint mask (RW) */
   PSX_COP0_SR = 12,      /* System status (RW) */
   PSX_COP0_CAUSE = 13,   /* Exception cause (RW) */
   PSX_COP0_EPC = 14,     /* Exception PC (R) */
   PSX_COP0_PRID = 15,    /* CPU ID (R) */
};

enum PSX_CPU_EXCEPTION {
   /* Interrupt Request */
   PSX_EXCEPTION_INTERRUPT = 0x0,
   /* Alignment error on load */
   PSX_EXCEPTION_LOAD_ALIGN = 0x4,
   /* Alignment error on store */
   PSX_EXCEPTION_STORE_ALIGN = 0x5,
   /* System call (caused by the SYSCALL opcode) */
   PSX_EXCEPTION_SYSCALL = 0x8,
   /* Breakpoint (caused by the BREAK opcode) */
   PSX_EXCEPTION_BREAK = 0x9,
   /* CPU encountered an unknown instruction */
   PSX_EXCEPTION_ILLEGAL_INSTRUCTION = 0xa,
   /* Unsupported coprocessor operation */
   PSX_COPROCESSOR_ERROR = 0xb,
   /* Arithmetic overflow */
   PSX_OVERFLOW = 0xc,
   /* Fake exception for dynarec use */
   PSX_DYNAREC_UNIMPLEMENTED = 0xdead,
};

/* Structure representing one block of recompiled code */
struct dynarec_block {
   /* Entry in the Red Black tree. The start address of the block in
      PSX memory is the tree key */
   struct rbt_node tree_node;
   /* Recompiled code follows direcly */
   uint8_t code[];
};
#define TO_DYNAREC_BLOCK(_rbtn) \
   CONTAINER_OF((_rbtn), struct dynarec_block, tree_node)

struct dynarec_state {
   /* Current value of the PC */
   uint32_t pc;
   /* Region mask, it's used heavily in the dynarec'd code so it's
      convenient to have it accessible in this struct. */
   uint32_t region_mask[8];
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
   /* Recompilation options (see DYNAREC_OPT_* )*/
   uint32_t options;
   /* Recompiled blocks stored by PSX start address */
   struct rbtree blocks;
};

extern struct dynarec_state *dynarec_init(uint8_t *ram,
                                          uint8_t *scratchpad,
                                          const uint8_t *bios);

extern int32_t dynarec_find_page_index(struct dynarec_state *state,
                                       uint32_t addr);
extern uint8_t *dynarec_page_start(struct dynarec_state *state,
                                   uint32_t page_index);
extern uint8_t *dynarec_instruction_address(struct dynarec_state *state,
                                            uint32_t addr);
extern void dynarec_delete(struct dynarec_state *state);
extern void dynarec_set_next_event(struct dynarec_state *state,
                                   int32_t cycles);
extern void dynarec_set_pc(struct dynarec_state *state,
                           uint32_t pc);
extern uint32_t dynarec_run(struct dynarec_state *state,
                            int32_t cycles_to_run);

#ifdef __cplusplus
}
#endif

#endif /* __DYNAREC_H__ */
