/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 KP3M */
/* Target glue: AFSK1200 demodulator + a custom-drawn APRS RX pop-up for the AnyTone
 * popup (option B).
 *
 * The firmware's shared popup body for screen id 0x68 (0x08041f10) draws exactly two
 * CENTERED, standard-font lines (group[0] @ y=65, group[1] @ y=84) and is shared with
 * the APRS-TX "Sending Ana / Aprs Data..." popup and with screen 0x38. We patch that
 * body to call our_draw68() (build_image.py), which:
 *   - if the popup string-group pointer (0x20000918) is NOT our GROUP -> replicate the
 *     original two std-font draws exactly (TX popup + screen 0x38 look unchanged);
 *   - if it IS our GROUP (our RX popup) -> draw our own layout: sender + direct/via on
 *     line 0, then the message word-wrapped across several lines, in a SMALL font,
 *     contiguous from the top of the box so the top lines OVERWRITE the APRS diamond
 *     icon (the text routine fills its full line width with the popup background, so
 *     drawing over the icon erases it - no fill-rect needed).
 *
 * Font is the 6th argument (stack [sp+4]) of the text routine 0x080305e2: 0x20/0x10/
 * 0x0c/0x0a/0x08 select glyph heights 24/16/12/10/8 px. We use 0x0c (12 px) for RX.
 *
 * The demod samples the live audio tap PC0 (0x20001048) at 9600 Hz UNCONDITIONALLY -
 * there is no per-channel gate, so APRS is decoded on ANY analog channel / VFO
 * frequency, not just the test channel. On a digital (DMR) channel PC0 carries no
 * valid AFSK, so the CRC check rejects it (nothing shows) - harmless.
 *
 * All popup state is set from our SysTick worker with plain memory writes only
 * (ISR-safe; no firmware calls from interrupt context). our_draw68() itself runs in
 * the UI-task context (the firmware calls it during a popup redraw), so it may call
 * the firmware text routine safely. */
#include "afsk.h"

#define REG(a)   (*(volatile unsigned *)(a))
#define REG16(a) (*(volatile unsigned short *)(a))
#define REG8(a)  (*(volatile unsigned char *)(a))
#define BUF      ((volatile char *)0x20015800)
#define AF       ((afsk_t *)0x20015900)

/* scalar state (0x20015C00..) */
#define MAGIC    REG(0x20015C00)
#define LASTCYC  REG(0x20015C04)
#define PREVSC   REG(0x20015C08)
#define SCNT     REG(0x20015C0C)
#define LASTPKT  REG(0x20015C10)
#define SHOWING  REG(0x20015C14)
#define TSHOW    REG(0x20015C18)
#define RNLINES  REG(0x20015C1C)   /* total drawn lines (line0 = sender + message lines) */
#define MAGICV   0x53743263u

/* buffers (0x20015D00..) */
#define GROUP    ((volatile unsigned *)0x20015D00)   /* 4 string pointers (RX marker) */
#define RLINE(i) ((char *)(0x20015D20 + (i)*24))     /* 7 drawable lines x 24 bytes */
#define RCOLOR   ((volatile unsigned short *)0x20015CE0) /* per-line RGB565 fg colour (7) */
#define MSG      ((char *)0x20015E00)                /* full clean message */

/* firmware text foreground colour global (RGB565); background is 0x2000094e. */
#define FGCOLOR  0x2000094c
#define C_WHITE  0xFFFF
#define C_YELLOW 0xFFE0
#define C_BLUE   0x001F
#define C_GREEN  0x07E0
#define C_RED    0xF800

#define MAXLN    6u                 /* max lines drawn (line0 + up to 5 message lines) */
#define ICONLN   3u                 /* min lines to fully cover the diamond icon (y27-58) */
#define WRAPCH   14u                /* chars per wrapped line (kept < draw width so no char is clipped) */
#define Y0       24                 /* top y of line 0 (covers the diamond icon area) */
#define PITCH    12                 /* line pitch = small-font height (contiguous fill) */
#define FONT_SM  0x0c               /* small font (12 px) */
#define FONT_STD 0x10               /* standard font (16 px) */
#define XL       0x0d               /* left x */
#define XW       0x81               /* draw width (matches the firmware popup lines) */

