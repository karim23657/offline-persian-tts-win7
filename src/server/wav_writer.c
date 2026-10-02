#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wav_writer.h"

#define WAV_HEADER_BYTES 44
#define MAX_SECONDS 600   /* refuse to buffer unbounded audio */

void wav_init(WavWriter *w, int sample_rate) {
    memset(w, 0, sizeof(*w));
    w->sample_rate = sample_rate > 0 ? sample_rate : 22050;
    w->channels = 1;
    w->cap = 1 << 16;
    w->buf = (unsigned char *)malloc(w->cap);
    if (!w->buf) {
        w->failed = 1;
        w->cap = 0;
    }
}

static int wav_reserve(WavWriter *w, size_t extra) {
    size_t need = w->len + extra;
    size_t cap;
    unsigned char *nb;

    if (w->failed) return 0;
    /* Cap at roughly MAX_SECONDS of mono 16-bit audio. */
    cap = (size_t)MAX_SECONDS * w->sample_rate * 2 + WAV_HEADER_BYTES;
    if (need > cap) need = cap;
    if (need <= w->cap) return 1;
    while (w->cap < need) w->cap = w->cap ? w->cap * 2 : (1 << 16);
    nb = (unsigned char *)realloc(w->buf, w->cap);
    if (!nb) {
        w->failed = 1;
        return 0;
    }
    w->buf = nb;
    return 1;
}

void wav_push_samples(WavWriter *w, const float *samples, size_t count) {
    size_t i;
    if (w->failed) return;
    if (!wav_reserve(w, count * 2)) return;
    for (i = 0; i < count; ++i) {
        float v = samples[i];
        int s;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        if (isnan(v)) v = 0.0f;
        s = (int)(v * 32767.0f);
        w->buf[w->len++] = (unsigned char)(s & 0xFF);
        w->buf[w->len++] = (unsigned char)((s >> 8) & 0xFF);
    }
}

const unsigned char *wav_header(WavWriter *w, size_t *len) {
    static unsigned char header[WAV_HEADER_BYTES];
    unsigned int data_bytes = (unsigned int)(w->len & ~1u);
    unsigned int rate = (unsigned int)w->sample_rate;
    unsigned short ch = (unsigned short)w->channels;
    unsigned short bits = 16;
    unsigned int byte_rate = rate * ch * bits / 8;
    unsigned short block_align = (unsigned short)(ch * bits / 8);

    memcpy(header + 0, "RIFF", 4);
    header[4] = (unsigned char)(data_bytes + 36);
    header[5] = (unsigned char)((data_bytes + 36) >> 8);
    header[6] = (unsigned char)((data_bytes + 36) >> 16);
    header[7] = (unsigned char)((data_bytes + 36) >> 24);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;  /* fmt chunk size */
    header[20] = 1; header[21] = 0;                                    /* PCM */
    header[22] = (unsigned char)ch; header[23] = 0;
    header[24] = (unsigned char)rate; header[25] = (unsigned char)(rate >> 8);
    header[26] = (unsigned char)(rate >> 16); header[27] = (unsigned char)(rate >> 24);
    header[28] = (unsigned char)byte_rate; header[29] = (unsigned char)(byte_rate >> 8);
    header[30] = (unsigned char)(byte_rate >> 16); header[31] = (unsigned char)(byte_rate >> 24);
    header[32] = (unsigned char)block_align; header[33] = 0;
    header[34] = bits; header[35] = 0;
    memcpy(header + 36, "data", 4);
    header[40] = (unsigned char)data_bytes;
    header[41] = (unsigned char)(data_bytes >> 8);
    header[42] = (unsigned char)(data_bytes >> 16);
    header[43] = (unsigned char)(data_bytes >> 24);

    if (len) *len = WAV_HEADER_BYTES;
    w->header_written = 1;
    return header;
}

void wav_free(WavWriter *w) {
    if (w->buf) {
        free(w->buf);
        w->buf = NULL;
    }
    w->len = w->cap = 0;
}
