#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE 64

/*
 * Compute the SHA-256 digest of an in-memory buffer.
 *
 * out must point to a buffer of at least SHA256_DIGEST_SIZE
 * bytes. This is a raw output buffer, not a hex string; the
 * caller formats it for display or comparison.
 *
 * Returns 1 on success.
 * Returns 0 on invalid input (NULL out, or NULL data with
 * length > 0).
 *
 * This is FIPS 180-4 SHA-256. It is not a MAC on its own;
 * see hmac.h for HMAC-SHA256, added in a later commit.
 */
int sha256(const void *data,
           size_t length,
           unsigned char out[SHA256_DIGEST_SIZE]);

/*
 * Format a raw SHA-256 digest as 64 lowercase hex characters
 * followed by a NUL terminator. out must be at least 65 bytes.
 *
 * This is the canonical form for hex-encoded MACs on the wire.
 */
void sha256_hex(const unsigned char digest[SHA256_DIGEST_SIZE],
                char out[65]);

#endif
