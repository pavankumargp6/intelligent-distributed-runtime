#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>
#include <time.h>

#include "network_protocol.h"

/*
 * Authentication primitives for the IDR wire protocol.
 *
 * A signed message has this wire form:
 *
 *   <original_message>|<timestamp>|<hex_hmac>
 *
 * where <hex_hmac> is 64 lowercase hex characters: the
 * HMAC-SHA256 of
 *
 *   IDR-AUTH-V1|<direction>|<role>|<timestamp>|<original_message>
 *
 * computed with a role-specific shared secret.
 *
 * This module authenticates messages. It does not encrypt
 * them. Traffic is still plaintext; TLS is out of scope.
 *
 * Replay protection: after a message passes MAC verification,
 * its MAC is recorded in a bounded in-process cache. A second
 * verification of a byte-identical message within
 * AUTH_MAX_AGE_SECONDS is rejected as a replay. The cache is
 * shared across all calls in a process; the coordinator is the
 * only verifier, so this is sufficient.
 */

#define AUTH_MAX_SIGNED_SIZE (NETWORK_MAX_MESSAGE_SIZE)

/*
 * Accepted timestamp window. A signed message whose timestamp
 * is more than AUTH_MAX_AGE_SECONDS in the past, or more than
 * AUTH_MAX_FUTURE_SECONDS in the future, is rejected before
 * MAC verification.
 *
 * The "future" margin covers modest clock skew between hosts.
 * These values also define how long a MAC must remain in the
 * replay cache to keep replay detection meaningful.
 */
#define AUTH_MAX_AGE_SECONDS    60
#define AUTH_MAX_FUTURE_SECONDS 5

/*
 * Capacity of the recent-MAC cache. The cache holds at most
 * this many entries; when all slots hold unexpired MACs, a new
 * verification returns AUTH_ERR_REPLAY_CACHE_FULL rather than
 * silently disabling replay checks.
 *
 * AUTH_MAX_AGE_SECONDS is 60. 8192 entries sustain roughly 136
 * verifications per second continuously. Above that, the
 * coordinator rejects until slots expire.
 */
#define AUTH_REPLAY_CACHE_SIZE 8192

typedef enum
{
    AUTH_DIR_WORKER_TO_COORD = 0,
    AUTH_DIR_COORD_TO_WORKER = 1,
    AUTH_DIR_CLIENT_TO_COORD = 2,
    AUTH_DIR_COORD_TO_CLIENT = 3
} AuthDirection;

typedef enum
{
    AUTH_ROLE_WORKER      = 0,
    AUTH_ROLE_CLIENT      = 1,
    AUTH_ROLE_COORDINATOR = 2
} AuthRole;

typedef enum
{
    AUTH_OK = 0,
    AUTH_ERR_INVALID_INPUT,
    AUTH_ERR_OUTPUT_TOO_SMALL,
    AUTH_ERR_MALFORMED,
    AUTH_ERR_TIMESTAMP_RANGE,
    AUTH_ERR_MAC_MISMATCH,
    AUTH_ERR_REPLAY_DETECTED,
    AUTH_ERR_REPLAY_CACHE_FULL
} AuthResult;

AuthResult auth_sign(const char *secret,
                     AuthDirection direction,
                     AuthRole signer_role,
                     const char *message,
                     char *out,
                     size_t out_size);

AuthResult auth_sign_at(const char *secret,
                        AuthDirection direction,
                        AuthRole signer_role,
                        const char *message,
                        time_t timestamp,
                        char *out,
                        size_t out_size);

AuthResult auth_verify(const char *secret,
                       AuthDirection expected_direction,
                       AuthRole expected_signer_role,
                       const char *signed_message,
                       char *message_out,
                       size_t message_out_size);

AuthResult auth_verify_at(const char *secret,
                          AuthDirection expected_direction,
                          AuthRole expected_signer_role,
                          const char *signed_message,
                          time_t now,
                          char *message_out,
                          size_t message_out_size);

/*
 * Clear the replay cache. Intended for unit tests so that
 * successive test cases start from a known state. The
 * production code path never needs to call this.
 */
void auth_replay_cache_reset(void);

const char *auth_result_name(AuthResult result);

#endif
