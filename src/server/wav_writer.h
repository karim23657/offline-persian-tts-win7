/*
 * Streaming 16-bit WAV writer.
 *
 * We deliberately buffer rather than stream.  A streamed WAV would need its
 * RIFF/data sizes sent before the length is known, either by using the
 * 0xFFFFFFFF "unknown size" marker (not every player copes) or by sending a
 * wrong size.  For the clip lengths people actually synthesise - a few seconds
 * - the whole file is on the wire before a browser needs it, and the result is
 * a file every player accepts.  The real latency win is the engine cache in
 * tts_engine.c, not tricking the container.
 *
 * So: push every sample, then ask for the header, which now knows the exact
 * data size.
 */
#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned char *buf;
    size_t len;        /* bytes of PCM written so far */
    size_t cap;
    int sample_rate;
    int channels;
    int header_written;
    int failed;
} WavWriter;

/* Prepare a writer for mono 16-bit at `sample_rate`. */
void wav_init(WavWriter *w, int sample_rate);

/* Feed float samples in [-1, 1]; they are clipped and scaled to int16. */
void wav_push_samples(WavWriter *w, const float *samples, size_t count);

/* Serialised header (44 bytes) - send this before any samples. */
const unsigned char *wav_header(WavWriter *w, size_t *len);

void wav_free(WavWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* WAV_WRITER_H */
