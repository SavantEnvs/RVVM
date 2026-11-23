/*
 * Fuzz harnesses that drive a whole RVVM machine through its public API
 * (the headers in include/rvvm). One source, one mode per binary (-DMODE_*):
 *
 *  MODE_CODE      fuzz_guest_jit / fuzz_guest_interp: the input is guest code.
 *                 Byte 0 bit 0 picks RV32 (1) or RV64 (0); the rest is loaded as
 *                 firmware behind an M-mode stub (guest/stub_code.S) and runs in
 *                 S-mode on the interpreter (USE_JIT_OPT=0) or RVJIT (=1) until
 *                 the guest timer powers the machine off.
 *  MODE_KERNEL    fuzz_kernel_load: the input is the kernel image
 *                 (rvvm_load_kernel), parsed by the ELF loader on machine start.
 *  MODE_NVME      fuzz_nvme: riscv virt board + NVMe; the input is a list of
 *                 16-byte MMIO/RAM access records replayed by guest/stub_mmio.S.
 *  MODE_DEVICES   fuzz_mmio_devices: as MODE_NVME, plus every other guest-facing
 *                 device the CLI can attach: ATA, RTL8169 and OpenCores Ethernet
 *                 (each on its own userland TAP stack), the HDA sound controller
 *                 (PCI, silent backend), the default NS16550A UART at 0x10000000
 *                 (on a dummy chardev that always has RX data) and the default
 *                 OpenCores I2C controller at 0x10030000 with i2c-hid keyboard and
 *                 mouse behind it.
 *  MODE_SNAPSHOT  fuzz_snapshot: the input is a machine snapshot, loaded with
 *                 rvvm_machine_snapshot(machine, blk, false) into a paused 64 KiB
 *                 RV64 virt machine with an NVMe drive (machine, hart, device state).
 *
 * State: the machine, firmware file and disk images are recreated per input; the
 * only thing kept across inputs is the scratch file paths in $TMPDIR.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <rvvm/rvvm.h>
#include <rvvm/rvvm_blk.h>
#include <rvvm/rvvm_board.h>
#include <rvvm/rvvm_pci.h>

#include "guest_stubs.h"

#if defined(MODE_DEVICES)
#include "devices/chardev.h"
#include "devices/eth-oc.h"
#include "devices/hid_api.h"
#include "devices/i2c-oc.h"
#include "devices/ns16550a.h"
#include "devices/rtl8169.h"
#include "devices/sound-hda.h"
#include "devices/tap_api.h"

/* UART backend: always readable (RX byte 0x55) and writable, output dropped. */
static uint32_t dummy_poll(chardev_t* dev)
{
    (void)dev;
    return CHARDEV_RX | CHARDEV_TX;
}

static size_t dummy_read(chardev_t* dev, void* buf, size_t nbytes)
{
    (void)dev;
    memset(buf, 0x55, nbytes);
    return nbytes;
}

static size_t dummy_write(chardev_t* dev, const void* buf, size_t nbytes)
{
    (void)dev;
    (void)buf;
    return nbytes;
}

static void dummy_remove(chardev_t* dev)
{
    free(dev);
}

static chardev_t* dummy_chardev(void)
{
    chardev_t* dev = calloc(1, sizeof(chardev_t));
    if (!dev) {
        abort();
    }
    dev->poll   = dummy_poll;
    dev->read   = dummy_read;
    dev->write  = dummy_write;
    dev->remove = dummy_remove;
    return dev;
}
#endif

#define RAM_SIZE (16u << 20)
#define MAX_INPUT (1u << 20)

static char fw_path[512];
static char aux_path[512];
static char disk1_path[512];
static char disk2_path[512];

static void make_tmp(char* buf, size_t len, const char* tag)
{
    const char* dir = getenv("TMPDIR");
    if (!dir || !*dir) {
        dir = "/tmp";
    }
    snprintf(buf, len, "%s/rvvm_fuzz_%s_XXXXXX", dir, tag);
    int fd = mkstemp(buf);
    if (fd < 0) {
        perror("mkstemp");
        abort();
    }
    close(fd);
}

static void write_file(const char* path, const void* a, size_t alen, const void* b, size_t blen)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        abort();
    }
    if ((alen && fwrite(a, 1, alen, f) != alen) || (blen && fwrite(b, 1, blen, f) != blen)) {
        abort();
    }
    fclose(f);
}

static void blank_disk(const char* path)
{
    int fd = open(path, O_RDWR | O_TRUNC);
    if (fd < 0 || ftruncate(fd, 1u << 20) != 0) {
        abort();
    }
    close(fd);
}

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc;
    (void)argv;
    make_tmp(fw_path, sizeof(fw_path), "fw");
    make_tmp(aux_path, sizeof(aux_path), "aux");
    make_tmp(disk1_path, sizeof(disk1_path), "disk1");
    make_tmp(disk2_path, sizeof(disk2_path), "disk2");
    return 0;
}

