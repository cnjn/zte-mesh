/* Exercise real factory DB + mesh policies/WLAN/identity, not a success shim.
 * Run only through ns-run. Public test fixtures; no Controller or live LAN.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "multiap-cfg.h"
#include "mesh-policies.h"
#include "wlan-profiles.h"
#include "mesh-bridge.h"

_Static_assert(sizeof(void *) == 4, "factory DB pointers require ARM32");

extern int DBShmSrvInit(void);
extern int DBShmCliInit(void);
extern int dbInitHashTble(void);
extern int dbAPIGetView(const char *, const char *, void *);
extern int dbAPISetView(const char *, const char *, void *);
extern int CmMultiAPGetMultiAPCfg(const char *, void *);
extern int ShmPoolLock(unsigned int);
extern int ShmPoolUnlock(void);
extern void *GetPUB(unsigned int);
extern void FreePUB(void *);
extern int MapGetDB(void *);
extern int controller_startup(void);
extern int controller_agent_startup(void);
extern int controller_service(void);
extern void *g_ptTblHead, *g_ptViewHead, *g_pGetCount, *g_pTblHash, *g_pViewHash;
extern void *g_pShmHandler;

#define DB_KEY ((key_t)65535)
#define DB_SIZE (4U * 1024 * 1024)
#define DB_BASE ((void *)0x60000000U)
#define MAGIC 0x01020304U

static void require(int condition, const char *step)
{
    if (!condition) {
        fprintf(stderr, "ZTE_DB_ENGINE fail=%s errno=%d\n", step, errno);
        _exit(81);
    }
}

static uint32_t read_count(void)
{
    return *(volatile uint32_t *)g_pGetCount;
}

static void verify_attachment(void)
{
    struct shmid_ds info;
    int id = shmget(DB_KEY, 0, 0);
    require(id >= 0 && shmctl(id, IPC_STAT, &info) == 0, "shm-stat");
    require(info.shm_segsz == DB_SIZE, "shm-size");
    require(g_ptTblHead == DB_BASE && g_ptViewHead == (void *)0x60000008U &&
            g_pGetCount == (void *)0x60000874U, "global-pointer-layout");
}

static int client(void)
{
    struct {
        uint32_t before[4];
        uint32_t data[MAP_CFG_SIZE / 4];
        uint32_t after[4];
    } guarded;
    struct {
        uint32_t before[4];
        uint8_t data[800];
        uint32_t after[4];
    } cm;
    uint32_t *query = guarded.data, mode;
    unsigned char *block;
    unsigned int i;
    uint32_t before;
    int result;
    memset(&guarded, 0xa5, sizeof(guarded));
    memset(query, 0, sizeof(guarded.data));
    query[0] = MAP_CFG_SIZE;
    query[1] = MAGIC;
    /* The original return is an address, not a zero/nonzero success code. */
    (void)DBShmCliInit();
    verify_attachment();
    require(ShmPoolLock(1000) == 0, "client-lock");
    block = GetPUB(64);
    require(block != NULL, "client-allocation");
    require((uintptr_t)block >= (uintptr_t)DB_BASE + 2228U &&
            (uintptr_t)block <= (uintptr_t)DB_BASE + DB_SIZE - 64U,
            "allocation-outside-shared-pool");
    memset(block, 0x5a, 64);
    FreePUB(block);
    (void)ShmPoolUnlock();
    before = read_count();
    result = dbAPIGetView("__zte_probe_missing__", "", query);
    require(result == -2 && read_count() == before + 1, "missing-view-not-success");
    printf("ZTE_DB_ENGINE client_attached=1 allocator=1 missing_view_status=%d\n", result);
    before = read_count();
    result = MapGetDB(query);
    require(result == 0 && read_count() == before + 1 && map_cfg_validate(query) == 0,
            "original-multiap-config-query");
    require(map_cfg_scalar(query, MAP_MODE) == 2 &&
            map_cfg_scalar(query, MAP_ENABLE) == 1 &&
            map_cfg_scalar(query, MAP_SYNC_ENABLE) == 1 &&
            map_cfg_scalar(query, MAP_MASTER_ONLY) == 1 &&
            map_cfg_scalar(query, MAP_MAX_HOP) == 2 &&
            (int32_t)map_cfg_scalar(query, MAP_CUR_HOP) == -2 &&
            !strcmp(map_cfg_string(query, MAP_IF_LINK), "br-lan") &&
            !strcmp(map_cfg_string(query, MAP_MESH_NAME), "zte-shim-lab"),
            "public-master-fixture-values");
    printf("ZTE_DB_ENGINE factory_map_query=1 cfg_fields=50 cfg_bytes=2952 map_status=0 payload_logged=0\n");
    memset(&cm, 0xa5, sizeof(cm));
    memset(cm.data, 0, sizeof(cm.data));
    before = read_count();
    result = CmMultiAPGetMultiAPCfg("", cm.data);
    memcpy(&mode, cm.data + 28, sizeof(mode));
    require(result == 0 && read_count() == before + 1 && mode == 2, "cmapi-config-marshaling");
    /* Real DB rejection tests: do not turn malformed input into success. */
    query[1] = 0;
    require(dbAPIGetView("MultiAPCfg", "", query) == -19, "bad-get-magic");
    require(dbAPISetView("MultiAPCfg", "", query) == -19, "bad-set-magic");
    query[1] = MAGIC;
    query[0] = MAP_CFG_SIZE - 4;
    require(dbAPIGetView("MultiAPCfg", "", query) < 0, "bad-view-length");
    query[0] = MAP_CFG_SIZE;
    /* Masked update through the original common-view SET engine. Only two
     * fields are enabled; an unrelated Master/bridge policy must survive.
     */
    memset(query + 2, 0, sizeof(guarded.data) - 8);
    require(map_cfg_set_scalar(query, MAP_MAX_HOP, 3) == 0 &&
            map_cfg_set_string(query, MAP_MESH_NAME, "zte-shim-roundtrip") == 0,
            "masked-update-construction");
    require(dbAPISetView("MultiAPCfg", "", query) == 0, "original-view-set");
    memset(query + 2, 0, sizeof(guarded.data) - 8);
    require(MapGetDB(query) == 0 && map_cfg_validate(query) == 0 &&
            map_cfg_scalar(query, MAP_MAX_HOP) == 3 &&
            map_cfg_scalar(query, MAP_MODE) == 2 &&
            map_cfg_scalar(query, MAP_ENABLE) == 1 &&
            !strcmp(map_cfg_string(query, MAP_IF_LINK), "br-lan") &&
            !strcmp(map_cfg_string(query, MAP_MESH_NAME), "zte-shim-roundtrip"),
            "original-view-set-readback");
    for (i = 0; i < 4; i++)
        require(guarded.before[i] == 0xa5a5a5a5U && guarded.after[i] == 0xa5a5a5a5U &&
                cm.before[i] == 0xa5a5a5a5U && cm.after[i] == 0xa5a5a5a5U,
                "marshaling-buffer-guard");
    printf("ZTE_DB_ENGINE cfg_get_set=1 cmapi_get=1 negative_queries=1 buffer_guards=1 payload_logged=0\n");
    fflush(NULL);
    require(mesh_policies_client_test() == 0, "master-domain-client-test");
    require(wlan_profiles_client_test() == 0, "profile-identity-client-test");
    return 0;
}

