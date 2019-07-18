/* offsetof(struct dynarec_state, pc) */
.EQU PC_REG_OFFSET,    32

/* offsetof(struct dynarec_state, regs) */
.EQU STATE_REG_OFFSET, 0x40

.EQU AT_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 0)
.EQU V0_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 1)
.EQU V1_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 2)
.EQU A0_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 3)
.EQU A1_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 4)
.EQU T0_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 7)
.EQU SP_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 28)
.EQU RA_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 30)
.EQU DT_REG_OFFSET,    (STATE_REG_OFFSET + 4 * 31)

/* offsetof(struct dynarec_state, cause) */
.EQU STATE_CAUSE_OFFSET, 0xc8

/* offsetof(struct dynarec_state, sr) */
.EQU STATE_SR_OFFSET, 0xcc

.EQU DYNAREC_CACHE_FLUSH, 1

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
        mov     DT_REG_OFFSET(%rdi), %ebx

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
        mov     %ebx,  DT_REG_OFFSET(%rdi)

        /* Move PC into dynarec_state */
        mov     %edx,  PC_REG_OFFSET(%rdi)

        /* Move counter to high 32bits of %rax */
        shl     $32, %rcx
        or      %rcx, %rax

        /* Pop the preserved registers */
        pop     %r12
        pop     %r13
        pop     %r14
        pop     %r15
        pop     %rbx

        pop     %rbp
        ret

/* Macro to call a C function from recompiler context. Banks all
 * registers that are not preserved by the calling convention, calls
 * the function and restores them.
 *
 * /!\ Don't forget to spill the counter in %rcx if you need to preserve it
 */
.macro c_call _func
        /* Bank the registers not preserved by function calls */
        mov     %r8d,  AT_REG_OFFSET(%rdi)
        mov     %r9d,  V0_REG_OFFSET(%rdi)
        mov     %r10d, V1_REG_OFFSET(%rdi)
        mov     %r11d, A0_REG_OFFSET(%rdi)

	/* Preserve dynarec_state pointer */
        push    %rdi

        /* Call C function */
        call    \_func

        /* Restore dynarec_state pointer */
        pop     %rdi

        /* Reload registers */
        mov     AT_REG_OFFSET(%rdi), %r8d
        mov     V0_REG_OFFSET(%rdi), %r9d
        mov     V1_REG_OFFSET(%rdi), %r10d
        mov     A0_REG_OFFSET(%rdi), %r11d
.endm

.global dynabi_device_sw
.type   dynabi_device_sw, function
/* Called by the dynarec code when a SW instruction targets device
 * memory */
dynabi_device_sw:

        c_call dynarec_callback_sw

        /* Move return value to the counter */
        mov     %eax, %ecx

        ret

.global dynabi_device_sh
.type   dynabi_device_sh, function
/* Called by the dynarec code when a SH instruction targets device
 * memory */
dynabi_device_sh:
        c_call dynarec_callback_sh

        /* Move return value to the counter */
        mov     %eax, %ecx

        ret

.global dynabi_device_sb
.type   dynabi_device_sb, function
/* Called by the dynarec code when a SB instruction targets device
 * memory */
dynabi_device_sb:
        c_call dynarec_callback_sb

        /* Move return value to the counter */
        mov     %eax, %ecx

        ret

.global dynabi_device_lb
.type   dynabi_device_lb, function
/* Called by the dynarec code when a LB instruction targets device
 * memory. %edx contains the target address. */
dynabi_device_lb:
        /* Move target address to arg 1 */
        mov %edx, %esi
        /* Move counter to arg 2 */
        mov %ecx, %edx

        c_call dynarec_callback_lb

        /* Move first return value to the counter */
        mov %eax, %ecx

        /* Value is returned as high 8bits of %rax. Shift all the way
	to the left then use an arithmetic shift right to sign-extend */
        shl $24, %rax
        sar $56, %rax

        ret

.global dynabi_device_lbu
.type   dynabi_device_lbu, function
/* Same as LB without sign extension */
dynabi_device_lbu:
        /* Move target address to arg 1 */
        mov %edx, %esi
        /* Move counter to arg 2 */
        mov %ecx, %edx

        c_call dynarec_callback_lb

        /* Move first return value to the counter */
        mov %eax, %ecx

        /* Value is returned as high 8bits of %rax. */
        shr $32, %rax
        movzbl %al, %eax

        ret

