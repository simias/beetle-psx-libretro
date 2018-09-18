#ifndef __PSX_INSTRUCTION_H__
#define __PSX_INSTRUCTION_H__

#include <stdint.h>

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

   /* Must be last */
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

enum psx_cpu_exception {
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
};

#define MIPS_OP_FN             0x00U
# define MIPS_FN_SLL            0x00U
# define MIPS_FN_SRL            0x02U
# define MIPS_FN_SRA            0x03U
# define MIPS_FN_JR             0x08U
# define MIPS_FN_JALR           0x09U
# define MIPS_FN_SYSCALL        0x0CU
# define MIPS_FN_BREAK          0x0DU
# define MIPS_FN_MFHI           0x10U
# define MIPS_FN_MTHI           0x11U
# define MIPS_FN_MFLO           0x12U
# define MIPS_FN_MTLO           0x13U
# define MIPS_FN_MULTU          0x19U
# define MIPS_FN_DIV            0x1AU
# define MIPS_FN_DIVU           0x1BU
# define MIPS_FN_ADD            0x20U
# define MIPS_FN_ADDU           0x21U
# define MIPS_FN_SUBU           0x23U
# define MIPS_FN_AND            0x24U
# define MIPS_FN_OR             0x25U
# define MIPS_FN_SLT            0x2AU
# define MIPS_FN_SLTU           0x2BU
#define MIPS_OP_BXX            0x01U
#define MIPS_OP_J              0x02U
#define MIPS_OP_JAL            0x03U
#define MIPS_OP_BEQ            0x04U
#define MIPS_OP_BNE            0x05U
#define MIPS_OP_BLEZ           0x06U
#define MIPS_OP_BGTZ           0x07U
#define MIPS_OP_ADDI           0x08U
#define MIPS_OP_ADDIU          0x09U
#define MIPS_OP_SLTI           0x0AU
#define MIPS_OP_SLTIU          0x0BU
#define MIPS_OP_ANDI           0x0CU
#define MIPS_OP_ORI            0x0DU
#define MIPS_OP_LUI            0x0FU
#define MIPS_OP_COP0           0x10U
# define MIPS_COP_MFC           0x00U
# define MIPS_COP_MTC           0x04U
# define MIPS_COP_RFE           0x10U
#define MIPS_OP_LB             0x20U
#define MIPS_OP_LH             0x21U
#define MIPS_OP_LW             0x23U
#define MIPS_OP_LBU            0x24U
#define MIPS_OP_LHU            0x25U
#define MIPS_OP_SB             0x28U
#define MIPS_OP_SH             0x29U
#define MIPS_OP_SW             0x2BU

union mips_instruction {
   uint32_t encoded;

   struct {
      unsigned fn: 6;
      unsigned params: 20;
      unsigned opcode: 6;
   } op;

   struct {
      unsigned fn: 6;
      unsigned shift: 5;
      unsigned reg_d: 5;
      unsigned reg_t: 5;
      unsigned reg_s: 5;
      unsigned opcode: 6;
   } shift_ri;

   struct {
      unsigned fn: 6;
      unsigned pad: 5;
      unsigned reg_d: 5;
      unsigned reg_t: 5;
      unsigned reg_s: 5;
      unsigned opcode: 6;
   } fn_rr;

   struct {
      unsigned imm: 16;
      unsigned reg_t: 5;
      unsigned reg_s: 5;
      unsigned opcode: 6;
   } fn_ri;

   struct {
      unsigned fn: 6;
      unsigned code: 20;
      unsigned opcode: 6;
   } sysbrk;

   struct {
      unsigned target: 26;
      unsigned opcode: 6;
   } jump_i;

   struct {
      signed off: 16;
      unsigned reg_t: 5;
      unsigned reg_s: 5;
      unsigned opcode: 6;
   } load_store;

   struct {
      unsigned pad: 11;
      unsigned reg_cop: 5;
      unsigned reg_t: 5;
      unsigned cop_op: 5;
      unsigned opcode: 6;
   } cop;
};

#endif /* __PSX_INSTRUCTION_H__ */
