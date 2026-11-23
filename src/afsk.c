/* AFSK1200 (Bell 202) demodulator + AX.25 HDLC deframer for the AnyTone D878UV.
 * Sampled at 9600 Hz (8 samples/bit). Non-coherent mark/space correlator -> tone
 * decision -> edge-resync bit sampler -> NRZI decode -> HDLC (flag detect, bit
 * destuffing, byte assembly) -> CRC-16/X.25 FCS check. Counts HDLC flags and valid
 * frames. Pure integer, no libc, no static init needed (state is caller-owned).
 *
 * This file compiles both for the target (fixed-address state) and for a host test
 * harness (test_afsk.c), so the framing/CRC logic is verified off-target first. */
#include "afsk.h"

/* 1200/2200 Hz correlator coefficients at 9600 Hz, one bit (8 samples), scaled x100 */
static const signed char MC[8] = {100, 71,   0, -71,-100,-71,   0, 71};
static const signed char MS[8] = {  0, 71, 100,  71,   0,-71,-100,-71};
static const signed char SC[8] = {100, 13, -97, -38,  87, 61, -71,-79};
static const signed char SS[8] = {  0, 99,  26, -92, -50, 79,  71,-61};

void afsk_init(afsk_t *a)
{
    int i;
    a->mean = 2048;
    for (i = 0; i < 8; i++) a->ring[i] = 0;
    a->rpos = 0;
    a->dlp = 0;
    a->pll = 0;
    a->prev_ddata = 1;
    a->nrzi_prev = 1;
    a->rxreg = 0;
    a->ones = 0;
    a->inframe = 0;
    a->bitpos = 0;
    a->curbyte = 0;
    a->nbytes = 0;
    a->fcs = 0xFFFF;
    a->flags = 0;
    a->pkt = 0;
    a->tnc2[0] = 0;
    a->msg[0] = 0;
    a->src[0] = 0;
    a->heard[0] = 0;
}

void aprs_path(afsk_t *a, const unsigned char *f, int n);  /* fwd (defined below) */

/* emit one AX.25 address (6 shifted chars + SSID) as "CALL" or "CALL-n"; returns len */
static int emit_call(char *o, const unsigned char *a)
{
    int p = 0, k, ssid;
    for (k = 0; k < 6; k++) {
        char c = (char)((a[k] >> 1) & 0x7F);
        if (c == ' ') break;
        o[p++] = c;
    }
    ssid = (a[6] >> 1) & 0x0F;
    if (ssid) {
        o[p++] = '-';
        if (ssid >= 10) { o[p++] = '1'; o[p++] = (char)('0' + ssid - 10); }
        else o[p++] = (char)('0' + ssid);
    }
    return p;
}

/* find the index just past the AX.25 address field (0 if malformed) */
static int addr_end_of(const unsigned char *f, int n)
{
    int j;
    if (n < 15) return 0;
    if (f[13] & 1) return 14;                 /* no digipeaters */
    j = 14;
    while (j + 7 <= n) {
        j += 7;
        if (f[j - 1] & 1) break;              /* this address had the ext/last bit */
    }
    return j;
}

/* Fill a->src (sender) and a->heard (the digipeater whose has-been-repeated H-bit is
 * set = the station we actually heard it via; empty => received direct). */
void aprs_path(afsk_t *a, const unsigned char *f, int n)
{
    int addr_end = addr_end_of(f, n), j, best = -1, l;
    a->src[0] = 0; a->heard[0] = 0;
    if (addr_end < 14) return;
    l = emit_call(a->src, f + 7); a->src[l] = 0;        /* source address */
    for (j = 14; j + 7 <= addr_end; j += 7)             /* digipeaters */
        if (f[j + 6] & 0x80) best = j;                  /* H-bit: this digi repeated it */
    if (best >= 0) { l = emit_call(a->heard, f + best); a->heard[l] = 0; }
}

/* For a position report (type '!','=','@','/'), return the index where the human
 * comment begins - i.e. AFTER the (optional) timestamp, latitude, symbol table, long-
 * itude and symbol code - so the timestamp and coordinates are excluded. Handles both
 * uncompressed ("DDMM.hhN/DDDMM.hhW") and compressed (13-byte) position data. Falls
 * back to "just after the type char" if the layout doesn't validate (never cuts text). */
