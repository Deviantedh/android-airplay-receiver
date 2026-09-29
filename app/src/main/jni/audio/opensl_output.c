/*
 * opensl_output.c  –  OpenSL ES audio output (Android API 9/10)
 *
 * Uses a double-buffer queue (SL_DATAFORMAT_PCM + SLAndroidSimpleBufferQueue).
 * Two 4096-frame ping-pong buffers → ~185 ms total latency on Cortex-A5.
 *
 * Reference: Android NDK samples/audio-echo + developer.android.com/ndk/guides/audio/opensl
 */

#include "opensl_output.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <android/log.h>

#define TAG "OpenSL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE   44100
#define CHANNELS      2
#define BITS          16
#define FRAME_BYTES   (CHANNELS * (BITS / 8))
#define BUF_FRAMES    1024
#define BUF_BYTES     (BUF_FRAMES * FRAME_BYTES)
#define NUM_BUFS      32
#define PREBUFFER_BUFS 8

struct osl_ctx {
    SLObjectItf  engine_obj;
    SLEngineItf  engine;
    SLObjectItf  output_mix_obj;
    SLObjectItf  player_obj;
    SLPlayItf    player;
    SLAndroidSimpleBufferQueueItf buf_queue;

    int16_t      bufs[NUM_BUFS][BUF_FRAMES * CHANNELS];
    int          cur_buf;

    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             buf_free;      /* count of free slots in queue */
    int             queued_count;  /* count of buffers currently queued */
    int             is_playing;    /* 1 if SetPlayState is SL_PLAYSTATE_PLAYING */
};

/* Called by OpenSL when it has finished consuming a buffer */
static void buf_queue_cb(SLAndroidSimpleBufferQueueItf bq, void *ctx_) {
    osl_ctx_t *ctx = (osl_ctx_t *)ctx_;
    (void)bq;
    pthread_mutex_lock(&ctx->lock);
    ctx->buf_free++;
    if (ctx->queued_count > 0) ctx->queued_count--;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

osl_ctx_t *osl_open(void) {
    SLresult r;
    osl_ctx_t *ctx = (osl_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->buf_free     = NUM_BUFS;
    ctx->queued_count = 0;
    ctx->is_playing   = 0;

    /* ── Engine ── */
    r = slCreateEngine(&ctx->engine_obj, 0, NULL, 0, NULL, NULL);
    if (r != SL_RESULT_SUCCESS) { LOGE("slCreateEngine %d", r); goto fail; }
    (*ctx->engine_obj)->Realize(ctx->engine_obj, SL_BOOLEAN_FALSE);
    (*ctx->engine_obj)->GetInterface(ctx->engine_obj, SL_IID_ENGINE, &ctx->engine);

    /* ── Output mix ── */
    r = (*ctx->engine)->CreateOutputMix(ctx->engine, &ctx->output_mix_obj, 0, NULL, NULL);
    if (r != SL_RESULT_SUCCESS) { LOGE("CreateOutputMix %d", r); goto fail; }
    (*ctx->output_mix_obj)->Realize(ctx->output_mix_obj, SL_BOOLEAN_FALSE);

    /* ── Audio player ── */
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NUM_BUFS
    };
    SLDataFormat_PCM fmt_pcm = {
        SL_DATAFORMAT_PCM,
        CHANNELS,
        SL_SAMPLINGRATE_44_1,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource audio_src = { &loc_bq, &fmt_pcm };

    SLDataLocator_OutputMix loc_outmix = {
        SL_DATALOCATOR_OUTPUTMIX, ctx->output_mix_obj
    };
    SLDataSink audio_sink = { &loc_outmix, NULL };

    const SLInterfaceID iids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
    const SLboolean     req[]  = { SL_BOOLEAN_TRUE };
    r = (*ctx->engine)->CreateAudioPlayer(ctx->engine, &ctx->player_obj,
                                          &audio_src, &audio_sink,
                                          1, iids, req);
    if (r != SL_RESULT_SUCCESS) { LOGE("CreateAudioPlayer %d", r); goto fail; }
    (*ctx->player_obj)->Realize(ctx->player_obj, SL_BOOLEAN_FALSE);
    (*ctx->player_obj)->GetInterface(ctx->player_obj, SL_IID_PLAY, &ctx->player);
    (*ctx->player_obj)->GetInterface(ctx->player_obj,
                                     SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                     &ctx->buf_queue);

    (*ctx->buf_queue)->RegisterCallback(ctx->buf_queue, buf_queue_cb, ctx);
    /* Start paused; unpaused once prebuffer threshold is met */
    (*ctx->player)->SetPlayState(ctx->player, SL_PLAYSTATE_PAUSED);

    LOGI("OpenSL ES player ready: 16 buffers, prebuffer=%d, %d Hz stereo", PREBUFFER_BUFS, SAMPLE_RATE);
    return ctx;

fail:
    osl_close(ctx);
    return NULL;
}

int osl_play(osl_ctx_t *ctx, const int16_t *pcm, int frames) {
    if (!ctx || !pcm || frames <= 0) return -1;

    /* Wait for a free buffer slot */
    pthread_mutex_lock(&ctx->lock);
    while (ctx->buf_free == 0)
        pthread_cond_wait(&ctx->cond, &ctx->lock);
    ctx->buf_free--;
    ctx->queued_count++;

    int16_t *dst = ctx->bufs[ctx->cur_buf];
    int copy_frames = frames < BUF_FRAMES ? frames : BUF_FRAMES;
    memcpy(dst, pcm, copy_frames * FRAME_BYTES);

    (*ctx->buf_queue)->Enqueue(ctx->buf_queue, dst, copy_frames * FRAME_BYTES);
    ctx->cur_buf = (ctx->cur_buf + 1) % NUM_BUFS;

    /* Once enough buffers are queued to absorb network jitter, start playing */
    if (!ctx->is_playing && ctx->queued_count >= PREBUFFER_BUFS) {
        (*ctx->player)->SetPlayState(ctx->player, SL_PLAYSTATE_PLAYING);
        ctx->is_playing = 1;
    }
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

void osl_flush(osl_ctx_t *ctx) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->buf_queue) {
        (*ctx->buf_queue)->Clear(ctx->buf_queue);
    }
    ctx->buf_free     = NUM_BUFS;
    ctx->queued_count = 0;
    ctx->is_playing   = 0;
    if (ctx->player) {
        (*ctx->player)->SetPlayState(ctx->player, SL_PLAYSTATE_PAUSED);
    }
    pthread_cond_broadcast(&ctx->cond);
    pthread_mutex_unlock(&ctx->lock);
}

void osl_close(osl_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->player_obj) {
        (*ctx->player)->SetPlayState(ctx->player, SL_PLAYSTATE_STOPPED);
        (*ctx->player_obj)->Destroy(ctx->player_obj);
    }
    if (ctx->output_mix_obj) (*ctx->output_mix_obj)->Destroy(ctx->output_mix_obj);
    if (ctx->engine_obj)     (*ctx->engine_obj)->Destroy(ctx->engine_obj);
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->cond);
    free(ctx);
    LOGI("OpenSL ES closed");
}
