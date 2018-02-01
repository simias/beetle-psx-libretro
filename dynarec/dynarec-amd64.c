#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#include "dynarec.h"

/* AMD64 register encoding.
 *
 * PAFC = Preserved Across Function Calls, per the x86-64 ABI:
 *
 * http://refspecs.linuxfoundation.org/elf/x86_64-abi-0.99.pdf
 */
enum X86_REG {
   REG_AX  = 0,  /* Temporary variable */
   REG_BX  = 3,  /* PSX V0 (R2) [PAFC] */
   REG_CX  = 1,  /* PSX V1 (R3) */
   REG_DX  = 2,  /* PSX A0 (R4) */
   REG_BP  = 5,  /* PSX T0 (R8) [PAFC] */
   REG_SI  = 6,  /* PSX SP (R29) */
   REG_DI  = 7,  /* PSX RA (R31) */
   REG_SP  = 4,  /* Host stack [PAFC] */
   REG_R8  = 8,
   REG_R9  = 9,
   REG_R10 = 10, /* Temporary variable: holds value for SW */
   REG_R11 = 11, /* Temporary variable: holds address for SW */
   REG_R12 = 12, /* struct dynarec_state pointer [PAFC] */
   REG_R13 = 13, /* [PAFC] */
   REG_R14 = 14, /* [PAFC] */
   REG_R15 = 15, /* [PAFC] */
};

/* Returns the host register location for the PSX-emulated register
   `reg`. Returns -1 if no host register is allocated, in which case
   it must be accessed in memory. */
static int register_location(enum PSX_REG reg) {
   switch (reg) {
   case PSX_REG_V0:
      return REG_BX;
   case PSX_REG_V1:
      return REG_CX;
   case PSX_REG_A0:
      return REG_DX;
   case PSX_REG_T0:
      return REG_BP;
   case PSX_REG_SP:
      return REG_SI;
   case PSX_REG_RA:
      return REG_DI;
   default:
      return -1;
   }
}

#define UNIMPLEMENTED do {                                       \
      printf("%s:%d not implemented\n", __func__, __LINE__);     \
      abort();                                                   \
   } while (0)

#define EMIT_IF(_compiler, _opcode) do {           \
   uint8_t *_jump_patch;                           \
   *((_compiler)->map++) = (_opcode);              \
   _jump_patch = (_compiler)->map++;

#define EMIT_ENDIF(_compiler) {                                   \
      uint32_t _jump_off = ((_compiler)->map - _jump_patch) - 1;  \
      assert(_jump_off < 128);                                    \
      *_jump_patch = _jump_off;                                   \
   }} while(0)

#define EMIT_IF_EQUAL(_compiler) EMIT_IF(_compiler, 0x74)
#define EMIT_IF_BELOW(_compiler)  EMIT_IF(_compiler, 0x72)

static void emit_imm32(struct dynarec_compiler *compiler,
                       uint32_t val) {
   int i;

   for (i = 0; i < 4; i++) {
      *(compiler->map++) = val & 0xff;
      val >>= 8;
   }
}

static void emit_imm7(struct dynarec_compiler *compiler,
                      uint32_t val) {
   assert(val <= 0x7f);

   *(compiler->map++) = val;
}

/* Emit a signed value on 24 bits  */
static void emit_simm24(struct dynarec_compiler *compiler,
                        int32_t val) {
   int i;

   for (i = 0; i < 3; i++) {
      *(compiler->map++) = val & 0xff;
      val >>= 8;
   }
}

static void emit_trap(struct dynarec_compiler *compiler) {
   /* INT 3 */
   *(compiler->map++) = 0xcc;
}

void dynarec_emit_mov(struct dynarec_compiler *compiler,
                      uint8_t reg_t,
                      uint8_t reg_s) {
   UNIMPLEMENTED;
}

void dynarec_emit_li(struct dynarec_compiler *compiler,
                     uint8_t reg_t,
                     uint32_t val) {
   const int target = register_location(reg_t);

   if (target >= 0) {
      /* MOV $imm32, $r0-7 */
      *(compiler->map++) = 0xb8 | target;
      emit_imm32(compiler, val);
   } else {
      uint32_t reg_offset;

      reg_offset = offsetof(struct dynarec_state, regs);
      reg_offset += 4 * reg_t;

      /* MOV $imm32, reg_offset(%r12) */
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0xc7;
      *(compiler->map++) = 0x84;
      *(compiler->map++) = 0x24;
      emit_imm32(compiler, reg_offset);
      emit_imm32(compiler, val);
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
         /* MOV %source, %target */
         UNIMPLEMENTED;
      }

      /* OR $imm32, %r0-7 */
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xc8 | target;
      emit_imm32(compiler, val);
      return;
   }

   UNIMPLEMENTED;
}