static int pos_comment_start(const unsigned char *f, int n, int info, unsigned char t)
{
    int p = info + 1;                       /* after the data-type char */
    if (t == '@' || t == '/') p += 7;       /* DHM timestamp (DDHHMMz / DDHHMMh / etc.) */
    if (p >= n) return info + 1;
    if (f[p] >= '0' && f[p] <= '9') {       /* uncompressed: 8 lat + symtab + 9 lon + sym */
        if (p + 19 <= n) {
            unsigned char ns = f[p + 7], ew = f[p + 17];
            if ((ns == 'N' || ns == 'S') && (ew == 'E' || ew == 'W'))
                return p + 19;              /* comment starts after the symbol code */
        }
    } else if (f[p] == '/' || f[p] == '\\' ||
               (f[p] >= 'A' && f[p] <= 'Z') || (f[p] >= 'a' && f[p] <= 'j')) {
        /* compressed: symtab(valid id) + 4 lat + 4 lon (base-91 0x21..0x7b) + symcode +
         * cs + comptype = 13 bytes. Validate the lat/lon bytes so non-position payloads
         * (e.g. Ultimeter weather "!!....") are NOT mistaken for a compressed position. */
        if (p + 13 <= n) {
            int j, ok = 1;
            for (j = 1; j <= 8; j++)
                if (f[p + j] < 0x21 || f[p + j] > 0x7b) { ok = 0; break; }
            if (ok) return p + 13;
        }
    }
    return info + 1;                        /* unknown layout: keep everything after type */
}

/* Extract the readable message from an APRS frame by type, stripping binary/position
 * bytes, into out (NUL-terminated). Mic-E: skip the 8 binary bytes -> comment. Message
 * (':'): text after the addressee. Status ('>'): the text. Position ('!','=','@','/'):
 * skip the timestamp + lat/lon + symbols and show only the comment. Only printable
 * ASCII is kept; leading and trailing spaces are trimmed. */
void aprs_message(const unsigned char *f, int n, char *out, int outmax)
{
    int addr_end = addr_end_of(f, n);
    int info, start, k, w = 0;
    unsigned char t;
    if (addr_end == 0 || addr_end + 2 >= n || outmax < 2) { out[0] = 0; return; }
    info = addr_end + 2;                        /* skip control + PID */
    t = f[info];
    if (t == 0x1c || t == 0x1d || t == '`' || t == '\'') {
        start = info + 9;                      /* Mic-E: type + 8 binary bytes */
        if (start < n) {                       /* strip a leading device-type/telemetry */
            unsigned char c0 = f[start];       /* prefix char (e.g. '`' renders as an    */
            if (c0=='>'||c0=='<'||c0=='`'||c0=='\''||c0==']'||c0=='}') start++;  /* accent) */
        }
    } else if (t == ':') {                     /* message: :ADDRESSEE:text */
        int c = info + 1;
        while (c < n && f[c] != ':') c++;      /* addressee terminator */
        if (c < n) c++;
        start = c;
    } else if (t == '>' || t == '<') {         /* status: text after the type char */
        start = info + 1;
    } else if (t == '!' || t == '=' || t == '@' || t == '/') {  /* position report */
        start = pos_comment_start(f, n, info, t);
    } else {
        start = info;
    }
    if (start < info) start = info;
    while (start < n && f[start] == ' ') start++;   /* trim leading spaces of the field */
    for (k = start; k < n && w < outmax - 1; k++) {
        unsigned char c = f[k];
        if (c >= 32 && c < 127) out[w++] = (char)c;
    }
    while (w > 0 && out[w - 1] == ' ') w--;     /* trim trailing spaces */
    out[w] = 0;
}