static int bridge_test(int injected_failure)
{
    uint8_t wire[MB_REQUEST_SIZE], response[MB_RESPONSE_SIZE];
    struct mb_profile profile;
    struct mb_result result = {0};
    void **table, **view;
    int id;
    memset(wire, 0, sizeof(wire));
    memset(&profile, 0, sizeof(profile));
    result.status = MB_TRANSPORT_ERROR;
    result.flags = MB_REJECTED;
    if (!mb_read_message(STDIN_FILENO, wire, sizeof(wire), 3000)) {
        result.status = mb_decode_request(wire, sizeof(wire), &profile);
        if (!result.status) {
            result.request_id = profile.request_id;
            errno = 0;
            require(shmget(DB_KEY, 0, 0) == -1 && errno == ENOENT, "bridge-namespace-not-empty");
            (void)DBShmSrvInit();
            verify_attachment();
            table = g_ptTblHead; view = g_ptViewHead;
            table[0] = table[1] = table;
            view[0] = view[1] = view;
            require(dbInitHashTble() == 0 && ShmPoolLock(1000) == 0, "bridge-engine-init");
            require(wlan_profiles_create_test_tables() == 0, "bridge-profile-registration");
            (void)ShmPoolUnlock();
            (void)wlan_profiles_bridge_apply(&profile, injected_failure, &result);
            id = shmget(DB_KEY, 0, 0);
            require(id >= 0 && shmctl(id, IPC_RMID, NULL) == 0 && shmdt(DB_BASE) == 0,
                    "bridge-private-engine-cleanup");
        }
    }
    require(mb_encode_response(&result, response, sizeof(response)) == 0, "bridge-response-encode");
    mb_wipe(wire, sizeof(wire)); mb_wipe(&profile, sizeof(profile));
    require(mb_write_message(STDOUT_FILENO, response, sizeof(response), 3000) == 0,
            "bridge-response-write");
    mb_wipe(response, sizeof(response));
    return result.status == MB_ROLLBACK_ERROR ? 81 : 0;
}

