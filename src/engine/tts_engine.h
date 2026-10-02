/*
 * Shared engine cache.
 *
 * The single biggest win for interactive use: sherpa-onnx takes ~1-2 s to load a
 * 63 MB model and build its ORT memory arenas.  Doing that per utterance makes
 * a GUI feel broken.  This keeps engines alive between calls, keyed by model
 * directory, so the second generation starts instantly.
 *
 * Three further optimisations live here:
 *
 *  - warm-up:  after loading, a throwaway synthesis is run.  That forces the
 *    ORT arenas to be allocated and the first-run kernel selection to happen,
 *    so the user's *first real* request is not several times slower than the
 *    rest.
 *  - sample rate caching: the warm-up also tells us the sample rate, which the
 *    streaming WAV writer needs before any audio exists.
 *  - bounded cache: at most MAX_CACHED_ENGINES stay resident, so switching
 *    between models cannot grow without limit.  Eviction is least-recently-used.
 *
 * Thread safety: an engine is *not* re-entrant, so generation is serialised by a
 * critical section.  Callers block rather than corrupt state; a queue in front
 * of this is a better place to manage concurrency.
 */

#ifndef TTS_ENGINE_H
#define TTS_ENGINE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CACHED_ENGINES 4

typedef struct TtsEngine TtsEngine;

/* Create the process-wide cache. Call once at startup. */
void TtsEngineInit(void);

/*
 * Get (loading if necessary) the engine for a model directory.
 * Returns NULL and fills `error` (size `errlen`) on failure.
 */
TtsEngine *TtsEngineAcquire(const wchar_t *model_dir, wchar_t *error, size_t errlen);

/* Mark as most-recently-used; called after generation. */
void TtsEngineTouch(TtsEngine *engine);

/* Release the per-call lock. Always pair with TtsEngineAcquire. */
void TtsEngineRelease(void);

/* Forget every cached engine (used on shutdown). */
void TtsEngineShutdown(void);

/* Number of engines currently resident - reported by /api/health. */
int TtsEngineResidentCount(void);

/* Non-zero when this model directory is already loaded (no locking needed:
 * safe to call from the /api/models listing). */
int TtsEngineIsLoaded(const wchar_t *model_dir);

/* Sample rate of a resident model, or 0 when it is not loaded. */
int TtsEngineSampleRate(const wchar_t *model_dir);

/*
 * Sink for generated samples, so callers (e.g. the WAV writer) can consume
 * audio as it is produced instead of only at the end.
 */
typedef void (*TtsSampleSink)(const float *samples, int count, void *user);

/*
 * Synthesise UTF-8 `text`.  Samples are pushed to `sink` as they arrive.
 * Fills *out_len with the sample count.  Returns 1 on success, 0 on failure
 * with a UTF-8 message in `err`.
 */
int TtsEngineGenerate(TtsEngine *engine, const char *text, float speed, int sid,
                      TtsSampleSink sink, void *user, int *out_samples,
                      char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* TTS_ENGINE_H */
