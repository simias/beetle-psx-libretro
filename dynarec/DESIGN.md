# Forewords

In this document "native" describes the code that runs natively on
the host controller (an x86 PC, an ARM CPU, etc...). "Emulated"
instructions are the original MIPS instructions that the PSX uses
and are either interpreted or dynamically recompiled in the
emulator.

All assembly listings assume `.noreorder`, that is the order of the
ASM instructions is the order of the resulting machine code, the
assembler doesn't shuffle code around.

# Overview of the recompilation process

The recompiler works by recompiling "blocks" of instructions which is
a contiguous block of emulated instructions. Simply put it goes like
this:

* The emulator wants to run emulated code at a given address

For instance when the console start the BIOS starts running at
`0xbfc00000`

* The dynarec looks up into its cache of blocks, looking for a block
  starting at the given address

Blocks are *always* executed from the start. For instance if the
emulator wants to run code at address `0x104` and the dynarec has a
block starting at address `0x100` and containing the instructions up
to `0x110` in its cache it won't be used and a new block will be
compiled. That means that the same emulated instruction can be
recompiled in several overlapping blocks.

This might seem counterintuitive and sub-optimal but opting for
non-overlapping blocks severely limits how much you can optimize the
recompiled native code since you can never be sure where the control
is going to jump. In the example above if you decided to re-use your
block at address `0x100` to run code at address `0x104` it means that
you'll have to jump over the beginning of the block and start
somewhere in the middle of the block code. It means that the
recompiled code needs to be designed to allow this, it means that you
can't easily reorder or fold instructions etc... It ends up being very
limiting. If you look back in the git history of this dynarec you'll
see that it started that way and I ended up switching to overlapping
blocks because it was just too frustrating to work with.

* If no block starting at the given address is found a new block is
  recompiled

The recompiler starts at the given address and generates native code
until it either ends up at an unconditional branch (J, JAL, B, etc...)
or a block size limit is reached
(`DYNAREC_MAX_BLOCK_INSTRUCTIONS`). Unconditional branches necessarily
end a block prematurely because they *always* lead to the execution of
a different block (or potentially re-run the current block if it jumps
to the start). Therefore it's useless to continue recompiling after
them since that code in unreachable directly from this block.

If no unconditional branch is encountered after
`DYNAREC_MAX_BLOCK_INSTRUCTIONS` instructions have been recompiled the
block is ended and an artificial jump to the next unrecompiled address
is inserted at the end of the block. Therefore if it's reached it'll
automatically jump to the next block, causing it to be recomplied if
necessary.

* Once the block is found in the cache or recompiled, the dynarec
  jumps into the recompiled block, passing the number of emulated CPU
  cycles to run for as parameter

* The recompiled code runs, potentially calling the dynarec to
  recompile emulated code when necessary until the cycle counter has
  elapsed

I'll describe the cycle counting mechanism later in this document.

* Recompiled code might have to call emulator code to handle certain
  instructions, for instance reading device memory (say, reading
  controller state, writing commands to the GPU or setting a timer).

This is done by calling C callbacks implemented by the emulator. When
these callbacks return the recompiled code keeps running.

* When the cycle counter is below or equal to zero the control is
  returned to the emulator

The emulator can then update the device state, trigger interruptions
etc... Then call the dynarec once again to continue running the game
code.

# Recompilation implementation details

## Branch delay slot

One quick of the MIPS processor used in the PSX is that the
instruction immediately after a branch instruction is always included,
even if the branch is taken.

For instance if you consider the following sequence:

```asm
    j       some_func
    mov     a0, t0
```

The `j` instruction unconditionally jump to `some_func` so you'd expect
the following `mov` not to be executed. However due to branch delay
the `mov` is actually run before the jump takes effect.

When recompiling the code it's therefore important to swap these
instructions around: we run the instruction in the delay slot and only
then we jump to the next block. So in this case we'd recompile the
`mov` before the `j`.

### Register hazards

Consider the following branch and delay slot:

```asm
    bne     t2, t3, 0x250
    addi    t2, t2, 0x80
```

When recompiling this code we need to make sure that the `addi` in the
delay slot gets executed before we actually execute the branch. That
means effectively swapping the order: we run the `addi`, then the
`bne`. Except that in this situation there's a problem: the `addi`
changes `t2` which is also an operand in `bne`. If we blindly swap the
instructions we end up with broken code. Therefore we do something
like that instead:

```asm
   mov      tmp, t2
   addi     t2, t2, 0x80
   bne      tmp, t3, 0x250
```

That is, we use an additional temporary register to hold the
problematic variable. For this reason an additional "fake" PSX
register is added to dynarec, `PSX_REG_DT`, which is used to recompile
these instruction sequences.

## Load delay slots

The PSX MIPS CPU has an other type of delay slots: load delay
slots. Those are for load instructions and work a differently from
branch delays. Load delay slots mean that when loading data from
memory into a register the load only takes effect *after* the next
instruction.

For instance:
```asm
   ; Initialize s0 to 1
   li       s0, 1

   ; Load a value from memory to s0
   lw       s0, <some_memory_location>

   ; Load delay slot
   mov      v0, s0

   mov      v1, s0
```

