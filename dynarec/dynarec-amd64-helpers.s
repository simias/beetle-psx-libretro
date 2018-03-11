/* offsetof(struct dynarec_state, regs) */
.EQU STATE_REG_OFFSET, 64

.EQU AT_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 0)
.EQU V0_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 1)
.EQU V1_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 2)
.EQU A0_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 3)
.EQU A1_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 4)
.EQU T0_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 7)
.EQU SP_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 28)
.EQU RA_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 30)
.EQU DT_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 31)

.text

.global dynasm_execute
.type   dynasm_execute, function
/* Switch to dynarec register layout, call the dynarec code then
 * revert to the proper C calling convention. */
dynasm_execute:
        /* Be a good function and create a stack frame */
        push    %rbp
        mov     %rsp, %rbp

        /* Push registers that the dynarec'd code is going to modify
	 * but must be saved per the calling convention */
        push    %rbx
        push    %r15
        push    %r14
        push    %r13
        push    %r12

        /* Load the "cached" PSX registers into the host's */
        mov     AT_REG_OFFSET(%rdi), %r8d
        mov     V0_REG_OFFSET(%rdi), %r9d
        mov     V1_REG_OFFSET(%rdi), %r10d
        mov     A0_REG_OFFSET(%rdi), %r11d
        mov     A1_REG_OFFSET(%rdi), %r12d
        mov     T0_REG_OFFSET(%rdi), %r13d
        mov     SP_REG_OFFSET(%rdi), %r14d
        mov     RA_REG_OFFSET(%rdi), %r15d
        mov     DT_REG_OFFSET(%rdi), %rbx

        /* Move cycles_to_run to %ecx */
        mov     %edx, %ecx

        /* Call dynarec'd code */
        call    *%rsi

        /* Store the "cached" PSX registers back into the state struct.
         * Dynarec'd code always keeps the dynarec_state pointer in
	 * %rdi, so we don't have to worry about preserving it here. */
        mov     %r8d,  AT_REG_OFFSET(%rdi)
        mov     %r9d,  V0_REG_OFFSET(%rdi)
        mov     %r10d, V1_REG_OFFSET(%rdi)
        mov     %r11d, A0_REG_OFFSET(%rdi)
        mov     %r12d, A1_REG_OFFSET(%rdi)
        mov     %r13d, T0_REG_OFFSET(%rdi)
        mov     %r14d, SP_REG_OFFSET(%rdi)
        mov     %r15d, RA_REG_OFFSET(%rdi)
        mov     %rbx,  DT_REG_OFFSET(%rdi)

        /* Pop the preserved registers */
        pop     %r12
        pop     %r13
        pop     %r14
        pop     %r15
        pop     %rbx

        pop     %rbp
        ret

.global dynabi_device_sw
.type   dynabi_device_sw, function
/* Called by the dynarec code when a SW instruction targets device
 * memory */
dynabi_device_sw:
        /* Preserve dynarec_state pointer */
        push    %rdi

        /* Bank the registers not preserved by function calls */
        mov     %r8d,  AT_REG_OFFSET(%rdi)
        mov     %r9d,  V0_REG_OFFSET(%rdi)
        mov     %r10d, V1_REG_OFFSET(%rdi)
        mov     %r11d, A0_REG_OFFSET(%rdi)

        /* Call emulator code */
        call    dynarec_callback_sw

        /* Move return value to the counter */
        mov     %eax, %ecx

        /* Reload registers */
        mov     AT_REG_OFFSET(%rdi), %r8d
        mov     V0_REG_OFFSET(%rdi), %r9d
        mov     V1_REG_OFFSET(%rdi), %r10d
        mov     A0_REG_OFFSET(%rdi), %r11d

        /* Restore dynarec_state pointer */
        pop     %rdi

        ret

.global dynabi_device_sh
.type   dynabi_device_sh, function
/* Called by the dynarec code when a Sh instruction targets device
 * memory */
dynabi_device_sh:
        int $3

.global dynabi_device_lw
.type   dynabi_device_lw, function
/* Called by the dynarec code when a LW instruction targets device
 * memory */
dynabi_device_lw:
        int $3

.global dynabi_exception
.type   dynabi_exception, function
/* Called by the dynarec code when an exception must be
 * generated. Exception number is in %rsi, exception PC in %rdx. */
dynabi_exception:
        /* TODO */
        int $3

.global dynabi_set_cop0_sr
.type   dynabi_set_cop0_sr, function
/* Called by the dynarec code when storing the value of the SR
 * register. The value is in %esi.*/
dynabi_set_cop0_sr:
        /* TODO: check for interrupt, check for cache isolation. */
        int $3

.global dynabi_set_cop0_cause
.type   dynabi_set_cop0_cause, function
/* Called by the dynarec code when storing the value of the CAUSE
 * register. The value is in %esi.*/
dynabi_set_cop0_cause:
        /* TODO: check for interrupt, check for cache isolation. */
        int $3

.global dynabi_set_cop0_misc
.type   dynabi_set_cop0_misc, function
/* Called by the dynarec code when storing the value of a COP0
 * register that's neither SR or CAUSE. The value is in %esi, the
 * register index in %edx */
dynabi_set_cop0_misc:
        /* TODO: check for interrupt, check for cache isolation. */
        int $3
