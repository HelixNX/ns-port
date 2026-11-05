/* The room client. One ENet host with one peer (the room server). room_join connects and waits for the join answer
 * on the calling thread; then a host thread owns ENet: it hands received LDN and proxy packets to the services'
 * handlers and sends what other threads queued (ENet is not thread-safe, so no other thread touches it). It sleeps
 * until the socket has data or a thread queues a packet (send_event), else wakes every few ms for ENet's timers:
 * a packet the game sends goes out at once (with a plain timeout it waited for the next tick, up to the system
 * timer's 15.6 ms, on every hop of every exchange).
 * Messages (type byte first; see net.h for the encoding):
 *   JoinRequest   nickname, preferred fake ip (ff ff ff ff: any), network version, password, token ("": anonymous)
 *   SetGameInfo   name, title id, version
 *   RoomInformation  name, description, slots, port, preferred game name, host username, then the members
 *                    (nickname, fake ip, game name, game id, game version, username, display name, avatar url)
 *   JoinSuccess[AsMod]  our fake ip
 *   LdnPacket     type, local ip, remote ip, broadcast, data
 *   ProxyPacket   local (domain, ip, port), remote (domain, ip, port), protocol, broadcast, data (zstd frame)
 * The server relays LDN and proxy packets to the member with the remote ip, or to everyone else when broadcast. */
#include <enet/enet.h>
#include <zstd.h>
#include "net.h"
#include <stdlib.h>
#include <process.h>

enum {
    ID_JOIN_REQUEST = 1, ID_JOIN_SUCCESS, ID_ROOM_INFORMATION, ID_SET_GAME_INFO, ID_PROXY_PACKET, ID_LDN_PACKET,
    ID_CHAT_MESSAGE, ID_NAME_COLLISION, ID_IP_COLLISION, ID_VERSION_MISMATCH, ID_WRONG_PASSWORD, ID_CLOSE_ROOM,
    ID_ROOM_IS_FULL, ID_STATUS_MESSAGE, ID_HOST_KICKED, ID_HOST_BANNED,
    ID_MOD_KICK, ID_MOD_BAN, ID_MOD_UNBAN, ID_MOD_GET_BAN_LIST, ID_MOD_BAN_LIST_RESPONSE, ID_MOD_PERMISSION_DENIED,
    ID_MOD_NO_SUCH_USER, ID_JOIN_SUCCESS_AS_MOD,
};
enum { STATUS_MEMBER_JOIN = 1, STATUS_MEMBER_LEAVE, STATUS_MEMBER_KICKED, STATUS_MEMBER_BANNED, STATUS_UNBANNED };
#define DOMAIN_INET 1                       /* Network::Domain::INET */
#define CONNECT_TIMEOUT_MS 5000

/* ---------------------------------------------------------------------------------------------- packets */
typedef struct Pkt { u8 *p; u32 n, cap; } Pkt;

static void w_bytes(Pkt *k, const void *d, u32 n) {
    if (k->n + n > k->cap) {
        k->cap = (k->n + n) * 2 + 64;
        k->p = realloc(k->p, k->cap);
    }
    memcpy(k->p + k->n, d, n);
    k->n += n;
}
static void w_u8(Pkt *k, u8 v) { w_bytes(k, &v, 1); }
static void w_u16(Pkt *k, u16 v) { u8 b[2] = {v >> 8, (u8)v}; w_bytes(k, b, 2); }
static void w_u32(Pkt *k, u32 v) { u8 b[4] = {v >> 24, v >> 16, v >> 8, (u8)v}; w_bytes(k, b, 4); }
static void w_u64(Pkt *k, u64 v) { w_u32(k, (u32)(v >> 32)); w_u32(k, (u32)v); }
static void w_vec(Pkt *k, const void *d, u32 n) { w_u32(k, n); w_bytes(k, d, n); }
static void w_str(Pkt *k, const char *s) { w_vec(k, s, (u32)strlen(s)); }

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
/* a string into a fixed buffer (truncated) */
static void r_str(Rd *r, char *out, size_t n) {
    u32 len = r_u32(r);
    if (!r->ok || r->pos + len > r->n) { r->ok = false; if (n) out[0] = 0; return; }
    size_t c = len < n - 1 ? len : n - 1;
    memcpy(out, r->p + r->pos, c);
    out[c] = 0;
    r->pos += len;
}
/* a vector in place */
static const u8 *r_vec(Rd *r, u32 *len) {
    *len = r_u32(r);
    const u8 *d = r->p + r->pos;
    if (!r_take(r, NULL, *len)) { *len = 0; return NULL; }
    return d;
}

