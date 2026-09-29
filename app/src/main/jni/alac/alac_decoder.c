/*
 * alac_decoder.c  –  Thin wrapper connecting ALAC decoder and AES-128-CBC
 */

#include "alac_decoder.h"
#include "alac.h"
#include "../crypto/crypto.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <android/log.h>

#define TAG "ALAC-DEC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_PAYLOAD_BUF (16 * 1024)

struct alac_decoder {
    alac_file *alac;
    int        alac_ready;

    int        has_crypto;
    uint8_t    aeskey[16];
    uint8_t    aesiv[16];
    AES_CTX    aes_ctx;

    uint8_t    dec_buf[MAX_PAYLOAD_BUF];
};

static void configure_alac(alac_file *alac, const char *fmtp) {
    int frameLength = 352;
    int bitDepth = 16;
    int pb = 40, mb = 10, kb = 14;
    int numChannels = 2;
    int maxRun = 255;
    int maxFrameBytes = 0;
    int avgBitRate = 0;
    int sampleRate = 44100;

    if (fmtp) {
        /* Parse 12 integers from fmtp */
        int intarr[12] = {0};
        char tmp[256];
        strncpy(tmp, fmtp, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *token = strtok(tmp, " ");
        int idx = 0;
        while (token && idx < 12) {
            intarr[idx++] = atoi(token);
            token = strtok(NULL, " ");
        }

        if (idx >= 12) {
            frameLength   = intarr[1];
            bitDepth      = intarr[3];
            pb            = intarr[4];
            mb            = intarr[5];
            kb            = intarr[6];
            numChannels   = intarr[7];
            maxRun        = intarr[8];
            maxFrameBytes = intarr[9];
            avgBitRate    = intarr[10];
            sampleRate    = intarr[11];
        }
    }

    uint8_t info[48];
    memset(info, 0, sizeof(info));

    info[24] = (frameLength >> 24) & 0xFF;
    info[25] = (frameLength >> 16) & 0xFF;
    info[26] = (frameLength >> 8) & 0xFF;
    info[27] = frameLength & 0xFF;

    info[28] = 0;
    info[29] = (uint8_t)bitDepth;
    info[30] = (uint8_t)pb;
    info[31] = (uint8_t)mb;
    info[32] = (uint8_t)kb;
    info[33] = (uint8_t)numChannels;

    info[34] = (maxRun >> 8) & 0xFF;
    info[35] = maxRun & 0xFF;

    info[36] = (maxFrameBytes >> 24) & 0xFF;
    info[37] = (maxFrameBytes >> 16) & 0xFF;
    info[38] = (maxFrameBytes >> 8) & 0xFF;
    info[39] = maxFrameBytes & 0xFF;

    info[40] = (avgBitRate >> 24) & 0xFF;
    info[41] = (avgBitRate >> 16) & 0xFF;
    info[42] = (avgBitRate >> 8) & 0xFF;
    info[43] = avgBitRate & 0xFF;

    info[44] = (sampleRate >> 24) & 0xFF;
    info[45] = (sampleRate >> 16) & 0xFF;
    info[46] = (sampleRate >> 8) & 0xFF;
    info[47] = sampleRate & 0xFF;

    alac_set_info(alac, (char *)info);
    alac_allocate_buffers(alac);
}

alac_decoder_t *alac_decoder_create(const char *fmtp,
                                    const uint8_t aeskey[16],
                                    const uint8_t aesiv[16])
{
    alac_decoder_t *dec = (alac_decoder_t *)calloc(1, sizeof(*dec));
    if (!dec) return NULL;

    dec->alac = alac_create(16, 2);
    if (!dec->alac) {
        free(dec);
        return NULL;
    }

    configure_alac(dec->alac, fmtp);
    dec->alac_ready = 1;

    if (aeskey && aesiv) {
        memcpy(dec->aeskey, aeskey, 16);
        memcpy(dec->aesiv,  aesiv,  16);
        AES_set_key(&dec->aes_ctx, dec->aeskey, dec->aesiv, AES_MODE_128);
        AES_convert_key(&dec->aes_ctx);
        dec->has_crypto = 1;
        LOGI("ALAC decoder ready with AES-128-CBC encryption");
    } else {
        dec->has_crypto = 0;
        LOGI("ALAC decoder ready (unencrypted)");
    }

    return dec;
}

void alac_decoder_set_keys(alac_decoder_t *dec,
                           const char *fmtp,
                           const uint8_t aeskey[16],
                           const uint8_t aesiv[16])
{
    if (!dec) return;
    if (fmtp) configure_alac(dec->alac, fmtp);

    if (aeskey && aesiv) {
        memcpy(dec->aeskey, aeskey, 16);
        memcpy(dec->aesiv,  aesiv,  16);
        AES_set_key(&dec->aes_ctx, dec->aeskey, dec->aesiv, AES_MODE_128);
        AES_convert_key(&dec->aes_ctx);
        dec->has_crypto = 1;
    } else {
        dec->has_crypto = 0;
    }
}

int alac_decoder_decode(alac_decoder_t *dec,
                        const uint8_t *in, size_t in_len,
                        int16_t *out_pcm, int *out_frames)
{
    if (!dec || !in || in_len == 0 || !out_pcm || !out_frames) return -1;
    if (in_len > MAX_PAYLOAD_BUF) return -1;

    const uint8_t *data_to_decode = in;

    if (dec->has_crypto) {
        int enc_len = (int)(in_len / 16) * 16;
        AES_CTX aes_run = dec->aes_ctx; /* clone IV state for this frame */
        if (enc_len > 0) {
            AES_cbc_decrypt(&aes_run, in, dec->dec_buf, enc_len);
        }
        if (in_len > (size_t)enc_len) {
            memcpy(dec->dec_buf + enc_len, in + enc_len, in_len - enc_len);
        }
        data_to_decode = dec->dec_buf;
    }

    int output_bytes = 4096 * 4;
    alac_decode_frame(dec->alac, (unsigned char *)data_to_decode, out_pcm, &output_bytes);

    if (output_bytes > 0) {
        *out_frames = output_bytes / 4; /* 16-bit stereo = 4 bytes per frame */
        return 0;
    } else {
        *out_frames = 0;
        return -1;
    }
}

void alac_decoder_destroy(alac_decoder_t *dec) {
    if (!dec) return;
    if (dec->alac) {
        alac_free(dec->alac);
    }
    free(dec);
}