.global dynabi_device_lh
.type   dynabi_device_lh, function
/* Same as LB without sign extension */
dynabi_device_lh:
        /* Move target address to arg 1 */
        mov %edx, %esi
        /* Move counter to arg 2 */
        mov %ecx, %edx

        c_call dynarec_callback_lh

        /* Move first return value to the counter */
        mov %eax, %ecx

        /* Value is returned as high 16bits of %rax. Shift all the way
	to the left then use an arithmetic shift right to sign-extend */
        shl $16, %rax
        sar $48, %rax

        ret

.global dynabi_device_lhu
.type   dynabi_device_lhu, function
/* Same as LB without sign extension */
dynabi_device_lhu:
        /* Move target address to arg 1 */
        mov %edx, %esi
        /* Move counter to arg 2 */
        mov %ecx, %edx

        c_call dynarec_callback_lh

        /* Move first return value to the counter */
        mov %eax, %ecx

        /* Value is returned as high 16bits of %rax. */
        shr $32, %rax
        movzwl %ax, %eax

        ret

.global dynabi_device_lw
.type   dynabi_device_lw, function
/* Called by the dynarec code when a LW instruction targets device
 * memory */
dynabi_device_lw:
        /* Move target address to arg 1 */
        mov %edx, %esi
        /* Move counter to arg 2 */
        mov %ecx, %edx

        c_call dynarec_callback_lw

        /* Move first return value to the counter */
        mov %eax, %ecx

        /* Value is returned as high 32bits of %rax. */
        shr $32, %rax

        ret

.global dynabi_exception
.type   dynabi_exception, function
/* Called by the dynarec code when an exception must be
 * generated. Exception number is in %rsi, exception PC in %rdx. */
dynabi_exception:
        /* TODO */
        int $3

        ret /* useless return so debugger can still point to the correct function */

.global dynabi_rfe
.type   dynabi_rfe, function
/* Return From Exception */
dynabi_rfe:
        /* Load current value of the SR into %eax*/
        mov  STATE_SR_OFFSET(%rdi), %eax
        mov  %eax, %edx

        /* Clear the first two entries in the mode "stack" */
        and  $0xfffffff0, %eax
        /* Shift the top two entries to the right and or it back,
	effectively poping the first entry of the stack. The third
	entry is duplicated in 2nd position and isn't cleared. */
        shr  $2, %edx
        and  $0xf, %edx
        or   %edx, %eax

        /* Store SR back into state */
        mov  %eax, STATE_SR_OFFSET(%rdi)
        ret

.global dynabi_set_cop0_sr
.type   dynabi_set_cop0_sr, function
/* Called by the dynarec code when storing the value of the SR
 * register. The value is in %esi.
 */
dynabi_set_cop0_sr:
        /* Mask bits that are always set to zero*/
        and $0xf27fff3f, %esi

        /* Load current value of the SR into %edx */
        mov STATE_SR_OFFSET(%rdi), %edx
        /* Store new value of SR in state struct */
        mov %esi, STATE_SR_OFFSET(%rdi)

        /* Check if the value of the cache isolation bit changed */
        xor %esi, %edx
        and $0x10000, %edx
        /* Jump if no change */
        jz 1f

        /* The cache isolation bit changed, call the emulator code to
         * take care of it. We pass the cache isolation bit as 2nd
         * argument. */
        push %rsi
        and $0x10000, %esi
        shr $16, %esi

        /* Push counter */
        push %rcx

        c_call dynarec_set_cache_isolation

        pop %rcx
        pop %rsi

        mov %esi, %edx
        and $0x10000, %esi
        /* If cache isolation is set we continue normally */
        jnz 1f

        /* If we reach this part it means that cache isolation was
        just disabled. It probably means that the game or BIOS was
        just done clearing the cache, which in turn probably means
        that new code has been loaded. It's probably a good place to
        clean the dynarec cache. Return DYNAREC_CACHE_FLUSH to tell
        the calling code to stop running and flush the cache */
        mov $DYNAREC_CACHE_FLUSH, %eax

        ret

