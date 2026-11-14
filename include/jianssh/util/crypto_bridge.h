#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Return 0 on success, nonzero on failure. Fixed sizes: key32/salt16/nonce24/tag16.
 * Caller provides valid pointers and matching buffer lengths. No exceptions cross ABI. */
int js_crypto_random(uint8_t *out, size_t len);
int js_crypto_kdf(uint8_t *key32, const uint8_t *password, size_t len, const uint8_t *salt16, uint32_t memory_kib, uint32_t iterations, uint32_t lanes);
int js_crypto_encrypt(uint8_t *cipher, uint8_t *tag16, const uint8_t *plain, size_t len, const uint8_t *aad, size_t aad_len, const uint8_t *nonce24, const uint8_t *key32);
int js_crypto_decrypt(uint8_t *plain, const uint8_t *cipher, size_t len, const uint8_t *tag16, const uint8_t *aad, size_t aad_len, const uint8_t *nonce24, const uint8_t *key32);
void js_crypto_wipe(uint8_t *data, size_t len);
#ifdef __cplusplus
}
#endif
