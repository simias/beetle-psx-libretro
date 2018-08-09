# Emulated vs. native instructions

In this document "native" describes the code that runs natively on
the host controller (an x86 PC, an ARM CPU, etc...). "Emulated"
instructions are the original MIPS instructions that the PSX uses
and are either interpreted or dynamically recompiled in the
emulator.

# Recompilation pages and invalidation

A difficulty in the dynarec architecture is to handle invalidations
properly. If the emulated program writes to a memory location that
we've recompiled it's important that we invalidate the cached dynarec
version so that we do not risk running an outdated version of the
program if the execution flow goes to the modified instructions.

Doing this at the instruction level is rather complicated. A single
recompiled instruction will end up with multiple native
instructions. Depending on how complex the instruction is to emulate
we might need a variable amount of native instruction to implement
different opcodes. Handling of branch delays

To work around these issues invalidation and recompilation are not
handled at the instruction level but rather at a "page" level. A page
is recompiled all at once and invalidated all at once. Since you know
this you can micro-optimize all the code within each page (local loops
etc...)  but you need to be careful about code that crosses page
boundaries:

* You need to check that the target page is valid
* Otherwise you need to trigger the recompiler to compile it
* Then you need to lookup the address of the target instruction and
  jump to it.

When we statically know the address of the target we could "patch it"
directly as long as we're sure that it won't be invalidated. This
would be viable in "lazy invalidate" mode where all the pages are
invalidated at the same time.

# Handling of regions

The PlayStation memory map is divided in multiple regions:

* `0x00000000 - 0x7fffffff`: KUSEG (cached)
* `0x80000000 - 0x9fffffff`: KSEG0 (cached)
* `0xa0000000 - 0xbfffffff`: KSEG1 (not-cached)
* `0xc0000000 - 0xffffffff`: KSEG2 (device-only)

Out of these we don't have to worry too much about KSEG2 because it
only contains a few registers for cache control and other low level
configuration.

KUSEG, KSEG0 and KSEG1 are a bit more tricky however: they contain the
same mappings (except from the scratchpad missing from KSEG1). So for
instance you can access the first byte of RAM at address 0x00000000
(through KUSEG) or at address 0x800000000 (through KSEG0) or even at
address 0xa00000000 (through KSEG1).

The only difference is for code execution: KUSEG and KSEG0 go through
the instruction cache, KSEG1 bypasses it. The data cache is used as
"scratchpad" memory and we can therefore safely ignore it, it's never
used as a "true" cache.

## Region handling in data access

For recompiling data access (LW/SW and friends) we want to get rid of
the region offset to get an "absolute" address to ease range
matching. For instance to handle a RAM access if we don't mask the
region offset we have to compare the address against three different
memory ranges (through KUSEG, KSEG0 and KSEG1). This is clearly
sub-optimal.

Instead I use the `region_mask` look-up table to mask the address and
remove the region bits:

```C
static const uint32_t region_mask[8] = {
   0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, /* KUSEG: 2048MB */
   0x7fffffff,                                     /* KSEG0:  512MB */
   0x1fffffff,                                     /* KSEG1:  512MB */
   0xffffffff, 0xffffffff,                         /* KSEG2: 1024MB */
};

/* Mask "addr" to remove the region bits and return a "canonical"
   address. */
static uint32_t dynarec_mask_address(uint32_t addr) {
   return addr & region_mask[addr >> 29];
}
```

One that's done we can just test one range per device.

## Region handling during execution

That's a bit more tricky. As a first approach I ignore it and just
pretend I run through KUSEG all the time (I mask the PC to remove the
region information). Since the only difference is whether or not the
instruction cache is used (and it's not emulated in the dynarec yet)
it probably doesn't matter much, however if the game does something
"weird" like using the address of the current instruction to compute
an address to KSEG2 then it'll fail. I'm not sure why any game would
do that (but rule #1 of emulation is that if it's possible then you
can be sure some obscure game relies on it). Beyond that I can't
really imagine a situation where that would break but I guess we'll
see in practice.

If we want to actually emulate regions more accurately during
execution (might be necessary if we ever implement the icache) the
simplest solution would be simply to recompile independantly each
region (in the same way that we recompile the BIOS and RAM
independantly at the moment). That would increase RAM usage and make
page lookups slightly more expensive.

# Branch delay slot

## Register hazards

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
these instructions.

# Load delay slots

Load delay slots seem relatively straightforward to implement at first
but they compound poorly with branch delay slots. Consider the
following code:

```asm
lw    ra, 20(sp)
jr    ra
addi  sp, sp, 20
```

# To-do list
## Allow executing out of parport extension

For instance running code from xplorer/gameshark. I also use that to
load "raw" PSX executables for tests in Rustation (I think mednafen
does the same?)

## Fast SP mode

I expect that SP-relative loads and store are targetting the stack in
RAM, we might be able to assume that and remove all the memory mapping
checks from these. Since pushing and popping data from stack is pretty
common it might be worth to add an option for that.

## Lazy invalidate mode

Assume that memory writes don't modify code and assume that if they do
the game/BIOS will flush the icache before attempting to execute
it. This way we don't invalidate for every single write and instead
invalidate everything at once when we detect that a cache flush is in
progress. It will make cache flushes very expensive (everything will
have to be recompiled) but I expect they're probably rare enough that
it might be worth it.

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
