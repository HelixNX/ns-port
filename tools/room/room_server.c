/* A multiplayer room server in the protocol Citra, yuzu and Eden use (network_version 1), so our games and Eden
 * players can meet in a room we host. Our own implementation (Eden's room.cpp consulted for behaviour only).
 *   room_server [--port 24872] [--name "..."] [--password pw] [--slots 8] [--game-id 0100152000022000]
 *               [--game "Mario Kart 8 Deluxe"] [--log 0-2]
 * Every message is an ENet reliable packet on channel 0, type byte first; integers big-endian, strings and vectors
 * with a u32 length in front, IPv4 addresses 4 raw bytes. Members get fake addresses 192.168.1.1, .2, ... (as Eden's
 * rooms do), and LDN and proxy packets are relayed to the member with the remote address, or to everyone else when
 * the broadcast flag is set. Log 1 prints a summary of the relayed traffic every few seconds, log 2 every packet. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <enet/enet.h>
#include <zstd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

enum {
    ID_JOIN_REQUEST = 1, ID_JOIN_SUCCESS, ID_ROOM_INFORMATION, ID_SET_GAME_INFO, ID_PROXY_PACKET, ID_LDN_PACKET,
    ID_CHAT_MESSAGE, ID_NAME_COLLISION, ID_IP_COLLISION, ID_VERSION_MISMATCH, ID_WRONG_PASSWORD, ID_CLOSE_ROOM,
    ID_ROOM_IS_FULL, ID_STATUS_MESSAGE, ID_HOST_KICKED, ID_HOST_BANNED,
};
enum { STATUS_MEMBER_JOIN = 1, STATUS_MEMBER_LEAVE };
enum { NET_VERSION = 1, MAX_MEMBERS = 16 };
static const char *ldn_names[] = {"Scan", "ScanResp", "Connect", "SyncNetwork", "Disconnect", "DestroyNetwork"};

/* ---------------------------------------------------------------------------------------------- options */
static u16 port = 24872;
static const char *room_name = "nxrecomp room", *password = "", *game_name = "Mario Kart 8 Deluxe";
static u64 game_id = 0x0100152000022000ull;
static u32 slots = 8;
static int log_level = 1;

static void logf_(const char *fmt, ...) {
    char t[16];
    time_t now = time(NULL);
    strftime(t, sizeof t, "%H:%M:%S", localtime(&now));
    printf("[%s] ", t);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ---------------------------------------------------------------------------------------------- packets */
typedef struct Pkt { u8 *p; u32 n, cap; } Pkt;
static void w_bytes(Pkt *k, const void *d, u32 n) {
    if (k->n + n > k->cap) { k->cap = (k->n + n) * 2 + 64; k->p = realloc(k->p, k->cap); }
    memcpy(k->p + k->n, d, n);
    k->n += n;
}
static void w_u8(Pkt *k, u8 v) { w_bytes(k, &v, 1); }
static void w_u16(Pkt *k, u16 v) { u8 b[2] = {(u8)(v >> 8), (u8)v}; w_bytes(k, b, 2); }
static void w_u32(Pkt *k, u32 v) { u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; w_bytes(k, b, 4); }
static void w_u64(Pkt *k, u64 v) { w_u32(k, (u32)(v >> 32)); w_u32(k, (u32)v); }
static void w_str(Pkt *k, const char *s) { u32 n = (u32)strlen(s); w_u32(k, n); w_bytes(k, s, n); }

typedef struct Rd { const u8 *p; u32 n, pos; bool ok; } Rd;
static bool r_take(Rd *r, void *d, u32 n) {
    if (!r->ok || r->pos + n > r->n) { r->ok = false; if (d) memset(d, 0, n); return false; }
    if (d) memcpy(d, r->p + r->pos, n);
    r->pos += n;
    return true;
}
static u8 r_u8(Rd *r) { u8 v; r_take(r, &v, 1); return v; }
static u16 r_u16(Rd *r) { u8 b[2]; r_take(r, b, 2); return (u16)(b[0] << 8 | b[1]); }
static u32 r_u32(Rd *r) { u8 b[4]; r_take(r, b, 4); return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static u64 r_u64(Rd *r) { u64 hi = r_u32(r); return hi << 32 | r_u32(r); }
static void r_str(Rd *r, char *out, size_t n) {
    u32 len = r_u32(r);
    if (!r->ok || r->pos + len > r->n) { r->ok = false; out[0] = 0; return; }
    size_t c = len < n - 1 ? len : n - 1;
    memcpy(out, r->p + r->pos, c);
    out[c] = 0;
    r->pos += len;
}

/* ---------------------------------------------------------------------------------------------- members */
typedef struct Member {
    bool used;
    ENetPeer *peer;
    char nick[64], game[128], version[64];
    u64 game_id;
    u8 ip[4];
} Member;
static Member members[MAX_MEMBERS];
static ENetHost *host;

static Member *by_peer(ENetPeer *p) {
    for (int i = 0; i < MAX_MEMBERS; i++) if (members[i].used && members[i].peer == p) return &members[i];
    return NULL;
}
static Member *by_ip(const u8 ip[4]) {
    for (int i = 0; i < MAX_MEMBERS; i++) if (members[i].used && !memcmp(members[i].ip, ip, 4)) return &members[i];
    return NULL;
}
static int member_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_MEMBERS; i++) n += members[i].used;
    return n;
}