/* ---------------------------------------------------------------------------------------------- state */
static ENetHost *host;
static ENetPeer *server;
static volatile bool joined;
static volatile bool stopping;
static HANDLE loop_thread;
static u8 fake_ip[4] = {255, 255, 255, 255};
static char nickname[64], room_title[256];
static char game_name[128] = "", game_version[32] = "";
static u64 game_id;
static void (*ldn_handler)(const NetLdnPacket *p);
static void (*proxy_handler)(const NetProxyPacket *p);
static void (*disconnect_handler)(void);

/* packets other threads send: the host thread passes them to ENet */
typedef struct Queued { struct Queued *next; u32 n; u8 data[]; } Queued;
static SRWLOCK send_lock = SRWLOCK_INIT;
static Queued *send_head, *send_tail;
static HANDLE send_event;                   /* set when a packet is queued: wakes the host thread */

/* messages by type, for the traffic line in the log */
static volatile long rx_count[32], tx_count[32];

static void queue_send(Pkt *k) {
    if (k->n) tx_count[k->p[0] & 31]++;
    Queued *q = malloc(sizeof *q + k->n);
    q->next = NULL;
    q->n = k->n;
    memcpy(q->data, k->p, k->n);
    free(k->p);
    AcquireSRWLockExclusive(&send_lock);
    if (send_tail) send_tail->next = q; else send_head = q;
    send_tail = q;
    ReleaseSRWLockExclusive(&send_lock);
    if (send_event) SetEvent(send_event);
}

static void flush_sends(void) {
    AcquireSRWLockExclusive(&send_lock);
    Queued *q = send_head;
    send_head = send_tail = NULL;
    ReleaseSRWLockExclusive(&send_lock);
    while (q) {
        Queued *next = q->next;
        if (server) enet_peer_send(server, 0, enet_packet_create(q->data, q->n, ENET_PACKET_FLAG_RELIABLE));
        free(q);
        q = next;
    }
    if (host) enet_host_flush(host);
}

void room_set_game(const char *name, u64 id, const char *version) {
    snprintf(game_name, sizeof game_name, "%s", name);
    snprintf(game_version, sizeof game_version, "%s", version);
    game_id = id;
}
void room_on_ldn(void (*fn)(const NetLdnPacket *p)) { ldn_handler = fn; }
void room_on_proxy(void (*fn)(const NetProxyPacket *p)) { proxy_handler = fn; }
void room_on_disconnect(void (*fn)(void)) { disconnect_handler = fn; }
bool room_joined(void) { return joined; }
void room_fake_ip(u8 ip[4]) { memcpy(ip, fake_ip, 4); }
const char *room_name(void) { return room_title; }

/* ---------------------------------------------------------------------------------------------- receiving */
static void on_room_information(Rd *r) {
    char desc[1024], game[128], host_user[64], s[256];
    r_str(r, room_title, sizeof room_title);
    r_str(r, desc, sizeof desc);
    u32 slots = r_u32(r);
    r_u16(r);                                           /* port */
    r_str(r, game, sizeof game);
    r_str(r, host_user, sizeof host_user);
    u32 n = r_u32(r);
    hlog(1, "room: \"%s\", %u of %u members, preferred game \"%s\"", room_title, n, slots, game);
    for (u32 i = 0; i < n && r->ok; i++) {
        char nick[64], gname[128], gver[64];
        u8 ip[4];
        r_str(r, nick, sizeof nick);
        r_take(r, ip, 4);
        r_str(r, gname, sizeof gname);
        u64 gid = r_u64(r);
        r_str(r, gver, sizeof gver);
        r_str(r, s, sizeof s);                          /* username */
        r_str(r, s, sizeof s);                          /* display name */
        r_str(r, s, sizeof s);                          /* avatar url */
        hlog(1, "room:   %s (%u.%u.%u.%u) %s%s%016llx%s%s", nick, ip[0], ip[1], ip[2], ip[3], gname, *gname ? " " : "", gid,
             *gver ? " version " : "", gver);
    }
}

static void on_proxy(Rd *r) {
    NetProxyPacket p = {0};
    r_u8(r);
    r_take(r, p.local_ip, 4);
    p.local_port = r_u16(r);
    r_u8(r);
    r_take(r, p.remote_ip, 4);
    p.remote_port = r_u16(r);
    p.protocol = r_u8(r);
    p.broadcast = r_u8(r) != 0;
    u32 clen;
    const u8 *c = r_vec(r, &clen);
    if (!r->ok || !proxy_handler) return;
    unsigned long long n = ZSTD_getFrameContentSize(c, clen);
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n > 0x100000) {
        hlog(1, "room: proxy packet with a bad zstd frame (%u bytes)", clen);
        return;
    }
    u8 *d = malloc(n ? (size_t)n : 1);
    size_t got = ZSTD_decompress(d, (size_t)n, c, clen);
    if (ZSTD_isError(got) || got != n) hlog(1, "room: proxy packet that does not decompress (%u bytes)", clen);
    else {
        p.data = d;
        p.size = (u32)n;
        proxy_handler(&p);
    }
    free(d);
}

