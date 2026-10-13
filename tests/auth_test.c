/*
 * Unit test for the auth module.
 *
 * Covers signing, verification, every failure mode the module
 * distinguishes, and replay detection.
 *
 * Uses auth_sign_at / auth_verify_at where determinism matters,
 * and the wall-clock variants elsewhere.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "auth.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else      { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_roundtrip(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    AuthResult r = auth_sign(secret,
                             AUTH_DIR_CLIENT_TO_COORD,
                             AUTH_ROLE_CLIENT,
                             msg,
                             signed_buf,
                             sizeof(signed_buf));

    CHECK(r == AUTH_OK, "sign returns AUTH_OK");

    r = auth_verify(secret,
                    AUTH_DIR_CLIENT_TO_COORD,
                    AUTH_ROLE_CLIENT,
                    signed_buf,
                    recovered,
                    sizeof(recovered));

    CHECK(r == AUTH_OK, "verify returns AUTH_OK");
    CHECK(strcmp(recovered, msg) == 0, "recovered message matches");
}

static void test_tamper_message(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign(secret,
              AUTH_DIR_CLIENT_TO_COORD,
              AUTH_ROLE_CLIENT,
              msg,
              signed_buf,
              sizeof(signed_buf));

    signed_buf[0] = 'X';

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               signed_buf,
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MAC_MISMATCH, "tampered message rejected");
}

static void test_tamper_mac(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign(secret,
              AUTH_DIR_CLIENT_TO_COORD,
              AUTH_ROLE_CLIENT,
              msg,
              signed_buf,
              sizeof(signed_buf));

    size_t len = strlen(signed_buf);
    signed_buf[len - 1] = (signed_buf[len - 1] == 'a') ? 'b' : 'a';

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               signed_buf,
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MAC_MISMATCH, "tampered MAC rejected");
}

static void test_wrong_secret(void)
{
    const char *signing_secret = "signing-secret-000000000000";
    const char *other_secret   = "other-secret-11111111111111";
    const char *msg = "SUBMIT|4:echo|1|5:hello";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign(signing_secret,
              AUTH_DIR_CLIENT_TO_COORD,
              AUTH_ROLE_CLIENT,
              msg,
              signed_buf,
              sizeof(signed_buf));

    AuthResult r = auth_verify(other_secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               signed_buf,
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MAC_MISMATCH, "wrong secret rejected");
}

static void test_wrong_direction(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "REGISTER|1|800.00|7710";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign(secret,
              AUTH_DIR_WORKER_TO_COORD,
              AUTH_ROLE_WORKER,
              msg,
              signed_buf,
              sizeof(signed_buf));

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_COORD_TO_WORKER,
                               AUTH_ROLE_COORDINATOR,
                               signed_buf,
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MAC_MISMATCH, "wrong direction rejected");
}

static void test_wrong_role(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "REGISTER|1|800.00|7710";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign(secret,
              AUTH_DIR_WORKER_TO_COORD,
              AUTH_ROLE_WORKER,
              msg,
              signed_buf,
              sizeof(signed_buf));

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_WORKER_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               signed_buf,
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MAC_MISMATCH, "wrong signer role rejected");
}

static void test_old_timestamp(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";
    time_t now = 1700000000;

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign_at(secret,
                 AUTH_DIR_CLIENT_TO_COORD,
                 AUTH_ROLE_CLIENT,
                 msg,
                 now - 120,
                 signed_buf,
                 sizeof(signed_buf));

    AuthResult r = auth_verify_at(secret,
                                  AUTH_DIR_CLIENT_TO_COORD,
                                  AUTH_ROLE_CLIENT,
                                  signed_buf,
                                  now,
                                  recovered,
                                  sizeof(recovered));

    CHECK(r == AUTH_ERR_TIMESTAMP_RANGE,
          "old timestamp rejected");
}

static void test_future_timestamp(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";
    time_t now = 1700000000;

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign_at(secret,
                 AUTH_DIR_CLIENT_TO_COORD,
                 AUTH_ROLE_CLIENT,
                 msg,
                 now + 60,
                 signed_buf,
                 sizeof(signed_buf));

    AuthResult r = auth_verify_at(secret,
                                  AUTH_DIR_CLIENT_TO_COORD,
                                  AUTH_ROLE_CLIENT,
                                  signed_buf,
                                  now,
                                  recovered,
                                  sizeof(recovered));

    CHECK(r == AUTH_ERR_TIMESTAMP_RANGE,
          "future timestamp rejected");
}

static void test_short_skew_accepted(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";
    time_t now = 1700000000;

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign_at(secret,
                 AUTH_DIR_CLIENT_TO_COORD,
                 AUTH_ROLE_CLIENT,
                 msg,
                 now + 3,
                 signed_buf,
                 sizeof(signed_buf));

    AuthResult r = auth_verify_at(secret,
                                  AUTH_DIR_CLIENT_TO_COORD,
                                  AUTH_ROLE_CLIENT,
                                  signed_buf,
                                  now,
                                  recovered,
                                  sizeof(recovered));

    CHECK(r == AUTH_OK, "small forward skew accepted");
}

static void test_missing_separator(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    char recovered[AUTH_MAX_SIGNED_SIZE];

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               "no-separators-here",
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MALFORMED,
          "message with no separator rejected");
}

static void test_bad_hex_length(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    char recovered[AUTH_MAX_SIGNED_SIZE];

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               "SUBMIT|4:echo|1700000000|abc",
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MALFORMED, "short hex field rejected");
}

static void test_bad_hex_char(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    char recovered[AUTH_MAX_SIGNED_SIZE];

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               "SUBMIT|4:echo|1700000000|"
                               "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
                               "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MALFORMED, "non-hex MAC char rejected");
}

static void test_bad_timestamp_digit(void)
{
    const char *secret = "test-secret-0123456789abcdef";
    char recovered[AUTH_MAX_SIGNED_SIZE];

    AuthResult r = auth_verify(secret,
                               AUTH_DIR_CLIENT_TO_COORD,
                               AUTH_ROLE_CLIENT,
                               "SUBMIT|4:echo|17x0000000|"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                               recovered,
                               sizeof(recovered));

    CHECK(r == AUTH_ERR_MALFORMED,
          "non-digit timestamp rejected");
}

static void test_null_inputs(void)
{
    char buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    CHECK(auth_sign(NULL, AUTH_DIR_CLIENT_TO_COORD,
                    AUTH_ROLE_CLIENT, "x", buf, sizeof(buf))
          == AUTH_ERR_INVALID_INPUT,
          "sign with NULL secret rejected");

    CHECK(auth_sign("s", AUTH_DIR_CLIENT_TO_COORD,
                    AUTH_ROLE_CLIENT, NULL, buf, sizeof(buf))
          == AUTH_ERR_INVALID_INPUT,
          "sign with NULL message rejected");

    CHECK(auth_sign("s", AUTH_DIR_CLIENT_TO_COORD,
                    AUTH_ROLE_CLIENT, "x", NULL, 0)
          == AUTH_ERR_INVALID_INPUT,
          "sign with NULL out rejected");

    CHECK(auth_verify(NULL, AUTH_DIR_CLIENT_TO_COORD,
                      AUTH_ROLE_CLIENT, "x", recovered, sizeof(recovered))
          == AUTH_ERR_INVALID_INPUT,
          "verify with NULL secret rejected");

    CHECK(auth_verify("s", AUTH_DIR_CLIENT_TO_COORD,
                      AUTH_ROLE_CLIENT, NULL, recovered, sizeof(recovered))
          == AUTH_ERR_INVALID_INPUT,
          "verify with NULL signed message rejected");
}

static void test_replay_detection(void)
{
    auth_replay_cache_reset();

    const char *secret = "test-secret-0123456789abcdef";
    const char *msg = "SUBMIT|4:echo|1|5:hello";

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign(secret,
              AUTH_DIR_CLIENT_TO_COORD,
              AUTH_ROLE_CLIENT,
              msg,
              signed_buf,
              sizeof(signed_buf));

    AuthResult r1 = auth_verify(secret,
                                AUTH_DIR_CLIENT_TO_COORD,
                                AUTH_ROLE_CLIENT,
                                signed_buf,
                                recovered,
                                sizeof(recovered));

    CHECK(r1 == AUTH_OK, "replay test: first verify accepted");

    AuthResult r2 = auth_verify(secret,
                                AUTH_DIR_CLIENT_TO_COORD,
                                AUTH_ROLE_CLIENT,
                                signed_buf,
                                recovered,
                                sizeof(recovered));

    CHECK(r2 == AUTH_ERR_REPLAY_DETECTED,
          "replay test: identical second verify rejected");
}

int main(void)
{
    auth_replay_cache_reset();

    test_roundtrip();
    test_tamper_message();
    test_tamper_mac();
    test_wrong_secret();
    test_wrong_direction();
    test_wrong_role();
    test_old_timestamp();
    test_future_timestamp();
    test_short_skew_accepted();
    test_missing_separator();
    test_bad_hex_length();
    test_bad_hex_char();
    test_bad_timestamp_digit();
    test_null_inputs();
    test_replay_detection();

    if (failures == 0)
    {
        printf("\nauth_test: PASS\n");
        return 0;
    }

    printf("\nauth_test: FAIL (%d)\n", failures);
    return 1;
}
