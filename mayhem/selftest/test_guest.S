/*
 * Known-answer guest program for mayhem/selftest/rvvm_selftest.c. Runs in M-mode
 * from RAM start, stores its results at RAM + 0x1000 (one XLEN word each) and
 * powers the machine off through syscon. The hot loop makes RVJIT compile the
 * division/remainder overflow cases.
 */
.option norvc
#if __riscv_xlen == 64
#define SREG sd
#define W 8
#define XMIN 0x8000000000000000
#else
#define SREG sw
#define W 4
#define XMIN 0x80000000
#endif
.section .text
.globl _start
_start:
    li    s0, 0x80001000
    li    a0, 7
    li    a1, 6
    mul   a2, a0, a1
    SREG  a2, 0*W(s0)          /* 42 */
    li    a0, XMIN
    li    a1, -1
    div   a2, a0, a1
    SREG  a2, 1*W(s0)          /* XMIN (overflow) */
    rem   a2, a0, a1
    SREG  a2, 2*W(s0)          /* 0 */
    li    a3, 0
    div   a2, a0, a3
    SREG  a2, 3*W(s0)          /* -1 (division by zero) */
    li    t0, 0
    li    t1, 1000
1:  add   t0, t0, t1
    addi  t1, t1, -1
    bnez  t1, 1b
    SREG  t0, 4*W(s0)          /* 500500 */
    li    t1, 200000
    li    t2, 0
    li    t3, 0
2:  div   a2, a0, a1
    rem   a4, a0, a1
    add   t2, t2, a4
    xor   t3, t3, a2
    xor   t3, t3, a0
    addi  t1, t1, -1
    bnez  t1, 2b
    SREG  a2, 5*W(s0)          /* XMIN, computed in the hot (JIT) loop */
    SREG  t2, 6*W(s0)          /* 0 */
    SREG  t3, 7*W(s0)          /* 0 */
#if __riscv_xlen == 64
    li    a0, 0xFFFFFFFF80000000
    divw  a2, a0, a1
    SREG  a2, 8*W(s0)          /* 0xFFFFFFFF80000000 */
#else
    li    a2, 0x12345678
    SREG  a2, 8*W(s0)
#endif
    li    t0, 0x00100000
    li    t1, 0x5555
    sw    t1, 0(t0)
3:  j     3b