#define HOLD_CYC ((7u*ONESEC)/2u)   /* 3.5 s on screen (new traffic replaces it immediately) */
#define MAXSHOW_CYC (20u*ONESEC)    /* safety cap */

#define SCREEN   0x2000090c       /* current-screen byte */
#define POPGRP   0x20000918       /* popup string-group pointer */
#define POPID    0x68

#define ADC_PC0  0x20001048
#define DEMCR    0xE000EDFC
#define DWTCTRL  0xE0001000
#define CYCCNT   0xE0001004
#define ONESEC   120000000u

/* TIM6 / RCC / NVIC (9600 Hz) */
#define RCC_APB1EN 0x4002101C
#define T6_CR1   0x40001000
#define T6_DIER  0x4000100C
#define T6_SR    0x40001010
#define T6_EGR   0x40001014
#define T6_PSC   0x40001028
#define T6_ARR   0x4000102C
#define NVIC_ISER1 0xE000E104
#define NVIC_IPR54 0xE000E436

/* firmware text draw (centered, selectable font). ABI (from RE of 0x080305e2):
 *   r0=x, r1=y, r2=width, r3=cell-height, [sp]=y, [sp+4]=font, [sp+8]=string ptr. */
typedef void (*draw_t)(int x, int y, int w, int h, int y2, int font, int s);
#define DRAWFN ((draw_t)(0x080305e2u | 1u))

static void draw_line(int y, int font, int h, const char *s)
{
    DRAWFN(XL, y, XW, h, y, font, (int)s);
}

/* Called (via build_image.py's patch of the 0x68 body at 0x08041f10) in UI-task
 * context during a popup redraw. */
void our_draw68(void)
{
    unsigned grp = REG(POPGRP);
    if (grp != (unsigned)GROUP) {
        /* not our RX popup (TX popup / screen 0x38): replicate the firmware original */
        volatile unsigned *g = (volatile unsigned *)grp;
        if (g) {
            draw_line(0x41, FONT_STD, 0x10, (const char *)g[0]);
            draw_line(0x54, FONT_STD, 0x10, (const char *)g[1]);
        }
        return;
    }
    /* our RX popup: small-font multi-line, contiguous from the top (erases the icon).
     * Always draw at least ICONLN lines so the diamond is fully covered even for short
     * or empty messages (filler lines are blank - their bg fill still erases). */
    {
        unsigned n = RNLINES, ndraw, i;
        unsigned short saved = REG16(FGCOLOR);     /* keep the firmware's default fg */
        if (n > MAXLN) n = MAXLN;
        ndraw = (n < ICONLN) ? ICONLN : n;
        for (i = 0; i < ndraw; i++) {
            REG16(FGCOLOR) = (i < n) ? RCOLOR[i] : C_WHITE;   /* per-field colour */
            draw_line(Y0 + PITCH * (int)i, FONT_SM, PITCH, (i < n) ? RLINE(i) : "");
        }
        REG16(FGCOLOR) = saved;                    /* restore */
    }
}

static void tim6_init(void)
{
    REG(RCC_APB1EN) |= 0x10;
    REG(T6_PSC) = 0;
    REG(T6_ARR) = 12499;
    REG(T6_EGR) = 1;
    REG(T6_SR)  = 0;
    REG(T6_DIER) = 1;
    REG8(NVIC_IPR54) = 0x80;
    REG(NVIC_ISER1) = 0x00400000;
    REG(T6_CR1) = 1;
}

void t6_worker(void)
{
    int s = REG(ADC_PC0) & 0xFFF;
    SCNT++;
    afsk_sample(AF, s);
    REG(T6_SR) = 0;
}

/* set RLINE(li) = s (truncated) and its colour; return li+1 */
static unsigned put_line(unsigned li, const char *s, unsigned short color)
{
    char *p = RLINE(li), *e = p + 23;
    while (*s && p < e) *p++ = *s++;
    *p = 0;
    RCOLOR[li] = color;
    return li + 1;
}

/* Build the colour-coded popup lines (one field per line so each has its own colour):
 *   callsign (yellow); then "Direct" (green) OR "Via"(red)+digipeater(yellow); then the
 *   message word-wrapped (white). */
