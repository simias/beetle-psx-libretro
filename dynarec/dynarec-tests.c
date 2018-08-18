#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "dynarec.h"
#include "psx-instruction.h"

typedef int (*test_fn_t)(struct dynarec_state *);

static void *xcalloc(size_t nmemb, size_t size) {
   void *p = calloc(nmemb, size);

   assert(p != NULL);

   return p;
}

static int test_eq(const char *what, uint32_t val, uint32_t expected) {
   if (val != expected) {
      printf("%s: expected 0x%x, got 0x%x\n", what, expected, val);
      return -1;
   }

   return 0;
}
#define TEST_EQ(_v, _e) do {                    \
   if (test_eq(#_v, _v, _e)) {                  \
      return -1;                                \
   } } while (0)

struct reg_val {
   enum PSX_REG r;
   uint32_t v;
};

static const char *reg_names[] = {
   "PSX_REG_R0",
   "PSX_REG_AT",
   "PSX_REG_V0",
   "PSX_REG_V1",
   "PSX_REG_A0",
   "PSX_REG_A1",
   "PSX_REG_A2",
   "PSX_REG_A3",
   "PSX_REG_T0",
   "PSX_REG_T1",
   "PSX_REG_T2",
   "PSX_REG_T3",
   "PSX_REG_T4",
   "PSX_REG_T5",
   "PSX_REG_T6",
   "PSX_REG_T7",
   "PSX_REG_S0",
   "PSX_REG_S1",
   "PSX_REG_S2",
   "PSX_REG_S3",
   "PSX_REG_S4",
   "PSX_REG_S5",
   "PSX_REG_S6",
   "PSX_REG_S7",
   "PSX_REG_T8",
   "PSX_REG_T9",
   "PSX_REG_K0",
   "PSX_REG_K1",
   "PSX_REG_GP",
   "PSX_REG_SP",
   "PSX_REG_FP",
   "PSX_REG_RA",
   "PSX_REG_DT",
   "PSX_REG_HI",
   "PSX_REG_LO",
};

static int check_regs(struct dynarec_state *state,
                       struct reg_val *regs,
                       size_t nregs) {
   int ret = 0;
   unsigned i;

   for (i = 0; i < ARRAY_SIZE(state->regs); i++) {
      /* + 1 because we don't store R0 (since it's always 0) */
      const enum PSX_REG reg = i + 1;
      uint32_t expected;
      unsigned j;

      if (reg == PSX_REG_DT) {
         /* Don't bother validating the temporary register */
         continue;
      }

      /* Load with default register value */
      expected = (i << 24 | i << 16 | i << 8 | i);

      /* See if reg is in regs */
      for (j = 0; j < nregs; j++) {
         if (regs[j].r == reg) {
            expected = regs[j].v;
            break;
         }
      }

      assert(reg < ARRAY_SIZE(reg_names));

      if (test_eq(reg_names[reg], state->regs[i], expected)) {
         ret = -1;
      }
   }

   return ret;
}

static void load_code(struct dynarec_state *state,
                      const union mips_instruction *code,
                      size_t code_len,
                      uint32_t addr) {
   unsigned i;

   assert((addr & 3) == 0);
   assert((addr + code_len * 4) < PSX_RAM_SIZE);

   for (i = 0; i < code_len; i++) {
      uint32_t c = code[i].encoded;

      state->ram[addr + i * 4 + 0] = c;
      state->ram[addr + i * 4 + 1] = c >> 8;
      state->ram[addr + i * 4 + 2] = c >> 16;
      state->ram[addr + i * 4 + 3] = c >> 24;
   };
}