If you look at the two `mov`s at the end it seems that both `v0` and
`v1` are loaded with the same value of `s0`, however the first `mov`
is the in the load delay slot of the previous `lw`. As such in the
first `mov` the load has not taken effect yet and `s0` still has its
old value of 1. That means that after running this code `v1` will
contain 1 while `v0` and `s0` will both contain whatever value was at
`<some_memory_location>`.

### Branch in load delay

Load delay slots seem relatively straightforward to implement at first
but things can get messy when you have load delays in branch delays or
vice-versa. Consider the following code:

```asm
lw    ra, 20(sp)
jr    ra
addi  sp, sp, 20
```

This is a common sequence to return from a function. It restores the previous
value of RA and returns to the caller. Not however that the code makes use of
the load delay slot: the `jr ra` takes place in the LDS of `lw ra, 20(sp)`, that
means that it will use the _previous_ value of `ra`, not the one being restored
(which is what we want here).

In order to implement this correctly we need to be careful to keep the old value
of `ra` as a target for the `jr` and _then_ execute both the `addi` in the jump
delay slot _and_ update `ra` with the value from the `lw`.

### Load in branch delay

Trickier still is the (fortunately rare) situation where you have a load in a
branch delay slot, for instance:

```asm
jr    ra
lw    s0, 20(sp)
```

What happens here? Let's see:

* First the `jr` is encountered, the CPU prepares to jump to `ra` but...
* ... First the branch delay slot is executed, invoking the `lw`
* The next instruction will effectively be in the `lw`'s load delay slot... But
  what instruction is that?

The answer to that question is that the next instruction will be whatever code
is located at the address pointed to by `ra` since we've jumped over there. So
if the code at `ra` looks like this:

```asm
some_function:
   mov   t0, s0
   mov   t1, s0
```

We end up with the first `mov` being in the load delay slot while the second
will take whatever value was loaded in `s0`.

That means that the first instruction of any block could potentially end up in a
load delay slot if it's called in a sequence like this. Worse, it could be
called in a load delay slot in some cases but not in others. There's no way to
know ahead of time.

Fortunately this particular sequence is of little practical use so it's not very
common in the wild. I doubt for instance that a compiler would generate code
that relied on this behaviour (I'm not even sure if this is actually officially
defined by the MIPS spec, even though it seems to work reliably in practice on
the PSX CPU).

For this reason this particular sequence is not implemented. If we have a load
in a branch delay slot we assume that the target code will never attempt to
reference the loaded register in the first instruction. If this ever turns out
to be a problem we'll have to be more clever about it by keeping track of load
delays across block jumps.

# Debugger support

## DYNAREC_JIT_DEBUGGER

In order to help debugging the generated code the dynarec has limited
support for GDB JIT integration. In order to enable it you much define
`DYNAREC_JIT_DEBUGGER` and add `dynarec-jit-debugger.c` to the list of
compiled files. Check the `Makefile` for an example.

When JIT integration is enabled the recompiler will register every
compiled block with GDB giving it a name of the form
`block_0x<start_address>` where `start_address` is the address of the
beginning of the block in PSX memory.

For instance the first recompiled block of the BIOS will be named
`block_0xbfc00000`. You can use this information to make sense of
backtrace or add breakpoints on certain blocks.

## DYNAREC_NO_PATCH

When this symbol is defined blocks are never linked directly to each
other, jumps always go through `dynarec_recompile_and_patch`. This is
useful if one wants to easily see the block call sequence.

# To-do list
## Allow executing out of parport extension

For instance running code from xplorer/gameshark. I also use that to
load "raw" PSX executables for tests in Rustation (I think mednafen
does the same?)

## Fast SP mode

I expect that SP-relative loads and store are targeting the stack in
RAM, we might be able to assume that and remove all the memory mapping
checks from these. Since pushing and popping data from stack is pretty
common it might be worth to add an option for that.

## No-alignment check mode

I expect that alignment exception for PC and memory accesses are
extremely rare in practice (I doubt most games would even be able to
recover from that). We might optionally decide to disable these
checks.

## No Scratchpad in KSEG1

The current code treats KUSEG, KSEG0 and KSEG1 identically, however
the scratchpad is not supposed to be accessible through KSEG1. I doubt
this matters for most games but accuracy's sake we might add a test
for it.

## Reload SR and CAUSE from CP0.Regs[rd] when necessary

## Optimize block tree structure

At the moment I'm using a simple red-black tree to store and retrieve block
data. I think this could be massively improved but it requires more
benchmarking:

### Some blocks are probably a lot "hotter" than others

There are probably some code in a very hot path that are used a lot more than
others. Depending on how they get linked together they may end up being
looked-up very often in the tree. It could be good to monitor this and bump
these blocks towards the root of the tree for faster retrieval.

### Block garbage collection

Using a similar approach as above we could monitor which blocks are unused and
drop them. We need to be careful not to drop linked blocks however, so we'll
have to keep track of block references and use that in our algorithm.
