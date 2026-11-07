/* ldn:u on a room (games built with NETPLAY, after joining one in the room browser): local wireless where the
 * "air" is the room server. Compatible with Eden's LDN over rooms, so our players and theirs see each other's
 * networks: the same LDN packets (net.h), with the same contents:
 *   Scan (broadcast, no data)        an access point answers ScanResp with its NetworkInfo
 *   Connect (to the host, NodeInfo)  the host takes the station into a free node and sends everyone SyncNetwork
 *   SyncNetwork (NetworkInfo)        a station's copy of the network; the first one completes its Connect
 *   Disconnect (NodeInfo), DestroyNetwork (no data)
 * Addresses are the room's fake ones: NodeInfo and GetIpv4Address hold them as host-order u32s, LDN packets as
 * network-order bytes. MAC addresses are 02 00 followed by the fake address.
 * Command ids: IUserLocalCommunicationService (SDK 4.4: nn::ldn::Initialize is command 400). */
#include "../ipc.h"
#include "../net/net.h"
#include "../nca.h"
#include "game_config.h"
#include <stdlib.h>
#include <windows.h>
#include <bcrypt.h>

#define LDN_RES(d)              (203 | (u32)(d) << 9)
#define R_LDN_AIRPLANE          LDN_RES(23)
#define R_LDN_NODE_COUNT        LDN_RES(30)
#define R_LDN_CONNECT_FAILED    LDN_RES(31)
#define R_LDN_BAD_STATE         LDN_RES(32)
#define R_LDN_BAD_INPUT         LDN_RES(96)
#define R_LDN_ADVERTISE_TOO_BIG LDN_RES(10)

enum { ST_NONE, ST_INITIALIZED, ST_AP_OPENED, ST_AP_CREATED, ST_STATION_OPENED, ST_STATION_CONNECTED, ST_ERROR };
enum { DR_NONE = 0, DR_BY_USER = 1, DR_DESTROYED_BY_USER = 3, DR_SIGNAL_LOST = 6 };
enum { NODES = 8, ADVERTISE_MAX = 384 };
enum { SF_LOCAL_COMM_ID = 1, SF_SESSION_ID = 2, SF_NETWORK_TYPE = 4, SF_SSID = 16, SF_SCENE_ID = 32 };

typedef struct IntentId { u64 local_communication_id; u8 r0[2]; u16 scene_id; u8 r1[4]; } IntentId;
typedef struct SessionId { u64 high, low; } SessionId;
typedef struct NetworkId { IntentId intent; SessionId session; } NetworkId;
typedef struct Ssid { u8 length; char raw[33]; } Ssid;
typedef struct NodeInfo {
    u32 ipv4;                               /* host order */
    u8 mac[6];
    s8 node_id;
    u8 is_connected;
    char user_name[33];
    u8 r0;
    s16 local_communication_version;
    u8 r1[16];
} NodeInfo;
typedef struct NetworkInfo {
    NetworkId id;
    /* CommonNetworkInfo */
    u8 bssid[6];
    Ssid ssid;
    s16 channel;
    s8 link_level;
    u8 network_type;
    u8 r0[4];
    /* LdnNetworkInfo */
    u8 security_parameter[16];
    u16 security_mode;
    u8 accept_policy, has_action_frame;
    u8 r1[2];
    u8 node_count_max, node_count;
    NodeInfo nodes[NODES];
    u8 r2[2];
    u16 advertise_size;
    u8 advertise[ADVERTISE_MAX];
    u8 r3[0x8c];
    u64 random_authentication_id;
} NetworkInfo;
_Static_assert(sizeof(NodeInfo) == 0x40, "NodeInfo");
_Static_assert(sizeof(NetworkInfo) == 0x480, "NetworkInfo");
/* offsets in the request arguments */
enum {
    SCAN_FILTER = 8,                        /* Scan: s16 channel, then ScanFilter (0x60) */
    CREATE_USER = 0x44, CREATE_NETCFG = 0x78,               /* CreateNetworkConfig */
    CREATE_P_SECPARAM = 0x44, CREATE_P_USER = 0x64, CREATE_P_NETCFG = 0x98,   /* CreateNetworkConfigPrivate */
    CONNECT_USER = 0x44, CONNECT_VERSION = 0x74,            /* ConnectNetworkData */
    /* ConnectPrivate (SDK 13; Ryujinx's layout, checked in the log): SecurityConfig, SecurityParameter, UserConfig,
     * u32 version, u32 option, NetworkConfig */
    CONNECT_P_SECPARAM = 0x44, CONNECT_P_USER = 0x64, CONNECT_P_VERSION = 0x94, CONNECT_P_NETCFG = 0xa0,
};