static void send_to(ENetPeer *p, Pkt *k) {
    enet_peer_send(p, 0, enet_packet_create(k->p, k->n, ENET_PACKET_FLAG_RELIABLE));
}
static void send_all(Pkt *k, ENetPeer *except) {
    for (int i = 0; i < MAX_MEMBERS; i++)
        if (members[i].used && members[i].peer != except) send_to(members[i].peer, k);
}
static void send_id(ENetPeer *p, u8 id) {
    Pkt k = {0};
    w_u8(&k, id);
    send_to(p, &k);
    free(k.p);
    enet_host_flush(host);
}

static void broadcast_room_information(void) {
    Pkt k = {0};
    w_u8(&k, ID_ROOM_INFORMATION);
    w_str(&k, room_name);
    w_str(&k, "");                          /* description */
    w_u32(&k, slots);
    w_u16(&k, port);
    w_str(&k, game_name);
    w_str(&k, "");                          /* host username */
    w_u32(&k, (u32)member_count());
    for (int i = 0; i < MAX_MEMBERS; i++) {
        Member *m = &members[i];
        if (!m->used) continue;
        w_str(&k, m->nick);
        w_bytes(&k, m->ip, 4);
        w_str(&k, m->game);
        w_u64(&k, m->game_id);
        w_str(&k, m->version);
        w_str(&k, "");                      /* username, display name, avatar url */
        w_str(&k, "");
        w_str(&k, "");
    }
    send_all(&k, NULL);
    free(k.p);
    enet_host_flush(host);
}

static void send_status(u8 type, const char *nick) {
    Pkt k = {0};
    w_u8(&k, ID_STATUS_MESSAGE);
    w_u8(&k, type);
    w_str(&k, nick);
    w_str(&k, "");                          /* username */
    send_all(&k, NULL);
    free(k.p);
}

/* 192.168.1.1, 192.168.1.2, ...: the first one nobody has */
static void free_ip(u8 ip[4]) {
    for (int i = 1; i < 255; i++)
        for (int j = 1; j < 255; j++) {
            u8 a[4] = {192, 168, (u8)i, (u8)j};
            if (!by_ip(a)) { memcpy(ip, a, 4); return; }
        }
}

/* ---------------------------------------------------------------------------------------------- handlers */
static void on_join(ENetPeer *peer, Rd *r) {
    char nick[64], pw[256], token[512];
    u8 want[4];
    r_str(r, nick, sizeof nick);
    r_take(r, want, 4);
    u32 version = r_u32(r);
    r_str(r, pw, sizeof pw);
    r_str(r, token, sizeof token);
    char addr[64];
    enet_address_get_host_ip(&peer->address, addr, sizeof addr);
    if (member_count() >= (int)slots) { logf_("join from %s (%s): room full", nick, addr); send_id(peer, ID_ROOM_IS_FULL); return; }
    if (strcmp(pw, password)) { logf_("join from %s (%s): wrong password", nick, addr); send_id(peer, ID_WRONG_PASSWORD); return; }
    bool taken = !*nick;
    for (int i = 0; i < MAX_MEMBERS; i++) if (members[i].used && !strcmp(members[i].nick, nick)) taken = true;
    if (taken) { logf_("join from \"%s\" (%s): nickname taken or empty", nick, addr); send_id(peer, ID_NAME_COLLISION); return; }
    u8 ip[4];
    if (want[0] == 255 && want[1] == 255 && want[2] == 255 && want[3] == 255) free_ip(ip);
    else if (by_ip(want)) { logf_("join from %s (%s): address taken", nick, addr); send_id(peer, ID_IP_COLLISION); return; }
    else memcpy(ip, want, 4);
    if (version != NET_VERSION) {
        logf_("join from %s (%s): protocol version %u, ours %u", nick, addr, version, NET_VERSION);
        send_id(peer, ID_VERSION_MISMATCH);
        return;
    }
    Member *m = NULL;
    for (int i = 0; i < MAX_MEMBERS && !m; i++) if (!members[i].used) m = &members[i];
    if (!m) { send_id(peer, ID_ROOM_IS_FULL); return; }
    memset(m, 0, sizeof *m);
    m->used = true;
    m->peer = peer;
    snprintf(m->nick, sizeof m->nick, "%s", nick);
    memcpy(m->ip, ip, 4);
    logf_("%s joined from %s, address %u.%u.%u.%u", nick, addr, ip[0], ip[1], ip[2], ip[3]);
    send_status(STATUS_MEMBER_JOIN, nick);
    broadcast_room_information();
    Pkt k = {0};
    w_u8(&k, ID_JOIN_SUCCESS);
    w_bytes(&k, ip, 4);
    send_to(peer, &k);
    free(k.p);
    enet_host_flush(host);
}

