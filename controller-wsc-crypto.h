#ifndef CONTROLLER_WSC_CRYPTO_H
#define CONTROLLER_WSC_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#include "mesh-bridge.h"
#include "controller-packet.h"
/* PUBLIC lab peer only. No plaintext/key material is returned or logged. */
struct controller_wsc_check {
    unsigned int stage;
    unsigned int authenticator_verified, keywrap_verified, profile_matched;
    unsigned int map_teardown;
    /* The attribute tags that went into the M2 and that came in the M1, in order, as hex - the only way to
     * see why a parser that walks a fixed attribute set accepted or refused it. */
    char m2_tags[192];
    char m1_tags[256];
};
int controller_wsc_verify_m2(const uint8_t *, size_t, const uint8_t *, size_t,
                           const uint8_t[32], const struct mb_profile *,
                           struct controller_wsc_check *);
/* Registrar side, for the agent-role observation where the daemon is the WSC enrollee and the lab peer has
 * to answer its M1. Real DH/HMAC/AES with the same derivation the verifier uses; nothing is returned in the
 * clear, only the 96 key bytes the caller immediately consumes. */
int controller_wsc_registrar_keys(const uint8_t secret[32], const uint8_t enrollee_pub[192],
                                  const uint8_t enrollee_nonce[16], const uint8_t enrollee_mac[6],
                                  const uint8_t registrar_nonce[16], uint8_t keys[96]);
int controller_wsc_hmac8(const uint8_t authkey[32], const uint8_t *, size_t, uint8_t[8]);
int controller_wsc_encrypt(const uint8_t key[32], const uint8_t *, size_t, uint8_t *, size_t, size_t *);
int controller_wsc_decrypt(const uint8_t key[32], const uint8_t *, size_t, uint8_t *, size_t, size_t *);
/* Answer the agent's WSC M1 (already parsed out of its frame) with an M2 carrying `wanted`, and send it back
 * in a CMDU 0x0009. 0 = sent; the WSC steps that were taken are recorded in `check`. */
int controller_wsc_answer_m1(const struct controller_frame *, const struct mb_profile *,
                             struct controller_wsc_check *);
#endif
