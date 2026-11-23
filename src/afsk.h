#ifndef AFSK_H
#define AFSK_H

#define AFSK_MAXFRAME 340

typedef struct {
    int  mean;                 /* DC-tracking slicer baseline */
    short ring[8];             /* last 8 mean-removed samples (1 bit @ 9600 Hz) */
    int  rpos;
    long long dlp;             /* 1-pole low-pass of (mark^2 - space^2) */
    int  pll;                  /* DPLL bit-clock accumulator */
    unsigned char prev_ddata;  /* previous sliced tone decision (for DPLL nudge) */
    unsigned char nrzi_prev;   /* previous NRZI level */
    unsigned char rxreg;       /* 8-bit LSB-first window for flag detect */
    unsigned char ones;        /* consecutive ones (de-stuffing) */
    unsigned char inframe;
    unsigned char bitpos;
    unsigned char curbyte;
    int  nbytes;
    unsigned fcs;              /* running CRC-16/X.25 */
    unsigned flags;            /* HDLC flags seen */
    unsigned pkt;              /* frames with valid FCS */
    unsigned char fbuf[AFSK_MAXFRAME];
    char tnc2[128];            /* latest decoded frame as TNC2 text (SRC>DEST[,path]:info) */
    char msg[100];             /* latest frame's clean human-readable message (by APRS type) */
    char src[12];              /* sender callsign */
    char heard[12];            /* digipeater we heard it via (H-bit), empty = direct */
} afsk_t;

void afsk_init(afsk_t *a);
void afsk_sample(afsk_t *a, int s);
/* format an AX.25 frame (addresses+ctrl+pid+info, NO FCS) as a TNC2 line; returns len */
int  ax25_to_tnc2(const unsigned char *f, int n, char *out, int outmax);
/* extract the clean human-readable message from an AX.25 APRS frame (no FCS) into out */
void aprs_message(const unsigned char *f, int n, char *out, int outmax);
void aprs_path(afsk_t *a, const unsigned char *f, int n);

#endif
