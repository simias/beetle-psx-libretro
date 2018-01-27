#ifndef __CONSTANTS_H__
#define __CONSTANTS_H__

/* PSX RAM size in bytes: 2MB */
#define PSX_RAM_SIZE               0x200000U
/* BIOS ROM size in bytes: 512KB */
#define PSX_BIOS_SIZE              0x80000U

/* Length of a recompilation page in bytes */
#define DYNAREC_PAGE_SIZE          2048U
/* Number of instructions per page */
#define DYNAREC_PAGE_INSTRUCTIONS  (DYNAREC_PAGE_SIZE / 4U)

/* Total number of dynarec pages in RAM */
#define DYNAREC_RAM_PAGES          (PSX_RAM_SIZE / DYNAREC_PAGE_SIZE)
/* Total number of dynarec pages in BIOS ROM */
#define DYNAREC_BIOS_PAGES         (PSX_BIOS_SIZE / DYNAREC_PAGE_SIZE)

/* Total number of dynarec pages for the system */
#define DYNAREC_TOTAL_PAGES        (DYNAREC_RAM_PAGES + DYNAREC_BIOS_PAGES)

#endif /* __CONSTANTS_H__ */
