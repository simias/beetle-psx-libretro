#ifndef __DYNAREC_H__
#define __DYNAREC_H__

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
#if 0
/* Just to tell emacs to stop indenting everything because of the
   block above */
}
#endif

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

/* Log in base 2 of the page size*/
#define DYNAREC_PAGE_SIZE_SHIFT    11U
/* Length of a recompilation page in bytes */
#define DYNAREC_PAGE_SIZE          (1U << DYNAREC_PAGE_SIZE_SHIFT)
/* Number of instructions per page */
#define DYNAREC_PAGE_INSTRUCTIONS  (DYNAREC_PAGE_SIZE / 4U)

/* Total number of dynarec pages in RAM */
#define DYNAREC_RAM_PAGES          (PSX_RAM_SIZE / DYNAREC_PAGE_SIZE)
/* Total number of dynarec pages in BIOS ROM */
#define DYNAREC_BIOS_PAGES         (PSX_BIOS_SIZE / DYNAREC_PAGE_SIZE)

/* Total number of dynarec pages for the system */
#define DYNAREC_TOTAL_PAGES        (DYNAREC_RAM_PAGES + DYNAREC_BIOS_PAGES)

#ifdef DYNAREC_ARCH_AMD64
# include "dynarec-amd64.h"
#endif

/* Length of the portion of memory mmap'ed to recompile a single
   page. */
#define DYNAREC_MAP_LEN            (DYNAREC_INSTRUCTION_MAX_LEN * \
                                    DYNAREC_PAGE_INSTRUCTIONS)

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

enum PSX_REG {
   PSX_REG_R0 = 0,
   PSX_REG_AT = 1,
   PSX_REG_V0 = 2,
   PSX_REG_V1 = 3,
   PSX_REG_A0 = 4,
   PSX_REG_A1 = 5,
   PSX_REG_A2 = 6,
   PSX_REG_A3 = 7,
   PSX_REG_T0 = 8,
   PSX_REG_T1 = 9,
   PSX_REG_T2 = 10,
   PSX_REG_T3 = 11,
   PSX_REG_T4 = 12,
   PSX_REG_T5 = 13,
   PSX_REG_T6 = 14,
   PSX_REG_T7 = 15,
   PSX_REG_S0 = 16,
   PSX_REG_S1 = 17,
   PSX_REG_S2 = 18,
   PSX_REG_S3 = 19,
   PSX_REG_S4 = 20,
   PSX_REG_S5 = 21,
   PSX_REG_S6 = 22,
   PSX_REG_S7 = 23,
   PSX_REG_T8 = 24,
   PSX_REG_T9 = 25,
   PSX_REG_K0 = 26,
   PSX_REG_K1 = 27,
   PSX_REG_GP = 28,
   PSX_REG_SP = 29,
   PSX_REG_FP = 30,
   PSX_REG_RA = 31,
};

enum PSX_CPU_EXCEPTIONS {
    /// Interrupt Request
    PSX_EXCEPTION_INTERRUPT = 0x0,
    /// Alignment error on load
    PSX_EXCEPTION_LOAD_ALIGN = 0x4,
    /// Alignment error on store
    PSX_EXCEPTION_STORE_ALIGN = 0x5,
    /// System call (caused by the SYSCALL opcode)
    PSX_EXCEPTION_SYSCALL = 0x8,
    /// Breakpoint (caused by the BREAK opcode)
    PSX_EXCEPTION_BREAK = 0x9,
    /// CPU encountered an unknown instruction
    PSX_EXCEPTION_ILLEGAL_INSTRUCTION = 0xa,
    /// Unsupported coprocessor operation
    PSX_COPROCESSOR_ERROR = 0xb,
    /// Arithmetic overflow
    PSX_OVERFLOW = 0xc,
};

struct dynarec_page {
   /* If true the page contains up-to-date recompiled code. Otherwise
      the page needs to be recompiled prior to execution. */
   uint32_t           valid;
   /* Executable portion of memory containing the recompiled code. */
   uint8_t           *map;
   /* Offsets into `map` to retreive the location of individual
      instructions. We have one additional pseudo-instruction in the
      end which is used either to cross the boundary to the next page
      or to trigger the recompiler if the next page is not yet
      recompiled. */
   uint32_t           instruction_offsets[DYNAREC_PAGE_INSTRUCTIONS + 1];
};

struct dynarec_state {
   /* Time until the next asynchronous event in CPU cycles (*not*
      number of instructions). Can become negative if we've passed the
      deadline. */
   int32_t             next_event_cycle;
   /* Current value of the PC */
   uint32_t            pc;
   /* Region mask, it's used heavily in the dynarec'd code so it's
      convenient to have it accessible in this struct. */
   uint32_t            region_mask[8];
   /* Pointer to the PSX RAM */
   uint32_t           *ram;
   /* Pointer to the PSX scratchpad */
   uint32_t           *scratchpad;
   /* Pointer to the PSX BIOS */
   const uint32_t     *bios;
   /* All general purpose CPU registers except R0 */
   uint32_t            regs[31];
   struct dynarec_page pages[DYNAREC_TOTAL_PAGES];
};

/* Get the offset of the location of a register within a struct
   dynarec_state. */
#define DYNAREC_STATE_REG_OFFSET(_r)                                    \
   (assert(_r != 0),                                                    \
    offsetof(struct dynarec_state, regs) + (_r - 1) * sizeof(uint32_t))

extern struct dynarec_state *dynarec_init(uint32_t *ram,
                                          uint32_t *scratchpad,
                                          const uint32_t *bios);
extern void dynarec_set_next_event(struct dynarec_state *state,
                                   int32_t cycles);
extern void dynarec_set_pc(struct dynarec_state *state,
                           uint32_t pc);
extern void dynarec_run(struct dynarec_state *state);

/* Structure holding the temporary variables during the recompilation
   sequence */
struct dynarec_compiler {
   struct dynarec_state *state;
   /* Pointer to the location where the next recompiled instruction
      will be written. */
   uint8_t *map;
   /* Current value of the PC */
   uint32_t pc;
};

typedef void (*dynarec_fn_t)(void);

/* These methods are provided by the various architecture-dependent
   backends */
extern void dynarec_emit_li(struct dynarec_compiler *compiler,
                            enum PSX_REG reg,
                            uint32_t val);
extern void dynarec_emit_mov(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s);
extern void dynarec_emit_ori(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s,
                             uint16_t val);
extern void dynarec_emit_sw(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_addr,
                            int16_t offset,
                            enum PSX_REG reg_val);

extern void dynarec_execute(struct dynarec_state *state,
                            dynarec_fn_t target);

#ifdef __cplusplus
}
#endif

#endif /* __DYNAREC_H__ */