1:
        /* XXX check for interrupts */

        xor %eax, %eax
        ret

.global dynabi_set_cop0_cause
.type   dynabi_set_cop0_cause, function
/* Called by the dynarec code when storing the value of the CAUSE
 * register. The value is in %esi.*/
dynabi_set_cop0_cause:
        /* Only bits 8 and 9 are writeable and can be used for
	"software" interrupt */
        mov STATE_CAUSE_OFFSET(%rdi), %edx
        and $0x00000300, %esi
        and $0xfffffcff, %edx
        or  %esi, %edx
        mov %edx, STATE_CAUSE_OFFSET(%rdi)

        /* TODO: check for interrupt */
        ret

.global dynabi_set_cop0_misc
.type   dynabi_set_cop0_misc, function
/* Called by the dynarec code when storing the value of a COP0
 * register that's neither SR or CAUSE. The value is in %esi, the
 * register index in %edx */
dynabi_set_cop0_misc:
        /* Push counter */
        push %rcx

        c_call dynarec_set_cop0_misc

        pop %rcx

        ret

.global dynabi_gte_mfc2
.type   dynabi_gte_mfc2, function
/* Called by the dynarec code when running a GTE MFC2.
 * The target index is in %esi, the gte_reg is in %edx, and the instruction in %eax */
dynabi_gte_mfc2:
        /* Push counter */
        push %rcx

        /* Move instruction to arg 3 */
        mov %eax, %ecx

        c_call dynarec_gte_mfc2

        /* Move return value to the target */
        mov     %eax, %esi

        pop %rcx

        ret

.global dynabi_gte_cfc2
.type   dynabi_gte_cfc2, function
/* Called by the dynarec code when running a GTE CFC2.
 * The target index is in %esi, the gte_reg is in %edx, and the instruction in %eax */
dynabi_gte_cfc2:
        /* Push counter */
        push %rcx

        /* Move instruction to arg 3 */
        mov %eax, %ecx

        c_call dynarec_gte_cfc2

        /* Move return value to the target */
        mov     %eax, %esi

        pop %rcx

        ret

.global dynabi_gte_mtc2
.type   dynabi_gte_mtc2, function
/* Called by the dynarec code when running a GTE MTC2.
 * The source is in %esi, the gte_reg is in %edx, and the instruction in %eax */
dynabi_gte_mtc2:
        /* Push counter */
        push %rcx

        /* Move instruction to arg 3 */
        mov %eax, %ecx

        c_call dynarec_gte_mtc2

        pop %rcx

        ret

.global dynabi_gte_ctc2
.type   dynabi_gte_ctc2, function
/* Called by the dynarec code when running a GTE CTC2.
 * The source is in %esi, the gte_reg is in %edx, and the instruction in %eax */
dynabi_gte_ctc2:
        /* Push counter */
        push %rcx

        /* Move instruction to arg 3 */
        mov %eax, %ecx

        c_call dynarec_gte_ctc2

        pop %rcx

        ret

.global dynabi_gte_lwc2
.type   dynabi_gte_lwc2, function
/* Called by the dynarec code when running a GTE LWC2.
 * The instruction is in %esi */
dynabi_gte_lwc2:
        /* Move counter to arg2
        mov %ecx, %edx

        c_call dynarec_gte_lwc2

        /* Move return value to the counter */
        mov     %eax, %ecx

        ret

.global dynabi_gte_swc2
.type   dynabi_gte_swc2, function
/* Called by the dynarec code when running a GTE Op.
 * The instruction is in %esi */
dynabi_gte_swc2:
        /* Move counter to arg2
        mov %ecx, %edx

        c_call dynarec_gte_swc2

        /* Move return value to the counter */
        mov     %eax, %ecx

        ret

.global dynabi_gte_instruction
.type   dynabi_gte_instruction, function
/* Called by the dynarec code when running a GTE instruction.
 * The imm25 is in %esi */
dynabi_gte_instruction:
        /* Push counter */
        push %rcx

        c_call dynarec_gte_instruction

        pop %rcx

        ret
