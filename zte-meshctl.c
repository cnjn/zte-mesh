/* Factory-style OSS client, the same pattern as the factory diagget tool:
 * attach to the shared database, take an OSS identity with InitAppComm, then
 * use the factory CM API. It links liboss_pub (the client OSS library), never
 * the full liboss that the daemon itself uses. Runs inside the sandbox only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *dlsym(void *, const char *);
extern int DBShmCliInit(void);
extern int InitAppComm(const char *, int);
extern int CmMultiAPWlanRenew(unsigned char);
extern int CmMultiAPWlanRenewNotify(unsigned char *);
extern int CmMultiAPGetDB(void *);
extern int CmSendMsg2MM(const char *destination, unsigned int event, unsigned int message, const void *name, const void *data,
                        unsigned int length, void *out, unsigned int *out_length);

/* Interposers (this executable wins symbol lookup) that report what the factory
 * CM layer asks the OSS to send. Arguments beyond the documented ones are
 * forwarded untouched, so the real functions see exactly what the caller passed. */
__attribute__((visibility("default"))) int SSEND(unsigned int event, void *in, unsigned int length, unsigned int timeout, void *a4, void *a5,
          const char *destination, void *a7, void *a8, void *a9)
{
    int (*real)(unsigned int, void *, unsigned int, unsigned int, void *, void *, const char *, void *, void *, void *) =
        dlsym((void *)-1, "SSEND");
    int status = real ? real(event, in, length, timeout, a4, a5, destination, a7, a8, a9) : -999;
    printf("ZTE_MESHCTL ssend event=0x%04x length=%u timeout=%u status=%d\n", event, length, timeout, status);
    fflush(NULL);
    return status;
}
__attribute__((visibility("default"))) int ASEND(unsigned int event, void *in, unsigned int length, const char *destination)
{
    int (*real)(unsigned int, void *, unsigned int, const char *) = dlsym((void *)-1, "ASEND");
    int status = real ? real(event, in, length, destination) : -999;
    printf("ZTE_MESHCTL asend event=0x%04x length=%u status=%d\n", event, length, status);
    fflush(NULL);
    return status;
}