static CRITICAL_SECTION lk;
static KEvent *state_event;
static int state = ST_NONE;
static bool initialized;
static NetworkInfo net;                     /* ours (access point) or the host's copy (station) */
/* SetAdvertiseData may come before CreateNetwork (Violet's raids do so), which starts net afresh: kept here */
static u8 advertise[ADVERTISE_MAX];
static u16 advertise_size;
static u8 last_connected[NODES], node_change[NODES];
static s16 disconnect_reason;
/* access point: the stations in nodes 1..7, by fake address */
static bool station_used[NODES];
static u8 station_ip[NODES][4];
/* station */
static NodeInfo self_node;
static u8 host_ip[4];
static bool have_host;
static NetworkInfo scan_results[16];
static int nscan;

/* ---------------------------------------------------------------------------------------------- helpers */
static void local_ip(u8 ip[4]) { room_fake_ip(ip); }
static u32 ip_to_host(const u8 ip[4]) { return (u32)ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3]; }
static void ip_from_host(u32 v, u8 ip[4]) { ip[0] = v >> 24; ip[1] = (u8)(v >> 16); ip[2] = (u8)(v >> 8); ip[3] = (u8)v; }
static void fake_mac(u8 mac[6]) { u8 ip[4]; local_ip(ip); mac[0] = 2; mac[1] = 0; memcpy(mac + 2, ip, 4); }

static void send_ldn(u8 type, const u8 *remote, const void *data, u32 size) {
    NetLdnPacket p = {type};
    local_ip(p.local_ip);
    if (remote) memcpy(p.remote_ip, remote, 4);
    p.broadcast = remote == NULL;
    p.data = data;
    p.size = size;
    room_send_ldn(&p);
}

/* the state event fires on state changes and when a node connects or disconnects (Eden: lan_event) */
static void fire(void) { event_signal(state_event); }

static void check_nodes(void) {
    bool changed = false;
    for (int i = 0; i < NODES; i++) {
        u8 c = net.nodes[i].is_connected;
        if (c == last_connected[i]) continue;
        node_change[i] |= c ? 1 : 2;        /* NodeStateChange Connect / Disconnect */
        last_connected[i] = c;
        changed = true;
    }
    if (changed) fire();
}

static void reset_node_changes(void) {
    memset(last_connected, 0, sizeof last_connected);
    memset(node_change, 0, sizeof node_change);
}

static void make_node(NodeInfo *n, const u8 *user_config, u16 version) {
    u8 ip[4];
    local_ip(ip);
    memset(n, 0, sizeof *n);
    n->ipv4 = ip_to_host(ip);
    fake_mac(n->mac);
    n->is_connected = 1;
    memcpy(n->user_name, user_config, 33);
    n->local_communication_version = (s16)version;
}

/* access point: count the stations, mark nodes, send every station the network */
static void update_nodes(void) {
    int count = 0;
    for (int i = 1; i < NODES; i++) {
        net.nodes[i].node_id = (s8)i;
        net.nodes[i].is_connected = station_used[i];
        count += station_used[i];
    }
    net.node_count = (u8)(count + 1);
    for (int i = 1; i < NODES; i++)
        if (station_used[i]) send_ldn(LDN_SYNC_NETWORK, station_ip[i], &net, sizeof net);
    check_nodes();
}

static void reset_stations(void) {
    memset(station_used, 0, sizeof station_used);
}

