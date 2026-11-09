/* bsd:u on a room (games built with NETPLAY, after joining one): sockets whose network is the room's. A datagram
 * the game sends goes to the room server as a proxy packet (net.h) from our fake address and the socket's port; a
 * proxy packet that arrives is queued on every socket bound to its destination port with its protocol. As with
 * Eden's proxy sockets (which ours must talk to), only UDP carries data; TCP connections are refused.
 * A proxy packet's protocol is the one the game passed to socket(), as a Network::Protocol; Eden drops packets whose
 * protocol differs from its socket's. Eden builds number it two ways: releases (and yuzu, Citra) have Unspecified 0,
 * ICMP 1, TCP 2, UDP 3; newer sources insert IP = 1, so TCP 3, UDP 4. We send the older numbering until a packet
 * shows the newer one (UDP 4, which the older numbering does not have), and datagram sockets accept either.
 * Without a room this service is not registered, and the generic stub answers the SDK as before.
 * Replies follow the SDK 4.4 proxies: (s32 ret, s32 errno[, u32 length]); errno values are the console's (Linux
 * numbering); a sockaddr is BSD's sockaddr_in: u8 len, u8 family (2), u16 port, u32 address (network order). */
#include "../ipc.h"
#include "../net/net.h"
#include <stdlib.h>
#include <windows.h>

enum { E_BADF = 9, E_AGAIN = 11, E_INVAL = 22, E_MFILE = 24, E_MSGSIZE = 90, E_OPNOTSUPP = 95, E_ADDRINUSE = 98,
       E_NOTCONN = 107, E_TIMEDOUT = 110, E_CONNREFUSED = 111 };
enum { SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2 };
enum { SOL_SOCKET_ = 0xffff, SO_REUSEADDR_ = 4, SO_BROADCAST_ = 0x20, SO_SNDBUF_ = 0x1001, SO_RCVBUF_ = 0x1002,
       SO_RCVTIMEO_ = 0x1006, SO_ERROR_ = 0x1007, SO_TYPE_ = 0x1008 };
enum { MSG_PEEK_ = 2, MSG_DONTWAIT_ = 0x80, O_NONBLOCK_ = 0x800, F_GETFL_ = 3, F_SETFL_ = 4 };
enum { POLLIN_ = 1, POLLOUT_ = 4, POLLERR_ = 8, POLLNVAL_ = 0x20 };
#define MAX_SOCKS 64
#define MAX_QUEUED 512
#define FD_BASE 3

typedef struct Dgram { struct Dgram *next; u8 ip[4]; u16 port; u32 size; u8 data[]; } Dgram;

typedef struct Sock {
    bool used, stream, bound, broadcast, nonblock, closed, connected;
    bool efd, efd_semaphore;                /* an eventfd (SDK 13 EventFd): no network, a counter */
    u64 efd_count;
    u32 proto;                              /* the game's socket() protocol (BSD numbers) */
    u8 ip[4];                               /* bound address (0.0.0.0: any) */
    u16 port;                               /* host order */
    u8 peer_ip[4];                          /* connect()ed UDP destination */
    u16 peer_port;
    u32 rcvtimeo_ms, sndbuf, rcvbuf;
    s32 flags;                              /* fcntl */
    Dgram *head, *tail;
    int queued;
} Sock;

static Sock socks[MAX_SOCKS];
static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE arrived = CONDITION_VARIABLE_INIT;   /* a datagram was queued or a socket closed */
static u16 next_ephemeral = 49152;
/* traffic counters for the log */
static volatile long sent_n, recv_n, drop_noport, drop_bcast;

/* which Network::Protocol numbering the room's other members use (see the top) */
static volatile bool newer_numbering;

/* the guest protocol (BSD numbers) as a Network::Protocol: older Unspecified 0, ICMP 1, TCP 2, UDP 3;
 * newer Unspecified 0, IP 1, ICMP 2, TCP 3, UDP 4 */
static u8 wire_protocol(u32 proto, bool stream) {
    bool n = newer_numbering;
    switch (proto) {
    case 0: return n ? 1 : 0;
    case 1: return n ? 2 : 1;
    case 6: return n ? 3 : 2;
    case 17: return n ? 4 : 3;
    default: return stream ? (n ? 3 : 2) : (n ? 4 : 3);
    }
}

