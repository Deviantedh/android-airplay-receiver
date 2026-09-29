/*
 * opensl_output.h  –  OpenSL ES audio output for Android API 9+
 *
 * Maintains a single SLObjectItf engine + output mix + player.
 * PCM feed: 44100 Hz, 16-bit, stereo, push-buffer queue.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct osl_ctx osl_ctx_t;

/** Open OpenSL ES engine and audio player. Returns NULL on failure. */
osl_ctx_t *osl_open(void);

/**
 * Enqueue a block of PCM frames for playback.
 * @param pcm    interleaved L/R 16-bit samples
 * @param frames number of sample-frames (bytes = frames * 2 * 2)
 * @returns 0 on success.
 */
int osl_play(osl_ctx_t *ctx, const int16_t *pcm, int frames);

/** Flush all queued buffers and pause the player (e.g. on seek/pause). */
void osl_flush(osl_ctx_t *ctx);

/** Stop playback and release all OpenSL objects. */
void osl_close(osl_ctx_t *ctx);