static int run_test(const char *name, test_fn_t f) {
   uint8_t *ram = xcalloc(1, PSX_RAM_SIZE);
   uint8_t *scratchpad = xcalloc(1, PSX_SCRATCHPAD_SIZE);
   uint8_t *bios = xcalloc(1, PSX_BIOS_SIZE);
   unsigned i;
   int ret;

   struct dynarec_state *state = dynarec_init(ram, scratchpad, bios);

   state->options |= DYNAREC_OPT_EXIT_ON_BREAK;

   /* Assume we're running from the beginning of the RAM */
   dynarec_set_pc(state, 0);

   /* Put dummy value in all other registers */
   for (i = 0; i < ARRAY_SIZE(state->regs); i++) {
      state->regs[i] = (i << 24 | i << 16 | i << 8 | i);
   }

   printf("[%s] running...\n", name);

   ret = f(state);

   printf((ret == 0) ? "[%s] success\n" : "[%s] failure\n",
          name);

   return ret;
}

/***************************
 * Pseudo-assembler macros *
 ***************************/
#define ALU_RI(_op, _rt, _ro, _i)               \
   (union mips_instruction){                    \
      .alu_ri =                                 \
         { .opcode = (_op),                     \
           .reg_t = (_rt),                      \
           .reg_s = (_ro),                      \
           .imm = (_i) }}

#define ALU_RR(_fn, _rt, _ro1, _ro2)            \
   (union mips_instruction){                    \
      .alu_rr =                                 \
         { .opcode = MIPS_OP_FN,                \
           .fn = (_fn),                         \
           .reg_d = (_rt),                      \
           .reg_s = (_ro1),                     \
           .reg_t = (_ro2), }}

#define SHIFT_RI(_fn, _rt, _ro, _s)             \
   (union mips_instruction){                    \
      .shift_ri =                               \
         { .opcode = MIPS_OP_FN,                \
           .fn = (_fn),                         \
           .reg_d = (_rt),                      \
           .reg_t = (_ro),                      \
           .shift = (_s) }}

#define BREAK(_code)                            \
   (union mips_instruction){                    \
      .brk =                                    \
         { .opcode = MIPS_OP_FN,                \
           .fn = MIPS_FN_BREAK,                 \
           .code = (_code) }}


#define SLL(_rt, _ro, _s)                       \
   SHIFT_RI(MIPS_FN_SLL, (_rt), (_ro), (_s))
#define SRL(_rt, _ro, _s)                       \
   SHIFT_RI(MIPS_FN_SRL, (_rt), (_ro), (_s))
#define SRA(_rt, _ro, _s)                       \
   SHIFT_RI(MIPS_FN_SRA, (_rt), (_ro), (_s))
#define NOP                                     \
   SLL(PSX_REG_R0, PSX_REG_R0, 0)
#define MFHI(_rt)                               \
   ALU_RR(MIPS_FN_MFHI, (_rt), 0, 0)
#define MTHI(_rs)                               \
   ALU_RR(MIPS_FN_MTHI, 0, (_rs), 0)
#define MFLO(_rt)                               \
   ALU_RR(MIPS_FN_MFLO, (_rt), 0, 0)
#define MTLO(_rs)                               \
   ALU_RR(MIPS_FN_MTLO, 0, (_rs), 0)
#define ADD(_rt, _ro1, _ro2)                    \
   ALU_RR(MIPS_FN_ADD, (_rt), (_ro1), (_ro2))
#define ADDU(_rt, _ro1, _ro2)                   \
   ALU_RR(MIPS_FN_ADDU, (_rt), (_ro1), (_ro2))
#define SUBU(_rt, _ro1, _ro2)                   \
   ALU_RR(MIPS_FN_SUBU, (_rt), (_ro1), (_ro2))
#define AND(_rt, _ro1, _ro2)                    \
   ALU_RR(MIPS_FN_AND, (_rt), (_ro1), (_ro2))
#define OR(_rt, _ro1, _ro2)                     \
   ALU_RR(MIPS_FN_OR, (_rt), (_ro1), (_ro2))
#define ORI(_rt, _ro, _i)                       \
   ALU_RI(MIPS_OP_ORI, (_rt), (_ro), (_i))
#define LUI(_rt, _i)                            \
   ALU_RI(MIPS_OP_LUI, (_rt), PSX_REG_R0, (_i))
/* Dumb 2-instruction Load Immediate implementation. For simplicity
   doesn't attempt to reduce to a single instruction if the immediate
   fits 16 bits. */
