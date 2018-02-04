/* AMD64 specific declarations */

#ifndef __DYNAREC_AMD64_H__
#define __DYNAREC_AMD64_H__

/* Maximum length of a recompiled instruction in bytes. */
#define DYNAREC_INSTRUCTION_MAX_LEN  118U

/* Average length of an instruction. Used while guessing how much
   space must be allocated for a page to try and avoid
   reallocations */
#define DYNAREC_INSTRUCTION_AVG_LEN  10U

#endif /* __DYNAREC_AMD64_H__ */
