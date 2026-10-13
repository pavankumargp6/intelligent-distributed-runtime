/*
 * HMAC-SHA256 unit test.
 *
 * Verifies against RFC 4231 test cases 1, 2, 3, 4, 6, and 7.
 * Case 5 (truncated 128-bit output) is skipped because IDR
 * uses the full 256-bit MAC; truncation is not implemented
 * and not required.
 *
 * If any of these fail, the implementation is wrong and
 * must be fixed before anything is layered on top of it.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hmac.h"
#include "sha256.h"

static int failures = 0;

static void check_hex(const char *label,
                      const char *got_hex,
                      const char *expected_hex)
{
    if (strcmp(got_hex, expected_hex) == 0)
    {
        printf("PASS: %s\n", label);
    }
    else
    {
        printf("FAIL: %s\n", label);
        printf("  expected: %s\n", expected_hex);
        printf("  got     : %s\n", got_hex);
        failures++;
    }
}

static void test_case_1(void)
{
    unsigned char key[20];
    memset(key, 0x0b, sizeof(key));

    const char *data = "Hi There";

    unsigned char mac[SHA256_DIGEST_SIZE];
    char hex[65];

    if (!hmac_sha256(key, sizeof(key),
                     data, strlen(data),
                     mac))
    {
        printf("FAIL: RFC 4231 case 1 hmac returned 0\n");
        failures++;
        return;
    }

    hmac_sha256_hex(mac, hex);
    check_hex("RFC 4231 case 1",
              hex,
              "b0344c61d8db38535ca8afceaf0bf12b"
              "881dc200c9833da726e9376c2e32cff7");
}

static void test_case_2(void)
{
    const char *key = "Jefe";
    const char *data = "what do ya want for nothing?";

    unsigned char mac[SHA256_DIGEST_SIZE];
    char hex[65];

    if (!hmac_sha256(key, strlen(key),
                     data, strlen(data),
                     mac))
    {
        printf("FAIL: RFC 4231 case 2 hmac returned 0\n");
        failures++;
        return;
    }

    hmac_sha256_hex(mac, hex);
    check_hex("RFC 4231 case 2",
              hex,
              "5bdcc146bf60754e6a042426089575c7"
              "5a003f089d2739839dec58b964ec3843");
}

static void test_case_3(void)
{
    unsigned char key[20];
    memset(key, 0xaa, sizeof(key));

    unsigned char data[50];
    memset(data, 0xdd, sizeof(data));

    unsigned char mac[SHA256_DIGEST_SIZE];
    char hex[65];

    if (!hmac_sha256(key, sizeof(key),
                     data, sizeof(data),
                     mac))
    {
        printf("FAIL: RFC 4231 case 3 hmac returned 0\n");
        failures++;
        return;
    }

    hmac_sha256_hex(mac, hex);
    check_hex("RFC 4231 case 3",
              hex,
              "773ea91e36800e46854db8ebd09181a7"
              "2959098b3ef8c122d9635514ced565fe");
}

static void test_case_4(void)
{
    unsigned char key[25] = {
        0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0x19
    };

    unsigned char data[50];
    memset(data, 0xcd, sizeof(data));

    unsigned char mac[SHA256_DIGEST_SIZE];
    char hex[65];

    if (!hmac_sha256(key, sizeof(key),
                     data, sizeof(data),
                     mac))
    {
        printf("FAIL: RFC 4231 case 4 hmac returned 0\n");
        failures++;
        return;
    }

    hmac_sha256_hex(mac, hex);
    check_hex("RFC 4231 case 4",
              hex,
              "82558a389a443c0ea4cc819899f2083a"
              "85f0faa3e578f8077a2e3ff46729665b");
}

static void test_case_6(void)
{
    /* Key longer than block size (131 > 64) */
    unsigned char key[131];
    memset(key, 0xaa, sizeof(key));

    const char *data =
        "Test Using Larger Than Block-Size Key - Hash Key First";

    unsigned char mac[SHA256_DIGEST_SIZE];
    char hex[65];

    if (!hmac_sha256(key, sizeof(key),
                     data, strlen(data),
                     mac))
    {
        printf("FAIL: RFC 4231 case 6 hmac returned 0\n");
        failures++;
        return;
    }

    hmac_sha256_hex(mac, hex);
    check_hex("RFC 4231 case 6",
              hex,
              "60e431591ee0b67f0d8a26aacbf5b77f"
              "8e0bc6213728c5140546040f0ee37f54");
}

static void test_case_7(void)
{
    unsigned char key[131];
    memset(key, 0xaa, sizeof(key));

    const char *data =
        "This is a test using a larger than block-size key and a "
        "larger than block-size data. The key needs to be hashed "
        "before being used by the HMAC algorithm.";

    unsigned char mac[SHA256_DIGEST_SIZE];
    char hex[65];

    if (!hmac_sha256(key, sizeof(key),
                     data, strlen(data),
                     mac))
    {
        printf("FAIL: RFC 4231 case 7 hmac returned 0\n");
        failures++;
        return;
    }

    hmac_sha256_hex(mac, hex);
    check_hex("RFC 4231 case 7",
              hex,
              "9b09ffa71b942fcb27635fbcd5b0e944"
              "bfdc63644f0713938a7f51535c3a35e2");
}

static void test_api_robustness(void)
{
    unsigned char mac[SHA256_DIGEST_SIZE];

    if (hmac_sha256("k", 1, "d", 1, NULL) != 0)
    {
        printf("FAIL: hmac with NULL out should return 0\n");
        failures++;
    }
    else
    {
        printf("PASS: hmac with NULL out returns 0\n");
    }

    if (hmac_sha256(NULL, 1, "d", 1, mac) != 0)
    {
        printf("FAIL: hmac with NULL key and key_len > 0 "
               "should return 0\n");
        failures++;
    }
    else
    {
        printf("PASS: hmac with NULL key and key_len > 0 "
               "returns 0\n");
    }

    if (hmac_sha256("k", 1, NULL, 1, mac) != 0)
    {
        printf("FAIL: hmac with NULL data and data_len > 0 "
               "should return 0\n");
        failures++;
    }
    else
    {
        printf("PASS: hmac with NULL data and data_len > 0 "
               "returns 0\n");
    }

    /* Empty key, empty data is valid — HMAC of empty inputs. */
    if (!hmac_sha256(NULL, 0, NULL, 0, mac))
    {
        printf("FAIL: hmac with empty key and empty data "
               "should succeed\n");
        failures++;
    }
    else
    {
        char hex[65];
        hmac_sha256_hex(mac, hex);

        /* RFC 2104 does not specify this case; this is the
         * value produced by the reference OpenSSL
         * implementation for HMAC-SHA256 with empty key and
         * empty message. */
        check_hex("empty key, empty data",
                  hex,
                  "b613679a0814d9ec772f95d778c35fc5"
                  "ff1697c493715653c6c712144292c5ad");
    }
}

int main(void)
{
    test_case_1();
    test_case_2();
    test_case_3();
    test_case_4();
    test_case_6();
    test_case_7();
    test_api_robustness();

    if (failures == 0)
    {
        printf("\nhmac_test: PASS\n");
        return 0;
    }

    printf("\nhmac_test: FAIL (%d)\n", failures);
    return 1;
}
