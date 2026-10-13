/*
 * Unit test for the auth replay cache.
 *
 * Reset is called at the top of every test so cases are
 * independent. All timestamps are explicit; no wall-clock waits.
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

static const char *SECRET = "replay-test-secret-0123456789abcdef";

static void test_basic_replay_detection(void)
{
    auth_replay_cache_reset();

    const char *msg = "SUBMIT|4:echo|1|5:hello";
    time_t now = 1700000000;

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char recovered[AUTH_MAX_SIGNED_SIZE];

    auth_sign_at(SECRET,
                 AUTH_DIR_CLIENT_TO_COORD,
                 AUTH_ROLE_CLIENT,
                 msg,
                 now,
                 signed_buf,
                 sizeof(signed_buf));

    AuthResult r1 = auth_verify_at(SECRET,
                                   AUTH_DIR_CLIENT_TO_COORD,
                                   AUTH_ROLE_CLIENT,
                                   signed_buf,
                                   now,
                                   recovered,
                                   sizeof(recovered));

    CHECK(r1 == AUTH_OK, "first verification succeeds");

    AuthResult r2 = auth_verify_at(SECRET,
                                   AUTH_DIR_CLIENT_TO_COORD,
                                   AUTH_ROLE_CLIENT,
                                   signed_buf,
                                   now,
                                   recovered,
                                   sizeof(recovered));

    CHECK(r2 == AUTH_ERR_REPLAY_DETECTED,
          "identical second verification is a replay");
}

static void test_different_messages_not_replay(void)
{
    auth_replay_cache_reset();

    time_t now = 1700000000;

    char s1[AUTH_MAX_SIGNED_SIZE];
    char s2[AUTH_MAX_SIGNED_SIZE];
    char out[AUTH_MAX_SIGNED_SIZE];

    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|1:a", now, s1, sizeof(s1));
    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|1:b", now, s2, sizeof(s2));

    CHECK(auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                         s1, now, out, sizeof(out)) == AUTH_OK,
          "message 1 accepted");
    CHECK(auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                         s2, now, out, sizeof(out)) == AUTH_OK,
          "message 2 accepted (different MAC)");
}

static void test_different_timestamps_not_replay(void)
{
    auth_replay_cache_reset();

    time_t now = 1700000000;

    char s1[AUTH_MAX_SIGNED_SIZE];
    char s2[AUTH_MAX_SIGNED_SIZE];
    char out[AUTH_MAX_SIGNED_SIZE];

    /* Same message, one second apart. Different MACs because
     * the timestamp is part of the MAC input. */
    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|1:a", now, s1, sizeof(s1));
    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|1:a", now + 1, s2, sizeof(s2));

    CHECK(auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                         s1, now + 1, out, sizeof(out)) == AUTH_OK,
          "first timestamp accepted");
    CHECK(auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                         s2, now + 1, out, sizeof(out)) == AUTH_OK,
          "second timestamp accepted (different MAC)");
}

static void test_cache_full_rejects(void)
{
    auth_replay_cache_reset();

    time_t now = 1700000000;

    char msg[64];
    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char out[AUTH_MAX_SIGNED_SIZE];

    for (int i = 0; i < AUTH_REPLAY_CACHE_SIZE; i++)
    {
        snprintf(msg, sizeof(msg), "SUBMIT|4:echo|1|%d:x", i);

        auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                     msg, now, signed_buf, sizeof(signed_buf));

        AuthResult r = auth_verify_at(SECRET,
                                      AUTH_DIR_CLIENT_TO_COORD,
                                      AUTH_ROLE_CLIENT,
                                      signed_buf,
                                      now,
                                      out,
                                      sizeof(out));

        if (r != AUTH_OK)
        {
            printf("FAIL: fill loop failed at entry %d (%s)\n",
                   i,
                   auth_result_name(r));
            failures++;
            return;
        }
    }

    printf("PASS: filled cache with %d entries\n",
           AUTH_REPLAY_CACHE_SIZE);

    /* One more distinct message: should hit the full-cache path. */
    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|overflow:x",
                 now, signed_buf, sizeof(signed_buf));

    AuthResult r = auth_verify_at(SECRET,
                                  AUTH_DIR_CLIENT_TO_COORD,
                                  AUTH_ROLE_CLIENT,
                                  signed_buf,
                                  now,
                                  out,
                                  sizeof(out));

    CHECK(r == AUTH_ERR_REPLAY_CACHE_FULL,
          "overflow message rejected with cache_full");
}

static void test_expired_slots_reused(void)
{
    /*
     * Reuses the state left by test_cache_full_rejects, so must
     * run immediately after it and must not reset. To keep the
     * tests independent we re-fill here instead.
     */
    auth_replay_cache_reset();

    time_t now = 1700000000;

    char msg[64];
    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char out[AUTH_MAX_SIGNED_SIZE];

    for (int i = 0; i < AUTH_REPLAY_CACHE_SIZE; i++)
    {
        snprintf(msg, sizeof(msg), "SUBMIT|4:echo|1|%d:x", i);

        auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                     msg, now, signed_buf, sizeof(signed_buf));
        auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                       signed_buf, now, out, sizeof(out));
    }

    /* Advance past the window. All slots are now expired. */
    time_t later = now + AUTH_MAX_AGE_SECONDS + 10;

    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|fresh:x",
                 later, signed_buf, sizeof(signed_buf));

    AuthResult r = auth_verify_at(SECRET,
                                  AUTH_DIR_CLIENT_TO_COORD,
                                  AUTH_ROLE_CLIENT,
                                  signed_buf,
                                  later,
                                  out,
                                  sizeof(out));

    CHECK(r == AUTH_OK, "expired slots are reused");
}

static void test_reset_clears_cache(void)
{
    auth_replay_cache_reset();

    time_t now = 1700000000;

    char signed_buf[AUTH_MAX_SIGNED_SIZE];
    char out[AUTH_MAX_SIGNED_SIZE];

    auth_sign_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                 "SUBMIT|4:echo|1|1:a", now, signed_buf, sizeof(signed_buf));

    CHECK(auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                         signed_buf, now, out, sizeof(out)) == AUTH_OK,
          "first verify before reset succeeds");

    auth_replay_cache_reset();

    CHECK(auth_verify_at(SECRET, AUTH_DIR_CLIENT_TO_COORD, AUTH_ROLE_CLIENT,
                         signed_buf, now, out, sizeof(out)) == AUTH_OK,
          "same message succeeds after reset");
}

int main(void)
{
    test_basic_replay_detection();
    test_different_messages_not_replay();
    test_different_timestamps_not_replay();
    test_cache_full_rejects();
    test_expired_slots_reused();
    test_reset_clears_cache();

    if (failures == 0)
    {
        printf("\nauth_replay_test: PASS\n");
        return 0;
    }

    printf("\nauth_replay_test: FAIL (%d)\n", failures);
    return 1;
}