static void run_machine(rvvm_machine_t* machine)
{
    if (rvvm_start_machine(machine)) {
        /* Returns once the guest powers the machine off (syscon). */
        rvvm_run_eventloop();
    }
    rvvm_free_machine(machine);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > MAX_INPUT) {
        /* Firmware and kernel images must fit in guest RAM (rvvm_load_firmware). */
        return -1;
    }
#if defined(MODE_CODE)
    if (size < 1) {
        return -1;
    }
    bool rv32 = data[0] & 1;
    rvvm_machine_t* machine = rvvm_create_machine(RAM_SIZE, 1, rv32 ? "rv32" : "rv64");
    if (!machine) {
        return 0;
    }
    rvvm_set_opt(machine, RVVM_OPT_JIT, USE_JIT_OPT);
    rvvm_board_riscv_virt_init(machine, false);
    if (rv32) {
        write_file(fw_path, stub_code32, sizeof(stub_code32), data + 1, size - 1);
    } else {
        write_file(fw_path, stub_code64, sizeof(stub_code64), data + 1, size - 1);
    }
    rvvm_load_firmware(machine, fw_path);
    run_machine(machine);
#elif defined(MODE_KERNEL)
    rvvm_machine_t* machine = rvvm_create_machine(RAM_SIZE, 1, "rv64");
    if (!machine) {
        return 0;
    }
    rvvm_board_riscv_virt_init(machine, false);
    write_file(fw_path, stub_off64, sizeof(stub_off64), NULL, 0);
    write_file(aux_path, data, size, NULL, 0);
    rvvm_load_firmware(machine, fw_path);
    rvvm_load_kernel(machine, aux_path);
    run_machine(machine);
#elif defined(MODE_NVME) || defined(MODE_DEVICES)
    uint64_t count = size / 16;
    rvvm_machine_t* machine = rvvm_create_machine(RAM_SIZE, 1, "rv64");
    if (!machine) {
        return 0;
    }
    rvvm_board_riscv_virt_init(machine, false);
    blank_disk(disk1_path);
    rvvm_nvme_init(machine, rvvm_blk_open(disk1_path, NULL, RVVM_BLK_RW), RVVM_PCI_ADDR_ANY);
#if defined(MODE_DEVICES)
    blank_disk(disk2_path);
    rvvm_ata_init(machine, rvvm_blk_open(disk2_path, NULL, RVVM_BLK_RW), RVVM_PCI_ADDR_ANY);
    tap_dev_t* tap = tap_open();
    if (tap) {
        rvvm_rtl8169_init(machine, tap, RVVM_PCI_ADDR_ANY);
    }
    tap_dev_t* tap2 = tap_open();
    if (tap2) {
        rvvm_intc_t* intc = rvvm_get_intc(machine);
        ethoc_init(machine, tap2, ETH_OC_ADDR_DEFAULT, intc, rvvm_alloc_irq(intc));
    }
    sound_hda_init(machine);
    /* The CLI defaults (src/main.c): I2C controller (+ i2c-hid input) and the UART. */
    i2c_oc_init_auto(machine);
    hid_keyboard_init_auto(machine);
    hid_mouse_init_auto(machine);
    ns16550a_init_auto(machine, dummy_chardev());
#endif
    /* stub_mmio.S reads the record count at RAM + 0xF8, the records at RAM + 0x100. */
    uint8_t fw[0x100] = {0};
    _Static_assert(sizeof(stub_mmio64) <= 0xF8, "stub_mmio.S overlaps the record count");
    memcpy(fw, stub_mmio64, sizeof(stub_mmio64));
    memcpy(fw + 0xF8, &count, sizeof(count));
    write_file(fw_path, fw, sizeof(fw), data, count * 16);
    rvvm_load_firmware(machine, fw_path);
    run_machine(machine);
#elif defined(MODE_SNAPSHOT)
    rvvm_machine_t* machine = rvvm_create_machine(64u << 10, 1, "rv64");
    if (!machine) {
        return 0;
    }
    rvvm_board_riscv_virt_init(machine, false);
    blank_disk(disk1_path);
    rvvm_nvme_init(machine, rvvm_blk_open(disk1_path, NULL, RVVM_BLK_RW), RVVM_PCI_ADDR_ANY);
    write_file(aux_path, data, size, NULL, 0);
    rvvm_blk_dev_t* blk = rvvm_blk_open(aux_path, NULL, RVVM_BLK_READ);
    if (blk) {
        rvvm_machine_snapshot(machine, blk, false);
        rvvm_blk_close(blk);
    }
    rvvm_free_machine(machine);
#else
#error "no MODE_* defined"
#endif
    return 0;
}
