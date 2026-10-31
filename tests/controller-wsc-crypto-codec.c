/* The encrypted-settings codec on its own: 0x1018 is IV(16) || AES-128-CBC(PKCS#7), and the lab only proves
 * the shape the daemon accepted for one plaintext length. These cases pin the boundaries the factory's
 * eap_wps_process_message_M2 depends on - an aligned plaintext must still grow a whole block, and the last
 * plaintext byte is the padding length. No key material is printed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../controller-wsc-crypto.h"

static unsigned int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "crypto codec failure line %d\n", __LINE__); exit(1); } } while (0)

static uint8_t key[32] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                          0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
static uint8_t wrong[32] = {0x5a};

/* One round trip at a given plaintext length; also checks the on-wire size the daemon's parser assumes. */
static void round_trip(size_t n)
{
    uint8_t plain[512], encrypted[640], back[640];
    size_t written = 0, recovered = 0, padded = n + 16 - (n % 16);
    size_t i;
    for (i = 0; i < n; i++)
        plain[i] = (uint8_t)(i * 7 + 3);
    memset(encrypted, 0, sizeof(encrypted));
    memset(back, 0xee, sizeof(back));
    CHECK(controller_wsc_encrypt(key, plain, n, encrypted, sizeof(encrypted), &written) == 0);
    CHECK(written == 16 + padded);
    CHECK(controller_wsc_decrypt(key, encrypted, written, back, sizeof(back), &recovered) == 0);
    /* PKCS#7 is stripped by the evp: what comes back is the plaintext, without the padding bytes. */
    CHECK(recovered == n);
    CHECK(!memcmp(back, plain, n));
    /* The IV must not be a constant, or two M2s would share a keystream prefix. */
    {
        uint8_t again[640];
        size_t other = 0;
        CHECK(controller_wsc_encrypt(key, plain, n, again, sizeof(again), &other) == 0);
        CHECK(other == written);
        CHECK(memcmp(again, encrypted, 16) != 0);
    }
    /* A wrong key must not reproduce the plaintext (garbage or an error are both acceptable). */
    {
        uint8_t bad[640];
        size_t got = 0;
        if (!controller_wsc_decrypt(wrong, encrypted, written, bad, sizeof(bad), &got))
            CHECK(got != n || memcmp(bad, plain, n) != 0);
    }
}

int main(void)
{
    static const size_t lengths[] = {1, 15, 16, 17, 32, 95, 96, 97, 255, 300};
    uint8_t plain[64], encrypted[128], back[128];
    size_t i, written = 0, recovered = 0;
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
        round_trip(lengths[i]);
    /* Rejected inputs: no plaintext, no room for IV + one block, truncated ciphertext, non-block length. */
    memset(plain, 0x41, sizeof(plain));
    CHECK(controller_wsc_encrypt(key, plain, 0, encrypted, sizeof(encrypted), &written) != 0);
    CHECK(controller_wsc_encrypt(key, plain, sizeof(plain), encrypted, 16 + 64, &written) != 0);
    CHECK(controller_wsc_encrypt(key, plain, sizeof(plain), encrypted, sizeof(encrypted), &written) == 0);
    CHECK(controller_wsc_decrypt(key, encrypted, 15, back, sizeof(back), &recovered) != 0);
    CHECK(controller_wsc_decrypt(key, encrypted, written - 1, back, sizeof(back), &recovered) != 0);
    CHECK(controller_wsc_decrypt(key, encrypted, written, back, 4, &recovered) != 0);
    CHECK(controller_wsc_decrypt(NULL, encrypted, written, back, sizeof(back), &recovered) != 0);
    printf("controller-wsc-crypto-codec: %u assertions; pkcs7 and random-iv round trips\n", checks);
    return 0;
}
