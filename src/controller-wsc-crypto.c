/* Bounded independent WPS-M2 verifier for the isolated PUBLIC enrollee.
 * Real DH/HMAC/AES, never a success shim. Only verification bits escape.
 * Baseline eap_wps_generate_kdk hashes 192 bytes, with a short DH result
 * left-aligned in a zeroed buffer (0x83dd0/0x83efc); retain that exact quirk.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controller-wsc-crypto.h"
typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;
typedef struct evp_md_st EVP_MD;
typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
extern BIGNUM *BN_new(void);
extern int BN_hex2bn(BIGNUM **, const char *);
extern BIGNUM *BN_bin2bn(const unsigned char *, int, BIGNUM *);
extern int BN_set_word(BIGNUM *, unsigned long);
extern int BN_add_word(BIGNUM *, unsigned long);
extern int BN_sub_word(BIGNUM *, unsigned long);
extern int BN_cmp(const BIGNUM *, const BIGNUM *);
extern int BN_mod_exp(BIGNUM *, const BIGNUM *, const BIGNUM *, const BIGNUM *, BN_CTX *);
extern int BN_num_bits(const BIGNUM *);
extern int BN_bn2bin(const BIGNUM *, unsigned char *);
extern void BN_clear_free(BIGNUM *);
extern BN_CTX *BN_CTX_new(void);
extern void BN_CTX_free(BN_CTX *);
extern unsigned char *SHA256(const unsigned char *, size_t, unsigned char *);
extern const EVP_MD *EVP_sha256(void);
extern unsigned char *HMAC(const EVP_MD *, const void *, int, const unsigned char *, size_t, unsigned char *, unsigned int *);
extern const EVP_CIPHER *EVP_aes_128_cbc(void);
extern EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
extern void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *);
extern int EVP_DecryptInit_ex(EVP_CIPHER_CTX *, const EVP_CIPHER *, void *, const unsigned char *, const unsigned char *);
extern int EVP_DecryptUpdate(EVP_CIPHER_CTX *, unsigned char *, int *, const unsigned char *, int);
extern int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *, unsigned char *, int *);
extern int EVP_EncryptInit_ex(EVP_CIPHER_CTX *, const EVP_CIPHER *, void *, const unsigned char *, const unsigned char *);
extern int EVP_EncryptUpdate(EVP_CIPHER_CTX *, unsigned char *, int *, const unsigned char *, int);
extern int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *, unsigned char *, int *);
extern int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *, int);
extern int RAND_bytes(unsigned char *, int);
static const char prime[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
    "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD"
    "3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625"
    "E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A8"
    "99FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163"
    "BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3A"
    "D961C62F356208552BB9ED529077096966D670C354E4ABC9804"
    "F1746C08CA237327FFFFFFFFFFFFFFFF";
static unsigned int u16(const uint8_t *p) { return ((unsigned int)p[0] << 8) | p[1]; }
static int equal(const void *aa, const void *bb, size_t n)
{
    const uint8_t *a = aa, *b = bb;
    size_t i; uint8_t different = 0;
    for (i = 0; i < n; i++) different |= a[i] ^ b[i];
    return different == 0;
}
/* Exact, unique attribute lookup; reject any malformed trailing bytes. */
static int lookup(const uint8_t *s, size_t n, unsigned int tag, const uint8_t **data, size_t *length)
{
    size_t at = 0; int found = 0;
    *data = NULL; *length = 0;
    while (n - at >= 4) {
        unsigned int type = u16(s + at); size_t len = u16(s + at + 2); at += 4;
        if (len > n - at) return -1;
        if (type == tag) { if (found++) return -1; *data = s + at; *length = len; }
        at += len;
    }
    return at == n && found == 1 ? 0 : -1;
}
static int hash_mac(const uint8_t key[32], const uint8_t *data, size_t n, uint8_t out[32])
{
    unsigned int size = 0;
    return HMAC(EVP_sha256(), key, 32, data, n, out, &size) && size == 32 ? 0 : -1;
}
static int derive(const uint8_t secret[32], const uint8_t pub[192], const uint8_t nonce[16],
                  const uint8_t mac[6], const uint8_t registrar[16], uint8_t keys[96])
{
    static const char label[] = "Wi-Fi Easy and Secure Key Derivation";
    uint8_t shared[192] = {0}, dhkey[32], kdk[32], seed[38], input[4 + sizeof(label) - 1 + 4];
    BIGNUM *p = NULL, *x = NULL, *y = NULL, *one = BN_new(), *z = BN_new();
    BN_CTX *ctx = BN_CTX_new();
    unsigned int i; int bytes, result = -1; uint8_t any = 0;
    for (i = 0; i < 32; i++) any |= secret[i];
    if (!any || !ctx || !one || !z || !BN_hex2bn(&p, prime) ||
        !(x = BN_bin2bn(secret, 32, NULL)) || !(y = BN_bin2bn(pub, 192, NULL)) ||
        !BN_set_word(one, 1) || BN_cmp(y, one) <= 0 || !BN_sub_word(p, 1) ||
        BN_cmp(y, p) >= 0 || !BN_add_word(p, 1) || !BN_mod_exp(z, y, x, p, ctx)) goto done;
    bytes = (BN_num_bits(z) + 7) / 8;
    if (bytes < 1 || bytes > 192 || BN_bn2bin(z, shared) != bytes ||
        !SHA256(shared, sizeof(shared), dhkey)) goto done;
    memcpy(seed, nonce, 16); memcpy(seed + 16, mac, 6); memcpy(seed + 22, registrar, 16);
    if (hash_mac(dhkey, seed, sizeof(seed), kdk)) goto done;
    memset(input, 0, sizeof(input)); memcpy(input + 4, label, sizeof(label) - 1);
    input[sizeof(input) - 2] = 2; input[sizeof(input) - 1] = 0x80; /* 640 bits */
    for (i = 0; i < 3; i++) {
        input[3] = (uint8_t)(i + 1);
        if (hash_mac(kdk, input, sizeof(input), keys + i * 32)) goto done;
    }
    result = 0;
done:
    BN_clear_free(p); BN_clear_free(x); BN_clear_free(y); BN_clear_free(z); BN_clear_free(one); BN_CTX_free(ctx);
    mb_wipe(shared, sizeof(shared)); mb_wipe(dhkey, sizeof(dhkey)); mb_wipe(kdk, sizeof(kdk));
    mb_wipe(seed, sizeof(seed)); mb_wipe(input, sizeof(input));
    return result;
}
static int profile(const uint8_t *plain, size_t n, const struct mb_profile *wanted, unsigned int *teardown)
{
    const uint8_t *data, *credential;
    size_t len, size;
    /* WPS credentials can be flat (Multi-AP) or wrapped in Credential. */
    if (!lookup(plain, n, 0x100e, &credential, &size)) { plain = credential; n = size; }
    if (lookup(plain, n, 0x1045, &data, &len) || len != wanted->ssid_length ||
        !equal(data, wanted->ssid, len) || lookup(plain, n, 0x1027, &data, &len) ||
        len != wanted->key_length || !equal(data, wanted->key, len) ||
        lookup(plain, n, 0x1003, &data, &len) || len != 2 || u16(data) != 0x20 ||
        lookup(plain, n, 0x100f, &data, &len) || len != 2 || u16(data) != 8) return -1;
    *teardown = 0;
    if (!lookup(plain, n, 0x1049, &data, &len) && len >= 3 &&
        data[0] == 0 && data[1] == 0x37 && data[2] == 0x2a) {
        size_t at = 3;
        while (len - at >= 2) {
            unsigned int type = data[at], bytes = data[at + 1]; at += 2;
            if (bytes > len - at) return -1;
            if (type == 6) { if (bytes != 1) return -1; *teardown = !!(data[at] & 0x10); }
            at += bytes;
        }
        if (at != len) return -1;
    }
    return 0;
}
/* Registrar side. The key derivation is the enrollee's, with the roles in the seed swapped: the nonce and the
 * MAC are the enrollee's, the second nonce is the registrar's, and the DH secret is ours against its public
 * key. Same 96 bytes out: AuthKey, KeyWrapKey, EMSK. */
