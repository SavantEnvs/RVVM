/*
 * Known-answer functional tests for librvvm (RVVM ships no test suite of its own).
 * Built with the project's normal flags by mayhem/build.sh, run by mayhem/test.sh.
 * Prints one "PASS <name>" / "FAIL <name>" line per check and a final
 * "SUMMARY passed=<n> failed=<m>" line.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rvvm/rvvm.h>
#include <rvvm/rvvm_board.h>

#include "devices/tap_api.h"
#include "selftest_guest.h"

static int passed, failed;

static void check(const char* name, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) {
        passed++;
    } else {
        failed++;
    }
}

static char tmpl[512];

static const char* write_tmp(const void* data, size_t size)
{
    const char* dir = getenv("TMPDIR");
    snprintf(tmpl, sizeof(tmpl), "%s/rvvm_selftest_XXXXXX", (dir && *dir) ? dir : "/tmp");
    int fd = mkstemp(tmpl);
    if (fd < 0 || write(fd, data, size) != (ssize_t)size) {
        return NULL;
    }
    close(fd);
    return tmpl;
}

static void run_guest(const char* tag, int rv64, int jit)
{
    char name[128];
    const uint8_t* fw = rv64 ? guest64 : guest32;
    size_t fw_size = rv64 ? sizeof(guest64) : sizeof(guest32);
    rvvm_machine_t* m = rvvm_create_machine(16u << 20, 1, rv64 ? "rv64" : "rv32");
    snprintf(name, sizeof(name), "%s.create", tag);
    check(name, m != NULL);
    if (!m) {
        return;
    }
    rvvm_set_opt(m, RVVM_OPT_JIT, jit);
    rvvm_board_riscv_virt_init(m, false);
    const char* path = write_tmp(fw, fw_size);
    snprintf(name, sizeof(name), "%s.load_firmware", tag);
    check(name, path && rvvm_load_firmware(m, path));
    snprintf(name, sizeof(name), "%s.start", tag);
    check(name, rvvm_start_machine(m));
    rvvm_run_eventloop();
    snprintf(name, sizeof(name), "%s.powered_off", tag);
    check(name, !rvvm_machine_powered(m));

    uint64_t res[9] = {0};
    for (int i = 0; i < 9; ++i) {
        if (rv64) {
            rvvm_read_ram(m, &res[i], 0x80001000 + 8 * i, 8);
        } else {
            uint32_t v = 0;
            rvvm_read_ram(m, &v, 0x80001000 + 4 * i, 4);
            res[i] = v;
        }
    }
    uint64_t xmin = rv64 ? 0x8000000000000000ULL : 0x80000000ULL;
    uint64_t m1   = rv64 ? ~0ULL : 0xFFFFFFFFULL;
    static const char* names[9] = {"mul", "div_overflow", "rem_overflow", "div_by_zero", "loop_sum",
                                   "hot_div_overflow", "hot_rem_overflow", "hot_xor", "divw_overflow"};
    uint64_t expect[9] = {42, xmin, 0, m1, 500500, xmin, 0, 0,
                          rv64 ? 0xFFFFFFFF80000000ULL : 0x12345678ULL};
    for (int i = 0; i < 9; ++i) {
        snprintf(name, sizeof(name), "%s.%s", tag, names[i]);
        check(name, res[i] == expect[i]);
        if (res[i] != expect[i]) {
            printf("  got %#llx expected %#llx\n", (unsigned long long)res[i], (unsigned long long)expect[i]);
        }
    }
    rvvm_free_machine(m);
    unlink(tmpl);
}

static uint8_t rx_frame[1600];
static volatile size_t rx_size;

static bool feed_rx(void* net_dev, const void* data, size_t size)
{
    (void)net_dev;
    if (!rx_size && size <= sizeof(rx_frame)) {
        memcpy(rx_frame, data, size);
        rx_size = size;
    }
    return true;
}

static void test_tap_arp(void)
{
    tap_dev_t* tap = tap_open();
    check("tap.open", tap != NULL);
    if (!tap) {
        return;
    }
    tap_net_dev_t nic = { .net_dev = NULL, .feed_rx = feed_rx };
    tap_attach(tap, &nic);
    uint8_t mac[6] = {0};
    tap_get_mac(tap, mac);

    static const uint8_t guest_mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    static const uint8_t guest_ip[4]  = {192, 168, 0, 200};
    static const uint8_t query_ip[4]  = {192, 168, 0, 1};
    uint8_t f[42] = {0};
    memset(f, 0xFF, 6);
    memcpy(f + 6, guest_mac, 6);
    f[12] = 0x08; f[13] = 0x06;             /* ARP */
    f[14] = 0x00; f[15] = 0x01;             /* HTYPE Ethernet */
    f[16] = 0x08; f[17] = 0x00;             /* PTYPE IPv4 */
    f[18] = 6;    f[19] = 4;
    f[20] = 0x00; f[21] = 0x01;             /* request */
    memcpy(f + 22, guest_mac, 6);
    memcpy(f + 28, guest_ip, 4);
    memcpy(f + 38, query_ip, 4);
    check("tap.send_arp", tap_send(tap, f, sizeof(f)));
    for (int i = 0; i < 200 && !rx_size; ++i) {
        usleep(10000);
    }
    check("tap.arp_reply_received", rx_size >= 42);
    check("tap.arp_reply_ethertype", rx_frame[12] == 0x08 && rx_frame[13] == 0x06);
    check("tap.arp_reply_oper", rx_frame[20] == 0 && rx_frame[21] == 2);
    check("tap.arp_reply_sender_ip", memcmp(rx_frame + 28, query_ip, 4) == 0);
    check("tap.arp_reply_sender_mac", memcmp(rx_frame + 22, rx_frame + 6, 6) == 0);
    tap_close(tap);
}

int main(void)
{
    run_guest("rv64.interp", 1, 0);
    run_guest("rv64.jit", 1, 1);
    run_guest("rv32.interp", 0, 0);
    run_guest("rv32.jit", 0, 1);
    test_tap_arp();
    printf("SUMMARY passed=%d failed=%d\n", passed, failed);
    return failed ? 1 : 0;
}
