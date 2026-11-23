/*
 * fuzz_gdbstub: the GDB remote serial protocol server (src/core/gdbstub.c),
 * reached exactly as a debugger reaches it: gdbstub_init() on a fresh machine,
 * then a TCP client on 127.0.0.1 sends the input bytes verbatim (packet
 * framing, '+'/'-'/Ctrl-C, m/M memory, g/G registers, H thread, q queries,
 * c/s continue, R reset).
 *
 * Synchronisation: after the last byte the client half-closes the connection;
 * the server parses everything it received, sees EOF and closes its side, so
 * the client's read() returning 0 means the whole input was handled. A 2 s
 * guard covers a server that never closes. A fresh machine + server per input,
 * so no state is kept across inputs.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <rvvm/rvvm.h>

#include "core/gdbstub.h"

#define RAM_SIZE  (1u << 20)
#define MAX_INPUT (64u << 10)

static uint16_t port_base;

int LLVMFuzzerInitialize(int* argc, char*** argv)
{
    (void)argc;
    (void)argv;
    /* Per-process port, so parallel fuzzer workers do not collide. */
    port_base = (uint16_t)(20000 + (getpid() % 20000));
    return 0;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void talk(uint16_t port, const uint8_t* data, size_t size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    size_t sent = 0;
    bool wr_closed = false;
    int64_t deadline = now_ms() + 2000;
    char sink[4096];
    for (;;) {
        if (!wr_closed && sent == size) {
            shutdown(fd, SHUT_WR);
            wr_closed = true;
        }
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            break;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN | (wr_closed ? 0 : POLLOUT) };
        if (poll(&p, 1, (int)left) <= 0) {
            continue;
        }
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = read(fd, sink, sizeof(sink));
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                break; /* server closed: input fully processed */
            }
        }
        if (!wr_closed && (p.revents & POLLOUT)) {
            ssize_t w = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
            if (w > 0) {
                sent += (size_t)w;
            } else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                break;
            }
        }
    }
    close(fd);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > MAX_INPUT) {
        return -1;
    }
    rvvm_machine_t* machine = rvvm_create_machine(RAM_SIZE, 1, "rv64");
    if (!machine) {
        return 0;
    }
    /* Guest code for 'c'/'s': a jump-to-self at the reset vector. */
    static const uint8_t loop[4] = { 0x6f, 0x00, 0x00, 0x00 };
    rvvm_write_ram(machine, 0x80000000, loop, sizeof(loop));

    uint16_t port = 0;
    for (int i = 0; i < 16 && !port; ++i) {
        char bind[32];
        uint16_t p = (uint16_t)(port_base + i * 20011 % 20000);
        snprintf(bind, sizeof(bind), "127.0.0.1:%u", (unsigned)p);
        if (gdbstub_init(machine, bind)) {
            port = p;
        }
    }
    /* As the CLI does (-gdbstub): the machine runs, the debugger attaches. */
    if (port && rvvm_start_machine(machine)) {
        talk(port, data, size);
    }
    rvvm_free_machine(machine);
    return 0;
}