int controller_wsc_registrar_keys(const uint8_t secret[32], const uint8_t enrollee_pub[192],
                                  const uint8_t enrollee_nonce[16], const uint8_t enrollee_mac[6],
                                  const uint8_t registrar_nonce[16], uint8_t keys[96])
{
    if (!secret || !enrollee_pub || !enrollee_nonce || !enrollee_mac || !registrar_nonce || !keys)
        return -1;
    return derive(secret, enrollee_pub, enrollee_nonce, enrollee_mac, registrar_nonce, keys);
}
/* First 8 bytes of HMAC-SHA256(AuthKey, data): the WPS "Authenticator" and "Key Wrap Authenticator". */
int controller_wsc_hmac8(const uint8_t authkey[32], const uint8_t *data, size_t n, uint8_t out[8])
{
    uint8_t digest[32];
    if (!authkey || !data || !out || !n || n > 2800)
        return -1;
    if (hash_mac(authkey, data, n, digest))
        return -1;
    memcpy(out, digest, 8);
    mb_wipe(digest, sizeof(digest));
    return 0;
}
/* WPS encrypted settings (attribute 0x1018) in the shape the factory produces. eap_wps_build_message_M2
 * picks a random 16-byte IV, encrypts the attribute list with AES-128-CBC and pads it PKCS#7 style with
 * 16 - (len % 16) bytes of that value, so an already aligned list still grows by a whole block; the IV goes
 * in front of the ciphertext. eap_wps_process_message_M2 reads exactly that back: buf + 16 is the ciphertext,
 * buf is the IV, and the last plaintext byte must be 1..16. */
