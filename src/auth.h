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
 * Both the sender and the receiver must agree on which
 * <direction> and <role> apply. The receiver chooses them
 * from the message type and its own role; the sender chooses
 * them from the message type and its own role. If they
 * disagree, the MACs will not match.
 *
 * This module authenticates messages. It does not encrypt
 * them. Traffic is still plaintext; TLS is out of scope.
 */

/*
 * Maximum size of a signed message. Bounded by the network
 * transport: the coordinator's framing layer rejects any
 * payload larger than NETWORK_MAX_MESSAGE_SIZE. The 128-byte
 * margin covers "|<ts>|<64 hex>" plus safety room.
 */
#define AUTH_MAX_SIGNED_SIZE (NETWORK_MAX_MESSAGE_SIZE)

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
    AUTH_ERR_MAC_MISMATCH
} AuthResult;

/*
 * Sign a message using the current wall-clock time.
 *
 * out must point to a buffer of at least AUTH_MAX_SIGNED_SIZE
 * bytes. On success, out contains the signed wire form and
 * is NUL-terminated.
 *
 * Returns AUTH_OK on success.
 * Returns AUTH_ERR_INVALID_INPUT on NULL secret, NULL message,
 * NULL out, or a message that would produce a signed result
 * larger than AUTH_MAX_SIGNED_SIZE.
 * Returns AUTH_ERR_OUTPUT_TOO_SMALL if out_size cannot hold
 * the result.
 */
AuthResult auth_sign(const char *secret,
                     AuthDirection direction,
                     AuthRole signer_role,
                     const char *message,
                     char *out,
                     size_t out_size);

/*
 * Like auth_sign, but uses the provided timestamp. Intended
 * for unit tests that need to exercise the timestamp window
 * without waiting wall-clock time.
 */
AuthResult auth_sign_at(const char *secret,
                        AuthDirection direction,
                        AuthRole signer_role,
                        const char *message,
                        time_t timestamp,
                        char *out,
                        size_t out_size);

/*
 * Verify a signed message.
 *
 * message_out must point to a buffer of at least
 * AUTH_MAX_SIGNED_SIZE bytes. On AUTH_OK, it contains the
 * original (unsigned) message and is NUL-terminated.
 *
 * Returns AUTH_OK on success.
 * Returns AUTH_ERR_INVALID_INPUT on NULL arguments or zero
 * sized buffers.
 * Returns AUTH_ERR_OUTPUT_TOO_SMALL if message_out_size cannot
 * hold the recovered message.
 * Returns AUTH_ERR_MALFORMED if the signed structure cannot be
 * parsed (missing separator, wrong hex length, non-hex MAC
 * characters, non-digit timestamp).
 * Returns AUTH_ERR_TIMESTAMP_RANGE if the timestamp is outside
 * the accepted window.
 * Returns AUTH_ERR_MAC_MISMATCH if the MAC does not match.
 */
AuthResult auth_verify(const char *secret,
                       AuthDirection expected_direction,
                       AuthRole expected_signer_role,
                       const char *signed_message,
                       char *message_out,
                       size_t message_out_size);

/*
 * Like auth_verify, but treats "now" as the provided time.
 * Intended for unit tests.
 */
AuthResult auth_verify_at(const char *secret,
                          AuthDirection expected_direction,
                          AuthRole expected_signer_role,
                          const char *signed_message,
                          time_t now,
                          char *message_out,
                          size_t message_out_size);

/*
 * Return a stable human-readable name for an AuthResult.
 * Never returns NULL.
 */
const char *auth_result_name(AuthResult result);

#endif
