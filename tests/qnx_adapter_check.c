#include "aa_usb.h"
#include "aa_video.h"
#include "aap_paths.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

int cicaa_start(void);
int cicaa_busy(void);
int cicaa_input(int, int, int);
const char *cicaa_status(void);
const char *cicaa_result(void);

static FILE *wire;
static unsigned left, messages;

static int write_usb(void *state, const unsigned char *data, int size)
{
    (void)state;
    if (left) {
        assert(left == (unsigned)size);
        left = 0;
        ++messages;
    } else {
        assert(size == 8);
        left = ((unsigned)data[6] << 8) | data[7];
    }
    return size;
}

static int read_usb(void *state, unsigned char *data, int size, int timeout)
{
    (void)state;
    (void)timeout;
    assert(size >= 257);
    size_t n = fread(data, 1, 257, wire);
    if (!n)
        Sleep(15);
    return (int)n;
}

int aa_usb_open(aa_log_fn lg, aa_status_fn st, int wait, aa_transport_t *out)
{
    (void)lg;
    (void)st;
    (void)wait;
    *out = (aa_transport_t){NULL, write_usb, read_usb};
    return 0;
}

void aa_usb_close(void) {}
int av_native_want_idr(void) { return 0; }

static void write_nal(FILE *out, const uint8_t *data, size_t size)
{
    static const uint8_t prefix[] = {0, 0, 0, 1};
    assert(fwrite(prefix, 1, sizeof prefix, out) == sizeof prefix);
    assert(fwrite(data, 1, size, out) == size);
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    wire = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    assert(wire && out);
    assert(cicaa_start() == 0);
    for (unsigned i = 0; i < 200 && cicaa_busy(); ++i) {
        if (strcmp(cicaa_status(), "CarLife: video active") == 0)
            break;
        Sleep(10);
    }
    assert(strcmp(cicaa_status(), "CarLife: video active") == 0);
    av_packet p;
    assert(av_take(&p, 0) == 1 && p.kind == AV_CONFIG && p.data[0] == 1);
    size_t sps = ((unsigned)p.data[6] << 8) | p.data[7];
    assert(sps && p.size > 11 + sps && p.data[8 + sps] == 1);
    size_t pps = ((unsigned)p.data[9 + sps] << 8) | p.data[10 + sps];
    assert(pps && p.size == 11 + sps + pps);
    write_nal(out, p.data + 8, sps);
    write_nal(out, p.data + 11 + sps, pps);
    av_packet_free(&p);
    assert(av_take(&p, 0) == 1 && p.kind == AV_FRAME && p.pts == 123000);
    for (size_t o = 0; o < p.size;) {
        assert(p.size - o >= 4);
        size_t n = ((uint32_t)p.data[o] << 24) | ((uint32_t)p.data[o + 1] << 16) |
                   ((uint32_t)p.data[o + 2] << 8) | p.data[o + 3];
        assert(n && n <= p.size - o - 4);
        write_nal(out, p.data + o + 4, n);
        o += 4 + n;
    }
    av_packet_free(&p);
    cicaa_input(CICAA_IN_BYEBYE, 0, 0);
    for (unsigned i = 0; i < 200 && cicaa_busy(); ++i)
        Sleep(10);
    assert(!cicaa_busy() && strcmp(cicaa_result(), "CarLife disconnected") == 0 && messages == 6);
    assert(fclose(out) == 0 && fclose(wire) == 0);
    puts("PASS HMI ABI with mock USB and upstream avcC/length4 converter");
    return 0;
}
