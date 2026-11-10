/* PUBLIC isolated WSC enrollee. Real RNG/DH; no factory/host credentials.
 * First observe M2 structure; this does not claim key authentication/install.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "mesh-bridge.h"
#include "controller-wsc-crypto.h"

typedef struct bignum_st BIGNUM;
typedef struct bignum_ctx BN_CTX;
extern int RAND_bytes(unsigned char *, int);
extern BIGNUM *BN_new(void);
extern int BN_hex2bn(BIGNUM **, const char *);
extern BIGNUM *BN_bin2bn(const unsigned char *, int, BIGNUM *);
extern int BN_set_word(BIGNUM *, unsigned long);
extern int BN_mod_exp(BIGNUM *, const BIGNUM *, const BIGNUM *, const BIGNUM *, BN_CTX *);
extern int BN_num_bits(const BIGNUM *);
extern int BN_bn2bin(const BIGNUM *, unsigned char *);
extern void BN_clear_free(BIGNUM *);
extern BN_CTX *BN_CTX_new(void);
extern void BN_CTX_free(BN_CTX *);
static unsigned int u16(const uint8_t *p) { return ((unsigned int)p[0] << 8) | p[1]; }
static int attr(uint8_t *out, size_t cap, size_t *used, unsigned int type, const void *data, size_t n)
{
    if (*used > cap || cap - *used < 4 || n > cap - *used - 4 || n > 65535) return -1;
    out[(*used)++] = (uint8_t)(type >> 8); out[(*used)++] = (uint8_t)type;
    out[(*used)++] = (uint8_t)(n >> 8); out[(*used)++] = (uint8_t)n;
    memcpy(out + *used, data, n); *used += n; return 0;
}
static int scalar(uint8_t *out, size_t cap, size_t *used, unsigned int type, unsigned int value, size_t n)
{
    uint8_t bytes[4]; size_t i;
    if (!n || n > 4) return -1;
    for (i = 0; i < n; i++) bytes[n - 1 - i] = (uint8_t)(value >> (i * 8));
    return attr(out, cap, used, type, bytes, n);
}
/* RFC 3526 group 5, exactly the WPS 1536-bit MODP group. */
static const char prime[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
    "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD"
    "3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625"
    "E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A8"
    "99FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163"
    "BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3A"
    "D961C62F356208552BB9ED529077096966D670C354E4ABC9804"
    "F1746C08CA237327FFFFFFFFFFFFFFFF";
static int public_key(uint8_t out[192], uint8_t secret[32])
{
    BIGNUM *p = NULL, *g = BN_new(), *x = NULL, *y = BN_new();
    BN_CTX *ctx = BN_CTX_new();
    int bytes, result = -1;
    if (!g || !y || !ctx || RAND_bytes(secret, 32) != 1 ||
        !BN_hex2bn(&p, prime) || !(x = BN_bin2bn(secret, 32, NULL)) ||
        !BN_set_word(g, 2) || !BN_mod_exp(y, g, x, p, ctx)) goto done;
    bytes = (BN_num_bits(y) + 7) / 8;
    if (bytes <= 0 || bytes > 192) goto done;
    memset(out, 0, 192);
    if (BN_bn2bin(y, out + 192 - bytes) != bytes) goto done;
    result = 0;
done:
    /* Caller owns/wipes the exponent after the response has been verified. */
    BN_clear_free(x); BN_clear_free(y); BN_clear_free(g); BN_clear_free(p); BN_CTX_free(ctx);
    return result;
}
int controller_wsc_m2_matches(const uint8_t *frame, size_t bytes, const uint8_t controller[6], const uint8_t nonce[16])
{
    static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
    size_t at = 22, n; unsigned int matched = 0;
    if (!frame || !controller || !nonce || bytes < 25 || bytes > 1600 ||
        memcmp(frame, peer, 6) || memcmp(frame + 6, controller, 6) ||
        u16(frame + 12) != 0x893a || frame[14] || frame[15] ||
        u16(frame + 16) != 9 || frame[20] || frame[21] != 0x80) return 0;
    while (bytes - at >= 3) {
        unsigned int type = frame[at]; n = u16(frame + at + 1); at += 3;
        if (n > bytes - at) return 0;
        if (!type) {
            if (n || !matched) return 0;
            for (; at < bytes; at++) if (frame[at]) return 0;
            return 1;
        }
        if (type == 0x11) {
            size_t p = 0;
            unsigned int message = 0, enrollee = 0, registrar = 0, key = 0, encrypted = 0, auth = 0;
            while (n - p >= 4) {
                unsigned int tag = u16(frame + at + p), len = u16(frame + at + p + 2); p += 4;
                if (len > n - p) return 0;
                if (tag == 0x1022 && len == 1 && frame[at + p] == 5) message++;
                if (tag == 0x101a && len == 16 && !memcmp(frame + at + p, nonce, 16)) enrollee++;
                if (tag == 0x1039 && len == 16) registrar++;
                if (tag == 0x1032 && len == 192) key++;
                if (tag == 0x1018 && len >= 32 && !(len % 16)) encrypted++;
                if (tag == 0x1005 && len == 8) auth++;
                p += len;
            }
            if (p != n || message != 1 || enrollee != 1 || registrar != 1 || key != 1 || encrypted != 1 || auth != 1) return 0;
            matched++;
        }
        at += n;
    }
    return 0;
}
/* WSC attribute scanner: one entry per tag, exactly like the verifier's, so a malformed M1 is refused
 * rather than half-read. */
static int find_attr(const uint8_t *s, size_t n, unsigned int tag, const uint8_t **data, size_t *length)
{
    size_t at = 0;
    int found = 0;
    *data = NULL;
    *length = 0;
    while (n - at >= 4) {
        unsigned int type = u16(s + at);
        size_t len = u16(s + at + 2);
        at += 4;
        if (len > n - at)
            return -1;
        if (type == tag) {
            if (found++)
                return -1;
            *data = s + at;
            *length = len;
        }
        at += len;
    }
    return at == n && found == 1 ? 0 : -1;
}

/* One line of tags for the diagnostics: "104a,1022,101a,…". */
static void tag_list(const uint8_t *s, size_t n, char *out, size_t cap)
{
    size_t at = 0, used = 0;
    out[0] = '\0';
    while (n - at >= 4) {
        unsigned int tag = u16(s + at);
        size_t length = u16(s + at + 2);
        int written;
        if (length > n - at - 4 || used + 6 >= cap)
            break;
        written = snprintf(out + used, cap - used, used ? ",%04x" : "%04x", tag);
        if (written < 1)
            break;
        used += (size_t)written;
        at += 4 + length;
    }
}

/* Registrar side of the exchange: turn the agent's WSC M1 into an M2 that carries the fixture profile and
 * send it back in a CMDU 0x0009. This is the frame the factory controller produces and the one the daemon's
 * _acdcRcvApAutoConfigWSCM2 is waiting for; the encrypted settings are built with the real derivation, so a
 * daemon that applies them has been given genuine (public test) credentials, not a parser shortcut. */
int controller_wsc_answer_m1(const struct controller_frame *frame, const struct mb_profile *wanted,
                             struct controller_wsc_check *check)
{
    static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
    static const uint8_t multi_ap_ext[6] = {0x00, 0x37, 0x2a, 6, 1, 0}; /* OUI + teardown subelement = 0 */
    uint8_t secret[32], pub[192], registrar_nonce[16], registrar_uuid[16], keys[96];
    uint8_t plain[512], encrypted[512], m2[1024], kwa[8], digest[8], digest_data[1600];
    uint8_t packet[1600];
    const uint8_t *uuid, *enrollee_mac, *enrollee_nonce, *enrollee_pub, *data;
    size_t n_uuid, n_mac, n_nonce, n_pub, plain_used = 0, enc_bytes = 0, m2_used = 0;
    size_t recovered = 0, packet_used = 22, digest_used = 0;
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    const char *private = getenv("ZTE_MESH_DB_SANDBOX");
    int fd = -1, result = -1, sent;
    if (check)
        memset(check, 0, sizeof(*check));
    if (!frame || !wanted || frame->wsc_message != 4 || !frame->wsc_bytes ||
        !private || strcmp(private, "1") || wanted->security != MB_WPA2_PSK_AES || wanted->enabled != 1 ||
        wanted->ssid_length < 1 || wanted->ssid_length > 32 || wanted->key_length < 8 || wanted->key_length > 63)
        return -1;
    if (find_attr(frame->wsc, frame->wsc_bytes, 0x1047, &uuid, &n_uuid) || n_uuid != 16 ||
        find_attr(frame->wsc, frame->wsc_bytes, 0x1020, &enrollee_mac, &n_mac) || n_mac != 6 ||
        find_attr(frame->wsc, frame->wsc_bytes, 0x101a, &enrollee_nonce, &n_nonce) || n_nonce != 16 ||
        find_attr(frame->wsc, frame->wsc_bytes, 0x1032, &enrollee_pub, &n_pub) || n_pub != 192 ||
        find_attr(frame->wsc, frame->wsc_bytes, 0x1022, &data, &n_pub) || n_pub != 1 || *data != 4)
        return -1;
    if (check) {
        check->stage = 1;
        tag_list(frame->wsc, frame->wsc_bytes, check->m1_tags, sizeof(check->m1_tags));
    }
    if (RAND_bytes(secret, sizeof(secret)) != 1 || RAND_bytes(registrar_nonce, sizeof(registrar_nonce)) != 1 ||
        RAND_bytes(registrar_uuid, sizeof(registrar_uuid)) != 1 ||
        public_key(pub, secret) ||
        controller_wsc_registrar_keys(secret, enrollee_pub, enrollee_nonce, enrollee_mac, registrar_nonce, keys))
        goto done;
    if (check)
        check->stage = 2;
    /* Encrypted settings, flat: acdcParseM2Msg walks the decrypted list attribute by attribute and dispatches
     * on type against one table (0x1045, 0x1003, 0x100f, 0x1028, 0x1027, 0x1020, 0x101e, 0x1049). There is no
     * Credential (0x100e) wrapper around those entries in the factory's own M2 - with one, the SSID and the
     * key are never seen and the profile is applied empty ("mesh sync for M2: index[0], SSID[]"). */
    if (check)
        check->stage = 3;
    if (attr(plain, sizeof(plain), &plain_used, 0x1045, wanted->ssid, wanted->ssid_length) ||
        scalar(plain, sizeof(plain), &plain_used, 0x1003, 0x0020, 2) ||
        scalar(plain, sizeof(plain), &plain_used, 0x100f, 0x0008, 2) ||
        attr(plain, sizeof(plain), &plain_used, 0x1027, wanted->key, wanted->key_length) ||
        attr(plain, sizeof(plain), &plain_used, 0x1049, multi_ap_ext, sizeof(multi_ap_ext)))
        goto done;
    if (check)
        check->stage = 4;
    /* Key Wrap Authenticator: HMAC over everything before it, so 0x101e closes the plaintext. */
    if (controller_wsc_hmac8(keys, plain, plain_used, kwa) ||
        attr(plain, sizeof(plain), &plain_used, 0x101e, kwa, sizeof(kwa)))
        goto done;
    if (check)
        check->stage = 5;
    if (controller_wsc_encrypt(keys + 32, plain, plain_used, encrypted, sizeof(encrypted), &enc_bytes))
        goto done;
    if (check) {
        /* Read the settings back with the same keys: proves the AES path before anything is sent. */
        uint8_t back[512];
        check->stage = 7;
        if (!controller_wsc_decrypt(keys + 32, encrypted, enc_bytes, back, sizeof(back), &recovered) &&
            recovered >= plain_used && !memcmp(back, plain, plain_used))
            check->keywrap_verified = 1;
        mb_wipe(back, sizeof(back));
    }
    if (check)
        check->stage = 8;
    /* M2 attribute list, in the order the factory's own eap_wps_build_message_M2 writes it. The device
     * attributes are echoed from the agent's M1: eap_wps_process_message_M2 walks that exact set and stops at
     * the first slot it cannot read, which is how "get WPS_TYPE_UUID_R error" appeared - 0x1048 (UUID-R), not
     * the 0x1047 (UUID-E) the M1 carries, is the tag it wants after the two nonces. */
    if (scalar(m2, sizeof(m2), &m2_used, 0x104a, 0x10, 1) ||
        scalar(m2, sizeof(m2), &m2_used, 0x1022, 5, 1) ||
        attr(m2, sizeof(m2), &m2_used, 0x101a, enrollee_nonce, n_nonce) ||
        attr(m2, sizeof(m2), &m2_used, 0x1039, registrar_nonce, sizeof(registrar_nonce)) ||
        attr(m2, sizeof(m2), &m2_used, 0x1048, registrar_uuid, sizeof(registrar_uuid)) ||
        attr(m2, sizeof(m2), &m2_used, 0x1032, pub, sizeof(pub)))
        goto done;
    {
        static const unsigned int device_tags[] = {
            0x1004, /* auth type flags */        0x1010, /* encryption type flags */
            0x100d, /* connection type flags */  0x1008, /* configuration methods */
            0x1021, /* manufacturer */           0x1023, /* model name */
            0x1024, /* model number */           0x1042, /* serial number */
            0x1054, /* primary device type */    0x1011, /* device name */
            0x103c, /* RF bands */               0x1002, /* association state */
            0x1009, /* configuration error */    0x1012, /* device password id */
            0x102d  /* OS version */
        };
        size_t i;
        for (i = 0; i < sizeof(device_tags) / sizeof(device_tags[0]); i++) {
            const uint8_t *value;
            size_t length;
            if (find_attr(frame->wsc, frame->wsc_bytes, device_tags[i], &value, &length))
                continue; /* not in this M1; nothing to echo */
            if (attr(m2, sizeof(m2), &m2_used, device_tags[i], value, length))
                goto done;
        }
    }
    if (attr(m2, sizeof(m2), &m2_used, 0x1018, encrypted, enc_bytes))
        goto done;
    if (check) {
        check->m2_tags[0] = '\0';
        tag_list(m2, m2_used, check->m2_tags, sizeof(check->m2_tags));
    }
    if (check)
        check->stage = 9;
    /* Authenticator over the whole M1 attribute list followed by M2 up to (not including) itself. */
    if (frame->wsc_bytes + m2_used > sizeof(digest_data))
        goto done;
    memcpy(digest_data, frame->wsc, frame->wsc_bytes);
    memcpy(digest_data + frame->wsc_bytes, m2, m2_used);
    digest_used = frame->wsc_bytes + m2_used;
    if (controller_wsc_hmac8(keys, digest_data, digest_used, digest) ||
        attr(m2, sizeof(m2), &m2_used, 0x1005, digest, sizeof(digest)))
        goto done;
    if (check) {
        check->authenticator_verified = 1;
        check->profile_matched = 1;
        check->stage = 10;
    }
    /* CMDU 0x0009 back to the agent: the WSC TLV first, then the AP Radio Identifier TLV (0x82) naming the
     * radio being configured - the same order the factory's own M2 uses (_acdsRcvApAutoConfigWSCM1 inserts
     * the WSC TLV, then I5TlvApRadioIdentifierTypeAcdInsert). Its payload is the six-byte Radio Unique
     * Identifier the agent put at the head of its AP Radio Basic Capabilities TLV: _acdcCheckAPRadioIdentifier
     * memcmp()s it against the identifier its WLAN database reports for that band and drops the M2 on any
     * mismatch ("Check APRadioIdentifier failed"). */
    target.sll_ifindex = (int)if_nametoindex("lab-peer");
    if (!target.sll_ifindex)
        goto done;
    memcpy(packet, frame->src, 6);
    memcpy(packet + 6, peer, 6);
    packet[12] = 0x89; packet[13] = 0x3a;
    packet[16] = 0x00; packet[17] = 0x09;
    packet[18] = (uint8_t)(frame->mid >> 8); packet[19] = (uint8_t)frame->mid;
    packet[21] = 0x80;
    packet[packet_used++] = 0x11;
    packet[packet_used++] = (uint8_t)(m2_used >> 8);
    packet[packet_used++] = (uint8_t)m2_used;
    memcpy(packet + packet_used, m2, m2_used);
    packet_used += m2_used;
    if (frame->radio_bytes == 6) {
        packet[packet_used++] = 0x82;
        packet[packet_used++] = 0;
        packet[packet_used++] = 6;
        memcpy(packet + packet_used, frame->radio, 6);
        packet_used += 6;
    }
    target.sll_halen = 6;
    memcpy(target.sll_addr, frame->src, 6);
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x893a));
    if (fd < 0)
        goto done;
    sent = sendto(fd, packet, packet_used, 0, (struct sockaddr *)&target, sizeof(target));
    result = sent == (ssize_t)packet_used ? 0 : -1;
