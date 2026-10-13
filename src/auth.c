#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "hmac.h"

#define AUTH_MAX_AGE_SECONDS    60
#define AUTH_MAX_FUTURE_SECONDS 5

#define AUTH_MAGIC      "IDR-AUTH-V1"
#define AUTH_HEX_LEN    64

/*
 * Recent-MAC cache. Populated only after a message passes MAC
 * verification, so unauthenticated input cannot poison it.
 *
 * Zero-initialized statics are equivalent to an empty cache:
 * in_use == 0 means the slot is free.
 */
struct ReplayEntry
{
    unsigned char mac[SHA256_DIGEST_SIZE];
    time_t inserted_at;
    int in_use;
};

static struct ReplayEntry g_replay_cache[AUTH_REPLAY_CACHE_SIZE];

void auth_replay_cache_reset(void)
{
    memset(g_replay_cache, 0, sizeof(g_replay_cache));
}

/*
 * Look for mac in the cache. Returns 1 on hit, 0 on miss.
 * Constant-time within each entry, but the scan itself is not;
 * cache content is not secret, so this is fine.
 */
static int replay_cache_lookup(const unsigned char mac[SHA256_DIGEST_SIZE])
{
    for (int i = 0; i < AUTH_REPLAY_CACHE_SIZE; i++)
    {
        if (!g_replay_cache[i].in_use)
        {
            continue;
        }

        unsigned char diff = 0;

        for (int j = 0; j < SHA256_DIGEST_SIZE; j++)
        {
            diff |= (unsigned char)(g_replay_cache[i].mac[j] ^ mac[j]);
        }

        if (diff == 0)
        {
            return 1;
        }
    }

    return 0;
}

/*
 * Pick a slot for a new entry. Preference order:
 *   1. Any unused slot.
 *   2. A slot whose entry is older than the timestamp window.
 * If neither exists, return -1.
 */
static int replay_cache_find_slot(time_t now)
{
    for (int i = 0; i < AUTH_REPLAY_CACHE_SIZE; i++)
    {
        if (!g_replay_cache[i].in_use)
        {
            return i;
        }
    }

    for (int i = 0; i < AUTH_REPLAY_CACHE_SIZE; i++)
    {
        double age = difftime(now, g_replay_cache[i].inserted_at);

        if (age > (double)AUTH_MAX_AGE_SECONDS)
        {
            return i;
        }
    }

    return -1;
}

static void replay_cache_insert(int slot,
                                const unsigned char mac[SHA256_DIGEST_SIZE],
                                time_t now)
{
    memcpy(g_replay_cache[slot].mac, mac, SHA256_DIGEST_SIZE);
    g_replay_cache[slot].inserted_at = now;
    g_replay_cache[slot].in_use = 1;
}

static const char *direction_name(AuthDirection direction)
{
    switch (direction)
    {
        case AUTH_DIR_WORKER_TO_COORD:  return "worker-to-coordinator";
        case AUTH_DIR_COORD_TO_WORKER:  return "coordinator-to-worker";
        case AUTH_DIR_CLIENT_TO_COORD:  return "client-to-coordinator";
        case AUTH_DIR_COORD_TO_CLIENT:  return "coordinator-to-client";
    }

    return NULL;
}

static const char *role_name(AuthRole role)
{
    switch (role)
    {
        case AUTH_ROLE_WORKER:      return "worker";
        case AUTH_ROLE_CLIENT:      return "client";
        case AUTH_ROLE_COORDINATOR: return "coordinator";
    }

    return NULL;
}

static int hex_char_to_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;

    return -1;
}

AuthResult auth_sign_at(const char *secret,
                        AuthDirection direction,
                        AuthRole signer_role,
                        const char *message,
                        time_t timestamp,
                        char *out,
                        size_t out_size)
{
    if (secret == NULL || message == NULL || out == NULL)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    const char *dir = direction_name(direction);
    const char *role = role_name(signer_role);

    if (dir == NULL || role == NULL)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    if (timestamp < 0)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    char ts_str[32];
    int n = snprintf(ts_str, sizeof(ts_str),
                     "%llu",
                     (unsigned long long)timestamp);

    if (n <= 0 || (size_t)n >= sizeof(ts_str))
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    char mac_input[AUTH_MAX_SIGNED_SIZE + 128];
    int m = snprintf(mac_input, sizeof(mac_input),
                     "%s|%s|%s|%s|%s",
                     AUTH_MAGIC,
                     dir,
                     role,
                     ts_str,
                     message);

    if (m <= 0 || (size_t)m >= sizeof(mac_input))
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    unsigned char mac[SHA256_DIGEST_SIZE];

    if (!hmac_sha256(secret, strlen(secret),
                     mac_input, (size_t)m,
                     mac))
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    char hex[AUTH_HEX_LEN + 1];
    hmac_sha256_hex(mac, hex);

    int w = snprintf(out, out_size,
                     "%s|%s|%s",
                     message,
                     ts_str,
                     hex);

    if (w <= 0)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    if ((size_t)w >= out_size)
    {
        return AUTH_ERR_OUTPUT_TOO_SMALL;
    }

    if ((size_t)w > AUTH_MAX_SIGNED_SIZE)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    return AUTH_OK;
}

AuthResult auth_sign(const char *secret,
                     AuthDirection direction,
                     AuthRole signer_role,
                     const char *message,
                     char *out,
                     size_t out_size)
{
    return auth_sign_at(secret,
                        direction,
                        signer_role,
                        message,
                        time(NULL),
                        out,
                        out_size);
}