static Sock *get(s32 fd) {
    s32 i = fd - FD_BASE;
    return i >= 0 && i < MAX_SOCKS && socks[i].used ? &socks[i] : NULL;
}

/* NX_LOG 1 traces the first calls of each command (what the game asks, what it gets) */
static bool trace(IpcReq *r) {
    static volatile long calls[64];
    return r->cmd < 64 && ++calls[r->cmd] <= 12;
}

static void reply(IpcReq *r, s32 ret, s32 err) { out_u32(r, (u32)ret); out_u32(r, (u32)err); }
static void reply_errno(IpcReq *r, s32 err) { reply(r, err ? -1 : 0, err); }

static void free_queue(Sock *s) {
    while (s->head) { Dgram *d = s->head; s->head = d->next; free(d); }
    s->tail = NULL;
    s->queued = 0;
}

static bool port_in_use(u16 port, bool stream) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used && socks[i].bound && socks[i].port == port && socks[i].stream == stream) return true;
    return false;
}

static void auto_bind(Sock *s) {
    if (s->bound) return;
    do { s->port = next_ephemeral++; if (!next_ephemeral) next_ephemeral = 49152; } while (port_in_use(s->port, s->stream));
    memset(s->ip, 0, 4);
    s->bound = true;
}

static void read_addr(const IpcBuf *b, u8 ip[4], u16 *port) {
    memset(ip, 0, 4);
    *port = 0;
    if (!b || b->size < 8) return;
    const u8 *a = (const u8 *)(uintptr_t)b->addr;
    *port = (u16)(a[2] << 8 | a[3]);
    memcpy(ip, a + 4, 4);
}

/* writes a sockaddr_in into the i-th output buffer; returns its length */
static u32 write_addr(IpcReq *r, int i, const u8 ip[4], u16 port) {
    const IpcBuf *b = buf_out(r, i);
    if (!b || b->size < 8) return 0;
    u8 a[16] = {16, 2, (u8)(port >> 8), (u8)port, ip[0], ip[1], ip[2], ip[3]};
    u32 n = b->size < 16 ? (u32)b->size : 16;
    memcpy((void *)(uintptr_t)b->addr, a, n);
    return n;
}

/* ---------------------------------------------------------------------------------------------- the room */
static void on_proxy(const NetProxyPacket *p) {
    u8 ours[4];
    room_fake_ip(ours);
    if (p->protocol == 4 && !newer_numbering) {
        newer_numbering = true;
        hlog(1, "bsd: a member sends UDP as protocol 4: using the newer Eden protocol numbering");
    } else if (p->protocol == 3 && newer_numbering) {
        newer_numbering = false;
        hlog(1, "bsd: a member sends UDP as protocol 3: using the older Eden protocol numbering");
    }
    AcquireSRWLockExclusive(&lock);
    bool any = false;
    for (int i = 0; i < MAX_SOCKS; i++) {
        Sock *s = &socks[i];
        if (!s->used || s->closed || !s->bound || s->port != p->remote_port) continue;
        /* only datagrams carry data (Eden's proxy TCP is a stub); they come as UDP, IP or unspecified in either
         * numbering, so datagram sockets take every protocol */
        if (s->stream) continue;
        if (p->broadcast && !s->broadcast) { drop_bcast++; continue; }
        if (s->queued >= MAX_QUEUED) { Dgram *d = s->head; s->head = d->next; if (!s->head) s->tail = NULL; free(d); s->queued--; }
        Dgram *d = malloc(sizeof *d + p->size);
        d->next = NULL;
        memcpy(d->ip, p->local_ip, 4);
        d->port = p->local_port;
        d->size = p->size;
        memcpy(d->data, p->data, p->size);
        if (s->tail) s->tail->next = d; else s->head = d;
        s->tail = d;
        s->queued++;
        any = true;
    }
    ReleaseSRWLockExclusive(&lock);
    if (any) { recv_n++; WakeAllConditionVariable(&arrived); }
    else drop_noport++;
    if (recv_n + drop_noport == 1 || (recv_n + drop_noport) % 2000 == 0)
        hlog(1, "bsd: %ld datagrams sent, %ld received; dropped %ld with no socket on their port, %ld broadcasts "
             "(last from %u.%u.%u.%u:%u to port %u, protocol %u, %u bytes)", sent_n, recv_n, drop_noport, drop_bcast,
             p->local_ip[0], p->local_ip[1], p->local_ip[2], p->local_ip[3], p->local_port, p->remote_port, p->protocol, p->size);
}

