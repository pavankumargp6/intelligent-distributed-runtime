/*
 * HMAC-SHA256 (RFC 2104).
 *
 * Verified against RFC 4231 test cases 1, 2, 3, 4, 6, and 7.
 * Case 5 is intentionally skipped: it uses a truncated 128-bit
 * output, which requires truncation support we do not need
 * (IDR uses the full 256-bit MAC).
 *
 * The implementation follows the RFC directly:
 *
 *   HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
 *
 * where K' is the key padded to the block size (or hashed
 * first if longer than the block size), and ipad / opad are
 * 0x36 / 0x5c repeated to the block size.
 *
 * Because sha256() is one-shot rather than streaming, this
 * implementation concatenates the inner and outer inputs into
 * temporary buffers. That is fine for the sizes IDR uses
 * (task commands are capped at 256 bytes; protocol messages
 * at NETWORK_MAX_MESSAGE_SIZE = 4096). A streaming SHA-256
 * interface would remove the allocations, but is not needed
 * yet.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hmac.h"
#include "sha256.h"

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5c

int hmac_sha256(const void *key,
                size_t key_len,
                const void *data,
                size_t data_len,
                unsigned char out[SHA256_DIGEST_SIZE])
{
    if (out == NULL)
    {
        return 0;
    }

    if (key == NULL && key_len > 0)
    {
        return 0;
    }

    if (data == NULL && data_len > 0)
    {
        return 0;
    }

    /*
     * Step 1: derive K', the block-sized key.
     *
     * If the key is longer than the block size, hash it first.
     * Otherwise, zero-pad it to exactly SHA256_BLOCK_SIZE
     * bytes.
     */
    unsigned char k_prime[SHA256_BLOCK_SIZE];

    if (key_len > SHA256_BLOCK_SIZE)
    {
        if (!sha256(key, key_len, k_prime))
        {
            return 0;
        }

        memset(k_prime + SHA256_DIGEST_SIZE,
               0,
               SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    }
    else
    {
        if (key_len > 0)
        {
            memcpy(k_prime, key, key_len);
        }

        memset(k_prime + key_len, 0, SHA256_BLOCK_SIZE - key_len);
    }

    /*
     * Step 2: build the inner and outer padded key blocks.
     */
    unsigned char ipad_block[SHA256_BLOCK_SIZE];
    unsigned char opad_block[SHA256_BLOCK_SIZE];

    for (int i = 0; i < SHA256_BLOCK_SIZE; i++)
    {
        ipad_block[i] = (unsigned char)(k_prime[i] ^ HMAC_IPAD);
        opad_block[i] = (unsigned char)(k_prime[i] ^ HMAC_OPAD);
    }

    /*
     * Step 3: inner hash = H(ipad || data).
     */
    size_t inner_len = SHA256_BLOCK_SIZE + data_len;

    unsigned char *inner = malloc(inner_len);

    if (inner == NULL)
    {
        return 0;
    }

    memcpy(inner, ipad_block, SHA256_BLOCK_SIZE);

    if (data_len > 0)
    {
        memcpy(inner + SHA256_BLOCK_SIZE, data, data_len);
    }

    unsigned char inner_hash[SHA256_DIGEST_SIZE];

    int ok = sha256(inner, inner_len, inner_hash);

    free(inner);

    if (!ok)
    {
        return 0;
    }

    /*
     * Step 4: outer hash = H(opad || inner_hash).
     */
    unsigned char outer[SHA256_BLOCK_SIZE + SHA256_DIGEST_SIZE];

    memcpy(outer, opad_block, SHA256_BLOCK_SIZE);
    memcpy(outer + SHA256_BLOCK_SIZE,
           inner_hash,
           SHA256_DIGEST_SIZE);

    return sha256(outer, sizeof(outer), out);
}

void hmac_sha256_hex(const unsigned char digest[SHA256_DIGEST_SIZE],
                     char out[65])
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }

    out[64] = '\0';
}