/* ---------------------------------------------------------------------------------------------- the room's packets */
static void on_ldn(const NetLdnPacket *p) {
    EnterCriticalSection(&lk);
    switch (p->type) {
    case LDN_SCAN:
        if (state == ST_AP_CREATED) send_ldn(LDN_SCAN_RESP, p->local_ip, &net, sizeof net);
        break;
    case LDN_SCAN_RESP:
        if (p->size >= sizeof(NetworkInfo)) {
            const NetworkInfo *info = (const NetworkInfo *)p->data;
            hlog(1, "ldn: network of %u.%u.%u.%u: local communication id %016llx, scene %u, version %d, %u of %u nodes, "
                 "%u bytes of advertise data", p->local_ip[0], p->local_ip[1], p->local_ip[2], p->local_ip[3],
                 info->id.intent.local_communication_id, info->id.intent.scene_id, info->nodes[0].local_communication_version,
                 info->node_count, info->node_count_max, info->advertise_size);
            int i = 0;
            while (i < nscan && memcmp(scan_results[i].bssid, info->bssid, 6)) i++;
            if (i < 16) { memcpy(&scan_results[i], info, sizeof *info); if (i == nscan) nscan++; }
        }
        break;
    case LDN_CONNECT:
        if (state == ST_AP_CREATED && p->size >= sizeof(NodeInfo)) {
            int i = 1;
            while (i < NODES && !(station_used[i] && !memcmp(station_ip[i], p->local_ip, 4))) i++;
            if (i == NODES) for (i = 1; i < NODES && station_used[i]; i++) {}
            if (i < NODES && i < net.node_count_max) {
                memcpy(&net.nodes[i], p->data, sizeof(NodeInfo));
                station_used[i] = true;
                memcpy(station_ip[i], p->local_ip, 4);
                hlog(1, "ldn: station %u.%u.%u.%u (\"%.32s\", local communication version %d; ours %d) joined as node %d",
                     p->local_ip[0], p->local_ip[1], p->local_ip[2], p->local_ip[3], net.nodes[i].user_name,
                     net.nodes[i].local_communication_version, net.nodes[0].local_communication_version, i);
            }
            update_nodes();
        }
        break;
    case LDN_DISCONNECT:
        if (state == ST_AP_CREATED) {
            for (int i = 1; i < NODES; i++)
                if (station_used[i] && !memcmp(station_ip[i], p->local_ip, 4)) station_used[i] = false;
            update_nodes();
        }
        break;
    case LDN_DESTROY_NETWORK:
        if (state == ST_STATION_CONNECTED && have_host && !memcmp(p->local_ip, host_ip, 4)) {
            have_host = false;
            state = ST_STATION_OPENED;
            disconnect_reason = DR_DESTROYED_BY_USER;
            fire();
        }
        break;
    case LDN_SYNC_NETWORK:
        if ((state == ST_STATION_OPENED || state == ST_STATION_CONNECTED) && p->size >= sizeof(NetworkInfo)) {
            memcpy(&net, p->data, sizeof net);
            if (state == ST_STATION_OPENED) { state = ST_STATION_CONNECTED; fire(); }
            check_nodes();
        }
        break;
    }
    LeaveCriticalSection(&lk);
}

static void on_room_lost(void) {
    EnterCriticalSection(&lk);
    if (state == ST_STATION_CONNECTED) {
        state = ST_STATION_OPENED;
        have_host = false;
        disconnect_reason = DR_SIGNAL_LOST;
        fire();
    } else if (state == ST_AP_CREATED) {
        reset_stations();
        update_nodes();
    }
    LeaveCriticalSection(&lk);
}

/* ---------------------------------------------------------------------------------------------- commands */
static bool have_network(void) { return state == ST_AP_CREATED || state == ST_STATION_CONNECTED; }
static void copy_out(IpcReq *r, int i, const void *p, u32 n) {
    const IpcBuf *b = buf_out(r, i);
    if (b && b->addr) memcpy((void *)(uintptr_t)b->addr, p, b->size < n ? b->size : n);
}

static void lc_state(SvcObj *o, IpcReq *r) { out_u32(r, initialized ? (u32)state : ST_ERROR); }

static void lc_network_info(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (have_network()) copy_out(r, 0, &net, sizeof net); else r->result = R_LDN_BAD_STATE;
    LeaveCriticalSection(&lk);
}

static void lc_ipv4(SvcObj *o, IpcReq *r) {
    u8 ip[4];
    local_ip(ip);
    out_u32(r, ip_to_host(ip));
    out_u32(r, 0xffffff00u);                /* 255.255.255.0, as Eden reports with a typical host network */
}

static void lc_disconnect_reason(SvcObj *o, IpcReq *r) { out_data(r, &disconnect_reason, 2); }

static void lc_security_parameter(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (have_network()) { out_data(r, net.security_parameter, 16); out_data(r, &net.id.session, 16); }
    else r->result = R_LDN_BAD_STATE;
    LeaveCriticalSection(&lk);
}

