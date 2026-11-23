/* Firmware for fuzz_kernel_load: power the machine off through syscon right away. */
.option norvc
.section .text
.globl _start
_start:
    li    t0, 0x00100000
    li    t1, 0x5555
    sw    t1, 0(t0)
1:  j     1b
