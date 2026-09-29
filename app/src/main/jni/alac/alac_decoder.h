/*
 * alac_decoder.h  –  Real ALAC decoding + AES-128-CBC audio decryptor
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct alac_decoder alac_decoder_t;

/**
 * Create ALAC decoder and AES decryption context.
 * @param fmtp       SDP fmtp string (e.g. "96 352 0 16 40 10 14 2 255 0 0 44100"), or NULL
 * @param aeskey     16-byte decrypted AES key, or NULL if unencrypted
 * @param aesiv      16-byte AES IV, or NULL
 */
alac_decoder_t *alac_decoder_create(const char *fmtp,
                                    const uint8_t aeskey[16],
                                    const uint8_t aesiv[16]);

/**
 * Update/reconfigure decoder with new fmtp and keys.
 */
void alac_decoder_set_keys(alac_decoder_t *dec,
                           const char *fmtp,
                           const uint8_t aeskey[16],
                           const uint8_t aesiv[16]);

/**
 * Decrypt and decode one RTP audio payload into 16-bit stereo PCM.
 * @param in         RTP audio payload bytes
 * @param in_len     payload length
 * @param out_pcm    output buffer (at least 4096 * 4 bytes)
 * @param out_frames output number of sample frames (each frame = 2 samples * 2 bytes = 4 bytes)
 * @returns 0 on success, <0 on error
 */
int alac_decoder_decode(alac_decoder_t *dec,
                        const uint8_t *in, size_t in_len,
                        int16_t *out_pcm, int *out_frames);

void alac_decoder_destroy(alac_decoder_t *dec);