static void send_dgram(Sock *s, const u8 ip[4], u16 port, const void *data, u32 size) {
    NetProxyPacket p = {0};
    memcpy(p.local_ip, s->ip, 4);
    if (!(s->ip[0] | s->ip[1] | s->ip[2] | s->ip[3])) room_fake_ip(p.local_ip);
    p.local_port = s->port;
    memcpy(p.remote_ip, ip, 4);
    p.remote_port = port;
    p.protocol = wire_protocol(s->proto, s->stream);
    p.broadcast = s->broadcast && ip[3] == 255;
    p.data = data;
    p.size = size;
    room_send_proxy(&p);
    long n = ++sent_n;
    if (n == 1 || n == 100 || n % 2000 == 0)
        hlog(1, "bsd: %ld datagrams sent, %ld received; dropped %ld with no socket on their port, %ld broadcasts",
             n, recv_n, drop_noport, drop_bcast);
    if (n == 1)
        hlog(1, "bsd: first datagram sent, to %u.%u.%u.%u:%u from port %u (protocol %u, %u bytes)", ip[0], ip[1], ip[2], ip[3],
             port, s->port, p.protocol, size);
}

/* milliseconds from QPC, for timeouts. GetTickCount moves in 15.6 ms steps: a 1-2 ms poll or receive timeout saw no
 * time pass and waited again, up to ~16 ms (as kernel.c's wait deadlines did) */
static DWORD tick_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (DWORD)(c.QuadPart / f.QuadPart * 1000 + c.QuadPart % f.QuadPart * 1000 / f.QuadPart);
}

/* waits (lock held, exclusive) until fd has a datagram; false with an errno */
static bool wait_data(s32 fd, u32 flags, s32 *err) {
    Sock *s = get(fd);
    DWORD start = tick_ms();
    for (;;) {
        if (!s || s->closed) { *err = E_BADF; return false; }
        if (s->head) return true;
        if (s->nonblock || flags & MSG_DONTWAIT_) { *err = E_AGAIN; return false; }
        DWORD wait = 100;
        if (s->rcvtimeo_ms) {
            DWORD el = tick_ms() - start;
            if (el >= s->rcvtimeo_ms) { *err = E_AGAIN; return false; }
            if (s->rcvtimeo_ms - el < wait) wait = s->rcvtimeo_ms - el;
        }
        SleepConditionVariableSRW(&arrived, &lock, wait, 0);
        s = get(fd);
    }
}

/* ---------------------------------------------------------------------------------------------- commands */
static void bsd_register_client(SvcObj *o, IpcReq *r) { out_u32(r, 0); }

static void bsd_socket(SvcObj *o, IpcReq *r) {
    u32 domain = in_u32(r, 0), type = in_u32(r, 4) & ~0x20000000u, proto = in_u32(r, 8);
    if (domain != 2 || (type != SOCK_STREAM_ && type != SOCK_DGRAM_)) { reply(r, -1, E_INVAL); return; }
    AcquireSRWLockExclusive(&lock);
    int i = 0;
    while (i < MAX_SOCKS && socks[i].used) i++;
    if (i == MAX_SOCKS) { ReleaseSRWLockExclusive(&lock); reply(r, -1, E_MFILE); return; }
    memset(&socks[i], 0, sizeof socks[i]);
    socks[i].used = true;
    socks[i].stream = type == SOCK_STREAM_;
    socks[i].proto = proto;
    socks[i].sndbuf = socks[i].rcvbuf = 0x10000;
    ReleaseSRWLockExclusive(&lock);
    hlog(1, "bsd: socket %d (%s, protocol %u)", i + FD_BASE, type == SOCK_STREAM_ ? "tcp" : "udp", proto);
    reply(r, i + FD_BASE, 0);
}