static void on_game_info(ENetPeer *peer, Rd *r) {
    Member *m = by_peer(peer);
    if (!m) return;
    r_str(r, m->game, sizeof m->game);
    m->game_id = r_u64(r);
    r_str(r, m->version, sizeof m->version);
    logf_("%s plays \"%s\" %016llx %s", m->nick, *m->game ? m->game : "(nothing)", (unsigned long long)m->game_id,
          m->version);
    broadcast_room_information();
}

static void on_chat(ENetPeer *peer, Rd *r) {
    Member *m = by_peer(peer);
    char msg[600];
    r_str(r, msg, sizeof msg);
    if (!m || !r->ok) return;
    logf_("chat <%s> %s", m->nick, msg);
    Pkt k = {0};
    w_u8(&k, ID_CHAT_MESSAGE);
    w_str(&k, m->nick);
    w_str(&k, "");
    w_str(&k, msg);
    send_all(&k, NULL);
    free(k.p);
}

/* traffic counters per sender, for the summary line */
static long n_ldn[MAX_MEMBERS], n_proxy[MAX_MEMBERS], n_proxy_bcast[MAX_MEMBERS], n_lost[MAX_MEMBERS];

/* LDN and proxy packets: forwarded unchanged */
static void relay(ENetPeer *peer, const u8 *data, size_t n, bool proxy) {
    Member *from = by_peer(peer);
    if (!from) return;
    int fi = (int)(from - members);
    Rd r = {data, (u32)n, 1, true};
    u8 local[4], remote[4], ldn_type = 0, proto = 0;
    u16 lport = 0, rport = 0;
    if (proxy) {
        r_u8(&r); r_take(&r, local, 4); lport = r_u16(&r);
        r_u8(&r); r_take(&r, remote, 4); rport = r_u16(&r);
        proto = r_u8(&r);
    } else {
        ldn_type = r_u8(&r);
        r_take(&r, local, 4);
        r_take(&r, remote, 4);
    }
    bool bcast = r_u8(&r) != 0;
    u32 len = r_u32(&r);
    if (!r.ok) { logf_("%s: malformed %s packet (%zu bytes)", from->nick, proxy ? "proxy" : "LDN", n); return; }
    Pkt k = {(u8 *)data, (u32)n, 0};
    Member *to = NULL;
    if (bcast) send_all(&k, peer);
    else if ((to = by_ip(remote))) send_to(to->peer, &k);
    else n_lost[fi]++;
    if (proxy) { n_proxy[fi]++; n_proxy_bcast[fi] += bcast; } else n_ldn[fi]++;
    bool first = proxy ? n_proxy[fi] == 1 : false;
    if (log_level >= 2 || !proxy || first) {
        char dest[64];
        if (bcast) snprintf(dest, sizeof dest, "everyone");
        else if (to) snprintf(dest, sizeof dest, "%s", to->nick);
        else snprintf(dest, sizeof dest, "nobody (no member has that address)");
        if (proxy) {
            /* the payload: decompressed, its size and first bytes */
            char hex[3 * 48 + 1] = "";
            long long plain = -1;
            const u8 *c = data + r.pos;
            u8 buf[2048];
            size_t got = ZSTD_decompress(buf, sizeof buf, c, len);
            if (!ZSTD_isError(got)) {
                plain = (long long)got;
                for (size_t i = 0; i < got && i < 48; i++) snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
            }
            logf_("%s%s: proxy %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, protocol %u, %s, %u bytes compressed, %lld plain, to %s",
                  first ? "first from " : "", from->nick, local[0], local[1], local[2], local[3], lport, remote[0],
                  remote[1], remote[2], remote[3], rport, proto, bcast ? "broadcast" : "unicast", len, plain, dest);
            if (plain >= 0) logf_("    %s", hex);
            else logf_("    payload does not decompress");
        }
        else
            logf_("%s: LDN %s %u.%u.%u.%u -> %u.%u.%u.%u%s, %u bytes, to %s", from->nick,
                  ldn_type < 6 ? ldn_names[ldn_type] : "?", local[0], local[1], local[2], local[3], remote[0], remote[1],
                  remote[2], remote[3], bcast ? " (broadcast)" : "", len, dest);
    }
}

