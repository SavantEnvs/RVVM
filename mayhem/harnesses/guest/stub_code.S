/*
 * M-mode bootstrap for fuzzed guest code (fuzz_guest_jit / fuzz_guest_interp).
 * The fuzz payload is appended at RAM + 0x100 and runs in S-mode. Every exception
 * is skipped (mepc += 4); the machine timer interrupt (armed below, guest time)
 * powers the machine off through syscon, so every input finishes on its own.
 */
.option norvc
#if __riscv_xlen == 64
#define LREG ld
#define SREG sd
#else
#define LREG lw
#define SREG sw
#endif
#ifndef GUEST_TICKS
#define GUEST_TICKS 100000
#endif
.section .text
.globl _start
_start:
    la    t0, trap
    csrw  mtvec, t0
    li    t0, 0x0200BFF8       /* ACLINT mtime */
    LREG  t1, 0(t0)
    li    t2, GUEST_TICKS
    add   t1, t1, t2
    li    t0, 0x02004000       /* ACLINT mtimecmp (hart 0) */
#if __riscv_xlen == 32
    li    t2, 0
    sw    t2, 4(t0)
#endif
    SREG  t1, 0(t0)
    li    t0, 0x80             /* mie.MTIE */
    csrw  mie, t0
    li    t0, 0x6800           /* mstatus.MPP = S, FS = dirty */
    csrw  mstatus, t0
    la    t0, payload
    csrw  mepc, t0
    li    t0, 0
    li    t1, 0
    li    t2, 0
    mret
trap:
    csrr  t0, mcause
    bltz  t0, poweroff
    csrr  t0, mepc
    addi  t0, t0, 4
    csrw  mepc, t0
    mret
poweroff:
    li    t0, 0x00100000       /* syscon */
    li    t1, 0x5555
    sw    t1, 0(t0)
1:  j     1b
.balign 256
payload:
