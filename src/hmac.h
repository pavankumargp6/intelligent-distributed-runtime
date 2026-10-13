#ifndef HMAC_H
#define HMAC_H

#include <stddef.h>

#include "sha256.h"

/*
 * Compute HMAC-SHA256 (RFC 2104) over an arbitrary-length
 * message with an arbitrary-length key.
 *
 * out must point to a buffer of at least SHA256_DIGEST_SIZE
 * (32) bytes.
 *
 * Keys longer than SHA256_BLOCK_SIZE (64) are hashed first;
 * shorter keys are zero-padded to the block size. This is
 * standard HMAC key preprocessing.
 *
 * Returns 1 on success.
 * Returns 0 on invalid input (NULL out, NULL key with key_len
 * > 0, NULL data with data_len > 0, or an allocation failure
 * inside the function).
 */
int hmac_sha256(const void *key,
                size_t key_len,
                const void *data,
                size_t data_len,
                unsigned char out[SHA256_DIGEST_SIZE]);

/*
 * Format a raw HMAC-SHA256 digest as 64 lowercase hex
 * characters followed by a NUL terminator. out must be at
 * least 65 bytes. Same output format as sha256_hex.
 */
void hmac_sha256_hex(const unsigned char digest[SHA256_DIGEST_SIZE],
                     char out[65]);

#endif