static void bsd_bind(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    u8 ip[4];
    u16 port;
    read_addr(buf_in(r, 0), ip, &port);
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    s32 err = 0;
    if (!s) err = E_BADF;
    else if (s->bound) err = E_INVAL;
    else {
        s->bound = true;
        if (!port) { s->bound = false; auto_bind(s); }
        else s->port = port;
        memcpy(s->ip, ip, 4);
        hlog(1, "bsd: socket %d bound to %u.%u.%u.%u:%u", fd, ip[0], ip[1], ip[2], ip[3], s->port);
    }
    ReleaseSRWLockExclusive(&lock);
    reply_errno(r, err);
}

static void bsd_connect(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    s32 err = 0;
    if (!s) err = E_BADF;
    else if (s->stream) err = E_CONNREFUSED;
    else {
        read_addr(buf_in(r, 0), s->peer_ip, &s->peer_port);
        s->connected = true;
        auto_bind(s);
    }
    ReleaseSRWLockExclusive(&lock);
    reply_errno(r, err);
}

static void bsd_sockname(SvcObj *o, IpcReq *r, bool peer) {
    s32 fd = (s32)in_u32(r, 0);
    AcquireSRWLockShared(&lock);
    Sock *s = get(fd);
    s32 err = 0;
    u32 len = 0;
    if (!s) err = E_BADF;
    else if (peer && !s->connected) err = E_NOTCONN;
    else if (peer) len = write_addr(r, 0, s->peer_ip, s->peer_port);
    else {
        u8 ip[4];
        memcpy(ip, s->ip, 4);
        len = write_addr(r, 0, ip, s->bound ? s->port : 0);
        if (trace(r)) hlog(1, "bsd: getsockname(%d) -> %u.%u.%u.%u:%u", fd, ip[0], ip[1], ip[2], ip[3], s->bound ? s->port : 0);
    }
    ReleaseSRWLockShared(&lock);
    reply_errno(r, err);
    out_u32(r, len);
}
static void bsd_getsockname(SvcObj *o, IpcReq *r) { bsd_sockname(o, r, false); }
static void bsd_getpeername(SvcObj *o, IpcReq *r) { bsd_sockname(o, r, true); }

static u32 opt_u32(IpcReq *r) {
    const IpcBuf *b = buf_in(r, 0);
    u32 v = 0;
    if (b && b->size >= 4) memcpy(&v, (const void *)(uintptr_t)b->addr, 4);
    return v;
}

