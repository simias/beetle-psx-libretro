#include <stdio.h>

#include "dynarec.h"

/* AMD64 register encoding */
enum {
   REG_AX = 0, /* General accumulator */
   REG_BX = 3, /* Contains V0 (R2) */
   REG_CX = 1, /* Contains V1 (R3) */
   REG_DX = 2, /* Contains A0 (R4) */
   REG_BP = 5, /* Contains T0 (R8) */
   REG_SI = 6,
   REG_DI = 7, /* Contains RA (R31) */
   REG_SP = 4, /* Contains SP (R29) */
};

/* Returns the host register location for the PSX-emulated register
   `reg`. Returns -1 if no host register is allocated, in which case
   it must be accessed in memory. */
static int register_location(uint8_t reg) {
   switch (reg) {
   case PSX_REG_V0:
      return REG_BX;
   case PSX_REG_V1:
      return REG_CX;
   case PSX_REG_A0:
      return REG_DX;
   case PSX_REG_T0:
      return REG_BP;
   case PSX_REG_RA:
      return REG_DI;
   case PSX_REG_SP:
      return REG_SP;
   default:
      return -1;
   }
}

#define UNIMPLEMENTED do {                                       \
      printf("%s:%d not implemented\n", __func__, __LINE__);     \
      abort();                                                   \
   } while (0)


static void emit_imm32(struct dynarec_compiler *compiler,
                  uint32_t val) {
   int i;

   for (i = 0; i < 4; i++) {
      *(compiler->map++) = val & 0xff;
      val >>= 8;
   }
}

void dynarec_emit_lui(struct dynarec_compiler *compiler,
                      uint8_t reg_t,
                      uint16_t val) {
   const int target = register_location(reg_t);

   if (target >= 0) {
      /* MOV $imm32, $r0-7 */
      *(compiler->map++) = 0xb8 | target;
      emit_imm32(compiler, ((uint32_t)val) << 16);
   } else {
      UNIMPLEMENTED;
   }
}


void dynarec_emit_ori(struct dynarec_compiler *compiler,
                      uint8_t reg_t,
                      uint8_t reg_s,
                      uint16_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   if (target >= 0 && source >= 0) {
      if (target != source) {
         /* MOV $source, $target */
         UNIMPLEMENTED;
      }

      /* OR $imm32, $r0-7 */
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xc8 | target;
      emit_imm32(compiler, val);
      return;
   }

   UNIMPLEMENTED;
}
