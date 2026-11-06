#ifndef ZTE_MESH_BRIDGE_H
#define ZTE_MESH_BRIDGE_H
#include <stddef.h>
#include <stdint.h>

/* V1 semantic wire format: explicit big endian; NEVER send native structs,
 * DB views, pointers, arbitrary view/field names, argv credentials or paths.
 * Test-only one-shot inherited pipes, not a production authenticated service.
 */
#define MB_REQUEST_SIZE 120U
#define MB_RESPONSE_SIZE 24U
#define MB_WPA2_PSK_AES 1U
/* Public enum values verified by bridge_evidence.py against pinned CFAPI. */
#define MB_VENDOR_BEACON_WPA2 3U
#define MB_VENDOR_AUTH_PSK 0U
#define MB_VENDOR_CIPHER_AES 1U
#define MB_MAX_SSID 31U /* conservative; full binary/32-byte SSID unsupported */
#define MB_MAX_KEY 63U

enum mb_status { MB_OK, MB_BAD_FRAME, MB_BAD_CONFIG, MB_DB_ERROR,
                 MB_VERIFY_ERROR, MB_ROLLBACK_ERROR, MB_TRANSPORT_ERROR };
enum mb_flags { MB_REJECTED = 1, MB_READBACK = 2, MB_OTHER_ROW = 4, MB_ROLLED_BACK = 8 };
struct mb_profile {
    uint32_t request_id;
    uint8_t radio, bss, security, enabled, ssid_length, key_length;
    char ssid[32], key[64];
};
struct mb_result { uint32_t request_id, generation, flags; uint16_t status; };
int mb_validate(const struct mb_profile *profile);
/* 5G name derived from the 2.4G profile when the bands are not synchronised: base name + suffix (the factory
 * WLANSetting.ESSIDSuffix, "-5G"; FEATURE_MAP section 4.1). Copies the base SSID into `out` with the suffix appended;
 * 0 = ok, MB_BAD_CONFIG if the suffix is empty/not printable ASCII or the result does not fit 32 bytes. */
int mb_derive_name(const struct mb_profile *base, const char *suffix, struct mb_profile *out);
int mb_encode_request(const struct mb_profile *, uint8_t *, size_t);
int mb_decode_request(const uint8_t *, size_t, struct mb_profile *);
int mb_encode_response(const struct mb_result *, uint8_t *, size_t);
int mb_decode_response(const uint8_t *, size_t, struct mb_result *);
int mb_read_message(int fd, uint8_t *buffer, size_t size, int timeout_ms);
int mb_write_message(int fd, const uint8_t *buffer, size_t size, int timeout_ms);
void mb_wipe(void *, size_t);
#endif