static void on_ldn(Rd *r) {
    NetLdnPacket p = {0};
    p.type = r_u8(r);
    r_take(r, p.local_ip, 4);
    r_take(r, p.remote_ip, 4);
    p.broadcast = r_u8(r) != 0;
    p.data = r_vec(r, &p.size);
    if (r->ok && ldn_handler) ldn_handler(&p);
}

static void on_chat(Rd *r) {
    char nick[64], user[64], msg[600];
    r_str(r, nick, sizeof nick);
    r_str(r, user, sizeof user);
    r_str(r, msg, sizeof msg);
    hlog(0, "room chat: <%s> %s", nick, msg);
}

static void on_status(Rd *r) {
    static const char *what[] = {"?", "joined", "left", "was kicked", "was banned", "was unbanned"};
    u8 type = r_u8(r);
    char nick[64], user[64];
    r_str(r, nick, sizeof nick);
    r_str(r, user, sizeof user);
    hlog(0, "room: %s %s", nick, what[type <= STATUS_UNBANNED ? type : 0]);
}

static const char *join_error(u8 id) {
    switch (id) {
    case ID_NAME_COLLISION: return "That nickname is already in use in this room, or is not valid there.";
    case ID_IP_COLLISION: return "The room gave our address to someone else.";
    case ID_VERSION_MISMATCH: return "The room runs a different version of the multiplayer protocol.";
    case ID_WRONG_PASSWORD: return "Wrong password.";
    case ID_ROOM_IS_FULL: return "The room is full.";
    case ID_HOST_BANNED: return "You are banned from this room.";
    case ID_HOST_KICKED: return "You were kicked from this room.";
    case ID_CLOSE_ROOM: return "The room was closed.";
    default: return NULL;
    }
}

/* one received packet; returns the join error it reports, if any */
static const char *handle_packet(const u8 *data, size_t n) {
    if (!n) return NULL;
    rx_count[data[0] & 31]++;
    Rd r = {data, (u32)n, 1, true};
    switch (data[0]) {
    case ID_PROXY_PACKET: on_proxy(&r); break;
    case ID_LDN_PACKET: on_ldn(&r); break;
    case ID_CHAT_MESSAGE: on_chat(&r); break;
    case ID_STATUS_MESSAGE: on_status(&r); break;
    case ID_ROOM_INFORMATION: on_room_information(&r); break;
    case ID_JOIN_SUCCESS:
    case ID_JOIN_SUCCESS_AS_MOD:
        r_take(&r, fake_ip, 4);
        joined = true;
        break;
    default:
        return join_error(data[0]);
    }
    return NULL;
}

static void log_traffic(void) {
    static long last;
    long sum = rx_count[ID_PROXY_PACKET] + rx_count[ID_LDN_PACKET] + tx_count[ID_PROXY_PACKET] + tx_count[ID_LDN_PACKET];
    if (sum == last) return;
    last = sum;
    hlog(1, "room traffic: received %ld proxy, %ld ldn, %ld other; sent %ld proxy, %ld ldn; round trip to the server %u ms",
         rx_count[ID_PROXY_PACKET], rx_count[ID_LDN_PACKET],
         rx_count[ID_ROOM_INFORMATION] + rx_count[ID_STATUS_MESSAGE] + rx_count[ID_CHAT_MESSAGE], tx_count[ID_PROXY_PACKET],
         tx_count[ID_LDN_PACKET], server ? server->roundTripTime : 0);
}

static unsigned __stdcall loop(void *arg) {
    (void)arg;
    DWORD last_log = GetTickCount();
    HANDLE sock_event = WSACreateEvent();
    WSAEventSelect(host->socket, sock_event, FD_READ);
    HANDLE waits[2] = {sock_event, send_event};
    while (!stopping) {
        if (GetTickCount() - last_log >= 3000) { last_log = GetTickCount(); log_traffic(); }
        bool lost = false;
        ENetEvent ev;
        int got;
        while (!lost && (got = enet_host_service(host, &ev, 0)) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                const char *err = handle_packet(ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                if (err) {
                    hlog(0, "room: %s", err);
                    joined = false;
                }
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                hlog(0, "room: lost the connection to the room server");
                server = NULL;
                joined = false;
                lost = true;
            }
        }
        bool was = joined;
        flush_sends();
        if (!server || !was) break;
        WaitForMultipleObjects(2, waits, FALSE, 5);
        WSAResetEvent(sock_event);
    }
    WSACloseEvent(sock_event);
    joined = false;
    if (!stopping && disconnect_handler) disconnect_handler();
    return 0;
}