AuthResult auth_verify_at(const char *secret,
                          AuthDirection expected_direction,
                          AuthRole expected_signer_role,
                          const char *signed_message,
                          time_t now,
                          char *message_out,
                          size_t message_out_size)
{
    if (secret == NULL || signed_message == NULL ||
        message_out == NULL || message_out_size == 0)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    const char *dir = direction_name(expected_direction);
    const char *role = role_name(expected_signer_role);

    if (dir == NULL || role == NULL)
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    size_t len = strlen(signed_message);

    if (len == 0 || len > AUTH_MAX_SIGNED_SIZE)
    {
        return AUTH_ERR_MALFORMED;
    }

    char scratch[AUTH_MAX_SIGNED_SIZE + 1];
    memcpy(scratch, signed_message, len + 1);

    char *last = strrchr(scratch, '|');

    if (last == NULL)
    {
        return AUTH_ERR_MALFORMED;
    }

    *last = '\0';
    const char *hex = last + 1;

    if (strlen(hex) != AUTH_HEX_LEN)
    {
        return AUTH_ERR_MALFORMED;
    }

    for (size_t i = 0; i < AUTH_HEX_LEN; i++)
    {
        if (hex_char_to_val(hex[i]) < 0)
        {
            return AUTH_ERR_MALFORMED;
        }
    }

    char *second = strrchr(scratch, '|');

    if (second == NULL)
    {
        return AUTH_ERR_MALFORMED;
    }

    *second = '\0';
    const char *ts_str = second + 1;
    const char *message = scratch;

    if (ts_str[0] == '\0')
    {
        return AUTH_ERR_MALFORMED;
    }

    size_t ts_len = strlen(ts_str);

    if (ts_len > 20)
    {
        return AUTH_ERR_MALFORMED;
    }

    for (size_t i = 0; i < ts_len; i++)
    {
        if (ts_str[i] < '0' || ts_str[i] > '9')
        {
            return AUTH_ERR_MALFORMED;
        }
    }

    errno = 0;
    char *endp = NULL;
    unsigned long long ts_u = strtoull(ts_str, &endp, 10);

    if (errno != 0 || endp == ts_str || *endp != '\0')
    {
        return AUTH_ERR_MALFORMED;
    }

    if (ts_u > (unsigned long long)LLONG_MAX)
    {
        return AUTH_ERR_TIMESTAMP_RANGE;
    }

    long long ts = (long long)ts_u;
    long long now_ll = (long long)now;

    if (ts > now_ll + AUTH_MAX_FUTURE_SECONDS)
    {
        return AUTH_ERR_TIMESTAMP_RANGE;
    }

    if (now_ll - ts > AUTH_MAX_AGE_SECONDS)
    {
        return AUTH_ERR_TIMESTAMP_RANGE;
    }

    char mac_input[AUTH_MAX_SIGNED_SIZE + 128];
    int m = snprintf(mac_input, sizeof(mac_input),
                     "%s|%s|%s|%s|%s",
                     AUTH_MAGIC,
                     dir,
                     role,
                     ts_str,
                     message);

    if (m <= 0 || (size_t)m >= sizeof(mac_input))
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    unsigned char expected_mac[SHA256_DIGEST_SIZE];

    if (!hmac_sha256(secret, strlen(secret),
                     mac_input, (size_t)m,
                     expected_mac))
    {
        return AUTH_ERR_INVALID_INPUT;
    }

    char expected_hex[AUTH_HEX_LEN + 1];
    hmac_sha256_hex(expected_mac, expected_hex);

    unsigned char diff = 0;

    for (size_t i = 0; i < AUTH_HEX_LEN; i++)
    {
        diff |= (unsigned char)(hex[i] ^ expected_hex[i]);
    }

    if (diff != 0)
    {
        return AUTH_ERR_MAC_MISMATCH;
    }

    /*
     * Replay check. Only reached after the MAC has matched,
     * so unauthenticated input cannot populate the cache.
     */
    if (replay_cache_lookup(expected_mac))
    {
        return AUTH_ERR_REPLAY_DETECTED;
    }

    int slot = replay_cache_find_slot(now);

    if (slot < 0)
    {
        return AUTH_ERR_REPLAY_CACHE_FULL;
    }

    replay_cache_insert(slot, expected_mac, now);

    size_t msg_len = strlen(message);

    if (msg_len >= message_out_size)
    {
        return AUTH_ERR_OUTPUT_TOO_SMALL;
    }

    memcpy(message_out, message, msg_len + 1);

    return AUTH_OK;
}

AuthResult auth_verify(const char *secret,
                       AuthDirection expected_direction,
                       AuthRole expected_signer_role,
                       const char *signed_message,
                       char *message_out,
                       size_t message_out_size)
{
    return auth_verify_at(secret,
                          expected_direction,
                          expected_signer_role,
                          signed_message,
                          time(NULL),
                          message_out,
                          message_out_size);
}

const char *auth_result_name(AuthResult result)
{
    switch (result)
    {
        case AUTH_OK:                       return "ok";
        case AUTH_ERR_INVALID_INPUT:        return "invalid_input";
        case AUTH_ERR_OUTPUT_TOO_SMALL:     return "output_too_small";
        case AUTH_ERR_MALFORMED:            return "malformed";
        case AUTH_ERR_TIMESTAMP_RANGE:      return "timestamp_range";
        case AUTH_ERR_MAC_MISMATCH:         return "mac_mismatch";
        case AUTH_ERR_REPLAY_DETECTED:      return "replay_detected";
        case AUTH_ERR_REPLAY_CACHE_FULL:    return "replay_cache_full";
    }

    return "unknown";
}
