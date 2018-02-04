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

.text

.global dynarec_execute
.type   dynarec_execute, function

dynarec_execute:
        /* Be a good function and create a stack frame */
        push    %rbp
        mov     %rsp, %rbp

        /* Push registers that the dynarec'd code is going to modify
	 * but must be saved per the calling convention */
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

        /* Call dynarec'd code */
        call    *%rsi

        /* Store the "cached" PSX registers back into the struct
         * Dynarec'd code always keeps the dynarec_state pointer in
	 * %rdi, so we don't have to worry about preserving it here */
        mov     %r8d,  AT_REG_OFFSET(%rdi)
        mov     %r9d,  V0_REG_OFFSET(%rdi)
        mov     %r10d, V1_REG_OFFSET(%rdi)
        mov     %r11d, A0_REG_OFFSET(%rdi)
        mov     %r12d, A1_REG_OFFSET(%rdi)
        mov     %r13d, T0_REG_OFFSET(%rdi)
        mov     %r14d, SP_REG_OFFSET(%rdi)
        mov     %r15d, RA_REG_OFFSET(%rdi)

        /* Pop the preserved registers */
        pop     %r12
        pop     %r13
        pop     %r14
        pop     %r15

        pop     %rbp
        ret