static void bsd_setsockopt(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    u32 level = in_u32(r, 4), name = in_u32(r, 8);
    if (trace(r)) { const IpcBuf *ob = buf_in(r, 0); hlog(1, "bsd: setsockopt(%d, %#x, %#x, %u bytes, value %u)", fd, level, name, ob ? (u32)ob->size : 0, opt_u32(r)); }
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    s32 err = 0;
    if (!s) err = E_BADF;
    else if (level == SOL_SOCKET_) {
        const IpcBuf *b = buf_in(r, 0);
        switch (name) {
        case SO_BROADCAST_:
            s->broadcast = opt_u32(r) != 0;
            hlog(1, "bsd: socket %d broadcast %s", fd, s->broadcast ? "on" : "off");
            break;
        case SO_SNDBUF_: s->sndbuf = opt_u32(r); break;
        case SO_RCVBUF_: s->rcvbuf = opt_u32(r); break;
        case SO_RCVTIMEO_:
            if (b && b->size >= 16) {           /* struct timeval { s64 sec, usec } */
                s64 tv[2];
                memcpy(tv, (const void *)(uintptr_t)b->addr, 16);
                s->rcvtimeo_ms = (u32)(tv[0] * 1000 + tv[1] / 1000);
            } else s->rcvtimeo_ms = opt_u32(r);
            break;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    reply_errno(r, err);
}

static void bsd_getsockopt(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    u32 level = in_u32(r, 4), name = in_u32(r, 8);
    AcquireSRWLockShared(&lock);
    Sock *s = get(fd);
    s32 err = 0;
    u32 v = 0;
    if (!s) err = E_BADF;
    else if (level == SOL_SOCKET_)
        switch (name) {
        case SO_BROADCAST_: v = s->broadcast; break;
        case SO_SNDBUF_: v = s->sndbuf; break;
        case SO_RCVBUF_: v = s->rcvbuf; break;
        case SO_TYPE_: v = s->stream ? SOCK_STREAM_ : SOCK_DGRAM_; break;
        case SO_ERROR_: v = 0; break;
        }
    ReleaseSRWLockShared(&lock);
    const IpcBuf *b = buf_out(r, 0);
    u32 len = 0;
    if (b && b->size >= 4) { memcpy((void *)(uintptr_t)b->addr, &v, 4); len = 4; }
    reply_errno(r, err);
    out_u32(r, len);
    if (trace(r)) hlog(1, "bsd: getsockopt(%d, %#x, %#x) -> %u", fd, level, name, v);
}

static void bsd_fcntl(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0), cmd = (s32)in_u32(r, 4), arg = (s32)in_u32(r, 8);
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    s32 ret = 0, err = 0;
    if (!s) ret = -1, err = E_BADF;
    else if (cmd == F_GETFL_) ret = s->flags;
    else if (cmd == F_SETFL_) { s->flags = arg; s->nonblock = (arg & O_NONBLOCK_) != 0; }
    else ret = -1, err = E_INVAL;
    ReleaseSRWLockExclusive(&lock);
    if (trace(r)) hlog(1, "bsd: fcntl(%d, %d, %#x) -> %d errno %d", fd, cmd, arg, ret, err);
    reply(r, ret, err);
}

/* recv / recvfrom: one datagram into buffer 0 (truncated if it does not fit), its source into buffer 1 */
static void do_recv(IpcReq *r, s32 fd, u32 flags, bool from) {
    s32 err = 0, ret = -1;
    u32 alen = 0;
    const IpcBuf *b = buf_out(r, 0);
    AcquireSRWLockExclusive(&lock);
    if (wait_data(fd, flags, &err)) {
        Sock *s = get(fd);
        Dgram *d = s->head;
        u32 n = b && d->size > b->size ? (u32)b->size : d->size;
        if (b && n) memcpy((void *)(uintptr_t)b->addr, d->data, n);
        ret = (s32)n;
        if (from) alen = write_addr(r, 1, d->ip, d->port);
        if (!(flags & MSG_PEEK_)) {
            s->head = d->next;
            if (!s->head) s->tail = NULL;
            s->queued--;
            free(d);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    if (trace(r) || (ret > 0 && recv_n <= 5))
        hlog(1, "bsd: recv%s(%d, flags %#x, %u-byte buffer) -> %d errno %d", from ? "from" : "", fd, flags, b ? (u32)b->size : 0, ret, err);
    reply(r, ret, err);
    if (from) out_u32(r, alen);
}
static void bsd_recv(SvcObj *o, IpcReq *r) { do_recv(r, (s32)in_u32(r, 0), in_u32(r, 4), false); }
static void bsd_recvfrom(SvcObj *o, IpcReq *r) { do_recv(r, (s32)in_u32(r, 0), in_u32(r, 4), true); }

/* ---------------------------------------------------------------------------------------------- eventfd */
/* EventFd(u64 initval, u32 flags): flags as Linux (EFD_SEMAPHORE 1, EFD_NONBLOCK 0x800). nn::socket::EventFdRead and
 * EventFdWrite are Read and Write of 8 bytes on it */
enum { EFD_SEMAPHORE_ = 1 };

static void bsd_eventfd(SvcObj *o, IpcReq *r) {
    u64 init;
    memcpy(&init, r->in, 8);
    u32 flags = in_u32(r, 8);
    AcquireSRWLockExclusive(&lock);
    int i = 0;
    while (i < MAX_SOCKS && socks[i].used) i++;
    if (i == MAX_SOCKS) { ReleaseSRWLockExclusive(&lock); reply(r, -1, E_MFILE); return; }
    memset(&socks[i], 0, sizeof socks[i]);
    socks[i].used = socks[i].efd = true;
    socks[i].efd_semaphore = (flags & EFD_SEMAPHORE_) != 0;
    socks[i].nonblock = (flags & O_NONBLOCK_) != 0;
    socks[i].efd_count = init;
    ReleaseSRWLockExclusive(&lock);
    hlog(1, "bsd: eventfd %d (initial %llu, flags %#x)", i + FD_BASE, init, flags);
    reply(r, i + FD_BASE, 0);
}

/* lock held, exclusive */
static void efd_read(IpcReq *r, s32 fd) {
    const IpcBuf *b = buf_out(r, 0);
    Sock *s = get(fd);
    if (!b || b->size < 8) { reply(r, -1, E_INVAL); return; }
    while (s && !s->closed && !s->efd_count) {
        if (s->nonblock) { reply(r, -1, E_AGAIN); return; }
        SleepConditionVariableSRW(&arrived, &lock, 100, 0);
        s = get(fd);
    }
    if (!s || s->closed) { reply(r, -1, E_BADF); return; }
    u64 v = s->efd_semaphore ? 1 : s->efd_count;
    s->efd_count -= v;
    memcpy((void *)(uintptr_t)b->addr, &v, 8);
    reply(r, 8, 0);
}

static void efd_write(IpcReq *r, s32 fd, const IpcBuf *msg) {
    Sock *s = get(fd);
    u64 v;
    if (!msg || msg->size < 8) { reply(r, -1, E_INVAL); return; }
    memcpy(&v, (const void *)(uintptr_t)msg->addr, 8);
    s->efd_count += v;
    WakeAllConditionVariable(&arrived);
    reply(r, 8, 0);
}

static void bsd_read(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    if (s && s->efd) { efd_read(r, fd); ReleaseSRWLockExclusive(&lock); return; }
    ReleaseSRWLockExclusive(&lock);
    do_recv(r, fd, 0, false);
}

static void do_send(IpcReq *r, s32 fd, const IpcBuf *msg, const IpcBuf *to) {
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    s32 ret = -1, err = 0;
    u8 ip[4];
    u16 port;
    if (!s) err = E_BADF;
    else if (s->stream) err = E_NOTCONN;
    else if (msg && msg->size > 0xffff) err = E_MSGSIZE;
    else {
        if (to && to->size >= 8) read_addr(to, ip, &port);
        else if (s->connected) { memcpy(ip, s->peer_ip, 4); port = s->peer_port; }
        else { err = E_NOTCONN; goto done; }
        auto_bind(s);
        u32 n = msg ? (u32)msg->size : 0;
        send_dgram(s, ip, port, msg ? (const void *)(uintptr_t)msg->addr : NULL, n);
        ret = (s32)n;
    }
done:
    ReleaseSRWLockExclusive(&lock);
    if (trace(r)) {
        u8 dip[4] = {0};
        u16 dport = 0;
        if (to) read_addr(to, dip, &dport);
        hlog(1, "bsd: send(%d, %u bytes, to %u.%u.%u.%u:%u) -> %d errno %d", fd, msg ? (u32)msg->size : 0, dip[0], dip[1], dip[2],
             dip[3], dport, ret, err);
    }
    reply(r, ret, err);
}
static void bsd_send(SvcObj *o, IpcReq *r) { do_send(r, (s32)in_u32(r, 0), buf_in(r, 0), NULL); }
static void bsd_sendto(SvcObj *o, IpcReq *r) { do_send(r, (s32)in_u32(r, 0), buf_in(r, 0), buf_in(r, 1)); }
static void bsd_write(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    if (s && s->efd) { efd_write(r, fd, buf_in(r, 0)); ReleaseSRWLockExclusive(&lock); return; }
    ReleaseSRWLockExclusive(&lock);
    do_send(r, fd, buf_in(r, 0), NULL);
}

/* poll: struct pollfd { s32 fd; s16 events, revents; } in buffer 0, results to output buffer 0 */
static void bsd_poll(SvcObj *o, IpcReq *r) {
    s32 nfds = (s32)in_u32(r, 0), timeout = (s32)in_u32(r, 4);
    const IpcBuf *in = buf_in(r, 0), *out = buf_out(r, 0);
    if (nfds < 0 || !in || in->size < (u64)nfds * 8) { reply(r, -1, E_INVAL); return; }
    u8 *fds = malloc(nfds ? nfds * 8 : 8);
    memcpy(fds, (const void *)(uintptr_t)in->addr, nfds * 8);
    DWORD start = tick_ms();
    s32 ready;
    AcquireSRWLockExclusive(&lock);
    for (;;) {
        ready = 0;
        for (s32 i = 0; i < nfds; i++) {
            s32 fd;
            u16 ev, rev = 0;
            memcpy(&fd, fds + i * 8, 4);
            memcpy(&ev, fds + i * 8 + 4, 2);
            Sock *s = get(fd);
            if (fd < 0) rev = 0;
            else if (!s) rev = POLLNVAL_;
            else if (s->efd) rev = (ev & POLLIN_ && s->efd_count ? POLLIN_ : 0) | (ev & POLLOUT_);
            else {
                if (ev & POLLIN_ && s->head) rev |= POLLIN_;
                if (ev & POLLOUT_ && !s->stream) rev |= POLLOUT_;
                if (s->closed) rev |= POLLERR_;
            }
            memcpy(fds + i * 8 + 6, &rev, 2);
            if (rev) ready++;
        }
        if (ready || timeout == 0) break;
        DWORD wait = 100;
        if (timeout > 0) {
            DWORD el = tick_ms() - start;
            if (el >= (DWORD)timeout) break;
            if ((DWORD)timeout - el < wait) wait = (DWORD)timeout - el;
        }
        SleepConditionVariableSRW(&arrived, &lock, wait, 0);
    }
    ReleaseSRWLockExclusive(&lock);
    if (trace(r) && nfds > 0) {
        s32 fd0;
        u16 ev0, rev0;
        memcpy(&fd0, fds, 4); memcpy(&ev0, fds + 4, 2); memcpy(&rev0, fds + 6, 2);
        hlog(1, "bsd: poll(%d fds, timeout %d) -> %d (first: fd %d events %#x revents %#x)", nfds, timeout, ready, fd0, ev0, rev0);
    }
    if (out && out->size) memcpy((void *)(uintptr_t)out->addr, fds, out->size < (u64)nfds * 8 ? out->size : (u64)nfds * 8);
    free(fds);
    reply(r, ready, 0);
}

static void bsd_select(SvcObj *o, IpcReq *r) { reply(r, 0, 0); }

static void bsd_listen(SvcObj *o, IpcReq *r) { reply_errno(r, get((s32)in_u32(r, 0)) ? E_OPNOTSUPP : E_BADF); }
static void bsd_accept(SvcObj *o, IpcReq *r) { reply(r, -1, get((s32)in_u32(r, 0)) ? E_OPNOTSUPP : E_BADF); out_u32(r, 0); }
static void bsd_shutdown(SvcObj *o, IpcReq *r) { reply_errno(r, get((s32)in_u32(r, 0)) ? 0 : E_BADF); }

static void bsd_close(SvcObj *o, IpcReq *r) {
    s32 fd = (s32)in_u32(r, 0);
    AcquireSRWLockExclusive(&lock);
    Sock *s = get(fd);
    if (s) { free_queue(s); s->closed = true; s->used = false; }
    hlog(1, "bsd: close(%d); %ld datagrams sent, %ld received", fd, sent_n, recv_n);
    ReleaseSRWLockExclusive(&lock);
    WakeAllConditionVariable(&arrived);
    reply_errno(r, s ? 0 : E_BADF);
}

static const Iface iface_bsd = {"bsd:u", (const IpcCmd[]){
    {0, bsd_register_client, "RegisterClient"},
    {1, ipc_noop, "StartMonitoring"},
    {2, bsd_socket, "Socket"},
    {3, bsd_socket, "SocketExempt"},
    {5, bsd_select, "Select"},
    {6, bsd_poll, "Poll"},
    {8, bsd_recv, "Recv"},
    {9, bsd_recvfrom, "RecvFrom"},
    {10, bsd_send, "Send"},
    {11, bsd_sendto, "SendTo"},
    {12, bsd_accept, "Accept"},
    {13, bsd_bind, "Bind"},
    {14, bsd_connect, "Connect"},
    {15, bsd_getpeername, "GetPeerName"},
    {16, bsd_getsockname, "GetSockName"},
    {17, bsd_getsockopt, "GetSockOpt"},
    {18, bsd_listen, "Listen"},
    {20, bsd_fcntl, "Fcntl"},
    {21, bsd_setsockopt, "SetSockOpt"},
    {22, bsd_shutdown, "Shutdown"},
    {24, bsd_write, "Write"},
    {25, bsd_read, "Read"},
    {26, bsd_close, "Close"},
    {31, bsd_eventfd, "EventFd"},
    IPC_END}, NULL};

static SvcObj *open_bsd(const char *name) { return svcobj_new(&iface_bsd, NULL, name); }

void bsd_register(void) {
    room_on_proxy(on_proxy);
    service_register("bsd:u", open_bsd);
    service_register("bsd:s", open_bsd);
}
