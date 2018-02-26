#ifndef __DYNAREC_COMPILER_H__
#define __DYNAREC_COMPILER_H__

#include "dynarec.h"

#ifdef DYNAREC_ARCH_AMD64
# include "dynarec-amd64.h"
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
   /* Dynarec temporary: not a real hardware register, used by the
      dynarec when it needs to reorder code for delay slots. */
   PSX_REG_DT = 32,
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
   PSX_COP0_BCPM = 11,    /* Execute breakpoint mask (RW) */
   PSX_COP0_SR = 12,      /* System status (RW) */
   PSX_COP0_CAUSE = 13,   /* Exception cause (RW) */
   PSX_COP0_EPC = 14,     /* Exception PC (R) */
   PSX_COP0_PRID = 15,    /* CPU ID (R) */
};

enum PSX_CPU_EXCEPTION {
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
    /// Fake exception for dynarec use
    PSX_DYNAREC_UNIMPLEMENTED = 0xdead,
};


/* Get the offset of the location of a register within a struct
   dynarec_state. */
#define DYNAREC_STATE_REG_OFFSET(_r)                                    \
   (assert((_r) != 0),                                                  \
    offsetof(struct dynarec_state, regs) + ((_r) - 1) * sizeof(uint32_t))

struct dynarec_page_local_patch {
   /* Location of the instruction that needs patching */
   uint8_t *patch_loc;
   /* Address of the target PSX instruction */
   uint32_t target;
};

/* Structure holding the temporary variables during the recompilation
   sequence */
struct dynarec_compiler {
   struct dynarec_state *state;
   /* Pointer to the location where the next recompiled instruction
      will be written. */
   uint8_t *map;
   /* Current value of the PC */
   uint32_t pc;
   /* Index of the page currently being recompiled */
   uint32_t page_index;
   /* Number of entries in local_patch */
   uint32_t local_patch_len;
   /* Contains offset of instructions that need patching */
   struct dynarec_page_local_patch local_patch[DYNAREC_PAGE_INSTRUCTIONS];
};

typedef void (*dynarec_fn_t)(void);

/* These methods are provided by the various architecture-dependent
   backends */
extern void dynasm_counter_maintenance(struct dynarec_compiler *compiler,
                                       unsigned cycles);
extern int32_t dynasm_execute(struct dynarec_state *state,
                              dynarec_fn_t target,
                              int32_t counter);
extern void dynasm_emit_exception(struct dynarec_compiler *compiler,
                                  enum PSX_CPU_EXCEPTION exception);
extern void dynasm_emit_li(struct dynarec_compiler *compiler,
                           enum PSX_REG reg,
                           uint32_t val);
extern void dynasm_emit_mov(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_source);
extern void dynasm_emit_sll(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op,
                            uint8_t shift);
extern void dynasm_emit_sra(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_target,
                            enum PSX_REG reg_op,
                            uint8_t shift);
extern void dynasm_emit_addi(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s,
                             uint32_t val);
extern void dynasm_emit_addiu(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_t,
                              enum PSX_REG reg_s,
                              uint32_t val);
extern void dynasm_emit_or(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           enum PSX_REG reg_op0,
                           enum PSX_REG reg_op1);
extern void dynasm_emit_ori(struct dynarec_compiler *compiler,
                            enum PSX_REG reg_t,
                            enum PSX_REG reg_s,
                            uint32_t val);
extern void dynasm_emit_andi(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_t,
                             enum PSX_REG reg_s,
                             uint32_t val);
extern void dynasm_emit_sltiu(struct dynarec_compiler *compiler,
                              enum PSX_REG reg_t,
                              enum PSX_REG reg_s,
                              uint32_t val);
extern void dynasm_emit_sw(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_addr,
                           int16_t offset,
                           enum PSX_REG reg_val);
extern void dynasm_emit_lw(struct dynarec_compiler *compiler,
                           enum PSX_REG reg_target,
                           int16_t offset,
                           enum PSX_REG reg_addr);
extern void dynasm_emit_page_local_jump(struct dynarec_compiler *compiler,
                                        int32_t offset,
                                        bool placeholder);
extern void dynasm_emit_mfhi(struct dynarec_compiler *compiler,
                             enum PSX_REG ret_target);
extern void dynasm_emit_mtlo(struct dynarec_compiler *compiler,
                             enum PSX_REG ret_source);

extern void dynasm_emit_mtc0(struct dynarec_compiler *compiler,
                             enum PSX_REG reg_source,
                             enum PSX_COP0_REG reg_cop0);

#endif /* __DYNAREC_COMPILER_H__ */
