/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 KP3M */
/* Host test for afsk.c: synthesize a real AX.25 UI frame as AFSK1200 audio at
 * 9600 Hz and verify the demodulator decodes it (valid FCS). Build & run natively:
 *   cc -O2 -o test_afsk test_afsk.c afsk.c && ./test_afsk
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "afsk.h"

static unsigned crc16(const unsigned char *p, int n)
{
    unsigned fcs = 0xFFFF; int i, k;
    for (i = 0; i < n; i++) {
        fcs ^= p[i];
        for (k = 0; k < 8; k++) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : (fcs >> 1);
    }
    return fcs & 0xFFFF;
}

static int addr(unsigned char *o, const char *call, int ssid, int last)
{
    int i;
    for (i = 0; i < 6; i++) {
        char c = i < (int)strlen(call) ? call[i] : ' ';
        o[i] = (unsigned char)(c << 1);
    }
    o[6] = (unsigned char)(0x60 | (ssid << 1) | (last ? 1 : 0));
    return 7;
}

/* bit emission with HDLC stuffing + NRZI + AFSK synthesis */
static double ph;
static int level;              /* 1 = mark(1200), 0 = space(2200) */
static int ones;
static short *buf; static int nbuf;

static void emit_tone(int lv)
{
    int i; double f = lv ? 1200.0 : 2200.0;
    for (i = 0; i < 8; i++) {                 /* 8 samples/bit at 9600 Hz */
        buf[nbuf++] = (short)(2048 + 1000.0 * sin(ph));
        ph += 2.0 * M_PI * f / 9600.0;
    }
}
static void emit_bit_nrzi(int bit)
{
    if (!bit) level ^= 1;                      /* NRZI: 0 toggles */
    emit_tone(level);
}
static void emit_flag(void)                    /* 0x7E, no stuffing */
{
    int i; ones = 0;
    for (i = 0; i < 8; i++) emit_bit_nrzi((0x7E >> i) & 1);
}
static void emit_byte(unsigned char b)         /* LSB first, with stuffing */
{
    int i, bit;
    for (i = 0; i < 8; i++) {
        bit = (b >> i) & 1;
        emit_bit_nrzi(bit);
        if (bit) { if (++ones == 5) { emit_bit_nrzi(0); ones = 0; } }
        else ones = 0;
    }
}

int main(int argc, char **argv)
{
    unsigned char frame[64]; int n = 0;
    unsigned f;
    static short samples[200000];
    afsk_t a; int i;
    int noise = argc > 1 ? atoi(argv[1]) : 0;   /* peak noise in counts, 0 = clean */

    n += addr(frame + n, "APRS", 0, 0);
    n += addr(frame + n, "KP4PRA", 1, 1);
    frame[n++] = 0x03;                         /* UI */
    frame[n++] = 0xF0;                          /* no layer 3 */
    const char *info = "!1830.00N/06630.00W-test";
    memcpy(frame + n, info, strlen(info)); n += strlen(info);

    f = crc16(frame, n);
    unsigned char fcsL = (~f) & 0xFF, fcsH = (~f >> 8) & 0xFF;

    buf = samples; nbuf = 0; ph = 0; level = 1; ones = 0;
    for (i = 0; i < 40; i++) emit_tone(1);     /* idle mark lead-in */
    for (i = 0; i < 20; i++) emit_flag();      /* preamble flags */
    for (i = 0; i < n; i++) emit_byte(frame[i]);
    emit_byte(fcsL); emit_byte(fcsH);          /* FCS, LSB first, complemented */
    for (i = 0; i < 5; i++) emit_flag();       /* closing flags */
    for (i = 0; i < 40; i++) emit_tone(1);

    afsk_init(&a);
    for (i = 0; i < nbuf; i++) {
        int v = samples[i];
        if (noise) { v += (rand() % (2 * noise + 1)) - noise; if (v < 0) v = 0; if (v > 4095) v = 4095; }
        afsk_sample(&a, v);
    }

    printf("frame bytes=%d crc=%04x  samples=%d\n", n, f, nbuf);
    printf("decoded: flags=%u pkt=%u\n", a.flags, a.pkt);
    printf("tnc2: \"%s\"\n", a.tnc2);
    {
        const char *want = "KP4PRA-1>APRS:!1830.00N/06630.00W-test";
        if (a.pkt >= 1 && strcmp(a.tnc2, want) == 0) { printf("PASS\n"); return 0; }
        if (a.pkt >= 1) printf("tnc2 mismatch, want \"%s\"\n", want);
    }
    printf("FAIL\n"); return 1;
}