int main(int argc, char **argv)
{
    int id, status, startup, service;
    pid_t child;
    void **table, **view;
    const char *sandbox = getenv("ZTE_MESH_DB_SANDBOX");
    require(sandbox && !strcmp(sandbox, "1"), "not-launched-by-sandbox");
    if (argc == 2 && !strcmp(argv[1], "--client"))
        return client();
    if (argc == 2 && !strcmp(argv[1], "--bridge-test"))
        return bridge_test(0);
    if (argc == 2 && !strcmp(argv[1], "--bridge-rollback-test"))
        return bridge_test(1);
    startup = argc == 2 && (!strcmp(argv[1], "--controller-startup") || !strcmp(argv[1], "--controller-service") ||
                            !strcmp(argv[1], "--agent-startup"));
    service = startup && !strcmp(argv[1], "--controller-service");
    require(argc == 1 || startup, "arguments");
    errno = 0;
    require(shmget(DB_KEY, 0, 0) == -1 && errno == ENOENT, "namespace-not-empty");
    (void)DBShmSrvInit();
    verify_attachment();
    /* DBShmSrvInit zeros these heads; cspd normally establishes their rings.
     * The exact two-pointer layout is recovered from dbCreateTbl and
     * dbMgrCommViewAddItem. This is an empty DB, not a fabricated mesh profile.
     */
    table = g_ptTblHead;
    view = g_ptViewHead;
    table[0] = table[1] = table;
    view[0] = view[1] = view;
    require(dbInitHashTble() == 0, "hash-init");
    require(*(uint32_t *)g_pTblHash == 600 && *(uint32_t *)g_pViewHash == 500,
            "hash-buckets");
    require(ShmPoolLock(1000) == 0, "server-lock");
    require(map_cfg_create_test_table() == 0, "multiap-schema-registration");
    require(mesh_policies_create_test_tables() == 0, "master-domain-registration");
    {
        /* Real identity (public ModelName/Manufacturer, never a serial) is opt-in by
         * explicit environment variable, and only honored on the live-LAN service
         * path. Every other path keeps the obvious PUBLIC/TEST placeholders. */
        const char *manufacturer = service ? getenv("ZTE_MESH_REAL_MANUFACTURER") : NULL;
        const char *model_name = service ? getenv("ZTE_MESH_REAL_MODEL") : NULL;
        if (manufacturer && model_name) {
            require(wlan_profiles_create_identity_tables(manufacturer, model_name) == 0,
                    "profile-identity-registration");
            printf("ZTE_DB_ENGINE identity=real manufacturer=%s model=%s\n", manufacturer, model_name);
        } else {
            require(wlan_profiles_create_test_tables() == 0, "profile-identity-registration");
        }
    }
    (void)ShmPoolUnlock();
    printf("ZTE_DB_ENGINE cfg_registered=1 cfg_fields=50 cfg_bytes=2952 test_only=1\n");
    printf("ZTE_DB_ENGINE policy_tables=2 master_fields=55 domain_fields=106 domain_rows=2 test_only=1\n");
    printf("ZTE_DB_ENGINE server_initialized=1 shared_bytes=%u callbacks_available=%d\n", DB_SIZE, g_pShmHandler != NULL);
    fflush(NULL);
    if (startup) {
        if (service)
            return controller_service() == 0 ? 0 : 1; /* private SysV objects die with the IPC namespace */
        if (!strcmp(argv[1], "--agent-startup")) {
            /* Slave-side observation: the daemon runs as an agent and this side plays the controller. */
            require(controller_agent_startup() == 0, "agent-startup-observation");
            id = shmget(DB_KEY, 0, 0);
            require(id >= 0 && shmctl(id, IPC_RMID, NULL) == 0 && shmdt(DB_BASE) == 0,
                    "startup-private-engine-cleanup");
            return 0;
        }
        require(controller_startup() == 0, "controller-startup-observation");
        id = shmget(DB_KEY, 0, 0);
        require(id >= 0 && shmctl(id, IPC_RMID, NULL) == 0 && shmdt(DB_BASE) == 0,
                "startup-private-engine-cleanup");
        return 0;
    }
    child = fork();
    require(child >= 0, "fork-client");
    if (!child) {
        /* exec releases the inherited fixed-address mapping. A second shmat
         * in the same address space would fail and the vendor ignores it.
         */
        execl("/lib/ld-linux.so.3", "ld-linux.so.3", "--library-path", "/lib",
              "/bin/db-engine-probe", "--client", (char *)NULL);
        require(0, "exec-client");
    }
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "client-result");
    require(mesh_policies_parent_verify() == 0, "cross-process-policy-write-readback");
    {
        uint32_t query[MAP_CFG_SIZE / 4] = {MAP_CFG_SIZE, MAGIC};
        require(MapGetDB(query) == 0 && map_cfg_validate(query) == 0 &&
                map_cfg_scalar(query, MAP_MAX_HOP) == 3, "cross-process-cfg-write-readback");
    }
    require(wlan_profiles_parent_verify() == 0, "cross-process-profile-write-readback");
    require(read_count() == 45, "cross-process-counter");
    id = shmget(DB_KEY, 0, 0);
    require(id >= 0 && shmctl(id, IPC_RMID, NULL) == 0, "remove-private-shm");
    require(shmdt(DB_BASE) == 0, "detach");
    printf("ZTE_DB_ENGINE PASS shared_counter=45 native_engine=1 multiap_cfg_ready=1 policies_ready=1 profile_fixture_ready=1 controller_ready=0\n");
    return 0;
}