static void emit_exception(struct dynarec_compiler *compiler,
                           enum PSX_CPU_EXCEPTIONS exception) {
   // XXX TODO
   (void)exception;
   emit_trap(compiler);
}

void dynarec_emit_sw(struct dynarec_compiler *compiler,
                     uint8_t reg_addr,
                     int16_t offset,
                     uint8_t reg_val) {
   int addr_r  = register_location(reg_addr);
   int value_r = register_location(reg_val);

   /* First we load the address into R11 and we add the offset */
   if (addr_r >= 0) {
      /* LEA offset(%r0-7), %r11d */
      *(compiler->map++) = 0x67;
      *(compiler->map++) = 0x44;
      *(compiler->map++) = 0x8d;
      *(compiler->map++) = 0x98 | addr_r;
      emit_simm24(compiler, offset);
   } else {
      uint32_t reg_offset;

      reg_offset = offsetof(struct dynarec_state, regs);
      reg_offset += 4 * reg_addr;

      /* MOV reg_offset(%r12), %r11d */
      *(compiler->map++) = 0x45;
      *(compiler->map++) = 0x8b;
      *(compiler->map++) = 0x9c;
      *(compiler->map++) = 0x24;
      emit_imm32(compiler, reg_offset);

      /* ADD $offset, %r11d */
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xc3;
      emit_imm32(compiler, (int32_t)offset);
   }

   /* Move address to %eax */
   /* MOV %r11d, %eax */
   *(compiler->map++) = 0x44;
   *(compiler->map++) = 0x89;
   *(compiler->map++) = 0xd8;

   /* Check alignment */
   /* AND $3, %eax */
   *(compiler->map++) = 0x83;
   *(compiler->map++) = 0xe0;
   *(compiler->map++) = 0x03;

   EMIT_IF_EQUAL(compiler) {
      /* Address is not aligned correctly. */
      emit_exception(compiler, PSX_EXCEPTION_LOAD_ALIGN);
   } EMIT_ENDIF(compiler);

   /* Move address to %eax */
   /* MOV %r11d, %eax */
   *(compiler->map++) = 0x44;
   *(compiler->map++) = 0x89;
   *(compiler->map++) = 0xd8;

   /* Compute offset into region_mask, i.e. addr >> 29 */
   /* SHR $29, %eax */
   *(compiler->map++) = 0xc1;
   *(compiler->map++) = 0xe8;
   *(compiler->map++) = 29;

   /* Mask the address. region_mask is pointed at by r13 */
   /* AND (%r13, %rax, 4), %r11d */
   *(compiler->map++) = 0x45;
   *(compiler->map++) = 0x23;
   *(compiler->map++) = 0x5c;
   *(compiler->map++) = 0x84;
   emit_imm7(compiler, offsetof(struct dynarec_state, region_mask));

   /* Move address to %eax */
   /* MOV %r11d, %eax */
   *(compiler->map++) = 0x44;
   *(compiler->map++) = 0x89;
   *(compiler->map++) = 0xd8;

   /* Test if the address is in RAM */
   /* CMP imm32, %rax */
   *(compiler->map++) = 0x48;
   *(compiler->map++) = 0x3d;
   /* The RAM is mirrored 4 times */
   emit_imm32(compiler, PSX_RAM_SIZE * 4);

   EMIT_IF_BELOW(compiler) {
      /* Mask the address in case it was in one of the mirrors */
      /* AND imm32, %eax */
      *(compiler->map++) = 0x48;
      *(compiler->map++) = 0x25;
      emit_imm32(compiler, PSX_RAM_SIZE - 1);

      /* Add the address of the RAM buffer in host memory */
      /* ADDQ imm32(%r12), %rax */
      *(compiler->map++) = 0x49;
      *(compiler->map++) = 0x03;
      *(compiler->map++) = 0x44;
      *(compiler->map++) = 0x24;
      emit_imm7(compiler, offsetof(struct dynarec_state, ram));

      if (value_r > 0) {
         /* MOVL %r0-7, (%rax) */
         *(compiler->map++) = 0x89;
         *(compiler->map++) = value_r << 3;
      } else {
         /* Load to a temporary register */
         UNIMPLEMENTED;
      }

      /* XXX invalidate page */
   } EMIT_ENDIF(compiler);

   /* XXX Finish me */
   emit_trap(compiler);
}

void dynarec_execute(struct dynarec_state *state,
                     dynarec_fn_t target) {

   __asm__ ("mov %%rdi, %%r12\n\t"
            "call *%%rax\n\t"
            :
            : "D"(state), "a"(target));

}