static void lc_network_config(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (have_network()) {
        u8 cfg[0x20] = {0};
        memcpy(cfg, &net.id.intent, 0x10);
        memcpy(cfg + 0x10, &net.channel, 2);
        cfg[0x12] = net.node_count_max;
        memcpy(cfg + 0x14, &net.nodes[0].local_communication_version, 2);
        out_data(r, cfg, sizeof cfg);
    } else r->result = R_LDN_BAD_STATE;
    LeaveCriticalSection(&lk);
}

static void lc_state_event(SvcObj *o, IpcReq *r) { out_event(r, state_event); }

static void lc_latest_update(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    const IpcBuf *ub = buf_out(r, 1);
    if (!ub || !ub->size) r->result = R_LDN_BAD_INPUT;
    else if (!have_network()) r->result = R_LDN_BAD_STATE;
    else {
        copy_out(r, 0, &net, sizeof net);
        u8 *u = (u8 *)(uintptr_t)ub->addr;
        u64 n = ub->size / 8;
        for (u64 i = 0; i < n && i < NODES; i++) {
            memset(u + i * 8, 0, 8);
            u[i * 8] = node_change[i];
            node_change[i] = 0;
        }
    }
    LeaveCriticalSection(&lk);
}

/* local communication id -1 is "this application's own": the console takes the first of the NACP's
 * LocalCommunicationIds (0x30B0, 8 u64s), which need not be the title id: Violet's is Scarlet's 0100A3D008C5C000, so
 * the two versions meet. A real console (ldn-beacon) or Eden filters on it. Read once at registration. */
#define LOCAL_COMM_DEFAULT 0xffffffffffffffffull
static u64 nacp_comm_id = NX_TITLE_ID;
static u64 own_comm_id(u64 id) { return id == LOCAL_COMM_DEFAULT ? nacp_comm_id : id; }

static void load_comm_id(const char *game_dir) {
    u8 v[8];
    bool ok = false;
    if (nx_from_nsp()) {
        u64 sz;
        u8 *nacp = nx_game_control_file("control.nacp", &sz);
        if (nacp && sz >= 0x30B8) { memcpy(v, nacp + 0x30B0, 8); ok = true; }
        free(nacp);
    } else {
        char path[1100];
        snprintf(path, sizeof path, "%s/control/control.nacp", game_dir);
        FILE *f = fopen(path, "rb");
        if (f) {
            ok = !fseek(f, 0x30B0, SEEK_SET) && fread(v, 1, 8, f) == 8;
            fclose(f);
        }
    }
    u64 id;
    memcpy(&id, v, 8);
    if (ok && id) nacp_comm_id = id;
    hlog(1, "ldn: local communication id %016llx (%s)", (unsigned long long)nacp_comm_id,
        ok && id ? "the NACP's" : "no NACP: the title id");
}

static bool scan_match(const u8 *filter, const NetworkInfo *n) {
    u32 flag;
    memcpy(&flag, filter + 0x5c, 4);
    const NetworkId *fid = (const NetworkId *)filter;
    if (flag & SF_LOCAL_COMM_ID && own_comm_id(fid->intent.local_communication_id) != n->id.intent.local_communication_id)
        return false;
    if (flag & SF_SESSION_ID && memcmp(&fid->session, &n->id.session, 16)) return false;
    if (flag & SF_NETWORK_TYPE) { u32 t; memcpy(&t, filter + 0x20, 4); if (t != n->network_type) return false; }
    if (flag & SF_SSID) {
        const Ssid *s = (const Ssid *)(filter + 0x2a);
        if (s->length != n->ssid.length || memcmp(s->raw, n->ssid.raw, s->length)) return false;
    }
    if (flag & SF_SCENE_ID && fid->intent.scene_id != n->id.intent.scene_id) return false;
    return true;
}

/* asks every member for its network and collects the answers in scan_results; returns how many */
static int scan_room(void) {
    EnterCriticalSection(&lk);
    nscan = 0;
    send_ldn(LDN_SCAN, NULL, NULL, 0);
    LeaveCriticalSection(&lk);
    Sleep(1000);                            /* the replies (Eden waits as long) */
    return nscan;
}