int main(int argc, char **argv)
{
    int status;
    unsigned char message[100] = {0};
    if (argc == 2 && !strcmp(argv[1], "cfg")) {
        static unsigned char view[2952];
        (void)DBShmCliInit();
        status = CmMultiAPGetDB(view);
        printf("ZTE_MESHCTL cfg status=%d mode=%u enable=%u devnum=%u version=%u\n", status,
               view[36] | view[37] << 8, view[68], view[388] | view[389] << 8, view[356] | view[357] << 8);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "renew-acd") && (!strcmp(argv[2], "0") || !strcmp(argv[2], "1"))) {
        /* What the factory manager task does for one band: tell the daemon's ACD
         * task directly (the CM wrapper routes through cspd once other devices
         * are known, and cspd does not run here). Same message as the CM layer's
         * single-device path: event 0xa93e, 8 bytes {band, 0, 0, 0, 0, 0, 0, 0}. */
        unsigned char in[8] = {0}, out[16] = {0};
        unsigned int out_length = 10;
        in[0] = (unsigned char)(argv[2][0] - '0');
        (void)DBShmCliInit();
        if (InitAppComm("zte-meshctl", 0)) {
            printf("ZTE_MESHCTL fail=init-app-comm\n");
            return 2;
        }
        status = SSEND(0xa93e, in, sizeof(in), 1000, out, &out_length, "multiapd.acd.acd", 0, 0, 0);
        printf("ZTE_MESHCTL renew-acd band=%u status=%d\n", in[0], status);
        return status == 0 ? 0 : 3;
    }
    if (argc == 2 && !strcmp(argv[1], "acd-election")) {
        /* The probe the agent observation uses for cspd's cooperator step (g_TriggerSelectFlag -> CfIeee1905RoleElect).
         * multiapd reaches that entry only through its acd task and the payload is not observable from the binary, so
         * this is a zeroed four-byte probe: a non-zero status means the event does not exist on this build. */
        unsigned char in[4] = {0};
        (void)DBShmCliInit();
        if (InitAppComm("zte-meshctl", 0)) {
            printf("ZTE_MESHCTL fail=init-app-comm\n");
            return 2;
        }
        status = ASEND(0x909b, in, sizeof(in), "multiapd.acd.acd");
        printf("ZTE_MESHCTL acd-election event=0x909b status=%d\n", status);
        return status == 0 ? 0 : 3;
    }
    if ((argc == 3 && !strcmp(argv[1], "reboot-node")) ||
        (argc == 4 && !strcmp(argv[1], "light-node") && (!strcmp(argv[3], "0") || !strcmp(argv[3], "1")))) {
        /* What the factory web page does for one child router (CmMultiApSendMsg2TopoDev, libieee1905cmapi 0x6574):
         * ASEND(event, 16 bytes {AL MAC[6], flags}, "multiapd.topo.dev") with event 0x90a0 "RebootDev" or 0x909f
         * "LightSet". The daemon's topo task (_topoDevRebootSetMsgHandle 0xa38b4, _topoDevLightSetMsgHandle 0xa2c74) turns
         * it into a tunnelled vendor message to that device (_topoDevSendVendorSetByTunnel: inner TLV {0x20 reboot | 0x21
         * light, length 7, MAC[6], value}, which the child's _topoDevHandle dispatches to its reboot / light handler).
         * The reboot handler reads the value from byte 8, the light handler from byte 6. */
        int reboot = !strcmp(argv[1], "reboot-node");
        unsigned int mac[6], i;
        unsigned char in[16] = {0};
        if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6 || strlen(argv[2]) != 17) {
            fprintf(stderr, "bad MAC\n");
            return 64;
        }
        for (i = 0; i < 6; i++) {
            if (mac[i] > 255)
                return 64;
            in[i] = (unsigned char)mac[i];
        }
        if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
            fprintf(stderr, "refusing the all-zero MAC (the daemon would flood it to every device)\n");
            return 64;
        }
        if (reboot)
            in[8] = 1;
        else
            in[6] = (unsigned char)(argv[3][0] - '0');
        (void)DBShmCliInit();
        if (InitAppComm("zte-meshctl", 0)) {
            printf("ZTE_MESHCTL fail=init-app-comm\n");
            return 2;
        }
        status = ASEND(reboot ? 0x90a0 : 0x909f, in, sizeof(in), "multiapd.topo.dev");
        printf("ZTE_MESHCTL %s mac=%s value=%u status=%d\n", argv[1], argv[2], reboot ? in[8] : in[6], status);
        return status == 0 ? 0 : 3;
    }
    if (argc == 4 && !strcmp(argv[1], "wlan-mgr") && (!strcmp(argv[3], "0") || !strcmp(argv[3], "1"))) {
        /* What cspd's wlan_mgr does when a device comes online (and when MLO is toggled): ask the daemon's
         * topo task (event 0xa97e) to tunnel a module message to that device's wlan_mgr. Payload:
         * {u16 0 = to the device, AL MAC[6]} then the module message {u16 BE name buffer length 36,
         * name[36], u32 BE message id 1, u32 BE value length 4, u32 BE value = MLOEnable}; the daemon
         * forwards the part after the 8-byte header as the vendor TLV 0200fe. */
        unsigned int mac[6], i;
        unsigned char in[58] = {0}, out[16] = {0};
        unsigned int out_length = 10;
        if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
            fprintf(stderr, "bad MAC\n");
            return 64;
        }
        for (i = 0; i < 6; i++) {
            if (mac[i] > 255)
                return 64;
            in[2 + i] = (unsigned char)mac[i];
        }
        in[9] = 36;
        memcpy(in + 10, "cspd.wlan_manage.wlan_mgr", 25);
        in[49] = 1;
        in[53] = 4;
        in[57] = (unsigned char)(argv[3][0] - '0');
        (void)DBShmCliInit();
        if (InitAppComm("zte-meshctl", 0)) {
            printf("ZTE_MESHCTL fail=init-app-comm\n");
            return 2;
        }
        /* Asynchronous, like the factory CfIeee1905TunnelToDevice (_prepareAndSendTunnelMessage): the topo
         * task dispatches events (kind 1) through its event table; a synchronous SSEND (kind 2) goes to the
         * request tables, which do not hold 0xa97e (replay run 19: status 0 but no frame on the wire). */
        status = ASEND(0xa97e, in, sizeof(in), "multiapd.topo.topo");
        (void)out;
        (void)out_length;
        printf("ZTE_MESHCTL wlan-mgr mac=%s value=%u status=%d\n", argv[2], in[57], status);
        return status == 0 ? 0 : 3;
    }
    if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "notify-master")) {
        /* The notifications the factory CM setters send to the master task after they write the database
         * (CmMultiAPSetRoamCfg / SetDomainCfg: 0x10206, CmMultiAPSetMasterCfg: 0x10205, SetBtmReqModeCfg: 0x10208):
         * CmSendMsg2MM("multiapd.map.master", 0x2401, id, name[32], no payload), i.e. a synchronous SSEND of
         * {u32 id, char name[32], u32 0} that the master dispatches through its SynSet table. 0x10206 re-reads the
         * Domain rows and pushes the roam-threshold PolicyConfigReq; 0x10205 re-reads MultiAPMaster and pushes the
         * vendor-only PolicyConfigReq. The three handlers ignore the name (only 0x10216 reads it as a radio id).
         * Several ids are sent in the given order from this one process (the factory web path sends the Domain
         * notification and the Master one back to back, 12 ms apart in the original capture). */
        unsigned long ids[3];
        int i, count = argc - 2;
        unsigned char name[32] = {0};
        for (i = 0; i < count; i++) {
            char *end;
            ids[i] = strtoul(argv[2 + i], &end, 0);
            if (*argv[2 + i] == 0 || *end || (ids[i] != 0x10205 && ids[i] != 0x10206 && ids[i] != 0x10208)) {
                fprintf(stderr, "notify-master ids must be 0x10205, 0x10206 or 0x10208\n");
                return 64;
            }
        }
        (void)DBShmCliInit();
        if (InitAppComm("zte-meshctl", 0)) {
            printf("ZTE_MESHCTL fail=init-app-comm\n");
            return 2;
        }
        for (i = 0; i < count; i++) {
            status = CmSendMsg2MM("multiapd.map.master", 0x2401, (unsigned int)ids[i], name, NULL, 0, NULL, NULL);
            printf("ZTE_MESHCTL notify-master id=0x%lx status=%d\n", ids[i], status);
            if (status)
                return 3;
        }
        return 0;
    }
    int notify = argc == 3 && !strcmp(argv[1], "notify") && (!strcmp(argv[2], "0") || !strcmp(argv[2], "1"));
    if (!notify && (argc != 2 || strcmp(argv[1], "renew"))) {
        fprintf(stderr, "usage: zte-meshctl renew | notify <band 0|1> | renew-acd <band> | acd-election | wlan-mgr <al-mac> <0|1> | notify-master <0x10205|0x10206|0x10208>... | reboot-node <al-mac> | light-node <al-mac> <0|1>\n");
        return 64;
    }
    (void)DBShmCliInit(); /* returns the mapping address, not a status */
    if (InitAppComm("zte-meshctl", 0)) {
        printf("ZTE_MESHCTL fail=init-app-comm\n");
        return 2;
    }
    if (notify) {
        message[0] = (unsigned char)(argv[2][0] - '0');
        status = CmMultiAPWlanRenewNotify(message);
        printf("ZTE_MESHCTL notify band=%u status=%d\n", message[0], status);
        return status == 0 ? 0 : 3;
    }
    status = CmMultiAPWlanRenew(0);
    printf("ZTE_MESHCTL renew status=%d\n", status);
    return status == 0 ? 0 : 3;
}
