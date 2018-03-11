/* AMD64 specific declarations */

#ifndef __DYNAREC_AMD64_H__
#define __DYNAREC_AMD64_H__

/* Maximum length of a recompiled instruction in bytes. */
#define DYNAREC_INSTRUCTION_MAX_LEN  121U

/* Helper assembly functions. They use a custom ABI and are not meant
 * to be called directly from C code */
extern void dynabi_exception(void);
extern void dynabi_device_sw(void);
extern void dynabi_device_sh(void);
extern void dynabi_device_lw(void);
extern void dynabi_set_cop0_sr(void);
extern void dynabi_set_cop0_cause(void);
extern void dynabi_set_cop0_misc(void);

#endif /* __DYNAREC_AMD64_H__ */
