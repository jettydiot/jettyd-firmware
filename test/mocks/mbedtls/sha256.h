/**
 * @file sha256.h
 * @brief Host-test mock for mbedtls SHA-256.
 *
 * Returns a fixed, test-controlled hash so tests can assert
 * that the sha256 hex string appears in media/complete payloads.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    size_t total_len;
} mbedtls_sha256_context;

/* Controlled output — defined in the test file */
extern uint8_t g_fake_sha256[32];

static inline void mbedtls_sha256_init(mbedtls_sha256_context *ctx)
{
    ctx->total_len = 0;
}

static inline int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224)
{
    (void)is224;
    ctx->total_len = 0;
    return 0;
}

static inline int mbedtls_sha256_update(mbedtls_sha256_context *ctx,
                                         const uint8_t *input, size_t ilen)
{
    (void)input;
    ctx->total_len += ilen;
    return 0;
}

static inline int mbedtls_sha256_finish(mbedtls_sha256_context *ctx,
                                         uint8_t *output)
{
    (void)ctx;
    memcpy(output, g_fake_sha256, 32);
    return 0;
}

static inline void mbedtls_sha256_free(mbedtls_sha256_context *ctx)
{
    (void)ctx;
}
