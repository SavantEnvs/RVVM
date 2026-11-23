/*
 * M-mode replay stub for fuzz_nvme / fuzz_mmio_devices (RV64).
 * RAM + 0xF8 holds the record count, RAM + 0x100 the records (16 bytes each):
 *   byte 0: op (bits 0-2: 0..3 = store 8/16/32/64, 4..7 = load 8/16/32/64)
 *   bytes 4..7: physical address (u32, little endian)
 *   bytes 8..15: value to store
 * Faulting accesses are skipped (mepc += 4). When the records run out, or the
 * machine timer fires (guest time), the machine powers off through syscon.
 */
.option norvc
#ifndef GUEST_TICKS
#define GUEST_TICKS 500000
#endif
.section .text
.globl _start
_start:
    la    t0, trap
    csrw  mtvec, t0
    li    t0, 0x0200BFF8
    ld    t1, 0(t0)
    li    t2, GUEST_TICKS
    add   t1, t1, t2
    li    t0, 0x02004000
    sd    t1, 0(t0)
    li    t0, 0x80
    csrw  mie, t0
    csrsi mstatus, 8           /* mstatus.MIE */
    li    t0, 0x800000F8       /* record count, filled in by the harness */
    ld    s1, 0(t0)
    li    s0, 0x80000100       /* records */
loop:
    beqz  s1, poweroff
    lbu   t0, 0(s0)
    lwu   t1, 4(s0)
    ld    t2, 8(s0)
    addi  s0, s0, 16
    addi  s1, s1, -1
    andi  t0, t0, 7
    slli  t0, t0, 3
    la    t3, table
    add   t3, t3, t0
    jr    t3
table:
    sb    t2, 0(t1)
    j     loop
    sh    t2, 0(t1)
    j     loop
    sw    t2, 0(t1)
    j     loop
    sd    t2, 0(t1)
    j     loop
    lbu   t4, 0(t1)
    j     loop
    lhu   t4, 0(t1)
    j     loop
    lwu   t4, 0(t1)
    j     loop
    ld    t4, 0(t1)
    j     loop
trap:
    csrr  t0, mcause
    bltz  t0, poweroff
    csrr  t0, mepc
    addi  t0, t0, 4
    csrw  mepc, t0
    mret
poweroff:
    li    t0, 0x00100000
    li    t1, 0x5555
    sw    t1, 0(t0)
1:  j     1b