int controller_wsc_encrypt(const uint8_t key[32], const uint8_t *plain, size_t n, uint8_t *out, size_t cap,
                           size_t *written)
{
    EVP_CIPHER_CTX *ctx = NULL;
    size_t length;
    int used = 0, final = 0, result = -1;
    if (!key || !plain || !out || !written || !n)
        return -1;
    length = n + 16 - (n % 16); /* PKCS#7: always at least one padding byte */
    if (cap < 16 + length || RAND_bytes(out, 16) != 1)
        return -1;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    /* Padding stays on: Init has to come first, it resets the context flags. */
    if (!EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, out) ||
        !EVP_EncryptUpdate(ctx, out + 16, &used, plain, (int)n) || used < 0 ||
        !EVP_EncryptFinal_ex(ctx, out + 16 + used, &final) || final < 0 ||
        (size_t)(used + final) != length)
        goto done;
    *written = 16 + length;
    result = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    return result;
}
/* The inverse, for the round-trip check before anything is put on the wire. */
int controller_wsc_decrypt(const uint8_t key[32], const uint8_t *in, size_t n, uint8_t *out, size_t cap,
                           size_t *written)
{
    EVP_CIPHER_CTX *ctx = NULL;
    int used = 0, final = 0, result = -1;
    if (!key || !in || !out || !written || n < 32 || (n - 16) % 16 || n - 16 > cap + 16)
        return -1;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx || !EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, in) ||
        !EVP_DecryptUpdate(ctx, out, &used, in + 16, (int)n - 16) || used < 0 ||
        !EVP_DecryptFinal_ex(ctx, out + used, &final) || final < 0 ||
        (size_t)(used + final) > cap)
        goto done;
    *written = (size_t)(used + final);
    result = 0;
done:
    EVP_CIPHER_CTX_free(ctx);
    return result;
}
int controller_wsc_verify_m2(const uint8_t *m1, size_t n1, const uint8_t *m2, size_t n2,
                           const uint8_t secret[32], const struct mb_profile *wanted,
                           struct controller_wsc_check *check)
{
    const uint8_t *nonce, *mac, *reg, *pub, *encrypted, *auth, *data, *kwa;
    size_t size, encsize, authsize, kwasize;
    uint8_t keys[96], digest[32], signed_data[2800], plain[1400];
    EVP_CIPHER_CTX *ctx = NULL;
    int result = -1, used = 0, final = 0;
    if (!check) return -1;
    memset(check, 0, sizeof(*check)); check->stage = 1;
    if (!m1 || !m2 || !secret || !wanted || n1 > 1400 || n2 > 1400 ||
        wanted->security != MB_WPA2_PSK_AES || wanted->enabled != 1 ||
        wanted->ssid_length < 1 || wanted->ssid_length > 32 || wanted->key_length < 8 || wanted->key_length > 63 ||
        lookup(m1, n1, 0x1022, &data, &size) || size != 1 || *data != 4 ||
        lookup(m1, n1, 0x101a, &nonce, &size) || size != 16 ||
        lookup(m1, n1, 0x1020, &mac, &size) || size != 6 ||
        lookup(m2, n2, 0x1022, &data, &size) || size != 1 || *data != 5 ||
        lookup(m2, n2, 0x101a, &data, &size) || size != 16 || !equal(data, nonce, 16) ||
        lookup(m2, n2, 0x1039, &reg, &size) || size != 16 ||
        lookup(m2, n2, 0x1032, &pub, &size) || size != 192 ||
        lookup(m2, n2, 0x1018, &encrypted, &encsize) || encsize < 32 || encsize % 16 ||
        lookup(m2, n2, 0x1005, &auth, &authsize) || authsize != 8 || auth + 8 != m2 + n2) goto done;
    check->stage = 2;
    if (derive(secret, pub, nonce, mac, reg, keys)) goto done;
    check->stage = 3;
    memcpy(signed_data, m1, n1); memcpy(signed_data + n1, m2, n2 - 12);
    if (hash_mac(keys, signed_data, n1 + n2 - 12, digest) || !equal(digest, auth, 8)) goto done;
    check->authenticator_verified = 1; check->stage = 4;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx || !EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, keys + 32, encrypted) ||
        !EVP_DecryptUpdate(ctx, plain, &used, encrypted + 16, (int)encsize - 16) ||
        used < 0 || (size_t)used > sizeof(plain) - 16 || !EVP_DecryptFinal_ex(ctx, plain + used, &final) ||
        final < 0 || (size_t)(used + final) > sizeof(plain)) goto done;
    size = (size_t)(used + final); check->stage = 5;
    if (lookup(plain, size, 0x101e, &kwa, &kwasize) || kwasize != 8 || kwa + 8 != plain + size ||
        hash_mac(keys, plain, size - 12, digest) || !equal(digest, kwa, 8)) goto done;
    check->keywrap_verified = 1; check->stage = 6;
    if (profile(plain, size - 12, wanted, &check->map_teardown)) goto done;
    check->profile_matched = 1; check->stage = check->map_teardown ? 7 : 0;
    result = 0;
done:
    EVP_CIPHER_CTX_free(ctx); mb_wipe(keys, sizeof(keys)); mb_wipe(digest, sizeof(digest));
    mb_wipe(signed_data, sizeof(signed_data)); mb_wipe(plain, sizeof(plain));
    return result;
}
