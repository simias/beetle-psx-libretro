#ifndef __DYNAREC_H__
#define __DYNAREC_H__

#include <stdint.h>
#include <stdbool.h>
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

/* Log in base 2 of the page size in bytes */
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

/* Total number of potential instructions in the system */
#define DYNAREC_TOTAL_INSTRUCTIONS (DYNAREC_TOTAL_PAGES *       \
                                    DYNAREC_PAGE_INSTRUCTIONS)

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(_a) (sizeof(_a) / sizeof((_a)[0]))
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

struct dynarec_state;

struct dynarec_state {
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
   /* All general purpose CPU registers except R0 plus the "dynarec
      temporary" register. */
   uint32_t            regs[32];
   /* Cop0r12: status register */
   uint32_t            sr;
   /* Executable region of memory containing the dynarec'd code */
   uint8_t            *map;
   /* Length of the map */
   uint32_t            map_len;
   /* Keeps track of whether each page is valid or needs to be
      recompiled */
   uint8_t             page_valid[DYNAREC_TOTAL_PAGES];
   /* Look up table for any (valid) recompiled instruction */
   void               *dynarec_instructions[DYNAREC_TOTAL_INSTRUCTIONS];
};

extern struct dynarec_state *dynarec_init(uint32_t *ram,
                                          uint32_t *scratchpad,
                                          const uint32_t *bios);

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
extern int32_t dynarec_run(struct dynarec_state *state,
                           int32_t cycles_to_run);

#ifdef __cplusplus
}
#endif

#endif /* __DYNAREC_H__ */
