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
   REG_CX  = 1,  /* Temporary variable */
   REG_DX  = 2,  /* Temporary variable */
   REG_BP  = 5,  /* struct dynarec_state pointer [PAFC] */
   REG_SI  = 6,
   REG_DI  = 7,
   REG_SP  = 4,  /* Host stack [PAFC] */
   REG_R8  = 8,  /* PSX AT */
   REG_R9  = 9,  /* PSX V0 */
   REG_R10 = 10, /* PSX V1 */
   REG_R11 = 11, /* PSX A0 */
   REG_R12 = 12, /* PSX A1 [PAFC] */
   REG_R13 = 13, /* PSX T0 [PAFC] */
   REG_R14 = 14, /* PSX SP [PAFC] */
   REG_R15 = 15, /* PSX RA [PAFC] */
};

/* Returns the host register location for the PSX-emulated register
   `reg`. Returns -1 if no host register is allocated, in which case
   it must be accessed in memory. */
static int register_location(enum PSX_REG reg) {
   switch (reg) {
   case PSX_REG_AT:
      return REG_R8;
   case PSX_REG_V0:
      return REG_R9;
   case PSX_REG_V1:
      return REG_R10;
   case PSX_REG_A0:
      return REG_R11;
   case PSX_REG_A1:
      return REG_R12;
   case PSX_REG_T0:
      return REG_R13;
   case PSX_REG_SP:
      return REG_R14;
   case PSX_REG_RA:
      return REG_R15;
   default:
      return -1;
   }
}

#define UNIMPLEMENTED do {                                       \
      printf("%s:%d not implemented\n", __func__, __LINE__);     \
      abort();                                                   \
   } while (0)

/* A set of rather ugly macros to generate if/else statements. The
 * "else" part can be ommited. These statemens introduce a new scope
 * and can be nested. These macros use the 2 byte jump instructions so
 * the bodies must not be bigger than 127 bytes.
 *
 * "else if" statements can be implementing by nesting the elses:
 *
 *   if (a) {                       if (a) {
 *      x();                           x();
 *   } else if (b) {   =======>     } else {
 *      y();                           if (b) {
 *   } else {                             y();
 *      z();                           } else {
 *   }                                    z();
 *                                     }
 *                                  }
 */

#define EMIT_IF(_compiler, _opcode) {              \
   uint8_t *_jump_patch;                           \
   *((_compiler)->map++) = (_opcode);              \
   _jump_patch = (_compiler)->map++;

#define EMIT_ELSE(_compiler) {                                    \
   uint32_t _jump_off = ((_compiler)->map - _jump_patch) + 1;     \
   assert(_jump_off < 128);                                       \
   *_jump_patch = _jump_off;                                      \
   /* JMP imms8 */                                                \
   *((_compiler)->map++) = 0xeb;                                  \
   _jump_patch = (_compiler)->map++;                              \
   }

#define EMIT_ENDIF(_compiler) {                                   \
      uint32_t _jump_off = ((_compiler)->map - _jump_patch) - 1;  \
      assert(_jump_off < 128);                                    \
      *_jump_patch = _jump_off;                                   \
   }}

#define EMIT_IF_EQUAL(_compiler)  EMIT_IF(_compiler, 0x74)
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
                      enum PSX_REG reg_t,
                      enum PSX_REG reg_s) {
   UNIMPLEMENTED;
}

void dynarec_emit_li(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_t,
                     uint32_t val) {
   const int target = register_location(reg_t);

   if (target >= 0) {
      /* MOV $imm32, $r8-15 */
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0xb0 | target;
      emit_imm32(compiler, val);
   } else {
      uint32_t reg_offset;

      reg_offset = offsetof(struct dynarec_state, regs);
      reg_offset += 4 * reg_t;

      /* MOV $imm32, reg_offset(%rbp) */
      *(compiler->map++) = 0xc7;
      *(compiler->map++) = 0x85;
      emit_imm32(compiler, reg_offset);
      emit_imm32(compiler, val);
   }
}