static void on_disconnect(ENetPeer *peer) {
    Member *m = by_peer(peer);
    if (!m) return;
    int i = (int)(m - members);
    logf_("%s left (sent %ld LDN, %ld proxy of which %ld broadcast, %ld to unknown addresses)", m->nick, n_ldn[i],
          n_proxy[i], n_proxy_bcast[i], n_lost[i]);
    char nick[64];
    snprintf(nick, sizeof nick, "%s", m->nick);
    m->used = false;
    n_ldn[i] = n_proxy[i] = n_proxy_bcast[i] = n_lost[i] = 0;
    send_status(STATUS_MEMBER_LEAVE, nick);
    broadcast_room_information();
}

static void summary(void) {
    static long last;
    long sum = 0;
    for (int i = 0; i < MAX_MEMBERS; i++) sum += n_ldn[i] + n_proxy[i];
    if (sum == last) return;
    last = sum;
    for (int i = 0; i < MAX_MEMBERS; i++)
        if (members[i].used)
            logf_("  %s: sent %ld LDN, %ld proxy (%ld broadcast), %ld to unknown addresses", members[i].nick, n_ldn[i],
                  n_proxy[i], n_proxy_bcast[i], n_lost[i]);
}

/* ---------------------------------------------------------------------------------------------- main */
int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *a = argv[i], *v = argv[i + 1];
        if (!strcmp(a, "--port")) port = (u16)atoi(v);
        else if (!strcmp(a, "--name")) room_name = v;
        else if (!strcmp(a, "--password")) password = v;
        else if (!strcmp(a, "--slots")) slots = (u32)atoi(v);
        else if (!strcmp(a, "--game")) game_name = v;
        else if (!strcmp(a, "--game-id")) game_id = strtoull(v, NULL, 16);
        else if (!strcmp(a, "--log")) log_level = atoi(v);
        else { fprintf(stderr, "unknown option %s\n", a); return 1; }
    }
    if (slots > MAX_MEMBERS) slots = MAX_MEMBERS;
    (void)game_id;
    if (enet_initialize() != 0) { fprintf(stderr, "ENet could not start\n"); return 1; }
    ENetAddress addr = {0};
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    host = enet_host_create(&addr, slots + 1, 1, 0, 0);     /* one spare slot, so a full room can say so */
    if (!host) { fprintf(stderr, "could not listen on UDP port %u\n", port); return 1; }
    logf_("room \"%s\" listening on UDP port %u, %u slots%s", room_name, port, slots, *password ? ", with a password" : "");
    time_t last = time(NULL);
    for (;;) {
        ENetEvent ev;
        int got = enet_host_service(host, &ev, 5);
        if (got > 0) {
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                char a[64];
                enet_address_get_host_ip(&ev.peer->address, a, sizeof a);
                if (log_level >= 2) logf_("connection from %s:%u", a, ev.peer->address.port);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const u8 *d = ev.packet->data;
                size_t n = ev.packet->dataLength;
                Rd r = {d, (u32)n, 1, true};
                if (n) switch (d[0]) {
                    case ID_JOIN_REQUEST: on_join(ev.peer, &r); break;
                    case ID_SET_GAME_INFO: on_game_info(ev.peer, &r); break;
                    case ID_CHAT_MESSAGE: on_chat(ev.peer, &r); break;
                    case ID_PROXY_PACKET: relay(ev.peer, d, n, true); break;
                    case ID_LDN_PACKET: relay(ev.peer, d, n, false); break;
                    default: if (log_level >= 2) logf_("message type %u ignored", d[0]); break;
                    }
                enet_packet_destroy(ev.packet);
                enet_host_flush(host);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                on_disconnect(ev.peer);
                break;
            default: break;
            }
        }
        if (log_level >= 1 && time(NULL) - last >= 5) { last = time(NULL); summary(); }
    }
}
