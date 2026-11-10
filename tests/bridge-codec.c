#define _POSIX_C_SOURCE 200809L
#include "../src/mesh-bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned int scenarios;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "codec-fail line=%d\n", __LINE__); return 1; } } while (0)
#define CASE(x) do { CHECK(x); scenarios++; } while (0)

static int blank(const void *memory, size_t length)
{
    const uint8_t *p = memory;
    while (length--)
        if (*p++) return 0;
    return 1;
}

int main(void)
{
    struct mb_profile p = { .request_id = 0x12345678, .security = 1, .enabled = 1 }, decoded;
    struct mb_result r = { .request_id = 0x12345678, .generation = 1, .flags = 6, .status = MB_OK }, got;
    uint8_t good[MB_REQUEST_SIZE], bad[MB_REQUEST_SIZE + 1], response[MB_RESPONSE_SIZE], io[MB_REQUEST_SIZE], canonical[MB_REQUEST_SIZE];
    int fds[2], status;
    unsigned int i;
    pid_t pid;
    strcpy(p.ssid, "public-codec-lab"); strcpy(p.key, "not-a-production-key");
    p.ssid_length = strlen(p.ssid); p.key_length = strlen(p.key);
    CASE(mb_validate(&p) == MB_OK);
    CASE(mb_encode_request(&p, good, sizeof(good)) == 0);
    CASE(good[12] == 0x12 && good[13] == 0x34 && good[14] == 0x56 && good[15] == 0x78);
    CASE(mb_decode_request(good, sizeof(good), &decoded) == 0 && decoded.request_id == p.request_id &&
         decoded.radio == p.radio && decoded.key_length == p.key_length && decoded.ssid_length == p.ssid_length &&
         !memcmp(p.key, decoded.key, sizeof(p.key)) && !memcmp(p.ssid, decoded.ssid, sizeof(p.ssid)));
    CASE(mb_decode_request(NULL, sizeof(good), &decoded) == MB_BAD_FRAME && blank(&decoded, sizeof(decoded)));
    CASE(mb_decode_request(good, sizeof(good) - 1, &decoded) == MB_BAD_FRAME);
    CASE(mb_encode_request(&p, bad, sizeof(good) - 1) != 0);
    memset(bad, 0xa5, sizeof(good));
    CASE(mb_encode_request(NULL, bad, sizeof(good)) != 0 && blank(bad, sizeof(good)));
    CASE(mb_validate(NULL) != 0);
    /* Each malformed semantic/header input must fail closed and wipe output. */
    for (i = 0; i < 18; i++) {
        memcpy(bad, good, sizeof(good));
        switch (i) {
        case 0: bad[0]++; break;
        case 1: bad[5]++; break;
        case 2: bad[7]++; break;
        case 3: bad[11]--; break;
        case 4: bad[16] = 2; break;
        case 5: bad[17] = 1; break;
        case 6: bad[18] = 0; break;
        case 7: bad[19] = 2; break;
        case 8: bad[20] = 0; break;
        case 9: bad[20] = 32; break;
        case 10: bad[21] = 7; break;
        case 11: bad[21] = 64; break;
        case 12: bad[22] = 1; break;
        case 13: bad[24] = 0; break;
        case 14: bad[56] = 127; break;
        case 15: bad[55] = 1; break;
        case 16: bad[119] = 1; break;
        default: memset(bad + 12, 0, 4); break;
        }
        memset(&decoded, 0xa5, sizeof(decoded));
        CASE(mb_decode_request(bad, sizeof(good), &decoded) != 0 && blank(&decoded, sizeof(decoded)));
    }
    memset(p.ssid, 's', 31); p.ssid[31] = 0; p.ssid_length = 31;
    memset(p.key, 'k', 63); p.key[63] = 0; p.key_length = 63;
    p.radio = 1; p.enabled = 0;
    CASE(mb_encode_request(&p, bad, sizeof(good)) == 0 && mb_decode_request(bad, sizeof(good), &decoded) == 0);
    memset(p.ssid, 0, sizeof(p.ssid)); p.ssid[0] = 's'; p.ssid_length = 1;
    memset(p.key, 0, sizeof(p.key)); memset(p.key, 'k', 8); p.key_length = 8;
    CASE(mb_validate(&p) == 0);
    CASE(mb_encode_response(&r, response, sizeof(response)) == 0 && mb_decode_response(response, sizeof(response), &got) == 0 && got.request_id == r.request_id);
    response[0] ^= 1;
    CASE(mb_decode_response(response, sizeof(response), &got) != 0);
    r.generation = 0;
    CASE(mb_encode_response(&r, response, sizeof(response)) != 0);
    r.status = MB_BAD_CONFIG; r.flags = MB_REJECTED;
    CASE(mb_encode_response(&r, response, sizeof(response)) == 0);
    r.status = MB_DB_ERROR; r.flags = MB_ROLLED_BACK;
    CASE(mb_encode_response(&r, response, sizeof(response)) == 0);
    r.status = MB_ROLLBACK_ERROR; r.flags = 0;
    CASE(mb_encode_response(&r, response, sizeof(response)) == 0);
    r.flags = MB_ROLLED_BACK;
    CASE(mb_encode_response(&r, response, sizeof(response)) != 0);
    r.flags = 0; r.status = 65535;
    CASE(mb_encode_response(&r, response, sizeof(response)) != 0);
    /* Deterministic malformed-input corpus: accepted bytes must re-encode
     * identically; rejected bytes must leave no semantic/credential output.
     */
    {
        uint32_t random = 0x12345678U;
        for (i = 0; i < 100000; i++) {
            unsigned int j;
            memcpy(bad, good, sizeof(good));
            for (j = 0; j < 1 + i % 5; j++) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                bad[random % sizeof(good)] ^= 1U << (random % 8);
            }
            if (!mb_decode_request(bad, sizeof(good), &decoded))
                CHECK(!mb_encode_request(&decoded, canonical, sizeof(canonical)) &&
                      !memcmp(bad, canonical, sizeof(canonical)));
            else
                CHECK(blank(&decoded, sizeof(decoded)));
        }
        scenarios++;
        mb_wipe(canonical, sizeof(canonical)); mb_wipe(&decoded, sizeof(decoded));
    }
    /* Exact, fragmented, truncated, trailing, EOF, timeout and bad-fd pipes. */
    CHECK(pipe(fds) == 0);
    CHECK(mb_write_message(fds[1], good, sizeof(good), 100) == 0); close(fds[1]);
    CASE(mb_read_message(fds[0], io, sizeof(io), 100) == 0 && !memcmp(io, good, sizeof(io))); close(fds[0]);
    CHECK(pipe(fds) == 0); pid = fork(); CHECK(pid >= 0);
    if (!pid) { close(fds[0]); if (write(fds[1], good, 7) != 7 || write(fds[1], good + 7, sizeof(good) - 7) != sizeof(good) - 7) _exit(1); close(fds[1]); _exit(0); }
    close(fds[1]); CASE(mb_read_message(fds[0], io, sizeof(io), 500) == 0); close(fds[0]);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(pipe(fds) == 0); CHECK(write(fds[1], good, sizeof(good) - 1) == sizeof(good) - 1); close(fds[1]);
    CASE(mb_read_message(fds[0], io, sizeof(io), 100) != 0 && blank(io, sizeof(io))); close(fds[0]);
    CHECK(pipe(fds) == 0); memcpy(bad, good, sizeof(good)); bad[sizeof(good)] = 0;
    CHECK(write(fds[1], bad, sizeof(bad)) == sizeof(bad)); close(fds[1]);
    CASE(mb_read_message(fds[0], io, sizeof(io), 100) != 0 && blank(io, sizeof(io))); close(fds[0]);
    CHECK(pipe(fds) == 0); close(fds[1]);
    CASE(mb_read_message(fds[0], io, sizeof(io), 100) != 0); close(fds[0]);
    CHECK(pipe(fds) == 0);
    CASE(mb_read_message(fds[0], io, sizeof(io), 25) != 0 && blank(io, sizeof(io))); close(fds[0]); close(fds[1]);
    CASE(mb_read_message(-1, io, sizeof(io), 10) != 0);
    memset(io, 0xa5, sizeof(io));
    CASE(mb_read_message(-1, io, sizeof(io), 0) != 0 && blank(io, sizeof(io)));
    CASE(mb_write_message(-1, good, sizeof(good), 10) != 0);
    CASE(mb_write_message(-1, bad, sizeof(bad), 10) != 0);
    mb_wipe(&p, sizeof(p)); mb_wipe(good, sizeof(good)); mb_wipe(io, sizeof(io));
    CASE(blank(&p, sizeof(p)) && blank(good, sizeof(good)) && blank(io, sizeof(io)));
    {
        struct mb_profile base = { .radio = 0, .security = 1, .enabled = 1 }, derived = { .radio = 1, .security = 1, .enabled = 1 };
        memset(base.ssid, 0, sizeof(base.ssid));
        strcpy(base.ssid, "cc"); base.ssid_length = 2;
        CASE(mb_derive_name(&base, "-5G", &derived) == MB_OK && derived.ssid_length == 5 && !memcmp(derived.ssid, "cc-5G", 5) &&
             derived.ssid[5] == 0);
        strcpy(base.ssid, "01234567890123456789012345678"); base.ssid_length = 28; /* 28 + 3 = 31, the longest name allowed */
        CASE(mb_derive_name(&base, "-5G", &derived) == MB_OK && derived.ssid_length == 31 && !derived.ssid[31]);
        base.ssid_length = 29; /* 32 does not fit */
        CASE(mb_derive_name(&base, "-5G", &derived) != MB_OK);
        base.ssid_length = 2;
        CASE(mb_derive_name(&base, "", &derived) != MB_OK && mb_derive_name(&base, NULL, &derived) != MB_OK);
        CASE(mb_derive_name(&base, "123456789", &derived) != MB_OK); /* longer than the 8-byte suffix limit */
        CASE(mb_derive_name(&base, "-\x01", &derived) != MB_OK);
        base.ssid_length = 0;
        CASE(mb_derive_name(&base, "-5G", &derived) != MB_OK);
    }
    printf("PASS: bridge codec scenarios=%u canonical endian/length/ranges/EOF/timeouts/wipe\n", scenarios);
    return 0;
}
