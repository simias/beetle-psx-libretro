#ifndef __PSX_INSTRUCTION_H__
#define __PSX_INSTRUCTION_H__

#include <stdint.h>

#define MIPS_OP_FN             0x00U
# define MIPS_FN_SLL            0x00U
# define MIPS_FN_SRL            0x02U
# define MIPS_FN_SRA            0x03U
# define MIPS_FN_BREAK          0x0DU
# define MIPS_FN_ADD            0x20U
# define MIPS_FN_ADDU           0x21U
#define MIPS_OP_ORI            0x0DU
#define MIPS_OP_LUI            0x0FU

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
   } alu_rr;

   struct {
      unsigned imm: 16;
      unsigned reg_t: 5;
      unsigned reg_s: 5;
      unsigned opcode: 6;
   } alu_ri;

   struct {
      unsigned fn: 6;
      unsigned code: 20;
      unsigned opcode: 6;
   } brk;
};

#endif /* __PSX_INSTRUCTION_H__ */
