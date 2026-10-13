/*
 * SHA-256 unit test.
 *
 * Verifies the implementation against NIST FIPS 180-4 test
 * vectors. If any of these fail, the implementation is wrong
 * and must be fixed before anything is layered on top of it.
 *
 * Vectors (from NIST's SHA test suite and the FIPS 180-4
 * examples):
 *
 *   ""                                    e3b0c442...b855
 *   "abc"                                 ba7816bf...5ad
 *   "abcdbcdecdefdefgefghfghighijhijk..." 248d6a61...6c1
 *   "abcdefghbcdefghicdefghijdefghijk..." cf5b16a7...9d1
 *   1,000,000 x 'a'                       cdc76e5c...cd0
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sha256.h"

static int failures = 0;

static int hex_equal(const unsigned char digest[SHA256_DIGEST_SIZE],
                     const char *expected_hex)
{
    char got[65];
    sha256_hex(digest, got);
    return strcmp(got, expected_hex) == 0;
}

static void check(const char *label,
                  const char *input,
                  size_t input_len,
                  const char *expected_hex)
{
    unsigned char digest[SHA256_DIGEST_SIZE];

    if (!sha256(input, input_len, digest))
    {
        printf("FAIL: %s — sha256 returned 0\n", label);
        failures++;
        return;
    }

    if (hex_equal(digest, expected_hex))
    {
        printf("PASS: %s\n", label);
    }
    else
    {
        char got[65];
        sha256_hex(digest, got);
        printf("FAIL: %s\n", label);
        printf("  expected: %s\n", expected_hex);
        printf("  got     : %s\n", got);
        failures++;
    }
}

int main(void)
{
    /* NIST vector 1: empty string */
    check("empty",
          "",
          0,
          "e3b0c44298fc1c149afbf4c8996fb924"
          "27ae41e4649b934ca495991b7852b855");

    /* NIST vector 2: "abc" */
    check("abc",
          "abc",
          3,
          "ba7816bf8f01cfea414140de5dae2223"
          "b00361a396177a9cb410ff61f20015ad");

    /* NIST vector 3: 56-byte message at the single-block
     * padding boundary */
    const char *v3 =
        "abcdbcdecdefdefgefghfghighijhijk"
        "ijkljklmklmnlmnomnopnopq";

    check("56-byte boundary",
          v3,
          strlen(v3),
          "248d6a61d20638b8e5c026930c3e6039"
          "a33ce45964ff2167f6ecedd419db06c1");

    /* NIST vector 4: 112-byte message at the double-block
     * padding boundary */
    const char *v4 =
        "abcdefghbcdefghicdefghijdefghijk"
        "efghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrs"
        "mnopqrstnopqrstu";

    check("112-byte boundary",
          v4,
          strlen(v4),
          "cf5b16a778af8380036ce59e7b049237"
          "0b249b11e8f07a51afac45037afee9d1");

    /* NIST vector 5: 1,000,000 x 'a'.
     *
     * Allocated dynamically because 1 MB on the stack is
     * unnecessarily large and some environments limit stack
     * size below that. */
    size_t big_len = 1000000;
    char *big = malloc(big_len);

    if (big == NULL)
    {
        printf("FAIL: could not allocate 1MB buffer\n");
        failures++;
    }
    else
    {
        memset(big, 'a', big_len);

        check("1,000,000 x 'a'",
              big,
              big_len,
              "cdc76e5c9914fb9281a1c7e284d73e67"
              "f1809a48a497200e046d39ccc7112cd0");

        free(big);
    }

    /* API robustness: NULL out returns 0 */
    if (sha256("x", 1, NULL) != 0)
    {
        printf("FAIL: sha256 with NULL out should return 0\n");
        failures++;
    }
    else
    {
        printf("PASS: sha256 with NULL out returns 0\n");
    }

    /* API robustness: NULL data with nonzero length returns 0 */
    unsigned char dummy[SHA256_DIGEST_SIZE];

    if (sha256(NULL, 1, dummy) != 0)
    {
        printf("FAIL: sha256 with NULL data and length > 0 "
               "should return 0\n");
        failures++;
    }
    else
    {
        printf("PASS: sha256 with NULL data and length > 0 "
               "returns 0\n");
    }

    /* API robustness: NULL data with length 0 is valid and
     * produces the empty-string digest */
    if (!sha256(NULL, 0, dummy) ||
        !hex_equal(dummy,
                   "e3b0c44298fc1c149afbf4c8996fb924"
                   "27ae41e4649b934ca495991b7852b855"))
    {
        printf("FAIL: sha256(NULL, 0) should equal the "
               "empty digest\n");
        failures++;
    }
    else
    {
        printf("PASS: sha256(NULL, 0) equals the empty digest\n");
    }

    if (failures == 0)
    {
        printf("\nsha256_test: PASS\n");
        return 0;
    }

    printf("\nsha256_test: FAIL (%d)\n", failures);
    return 1;
}