/* ---------------------------------------------------------------------------------------------- joining */
static void send_game_info(void) {
    Pkt k = {0};
    w_u8(&k, ID_SET_GAME_INFO);
    w_str(&k, game_name);
    w_u64(&k, game_id);
    w_str(&k, game_version);
    queue_send(&k);
}

static void drop_connection(void) {
    if (server) {
        enet_peer_disconnect(server, 0);
        ENetEvent ev;
        while (enet_host_service(host, &ev, 500) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { server = NULL; break; }
        }
        if (server) enet_peer_reset(server);
        server = NULL;
    }
    if (host) enet_host_destroy(host);
    host = NULL;
}

bool room_join(const char *hostname, u16 port, const char *nick, const char *password, char *err, size_t errn) {
    static bool enet_ready;
    if (!enet_ready) {
        if (enet_initialize() != 0) { snprintf(err, errn, "ENet could not start."); return false; }
        enet_ready = true;
    }
    room_leave();
    stopping = false;
    joined = false;
    memset(fake_ip, 255, 4);
    host = enet_host_create(NULL, 1, 1, 0, 0);
    if (!host) { snprintf(err, errn, "Could not create a network socket."); return false; }
    ENetAddress addr = {0};
    if (enet_address_set_host(&addr, hostname) != 0) {
        snprintf(err, errn, "Could not resolve \"%s\".", hostname);
        drop_connection();
        return false;
    }
    addr.port = port;
    server = enet_host_connect(host, &addr, 1, 0);
    ENetEvent ev;
    if (!server || enet_host_service(host, &ev, CONNECT_TIMEOUT_MS) <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) {
        snprintf(err, errn, "Could not connect to %s:%u (no answer).", hostname, port);
        drop_connection();
        return false;
    }
    snprintf(nickname, sizeof nickname, "%s", nick);
    Pkt k = {0};
    w_u8(&k, ID_JOIN_REQUEST);
    w_str(&k, nickname);
    w_bytes(&k, (const u8[]){255, 255, 255, 255}, 4);
    w_u32(&k, NET_VERSION);
    w_str(&k, password ? password : "");
    w_str(&k, "");
    queue_send(&k);
    send_game_info();
    flush_sends();
    /* the answer: room information, then JoinSuccess or an error */
    DWORD start = GetTickCount();
    const char *jerr = NULL;
    while (!joined && !jerr && GetTickCount() - start < CONNECT_TIMEOUT_MS) {
        int got = enet_host_service(host, &ev, 50);
        if (got > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
            jerr = handle_packet(ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
        } else if (got > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            server = NULL;
            jerr = "The room server closed the connection.";
        }
    }
    if (!joined) {
        snprintf(err, errn, "%s", jerr ? jerr : "The room did not answer the join request.");
        drop_connection();
        return false;
    }
    hlog(0, "room: joined \"%s\" at %s:%u as %s, address %u.%u.%u.%u", room_title, hostname, port, nickname,
         fake_ip[0], fake_ip[1], fake_ip[2], fake_ip[3]);
    if (!send_event) send_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    loop_thread = (HANDLE)_beginthreadex(NULL, 0, loop, NULL, 0, NULL);
    return true;
}

void room_leave(void) {
    if (loop_thread) {
        stopping = true;
        WaitForSingleObject(loop_thread, INFINITE);
        CloseHandle(loop_thread);
        loop_thread = NULL;
    }
    drop_connection();
    joined = false;
    memset(fake_ip, 255, 4);
}

/* ---------------------------------------------------------------------------------------------- sending */
void room_send_ldn(const NetLdnPacket *p) {
    if (!joined) return;
    Pkt k = {0};
    w_u8(&k, ID_LDN_PACKET);
    w_u8(&k, p->type);
    w_bytes(&k, p->local_ip, 4);
    w_bytes(&k, p->remote_ip, 4);
    w_u8(&k, p->broadcast);
    w_vec(&k, p->data, p->size);
    queue_send(&k);
}

void room_send_proxy(const NetProxyPacket *p) {
    if (!joined) return;
    size_t bound = ZSTD_compressBound(p->size);
    u8 *c = malloc(bound);
    size_t cn = ZSTD_compress(c, bound, p->data, p->size, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(cn)) { free(c); return; }
    Pkt k = {0};
    w_u8(&k, ID_PROXY_PACKET);
    w_u8(&k, DOMAIN_INET);
    w_bytes(&k, p->local_ip, 4);
    w_u16(&k, p->local_port);
    w_u8(&k, DOMAIN_INET);
    w_bytes(&k, p->remote_ip, 4);
    w_u16(&k, p->remote_port);
    w_u8(&k, p->protocol);
    w_u8(&k, p->broadcast);
    w_vec(&k, c, (u32)cn);
    free(c);
    queue_send(&k);
}
