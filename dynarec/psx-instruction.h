#ifndef __PSX_INSTRUCTION_H__
#define __PSX_INSTRUCTION_H__

#include <stdint.h>

#define MIPS_OP_FN             0x00U
# define MIPS_FN_SLL            0x00U
# define MIPS_FN_SRL            0x02U
# define MIPS_FN_SRA            0x03U
# define MIPS_FN_JR             0x08U
# define MIPS_FN_JALR           0x09U
# define MIPS_FN_BREAK          0x0DU
# define MIPS_FN_MFHI           0x10U
# define MIPS_FN_MTHI           0x11U
# define MIPS_FN_MFLO           0x12U
# define MIPS_FN_MTLO           0x13U
# define MIPS_FN_MULTU          0x19U
# define MIPS_FN_ADD            0x20U
# define MIPS_FN_ADDU           0x21U
# define MIPS_FN_SUBU           0x23U
# define MIPS_FN_AND            0x24U
# define MIPS_FN_OR             0x25U
# define MIPS_FN_SLTU           0x2BU
#define MIPS_OP_BXX            0x00U
#define MIPS_OP_J              0x02U
#define MIPS_OP_JAL            0x03U
#define MIPS_OP_BEQ            0x04U
#define MIPS_OP_BNE            0x05U
#define MIPS_OP_BLEZ           0x06U
#define MIPS_OP_BGTZ           0x07U
#define MIPS_OP_ADDI           0x08U
#define MIPS_OP_ADDIU          0x09U
#define MIPS_OP_ANDI           0x0CU
#define MIPS_OP_ORI            0x0DU
#define MIPS_OP_LUI            0x0FU
#define MIPS_OP_LB             0x20U
#define MIPS_OP_LH             0x21U
#define MIPS_OP_LW             0x23U
#define MIPS_OP_LBU            0x24U
#define MIPS_OP_LHU            0x25U

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
   } brk;

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
};

#endif /* __PSX_INSTRUCTION_H__ */