void dynarec_emit_ori(struct dynarec_compiler *compiler,
                      enum PSX_REG reg_t,
                      enum PSX_REG reg_s,
                      uint16_t val) {
   const int target = register_location(reg_t);
   const int source = register_location(reg_s);

   if (target >= 0 && source >= 0) {
      if (target != source) {
         /* MOV %source, %target */
         UNIMPLEMENTED;
      }

      /* OR $imm32, %r8-15 */
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xc0 | target;
      emit_imm32(compiler, val);
   } else {
      UNIMPLEMENTED;
   }
}

static void emit_exception(struct dynarec_compiler *compiler,
                           enum PSX_CPU_EXCEPTIONS exception) {
   // XXX TODO
   (void)exception;
   emit_trap(compiler);
}

void dynarec_emit_sw(struct dynarec_compiler *compiler,
                     enum PSX_REG reg_addr,
                     int16_t offset,
                     enum PSX_REG reg_val) {
   int addr_r  = register_location(reg_addr);
   int value_r = register_location(reg_val);

   /* First we load the address into %ecx and we add the offset */
   if (addr_r >= 0) {
      /* LEA offset(%r8-15), %ecx */
      *(compiler->map++) = 0x67;
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0x8d;
      *(compiler->map++) = 0x80 | addr_r;
      emit_imm32(compiler, (int32_t)offset);
   } else {
      if (reg_addr == PSX_REG_R0) {
         /* XXX We could optimize this since it means that the offset
            is static. Not sure if this is common enough to be worth
            it. */
         /* XOR %ecx, %ecx */
         *(compiler->map++) = 0x31;
         *(compiler->map++) = 0xc9;
      } else {
         uint32_t reg_offset;

         reg_offset = offsetof(struct dynarec_state, regs);
         reg_offset += 4 * reg_addr;

         /* MOV reg_offset(%rbp), %ecx */
         *(compiler->map++) = 0x8b;
         *(compiler->map++) = 0x8d;
         emit_imm32(compiler, reg_offset);
      }

      /* ADD $offset, %ecx */
      *(compiler->map++) = 0x41;
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xc3;
      emit_imm32(compiler, (int32_t)offset);
   }

   /* Move address to %eax */
   /* MOV %ecx, %eax */
   *(compiler->map++) = 0x89;
   *(compiler->map++) = 0xc8;

   /* Check alignment */
   /* AND $3, %eax */
   *(compiler->map++) = 0x83;
   *(compiler->map++) = 0xe0;
   *(compiler->map++) = 0x03;

   EMIT_IF_EQUAL(compiler) {
      /* Address is not aligned correctly. */
      emit_exception(compiler, PSX_EXCEPTION_LOAD_ALIGN);
   } EMIT_ENDIF(compiler)

   /* Move address to %eax */
   /* MOV %ecx, %eax */
   *(compiler->map++) = 0x89;
   *(compiler->map++) = 0xc8;

   /* Compute offset into region_mask, i.e. addr >> 29 */
   /* SHR $29, %eax */
   *(compiler->map++) = 0xc1;
   *(compiler->map++) = 0xe8;
   *(compiler->map++) = 29;

   /* Mask the address. region_mask is pointed */
   /* AND off7(%rbp, %rax, 4), %ecx */
   *(compiler->map++) = 0x23;
   *(compiler->map++) = 0x4c;
   *(compiler->map++) = 0x85;
   emit_imm7(compiler, offsetof(struct dynarec_state, region_mask));

   /* Test if the address is in RAM */
   /* CMP imm32, %rcx */
   *(compiler->map++) = 0x81;
   *(compiler->map++) = 0xf9;
   /* The RAM is mirrored 4 times */
   emit_imm32(compiler, PSX_RAM_SIZE * 4);

   if (value_r < 0) {
      /* Load value into %edx */
      uint32_t reg_offset;

      reg_offset = offsetof(struct dynarec_state, regs);
      reg_offset += 4 * reg_val;

      /* MOV reg_offset(%rbp), %edx */
      *(compiler->map++) = 0x8b;
      *(compiler->map++) = 0x95;
      emit_imm32(compiler, reg_offset);
   }

   EMIT_IF_BELOW(compiler) {
      /* We're targetting RAM */

      /* Mask the address in case it was in one of the mirrors */
      /* AND imm32, %ecx */
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xe1;
      emit_imm32(compiler, PSX_RAM_SIZE - 1);

      /* Copy to %eax so that we compute the page offset for
         invalidation */
      /* MOV %ecx, %eax */
      *(compiler->map++) = 0x89;
      *(compiler->map++) = 0xc8;

      /* Compute page index in %eax */
      /* SHR $imm8, %eax */
      *(compiler->map++) = 0xc1;
      *(compiler->map++) = 0xe8;
      *(compiler->map++) = DYNAREC_PAGE_SIZE_SHIFT;

      /* Compute offset in the page table */
      /* imul imm32, %rax, %rax */
      *(compiler->map++) = 0x48;
      *(compiler->map++) = 0x69;
      *(compiler->map++) = 0xc0;
      emit_imm32(compiler, sizeof(struct dynarec_page));

      /* Clear valid flag */
      /* MOVL $0, off32(%rbp, %rax, 1) */
      *(compiler->map++) = 0xc7;
      *(compiler->map++) = 0x84;
      *(compiler->map++) = 0x05;
      emit_imm32(compiler, offsetof(struct dynarec_state, pages));
      emit_imm32(compiler, 0);

      /* Add the address of the RAM buffer in host memory */
      /* ADD imm7(%rbp), %ecx */
      *(compiler->map++) = 0x03;
      *(compiler->map++) = 0x4d;
      emit_imm7(compiler, offsetof(struct dynarec_state, ram));

      if (value_r >= 0) {
         /* MOVL %r8-15, (%rcx) */
         *(compiler->map++) = 0x44;
         *(compiler->map++) = 0x89;
         *(compiler->map++) = ((value_r & 7) << 3) | 1;
      } else {
         /* MOVL %edx, (%rcx) */
         *(compiler->map++) = 0x89;
         *(compiler->map++) = 0x11;
      }
   } EMIT_ELSE(compiler) {
      /* Test if the address is in the scratchpad */
      /* SUB imm32, %ecx */
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xe9;
      emit_imm32(compiler, PSX_SCRATCHPAD_BASE);

      /* CMP imm32, %ecx */
      *(compiler->map++) = 0x81;
      *(compiler->map++) = 0xf9;
      emit_imm32(compiler, PSX_SCRATCHPAD_SIZE);

      EMIT_IF_BELOW(compiler) {
         /* We're targetting the scratchpad. This is the simplest
            case, no invalidation needed, we can store it directly in
            the scratchpad buffer */

         /* Add the address of the RAM buffer in host memory */
         /* ADD imm7(%rbp), %ecx */
         *(compiler->map++) = 0x03;
         *(compiler->map++) = 0x4d;
         emit_imm7(compiler, offsetof(struct dynarec_state, scratchpad));

         if (value_r >= 0) {
            /* MOVL %r8-15, (%rcx) */
            *(compiler->map++) = 0x44;
            *(compiler->map++) = 0x89;
            *(compiler->map++) = ((value_r & 7) << 3) | 1;
         } else {
            /* MOVL %edx, (%rcx) */
            *(compiler->map++) = 0x89;
            *(compiler->map++) = 0x11;
         }
      } EMIT_ELSE(compiler) {
         emit_trap(compiler);
      } EMIT_ENDIF(compiler)
   } EMIT_ENDIF(compiler)

   /* XXX Finish me */
   emit_trap(compiler);
}

void dynarec_execute(struct dynarec_state *state,
                     dynarec_fn_t target) {

   __asm__ ("mov %%rcx, %%rbp\n\t"
            "call *%%rax\n\t"
            :
            : "c"(state), "a"(target));

}
