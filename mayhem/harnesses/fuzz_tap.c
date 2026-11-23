/*
 * fuzz_tap: guest Ethernet frames into RVVM's userland NAT network stack
 * (src/devices/tap_user.c: Ethernet, ARP, IPv4/IPv6, ICMP, DHCP, UDP, TCP).
 * This is the path a guest NIC (rtl8169, eth-oc) uses for every frame the guest
 * transmits: tap_send().
 *
 * Input: a sequence of frames, each prefixed with a 16-bit little-endian length
 * (truncated to what is left and to TAP_FRAME_SIZE). A fresh TAP interface is
 * opened and closed per input, so no flow state is kept across inputs.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "devices/tap_api.h"

static bool feed_rx(void* net_dev, const void* data, size_t size)
{
    (void)net_dev;
    (void)data;
    (void)size;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    tap_dev_t* tap = tap_open();
    if (!tap) {
        return 0;
    }
    tap_net_dev_t nic = { .net_dev = NULL, .feed_rx = feed_rx };
    tap_attach(tap, &nic);

    while (size >= 2) {
        size_t len = (size_t)data[0] | ((size_t)data[1] << 8);
        data += 2;
        size -= 2;
        if (len > size) {
            len = size;
        }
        if (len > TAP_FRAME_SIZE) {
            len = TAP_FRAME_SIZE;
        }
        /* Exact-size heap copy, so ASan sees any read past the frame. */
        uint8_t* frame = malloc(len ? len : 1);
        memcpy(frame, data, len);
        tap_send(tap, frame, len);
        free(frame);
        data += len;
        size -= len;
    }
    tap_close(tap);
    return 0;
}