#define LI(_rt, _i)                             \
   LUI((_rt), ((_i) >> 16)),                    \
   ORI((_rt), (_rt), (_i) & 0xffff)

/*********
 * Tests *
 *********/

static int test_break(struct dynarec_state *state) {
   union mips_instruction code[] = {
      BREAK(0xdead),
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, NULL, 0);
}

static int test_lui(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LUI(PSX_REG_T0, 0xbeef),
      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0xbeef0000 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_nop(struct dynarec_state *state) {
   union mips_instruction code[] = {
      NOP,
      NOP,
      NOP,
      BREAK(0xdead),
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, NULL, 0);
}

static int test_ori(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LUI(PSX_REG_T0, 0xabcd),
      ORI(PSX_REG_T1, PSX_REG_T0, 0x1234),
      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0xabcd0000 },
      { .r = PSX_REG_T1, .v = 0xabcd1234 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_li(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 0x89abcdef),
      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0x89abcdef },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_r0(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 0x1),
      ADD(PSX_REG_T1, PSX_REG_R0, PSX_REG_R0),
      ADD(PSX_REG_R0, PSX_REG_T0, PSX_REG_T0),
      ADD(PSX_REG_T2, PSX_REG_R0, PSX_REG_R0),
      ADD(PSX_REG_R0, PSX_REG_R0, PSX_REG_T0),
      ADD(PSX_REG_T3, PSX_REG_T0, PSX_REG_R0),
      ADD(PSX_REG_T4, PSX_REG_T1, PSX_REG_R0),
      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 1 },
      { .r = PSX_REG_T1, .v = 0 },
      { .r = PSX_REG_T2, .v = 0 },
      { .r = PSX_REG_T3, .v = 1 },
      { .r = PSX_REG_T4, .v = 0 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_sll(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 0x89abcdef),
      SLL(PSX_REG_T1, PSX_REG_T0, 0),
      SLL(PSX_REG_V0, PSX_REG_T0, 8),
      SLL(PSX_REG_S0, PSX_REG_T0, 4),
      SLL(PSX_REG_V1, PSX_REG_S0, 1),
      SLL(PSX_REG_S1, PSX_REG_S0, 1),
      SLL(PSX_REG_T0, PSX_REG_T0, 16),
      SLL(PSX_REG_S1, PSX_REG_S1, 16),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0xcdef0000 },
      { .r = PSX_REG_T1, .v = 0x89abcdef },
      { .r = PSX_REG_V0, .v = 0xabcdef00 },
      { .r = PSX_REG_V1, .v = 0x3579bde0 },
      { .r = PSX_REG_S0, .v = 0x9abcdef0 },
      { .r = PSX_REG_S1, .v = 0xbde00000 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_srl(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 0x89abcdef),
      SRL(PSX_REG_T1, PSX_REG_T0, 0),
      SRL(PSX_REG_V0, PSX_REG_T0, 8),
      SRL(PSX_REG_S0, PSX_REG_T0, 4),
      SRL(PSX_REG_V1, PSX_REG_S0, 1),
      SRL(PSX_REG_S1, PSX_REG_S0, 1),
      SRL(PSX_REG_T0, PSX_REG_T0, 16),
      SRL(PSX_REG_S1, PSX_REG_S1, 16),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0x000089ab },
      { .r = PSX_REG_T1, .v = 0x89abcdef },
      { .r = PSX_REG_V0, .v = 0x0089abcd },
      { .r = PSX_REG_V1, .v = 0x044d5e6f },
      { .r = PSX_REG_S0, .v = 0x089abcde },
      { .r = PSX_REG_S1, .v = 0x0000044d },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_sra(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 0x89abcdef),
      SRA(PSX_REG_T1, PSX_REG_T0, 0),
      SRA(PSX_REG_V0, PSX_REG_T0, 8),
      SRA(PSX_REG_S0, PSX_REG_T0, 4),
      SRA(PSX_REG_V1, PSX_REG_S0, 1),
      SRA(PSX_REG_S1, PSX_REG_S0, 1),
      SRA(PSX_REG_T0, PSX_REG_T0, 16),
      SRA(PSX_REG_S1, PSX_REG_S1, 16),
      LI(PSX_REG_T4, 0x12345678),
      SRA(PSX_REG_T5, PSX_REG_T4, 16),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0xffff89ab },
      { .r = PSX_REG_T1, .v = 0x89abcdef },
      { .r = PSX_REG_V0, .v = 0xff89abcd },
      { .r = PSX_REG_V1, .v = 0xfc4d5e6f },
      { .r = PSX_REG_S0, .v = 0xf89abcde },
      { .r = PSX_REG_S1, .v = 0xfffffc4d },
      { .r = PSX_REG_T4, .v = 0x12345678 },
      { .r = PSX_REG_T5, .v = 0x00001234 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_addu(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 1),
      LI(PSX_REG_T1, 2),
      ADDU(PSX_REG_T3, PSX_REG_T0, PSX_REG_T1),
      ADDU(PSX_REG_T4, PSX_REG_T0, PSX_REG_T0),
      ADDU(PSX_REG_T5, PSX_REG_T1, PSX_REG_T1),
      ADDU(PSX_REG_V0, PSX_REG_T0, PSX_REG_T0),
      ADDU(PSX_REG_V1, PSX_REG_T1, PSX_REG_V0),
      ADDU(PSX_REG_V1, PSX_REG_V1, PSX_REG_V1),
      ADDU(PSX_REG_T5, PSX_REG_T5, PSX_REG_T5),

      LI(PSX_REG_S0, 0x7fffffff),
      LI(PSX_REG_S1, 0xffffffff),
      ADDU(PSX_REG_S2, PSX_REG_S0, PSX_REG_T0),
      ADDU(PSX_REG_S3, PSX_REG_S1, PSX_REG_T1),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0x1 },
      { .r = PSX_REG_T1, .v = 0x2 },
      { .r = PSX_REG_T3, .v = 0x3 },
      { .r = PSX_REG_T4, .v = 0x2 },
      { .r = PSX_REG_T5, .v = 0x8 },
      { .r = PSX_REG_V0, .v = 0x2 },
      { .r = PSX_REG_V1, .v = 0x8 },
      { .r = PSX_REG_S0, .v = 0x7fffffff },
      { .r = PSX_REG_S1, .v = 0xffffffff },
      { .r = PSX_REG_S2, .v = 0x80000000 },
      { .r = PSX_REG_S3, .v = 0x00000001 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_add_no_exception(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 1),
      LI(PSX_REG_T1, 2),
      ADD(PSX_REG_T3, PSX_REG_T0, PSX_REG_T1),
      ADD(PSX_REG_V0, PSX_REG_T0, PSX_REG_T0),
      ADD(PSX_REG_V0, PSX_REG_V0, PSX_REG_T1),
      ADD(PSX_REG_T5, PSX_REG_T1, PSX_REG_T1),
      ADD(PSX_REG_V1, PSX_REG_T1, PSX_REG_V0),
      ADD(PSX_REG_V1, PSX_REG_V1, PSX_REG_V1),
      ADD(PSX_REG_T5, PSX_REG_T5, PSX_REG_T5),
      ADD(PSX_REG_T4, PSX_REG_V0, PSX_REG_T1),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0x1 },
      { .r = PSX_REG_T1, .v = 0x2 },
      { .r = PSX_REG_T3, .v = 0x3 },
      { .r = PSX_REG_T4, .v = 0x6 },
      { .r = PSX_REG_T5, .v = 0x8 },
      { .r = PSX_REG_V0, .v = 0x4 },
      { .r = PSX_REG_V1, .v = 0xc },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_subu(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 1),
      LI(PSX_REG_T1, 2),
      LI(PSX_REG_T2, 10),
      LI(PSX_REG_T3, 0x80000000),
      SUBU(PSX_REG_V0, PSX_REG_T2, PSX_REG_T1),
      SUBU(PSX_REG_V1, PSX_REG_T0, PSX_REG_T1),
      SUBU(PSX_REG_AT, PSX_REG_V0, PSX_REG_T0),
      SUBU(PSX_REG_S0, PSX_REG_T0, PSX_REG_V0),
      SUBU(PSX_REG_S1, PSX_REG_T3, PSX_REG_T0),
      SUBU(PSX_REG_V0, PSX_REG_V0, PSX_REG_T1),
      SUBU(PSX_REG_T0, PSX_REG_T0, PSX_REG_T0),
      SUBU(PSX_REG_T1, PSX_REG_T1, PSX_REG_T1),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 0 },
      { .r = PSX_REG_T1, .v = 0 },
      { .r = PSX_REG_T2, .v = 10 },
      { .r = PSX_REG_T3, .v = 0x80000000 },
      { .r = PSX_REG_V0, .v = 6 },
      { .r = PSX_REG_V1, .v = 0xffffffff },
      { .r = PSX_REG_AT, .v = 7 },
      { .r = PSX_REG_S0, .v = 0xfffffff9 },
      { .r = PSX_REG_S1, .v = 0x7fffffff },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_and(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 6),
      LI(PSX_REG_T1, 3),
      LI(PSX_REG_T2, 0xffffffff),
      LI(PSX_REG_T3, 0),

      AND(PSX_REG_S0, PSX_REG_R0, PSX_REG_T2),
      AND(PSX_REG_S1, PSX_REG_T0, PSX_REG_T1),
      AND(PSX_REG_V0, PSX_REG_T0, PSX_REG_T1),
      AND(PSX_REG_V1, PSX_REG_T0, PSX_REG_V0),
      AND(PSX_REG_S2, PSX_REG_T0, PSX_REG_T2),
      AND(PSX_REG_S3, PSX_REG_T0, PSX_REG_T3),
      AND(PSX_REG_T0, PSX_REG_T0, PSX_REG_T0),
      AND(PSX_REG_T1, PSX_REG_T2, PSX_REG_T1),
      AND(PSX_REG_T1, PSX_REG_T1, PSX_REG_T2),
      AND(PSX_REG_T2, PSX_REG_T2, PSX_REG_T1),
      AND(PSX_REG_T0, PSX_REG_T0, PSX_REG_V0),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 2 },
      { .r = PSX_REG_T1, .v = 3 },
      { .r = PSX_REG_T2, .v = 3 },
      { .r = PSX_REG_T3, .v = 0 },
      { .r = PSX_REG_S0, .v = 0 },
      { .r = PSX_REG_S1, .v = 2 },
      { .r = PSX_REG_V0, .v = 2 },
      { .r = PSX_REG_V1, .v = 2 },
      { .r = PSX_REG_S2, .v = 6 },
      { .r = PSX_REG_S3, .v = 0 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_or(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 6),
      LI(PSX_REG_T1, 3),
      LI(PSX_REG_T2, 0xffffffff),
      LI(PSX_REG_T3, 0),

      OR(PSX_REG_S0, PSX_REG_R0, PSX_REG_T2),
      OR(PSX_REG_S1, PSX_REG_T0, PSX_REG_T1),
      OR(PSX_REG_V0, PSX_REG_T0, PSX_REG_T1),
      OR(PSX_REG_V1, PSX_REG_T0, PSX_REG_V0),
      OR(PSX_REG_S2, PSX_REG_T0, PSX_REG_T2),
      OR(PSX_REG_S3, PSX_REG_T0, PSX_REG_T3),
      OR(PSX_REG_T0, PSX_REG_T0, PSX_REG_T0),
      OR(PSX_REG_T1, PSX_REG_T2, PSX_REG_T1),
      OR(PSX_REG_T1, PSX_REG_T1, PSX_REG_T2),
      OR(PSX_REG_T2, PSX_REG_T2, PSX_REG_T1),
      OR(PSX_REG_T0, PSX_REG_T0, PSX_REG_V0),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 7 },
      { .r = PSX_REG_T1, .v = 0xffffffff },
      { .r = PSX_REG_T2, .v = 0xffffffff },
      { .r = PSX_REG_T3, .v = 0 },
      { .r = PSX_REG_S0, .v = 0xffffffff },
      { .r = PSX_REG_S1, .v = 7 },
      { .r = PSX_REG_V0, .v = 7 },
      { .r = PSX_REG_V1, .v = 7 },
      { .r = PSX_REG_S2, .v = 0xffffffff },
      { .r = PSX_REG_S3, .v = 6 },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