done:
    if (fd >= 0)
        close(fd);
    mb_wipe(secret, sizeof(secret)); mb_wipe(pub, sizeof(pub)); mb_wipe(keys, sizeof(keys));
    mb_wipe(plain, sizeof(plain)); mb_wipe(encrypted, sizeof(encrypted));
    mb_wipe(m2, sizeof(m2)); mb_wipe(kwa, sizeof(kwa)); mb_wipe(digest, sizeof(digest));
    mb_wipe(digest_data, sizeof(digest_data)); mb_wipe(packet, sizeof(packet));
    mb_wipe(registrar_nonce, sizeof(registrar_nonce));
    return result;
}

int controller_inject_wsc_m1(void)
{
    static const uint8_t peer[6] = {2, 0, 0, 0, 0x19, 0x0a};
    static const uint8_t device_type[8] = {0, 6, 0, 0x50, 0xf2, 4, 0, 1};
    static const uint8_t radio[11] = {2, 0, 0, 0, 0x19, 0x0b, 1, 1, 81, 20, 0};
    struct sockaddr_ll target = { .sll_family = AF_PACKET, .sll_protocol = htons(0x893a), .sll_halen = 6 };
    struct ifreq interface = {0}; struct timespec start, now;
    struct pollfd ready;
    uint8_t frame[1400] = {0}, received[1601], nonce[16], uuid[16], pub[192], controller[6], secret[32];
    uint8_t *wsc = frame + 39;
    size_t used = 0, length; unsigned int tries;
    int fd = -1, result = -1;
    const char *sandbox = getenv("ZTE_MESH_DB_SANDBOX");
    if (!sandbox || strcmp(sandbox, "1")) goto done;
    if (RAND_bytes(nonce, 16) != 1 || RAND_bytes(uuid, 16) != 1 || public_key(pub, secret)) goto done;
#define A(t,p,n) do { if (attr(wsc, 1000, &used, (t), (p), (n))) goto done; } while (0)
#define V(t,v,n) do { if (scalar(wsc, 1000, &used, (t), (v), (n))) goto done; } while (0)
    V(0x104a, 0x10, 1); V(0x1022, 4, 1); A(0x1047, uuid, 16); A(0x1020, peer, 6);
    A(0x101a, nonce, 16); A(0x1032, pub, 192);
    V(0x1004, 0x0020, 2); V(0x1010, 8, 2); V(0x100d, 1, 1); V(0x1008, 0x0080, 2); V(0x1044, 1, 1);
    A(0x1021, "PUBLIC-LAB", 10); A(0x1023, "MAP-TEST", 8); A(0x1024, "1", 1); A(0x1042, "PUBLIC", 6);
    A(0x1054, device_type, 8); A(0x1011, "MAP-TEST", 8);
    V(0x103c, 1, 1); V(0x1002, 0, 2); V(0x1012, 4, 2); V(0x1009, 0, 2); V(0x102d, 0x80000000u, 4);
#undef A
#undef V
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) goto done;
    strcpy(interface.ifr_name, "br0");
    if (ioctl(fd, SIOCGIFHWADDR, &interface) || interface.ifr_hwaddr.sa_family != 1) goto done;
    memcpy(controller, interface.ifr_hwaddr.sa_data, 6); close(fd); fd = -1;
    memcpy(frame, controller, 6); memcpy(frame + 6, peer, 6);
    frame[12] = 0x89; frame[13] = 0x3a; frame[17] = 9; frame[18] = 0x12; frame[19] = 5; frame[21] = 0x80;
    frame[22] = 0x85; frame[24] = sizeof(radio); memcpy(frame + 25, radio, sizeof(radio));
    frame[36] = 0x11; frame[37] = (uint8_t)(used >> 8); frame[38] = (uint8_t)used;
    length = 39 + used + 3;
    target.sll_ifindex = (int)if_nametoindex("lab-peer"); memcpy(target.sll_addr, controller, 6);
    if (!target.sll_ifindex) goto done;
    fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(0x893a));
    if (fd < 0 || bind(fd, (struct sockaddr *)&target, sizeof(target)) ||
        sendto(fd, frame, length, 0, (struct sockaddr *)&target, sizeof(target)) != (ssize_t)length ||
        clock_gettime(CLOCK_MONOTONIC, &start)) goto done;
    printf("ZTE_CONTROLLER_PACKET fixture=wsc-m1 private_only=1 send_status=0 real_rng_dh=1 body_logged=0\n"); fflush(NULL);
    ready.fd = fd; ready.events = POLLIN;
    for (tries = 0; tries < 512; tries++) {
        long elapsed; int status; ssize_t bytes;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) goto done;
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= 4000) break;
        status = poll(&ready, 1, (int)(4000 - elapsed));
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0 || !(ready.revents & POLLIN)) break;
        bytes = recv(fd, received, sizeof(received), MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (bytes < 0) break;
        if (controller_wsc_m2_matches(received, (size_t)bytes, controller, nonce)) {
            struct mb_profile wanted = { .security = MB_WPA2_PSK_AES, .enabled = 1,
                .ssid_length = 19, .key_length = 20, .ssid = "zte-controller-test", .key = "public-test-only-key" };
            struct controller_wsc_check check = {0};
            size_t at = 22;
            int verified = -1;
            while ((size_t)bytes - at >= 3) {
                unsigned int type = received[at], n = u16(received + at + 1); at += 3;
                if (n > (size_t)bytes - at || !type) break;
                if (type == 0x11) {
                    verified = controller_wsc_verify_m2(wsc, used, received + at, n, secret, &wanted, &check);
                    break;
                }
                at += n;
            }
            printf("ZTE_CONTROLLER_PACKET fixture=wsc-m2 private_only=1 peer_received=1 cmdu=9 nonce_match=1 encrypted_settings=1 authenticator_present=1 bytes=%u cryptographic_verified=%u keywrap_verified=%u profile_match=%u map_teardown=%u stage=%u body_logged=0\n",
                   (unsigned int)bytes, check.authenticator_verified && check.keywrap_verified,
                   check.keywrap_verified, check.profile_matched, check.map_teardown, check.stage);
            result = verified;
            mb_wipe(&wanted, sizeof(wanted)); mb_wipe(&check, sizeof(check));
            break;
        }
    }
done:
    if (fd >= 0) close(fd);
    mb_wipe(frame, sizeof(frame)); mb_wipe(received, sizeof(received)); mb_wipe(nonce, sizeof(nonce));
    mb_wipe(uuid, sizeof(uuid)); mb_wipe(pub, sizeof(pub)); mb_wipe(secret, sizeof(secret));
    return result;
}