static void lc_scan(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    if (!b || !b->size) { r->result = R_LDN_BAD_INPUT; return; }
    u8 filter[0x60] = {0};
    if (r->in_size >= SCAN_FILTER + 0x60) memcpy(filter, r->in + SCAN_FILTER, 0x60);
    scan_room();
    EnterCriticalSection(&lk);
    s16 count = 0;
    u64 max = b->size / sizeof(NetworkInfo);
    for (int i = 0; i < nscan && (u64)count < max; i++)
        if (scan_match(filter, &scan_results[i]))
            memcpy((u8 *)(uintptr_t)b->addr + count++ * sizeof(NetworkInfo), &scan_results[i], sizeof(NetworkInfo));
    LeaveCriticalSection(&lk);
    u32 fflag;
    memcpy(&fflag, filter + 0x5c, 4);
    const NetworkId *fid = (const NetworkId *)filter;
    hlog(1, "ldn: scan found %d network(s) of %d (filter flags %#x: local communication id %016llx, scene %u)", count,
         nscan, fflag, fid->intent.local_communication_id, fid->intent.scene_id);
    out_data(r, &count, 2);
}

static void lc_open_ap(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    disconnect_reason = DR_NONE;
    if (state == ST_NONE) r->result = R_LDN_BAD_STATE;
    else { reset_stations(); state = ST_AP_OPENED; }
    LeaveCriticalSection(&lk);
}

static void destroy_network(void) {
    for (int i = 1; i < NODES; i++)
        if (station_used[i]) send_ldn(LDN_DESTROY_NETWORK, station_ip[i], NULL, 0);
    reset_stations();
    state = ST_AP_OPENED;
    fire();
}

static void lc_close_ap(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (state == ST_NONE) r->result = R_LDN_BAD_STATE;
    else {
        if (state == ST_AP_CREATED) destroy_network();
        reset_stations();
        state = ST_INITIALIZED;
    }
    LeaveCriticalSection(&lk);
}