static void build_lines(void)
{
    char *p, *e;
    const char *s, *m;
    unsigned li;

    li = put_line(0, AF->src, C_YELLOW);           /* callsign */
    if (AF->heard[0]) {
        li = put_line(li, "Via", C_RED);
        li = put_line(li, AF->heard, C_YELLOW);    /* digipeater */
    } else {
        li = put_line(li, "Direct", C_GREEN);
    }

    /* copy clean message */
    p = MSG; e = MSG + 126;
    s = AF->msg; while (*s && p < e) *p++ = *s++;
    *p = 0;

    /* greedy word-wrap MSG into the remaining lines (white) */
    m = MSG;
    while (*m && li < MAXLN) {
        unsigned n, take, k;
        int lastsp;
        const char *start;
        while (*m == ' ') m++;            /* skip leading spaces */
        if (!*m) break;
        start = m;
        n = 0; lastsp = -1;
        while (start[n] && n < WRAPCH) {
            if (start[n] == ' ') lastsp = (int)n;
            n++;
        }
        if (!start[n]) take = n;                       /* remainder fits */
        else if (lastsp > 0) take = (unsigned)lastsp;  /* break at last space */
        else take = WRAPCH;                            /* long word: hard break */
        p = RLINE(li);
        for (k = 0; k < take; k++) p[k] = start[k];
        p[take] = 0;
        RCOLOR[li] = C_WHITE;
        m += take;
        li++;
    }
    RNLINES = li;                         /* header lines + message lines */
}

/* Replicate the state the firmware's 0x080100e8(0x68) sets, via plain writes only. */
#define SCR_CFG   0x20000930
#define SCR_B34   0x20000934
#define SCR_B35   0x20000935
#define SCR_W38   0x20000938
#define DIRTY     0x2000095c
#define POPTMR    0x20000914
#define POP_TABLE 0x080ab034
#define DIRTY_BITS 0x70000000u

static void show_popup(void)
{
    GROUP[0] = (unsigned)RLINE(0);
    GROUP[1] = (unsigned)RLINE(1);
    GROUP[2] = (unsigned)RLINE(0);
    GROUP[3] = (unsigned)RLINE(1);
    REG(POPGRP)  = (unsigned)GROUP;        /* also our RX marker (our_draw68 checks it) */
    REG8(SCREEN) = POPID;
    REG(SCR_CFG) = REG(POP_TABLE + POPID * 4);
    REG8(SCR_B34) = 0x20;
    REG8(SCR_B35) = 0x20;
    REG(SCR_W38) = 0x800;
    REG(DIRTY)  |= DIRTY_BITS;             /* trigger the redraw */
    REG16(POPTMR) = 0xC8;                  /* firmware auto-dismiss backup */
    SHOWING = 1;
}

static char *u2d(char *p, unsigned v)
{
    char tmp[10]; int n = 0;
    if (!v) { *p++ = '0'; return p; }
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

void st_worker(void)
{
    unsigned now;
    REG(DEMCR)   |= 0x01000000;
    REG(DWTCTRL) |= 1;

    if (MAGIC != MAGICV) {
        MAGIC = MAGICV;
        afsk_init(AF);
        SCNT = 0; PREVSC = 0; BUF[0] = 0;
        LASTPKT = 0; SHOWING = 0;
        LASTCYC = REG(CYCCNT);
    }
    if (SCNT == PREVSC) tim6_init();
    PREVSC = SCNT;

    now = REG(CYCCNT);

    if (AF->pkt != LASTPKT) {              /* new packet -> build + show popup */
        LASTPKT = AF->pkt;
        build_lines();
        show_popup();
        TSHOW = now;
    }
    if (SHOWING) {                         /* keep alive, dismiss after the hold */
        if (REG8(SCREEN) != POPID) {
            SHOWING = 0;                   /* user navigated away */
        } else {
            unsigned elapsed = (unsigned)(now - TSHOW);
            REG16(POPTMR) = 0xC8;          /* keep the firmware from dismissing early */
            if (elapsed >= HOLD_CYC || elapsed >= MAXSHOW_CYC) {
                REG16(POPTMR) = 2;
                SHOWING = 0;
            }
        }
    }

    if ((unsigned)(now - LASTCYC) >= ONESEC) {   /* Radio Info fallback status */
        char *p = (char *)BUF;
        *p++='P'; *p++='=';
        p = u2d(p, AF->pkt);
        *p++=' '; *p++='F'; *p++='=';
        p = u2d(p, AF->flags);
        *p = 0;
        LASTCYC = now;
    }
}