static int test_hi_lo(struct dynarec_state *state) {
   union mips_instruction code[] = {
      LI(PSX_REG_T0, 6),
      LI(PSX_REG_T1, 0xffffffff),

      MTHI(PSX_REG_T0),
      MTLO(PSX_REG_T1),
      MFHI(PSX_REG_V0),
      MFLO(PSX_REG_V1),
      MFLO(PSX_REG_V1),
      MTHI(PSX_REG_R0),
      MFLO(PSX_REG_R0),
      MFHI(PSX_REG_S0),
      MFLO(PSX_REG_S1),
      MTHI(PSX_REG_T0),

      BREAK(0xdead),
   };
   struct reg_val expected[] = {
      { .r = PSX_REG_T0, .v = 6 },
      { .r = PSX_REG_T1, .v = 0xffffffff },
      { .r = PSX_REG_V0, .v = 6 },
      { .r = PSX_REG_V1, .v = 0xffffffff },
      { .r = PSX_REG_S0, .v = 0 },
      { .r = PSX_REG_S1, .v = 0xffffffff },
      { .r = PSX_REG_HI, .v = 6 },
      { .r = PSX_REG_LO, .v = 0xffffffff },
   };
   uint32_t ret;

   load_code(state, code, ARRAY_SIZE(code), 0);

   ret = dynarec_run(state, 0x1000);

   TEST_EQ(ret >> 28, DYNAREC_EXIT_BREAK);
   TEST_EQ(ret & 0xfffffff, 0xdead);

   return check_regs(state, expected, ARRAY_SIZE(expected));
}

