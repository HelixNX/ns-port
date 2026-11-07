/* Netplay (games built with nx_game(... NETPLAY)): local wireless (ldn:u) and the sockets on its network (bsd:u),
 * carried by a community room server in the protocol Citra, yuzu and Eden use (their "multiplayer rooms"), so that
 * our players and theirs meet in the same rooms.
 *   room.c     the room client: ENet to the room server, its packets (join, LDN, proxy), zstd for proxy payloads
 *   lobby.c    the public room list (HTTPS GET <lobby>/lobby, JSON)
 *   browser.c  the room browser shown at start-up (Win32), or NX_ROOM to join without it
 * Room protocol facts (network_version 1): every message is an ENet reliable packet on channel 0 whose first byte is
 * its type; integers are big-endian, strings and vectors have a u32 length in front, IPv4 addresses are 4 raw bytes.
 * The room gives each member a fake address 192.168.x.y; LDN and proxy packets name members by it. */
#ifndef NET_H
#define NET_H

#include "../hos.h"

#define NET_VERSION       1                 /* the room protocol version we speak */
#define NET_DEFAULT_PORT  24872
#define NET_LOBBY_HOST    L"api.ynet-fun.xyz"   /* Eden's default web service (web_api_url) */

/* LDN packet types (the room relays them unchanged) */
enum { LDN_SCAN, LDN_SCAN_RESP, LDN_CONNECT, LDN_SYNC_NETWORK, LDN_DISCONNECT, LDN_DESTROY_NETWORK };

typedef struct NetLdnPacket {
    u8 type;
    u8 local_ip[4], remote_ip[4];           /* sender and receiver (network order) */
    bool broadcast;
    const u8 *data;
    u32 size;
} NetLdnPacket;

/* a datagram on the room's network; endpoints in network byte order, ports in host order */
typedef struct NetProxyPacket {
    u8 local_ip[4], remote_ip[4];           /* sender and receiver */
    u16 local_port, remote_port;
    u8 protocol;
    bool broadcast;
    const u8 *data;                         /* uncompressed */
    u32 size;
} NetProxyPacket;

/* ---------------------------------------------------------------------------------------------- room.c */
/* connects and joins (blocking, up to ~10 s); false with a message in err. On success a host thread keeps the
 * connection, and the handlers below are called on it */
bool room_join(const char *host, u16 port, const char *nickname, const char *password, char *err, size_t errn);
void room_leave(void);
bool room_joined(void);                     /* joined and still connected */
void room_fake_ip(u8 ip[4]);                /* ours; 255.255.255.255 when not joined */
const char *room_name(void);
void room_set_game(const char *name, u64 id, const char *version);   /* before room_join */
void room_send_ldn(const NetLdnPacket *p);
void room_send_proxy(const NetProxyPacket *p);
void room_on_ldn(void (*fn)(const NetLdnPacket *p));
void room_on_proxy(void (*fn)(const NetProxyPacket *p));
void room_on_disconnect(void (*fn)(void));

/* ---------------------------------------------------------------------------------------------- lobby.c */
typedef struct LobbyMember {
    char nickname[64], username[64], game_name[128];
    u64 game_id;
} LobbyMember;

typedef struct LobbyRoom {
    char name[256], description[1024], owner[128], address[256], game_name[256];
    u64 game_id;
    u16 port;
    u32 max_players, net_version;
    bool has_password;
    int nmembers;
    LobbyMember *members;
} LobbyRoom;

int lobby_fetch(LobbyRoom **out, char *err, size_t errn);      /* the number of rooms, or -1 */
void lobby_free(LobbyRoom *rooms, int n);

/* ---------------------------------------------------------------------------------------------- browser.c */
/* before the game starts: NX_ROOM=host[:port[:password]] (with NX_NICK) joins directly, NX_NETPLAY=0 skips;
 * otherwise the room browser. Exits the process if the browser is closed */
void netplay_startup(const char *save_dir, const char *game_dir);

/* ---------------------------------------------------------------------------------------------- services */
void ldn_net_register(const char *game_dir); /* services/ldn_net.c: ldn:u on the room */
void bsd_register(void);                    /* services/bsd.c: bsd:u sockets on the room's network */

#endif