static void create_network(IpcReq *r, u32 user_off, u32 netcfg_off, const u8 *secparam) {
    const u8 *in = r->in;
    if (r->in_size < netcfg_off + 0x20) { r->result = R_LDN_BAD_INPUT; return; }
    EnterCriticalSection(&lk);
    if (state != ST_AP_OPENED) { r->result = R_LDN_BAD_STATE; LeaveCriticalSection(&lk); return; }
    memset(&net, 0, sizeof net);
    fake_mac(net.bssid);
    static const char ssid[] = "YuzuFakeSsidForLdn";        /* Eden's fixed SSID */
    net.ssid.length = sizeof ssid - 1;
    memcpy(net.ssid.raw, ssid, sizeof ssid);
    s16 channel;
    memcpy(&channel, in + netcfg_off + 0x10, 2);
    net.channel = channel ? channel : 6;
    net.link_level = 2;                     /* Good */
    net.network_type = 2;                   /* Ldn */
    memcpy(&net.security_mode, in, 2);      /* SecurityConfig.security_mode */
    net.node_count_max = in[netcfg_off + 0x12];
    if (secparam) {
        memcpy(net.security_parameter, secparam, 16);
        memcpy(&net.id.session, secparam + 16, 16);
    } else BCryptGenRandom(NULL, (u8 *)&net.id.session, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    memcpy(&net.id.intent, in + netcfg_off, 0x10);
    net.id.intent.local_communication_id = own_comm_id(net.id.intent.local_communication_id);
    memcpy(net.advertise, advertise, advertise_size);
    net.advertise_size = advertise_size;
    for (int i = 0; i < NODES; i++) net.nodes[i].node_id = (s8)i;
    u16 version;
    memcpy(&version, in + netcfg_off + 0x14, 2);
    make_node(&net.nodes[0], in + user_off, version);
    state = ST_AP_CREATED;
    reset_node_changes();
    update_nodes();
    hlog(1, "ldn: created a network (local communication id %016llx, scene %u, version %u, up to %u nodes, %u bytes "
         "of advertise data)", net.id.intent.local_communication_id, net.id.intent.scene_id, version, net.node_count_max,
         net.advertise_size);
    LeaveCriticalSection(&lk);
}

static void lc_create(SvcObj *o, IpcReq *r) { create_network(r, CREATE_USER, CREATE_NETCFG, NULL); }
static void lc_create_private(SvcObj *o, IpcReq *r) {
    if (r->in_size < CREATE_P_NETCFG + 0x20) { r->result = R_LDN_BAD_INPUT; return; }
    create_network(r, CREATE_P_USER, CREATE_P_NETCFG, r->in + CREATE_P_SECPARAM);
}

static void lc_destroy(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (state == ST_AP_CREATED) destroy_network(); else r->result = R_LDN_BAD_STATE;
    LeaveCriticalSection(&lk);
}

static void lc_advertise(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_in(r, 0);
    u64 n = b ? b->size : 0;
    if (n > ADVERTISE_MAX) { r->result = R_LDN_ADVERTISE_TOO_BIG; return; }
    EnterCriticalSection(&lk);
    if (n) memcpy(advertise, (const void *)(uintptr_t)b->addr, n);
    advertise_size = (u16)n;
    memcpy(net.advertise, advertise, n);
    net.advertise_size = (u16)n;
    if (state == ST_AP_CREATED) update_nodes();
    static int logged;
    if (logged++ < 8) hlog(1, "ldn: advertise data, %u bytes (%s)", (u32)n, state == ST_AP_CREATED ? "network up" : "before the network");
    LeaveCriticalSection(&lk);
}

static void lc_open_station(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    disconnect_reason = DR_NONE;
    if (state == ST_NONE) r->result = R_LDN_BAD_STATE;
    else { reset_stations(); state = ST_STATION_OPENED; }
    LeaveCriticalSection(&lk);
}

static void station_disconnect(void) {
    if (have_host) send_ldn(LDN_DISCONNECT, host_ip, &self_node, sizeof self_node);
    have_host = false;
    state = ST_STATION_OPENED;
    fire();
}

static void lc_close_station(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (state == ST_NONE) r->result = R_LDN_BAD_STATE;
    else {
        if (state == ST_STATION_CONNECTED) station_disconnect();
        reset_stations();
        state = ST_INITIALIZED;
    }
    LeaveCriticalSection(&lk);
}

/* joins target's network as a station (Connect and ConnectPrivate) */
static void connect_to(IpcReq *r, const NetworkInfo *target, const u8 *user_config, u32 version) {
    if (!target->node_count) { r->result = R_LDN_NODE_COUNT; return; }
    EnterCriticalSection(&lk);
    if (state != ST_STATION_OPENED) { r->result = R_LDN_BAD_STATE; LeaveCriticalSection(&lk); return; }
    make_node(&self_node, user_config, (u16)version);
    ip_from_host(target->nodes[0].ipv4, host_ip);
    have_host = true;
    reset_node_changes();
    send_ldn(LDN_CONNECT, host_ip, &self_node, sizeof self_node);
    LeaveCriticalSection(&lk);
    /* the host answers with SyncNetwork, which moves us to StationConnected */
    for (int i = 0; i < 100 && state != ST_STATION_CONNECTED; i++) Sleep(50);
    if (state != ST_STATION_CONNECTED) {
        EnterCriticalSection(&lk);
        have_host = false;
        LeaveCriticalSection(&lk);
        hlog(0, "ldn: the host at %u.%u.%u.%u did not accept us", host_ip[0], host_ip[1], host_ip[2], host_ip[3]);
        r->result = R_LDN_CONNECT_FAILED;
        return;
    }
    hlog(1, "ldn: connected to the network of %u.%u.%u.%u (local communication version %d; ours %u)", host_ip[0], host_ip[1],
         host_ip[2], host_ip[3], net.nodes[0].local_communication_version, version);
}

/* Connect: ConnectNetworkData in the arguments, the NetworkInfo (from Scan) in a pointer buffer */
static void lc_connect(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_in(r, 0);
    if (!b || b->size < sizeof(NetworkInfo) || r->in_size < CONNECT_VERSION + 4) { r->result = R_LDN_BAD_INPUT; return; }
    NetworkInfo target;
    memcpy(&target, (const void *)(uintptr_t)b->addr, sizeof target);
    u32 version;
    memcpy(&version, r->in + CONNECT_VERSION, 4);
    connect_to(r, &target, r->in + CONNECT_USER, version);
}

/* ConnectPrivate: no NetworkInfo, only the network's intent (NetworkConfig) and session id (in the
 * SecurityParameter), which the game got from the host some other way. Find that network with a scan. */
static void lc_connect_private(SvcObj *o, IpcReq *r) {
    if (r->in_size < CONNECT_P_NETCFG + 0x20) { r->result = R_LDN_BAD_INPUT; return; }
    const u8 *in = r->in;
    IntentId intent;
    SessionId session;
    u32 version;
    memcpy(&intent, in + CONNECT_P_NETCFG, sizeof intent);
    memcpy(&session, in + CONNECT_P_SECPARAM + 16, sizeof session);
    memcpy(&version, in + CONNECT_P_VERSION, 4);
    hlog(1, "ldn: ConnectPrivate to local communication id %016llx, scene %u, session %016llx%016llx, version %u",
         intent.local_communication_id, intent.scene_id, session.high, session.low, version);
    int n = scan_room();
    NetworkInfo target;
    bool found = false;
    EnterCriticalSection(&lk);
    for (int i = 0; i < n && !found; i++)
        if (scan_results[i].id.intent.local_communication_id == intent.local_communication_id &&
            !memcmp(&scan_results[i].id.session, &session, sizeof session)) {
            target = scan_results[i];
            found = true;
        }
    LeaveCriticalSection(&lk);
    if (!found) {
        hlog(0, "ldn: ConnectPrivate: no network with that session among %d found", n);
        r->result = R_LDN_CONNECT_FAILED;
        return;
    }
    connect_to(r, &target, in + CONNECT_P_USER, version);
}

static void lc_disconnect(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    disconnect_reason = DR_BY_USER;
    station_disconnect();
    LeaveCriticalSection(&lk);
}

static void lc_initialize(SvcObj *o, IpcReq *r) {
    if (!room_joined()) { r->result = R_LDN_AIRPLANE; return; }
    EnterCriticalSection(&lk);
    if (!initialized) {
        reset_stations();
        have_host = false;
        state = ST_INITIALIZED;
        initialized = true;
    }
    LeaveCriticalSection(&lk);
}

static void lc_finalize(SvcObj *o, IpcReq *r) {
    EnterCriticalSection(&lk);
    if (initialized) {
        if (state == ST_AP_CREATED) destroy_network();
        if (state == ST_STATION_CONNECTED) station_disconnect();
        reset_stations();
        initialized = false;
    }
    state = ST_NONE;
    LeaveCriticalSection(&lk);
}

static const Iface iface_local = {"IUserLocalCommunicationService", (const IpcCmd[]){
    {0, lc_state, "GetState"},
    {1, lc_network_info, "GetNetworkInfo"},
    {2, lc_ipv4, "GetIpv4Address"},
    {3, lc_disconnect_reason, "GetDisconnectReason"},
    {4, lc_security_parameter, "GetSecurityParameter"},
    {5, lc_network_config, "GetNetworkConfig"},
    {100, lc_state_event, "AttachStateChangeEvent"},
    {101, lc_latest_update, "GetNetworkInfoLatestUpdate"},
    {102, lc_scan, "Scan"},
    {103, lc_scan, "ScanPrivate"},
    {104, ipc_noop, "SetWirelessControllerRestriction"},
    {200, lc_open_ap, "OpenAccessPoint"},
    {201, lc_close_ap, "CloseAccessPoint"},
    {202, lc_create, "CreateNetwork"},
    {203, lc_create_private, "CreateNetworkPrivate"},
    {204, lc_destroy, "DestroyNetwork"},
    {205, ipc_noop, "Reject"},
    {206, lc_advertise, "SetAdvertiseData"},
    {207, ipc_noop, "SetStationAcceptPolicy"},
    {208, ipc_noop, "AddAcceptFilterEntry"},
    {209, ipc_noop, "ClearAcceptFilter"},
    {300, lc_open_station, "OpenStation"},
    {301, lc_close_station, "CloseStation"},
    {302, lc_connect, "Connect"},
    {303, lc_connect_private, "ConnectPrivate"},
    {304, lc_disconnect, "Disconnect"},
    {400, lc_initialize, "Initialize"},
    {401, lc_finalize, "Finalize"},
    {402, lc_initialize, "Initialize2"},
    IPC_END}, NULL};

IPC_OPEN(ldn_create_user, &iface_local)

static const Iface iface_ldn = {"ldn:u", (const IpcCmd[]){
    {0, ldn_create_user, "CreateUserLocalCommunicationService"},
    IPC_END}, NULL};

static SvcObj *open_ldn(const char *name) { return svcobj_new(&iface_ldn, NULL, name); }

void ldn_net_register(const char *game_dir) {
    load_comm_id(game_dir);
    InitializeCriticalSection(&lk);
    state_event = event_create();
    room_on_ldn(on_ldn);
    room_on_disconnect(on_room_lost);
    service_register("ldn:u", open_ldn);
}