int main() {
   unsigned ntests = 0;
   unsigned nsuccess = 0;

#define RUN_TEST(_t) do {                       \
      ntests++;                                 \
      nsuccess += (run_test(#_t, _t) == 0);     \
   } while (0)

   RUN_TEST(test_break);
   RUN_TEST(test_nop);
   RUN_TEST(test_lui);
   RUN_TEST(test_ori);
   RUN_TEST(test_li);
   RUN_TEST(test_r0);
   RUN_TEST(test_sll);
   RUN_TEST(test_srl);
   RUN_TEST(test_sra);
   RUN_TEST(test_addu);
   RUN_TEST(test_add_no_exception);
   RUN_TEST(test_subu);
   RUN_TEST(test_and);
   RUN_TEST(test_or);
   RUN_TEST(test_hi_lo);

   printf("Tests done, results: %u/%u\n", nsuccess, ntests);
}

/****************************
 * Dummy emulator callbacks *
 ****************************/

/* Callback used by the dynarec to handle writes to "miscelanous" COP0
   registers (i.e. not SR nor CAUSE) */
void dynarec_set_cop0_misc(struct dynarec_state *s,
                                      uint32_t val,
                                      uint32_t cop0_reg) {
   printf("dynarec cop0 %08x @ %d\n", val, cop0_reg);
}

/* Callbacks used by the dynarec to handle device memory access */
int32_t dynarec_callback_sw(struct dynarec_state *s,
                                       uint32_t val,
                                       uint32_t addr,
                                       int32_t counter) {

   printf("dynarec sw %08x @ %08x (%d)\n", val, addr, counter);

   return 0;
}

int32_t dynarec_callback_sh(struct dynarec_state *s,
                                       uint32_t val,
                                       uint32_t addr,
                                       int32_t counter) {
   printf("dynarec sh %08x @ %08x (%d)\n", val, addr, counter);
   return 0;
}

int32_t dynarec_callback_sb(struct dynarec_state *s,
                                       uint32_t val,
                                       uint32_t addr,
                                       int32_t counter) {
   printf("dynarec sb %08x @ %08x (%d)\n", val, addr, counter);

   return 0;
}