int ax25_to_tnc2(const unsigned char *f, int n, char *out, int outmax)
{
    int pos = 0, addr_end, info, k;
    if (n < 15 || outmax < 16) { out[0] = 0; return 0; }
    pos += emit_call(out + pos, f + 7);     /* source */
    out[pos++] = '>';
    pos += emit_call(out + pos, f + 0);     /* destination */

    if (f[13] & 1) {
        addr_end = 14;                      /* no digipeaters */
    } else {
        int j = 14;
        while (j + 7 <= n) {
            out[pos++] = ',';
            pos += emit_call(out + pos, f + j);
            if (f[j + 6] & 1) { j += 7; break; }
            j += 7;
            if (pos > outmax - 16) break;
        }
        addr_end = j;
    }
    if (addr_end + 2 <= n) {                 /* skip control + PID */
        out[pos++] = ':';
        info = addr_end + 2;
        for (k = info; k < n && pos < outmax - 1; k++) {
            unsigned char c = f[k];
            out[pos++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
    }
    out[pos] = 0;
    return pos;
}

/* incremental CRC-16/X.25 (reflected poly 0x8408), one byte */
static unsigned crc_byte(unsigned fcs, unsigned char b)
{
    int i;
    fcs ^= b;
    for (i = 0; i < 8; i++)
        fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : (fcs >> 1);
    return fcs & 0xFFFF;
}

/* a complete HDLC frame delimited by flags has arrived in a->fbuf[0..nbytes-1]
 * (including its 2 FCS bytes). Valid if the running CRC residue == 0xF0B8. */
static void end_frame(afsk_t *a)
{
    if (a->nbytes >= 17 && a->fcs == 0xF0B8) { /* min AX.25 UI frame ~ 15 addr + ctl/pid + 2 fcs */
        a->pkt++;
        ax25_to_tnc2(a->fbuf, a->nbytes - 2, a->tnc2, (int)sizeof(a->tnc2));
        aprs_message(a->fbuf, a->nbytes - 2, a->msg, (int)sizeof(a->msg));
        aprs_path(a, a->fbuf, a->nbytes - 2);
    }
}

/* one NRZI-decoded data bit into the HDLC machine */
static void hdlc_bit(afsk_t *a, int b)
{
    /* 8-bit window for flag detection, LSB-first (AX.25 sends LSB first) */
    a->rxreg = ((a->rxreg >> 1) | (b ? 0x80 : 0)) & 0xFF;

    if (a->rxreg == 0x7E) {                 /* FLAG */
        if (a->inframe) end_frame(a);
        a->inframe = 1;
        a->ones = 0;
        a->bitpos = 0;
        a->curbyte = 0;
        a->nbytes = 0;
        a->fcs = 0xFFFF;
        a->flags++;
        return;
    }
    if ((a->rxreg & 0x7F) == 0x7F) {        /* 7+ ones: abort/idle */
        a->inframe = 0;
        a->ones = 0;
        return;
    }
    if (!a->inframe) return;

    if (b) {
        a->ones++;
    } else {
        if (a->ones == 5) { a->ones = 0; return; }   /* drop stuffed 0 */
        a->ones = 0;
    }

    /* assemble data byte, LSB first */
    a->curbyte = ((a->curbyte >> 1) | (b ? 0x80 : 0)) & 0xFF;
    a->bitpos++;
    if (a->bitpos == 8) {
        if (a->nbytes < AFSK_MAXFRAME) {
            a->fbuf[a->nbytes] = a->curbyte;
            a->fcs = crc_byte(a->fcs, a->curbyte);
            a->nbytes++;
        } else {
            a->inframe = 0;     /* overflow: abandon frame so it can't be counted */
        }
        a->bitpos = 0;
        a->curbyte = 0;
    }
}

/* DPLL bit-clock: 8 samples/bit at 9600 Hz -> step = 2^32 / 8 = 2^29.
 * Technique per Dire Wolf (reimplemented): accumulate each sample, sample the bit at
 * the +->- wrap (bit centre), and on a tone-decision transition pull the accumulator
 * toward zero (×~0.75) so the sample point tracks the data edges. */
#define PLL_STEP 0x20000000u

/* one 9600 Hz audio sample (12-bit, 0..4095) */
void afsk_sample(afsk_t *a, int s)
{
    long long mi = 0, mq = 0, si = 0, sq = 0;
    long long mark, space, d;
    int k, x, idx, ddata, before;

    /* DC tracking (slicer baseline) */
    a->mean += (s - a->mean) >> 7;
    x = s - a->mean;

    a->ring[a->rpos] = (short)x;
    a->rpos = (a->rpos + 1) & 7;

    for (k = 0; k < 8; k++) {
        idx = (a->rpos - 1 - k) & 7;
        x = a->ring[idx];
        mi += (long long)x * MC[k];
        mq += (long long)x * MS[k];
        si += (long long)x * SC[k];
        sq += (long long)x * SS[k];
    }
    mark  = mi * mi + mq * mq;
    space = si * si + sq * sq;
    d = mark - space;                       /* >0 => mark (1200) */

    /* 1-pole low-pass on the decision to cut per-sample noise */
    a->dlp += (d - a->dlp) >> 2;
    ddata = (a->dlp >= 0) ? 1 : 0;          /* 1 = mark */

    /* DPLL */
    if (ddata != a->prev_ddata)
        a->pll = a->pll - (a->pll >> 2);    /* nudge toward 0 on a data edge */
    a->prev_ddata = (unsigned char)ddata;

    before = a->pll;
    a->pll = (int)((unsigned)a->pll + PLL_STEP);
    if (a->pll < 0 && before >= 0) {         /* wrap +->- : bit centre */
        int sym = ddata;
        int databit = (sym == a->nrzi_prev) ? 1 : 0;   /* NRZI: no change = 1 */
        a->nrzi_prev = (unsigned char)sym;
        hdlc_bit(a, databit);
    }
}
