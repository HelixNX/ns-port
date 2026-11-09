/* Rooms during play (the menu's Multiplayer, ui.c / nxui): join, switch and leave rooms without restarting, and
 * the public room list. room_join blocks for up to ~10 s, so joins and leaves run on a worker thread of their own
 * and the menu polls the state. Leaving (or switching) ends a local session in progress as a lost connection would
 * (room.c room_leave -> ldn_net.c); between sessions the game does not notice: the services read the room's
 * address afresh each time (nifm, ldn, bsd). The nickname is the browser's (netplay.ini). */
#include "net.h"
#include "uiroom.h"
#include "game_config.h"
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void netplay_nickname(char *out, size_t n);
bool ldn_net_in_session(void);

static SRWLOCK lk = SRWLOCK_INIT;
static volatile LONG busy;                  /* a join or leave is running */
static int state = UI_ROOM_NONE;            /* the last result: joined, failed, none */
static char message[512], target[300];

static void set(int st, const char *fmt, ...) {
    AcquireSRWLockExclusive(&lk);
    state = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    ReleaseSRWLockExclusive(&lk);
}

typedef struct Req { char host[256], pw[128]; u16 port; bool leave; } Req;

static unsigned __stdcall worker(void *arg) {
    Req *r = arg;
    if (r->leave) {
        room_leave();
        set(UI_ROOM_NONE, "");
    } else {
        char nick[64], err[512];
        netplay_nickname(nick, sizeof nick);
        if (room_join(r->host, r->port, nick, r->pw, err, sizeof err)) set(UI_ROOM_JOINED, "%s", room_name());
        else set(UI_ROOM_FAILED, "%s", err);
    }
    free(r);
    InterlockedExchange(&busy, 0);
    return 0;
}

static void start(Req *r) {
    if (InterlockedCompareExchange(&busy, 1, 0)) { free(r); return; }      /* one at a time */
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, worker, r, 0, NULL);
    if (h) CloseHandle(h);
    else { free(r); InterlockedExchange(&busy, 0); }
}

void netmenu_join(const char *host, uint16_t port, const char *password) {
    Req *r = calloc(1, sizeof *r);
    snprintf(r->host, sizeof r->host, "%s", host);
    snprintf(r->pw, sizeof r->pw, "%s", password ? password : "");
    r->port = port ? port : NET_DEFAULT_PORT;
    AcquireSRWLockExclusive(&lk);
    snprintf(target, sizeof target, "%s:%u", r->host, r->port);
    ReleaseSRWLockExclusive(&lk);
    hlog(0, "netplay: joining %s:%u from the menu", r->host, r->port);
    start(r);
}

void netmenu_leave(void) {
    Req *r = calloc(1, sizeof *r);
    r->leave = true;
    start(r);
}

bool netmenu_in_session(void) { return room_joined() && ldn_net_in_session(); }

int netmenu_state(void) {
    if (busy) return UI_ROOM_JOINING;
    if (room_joined()) return UI_ROOM_JOINED;
    AcquireSRWLockShared(&lk);
    int s = state;
    ReleaseSRWLockShared(&lk);
    if (s == UI_ROOM_JOINED) {                  /* joined before (or at start-up) and not any more */
        set(UI_ROOM_FAILED, "Lost the connection to the room.");
        return UI_ROOM_FAILED;
    }
    return s;
}

/* the room's name, the error, or what is being joined (render thread; a copy) */
const char *netmenu_message(void) {
    static char out[512];
    int s = netmenu_state();
    AcquireSRWLockShared(&lk);
    if (s == UI_ROOM_JOINED) snprintf(out, sizeof out, "%s", room_name());
    else if (s == UI_ROOM_JOINING) snprintf(out, sizeof out, "%s", target);
    else snprintf(out, sizeof out, "%s", message);
    ReleaseSRWLockShared(&lk);
    return out;
}

/* ---------------------------------------------------------------------------------------------- the public list */
static UiRoom *lrooms;
static int nlrooms;
static volatile LONG lloading;
static char lerr[256];

static unsigned __stdcall lobby_worker(void *arg) {
    (void)arg;
    LobbyRoom *rs = NULL;
    char err[256] = "";
    int n = lobby_fetch(&rs, err, sizeof err);
    UiRoom *out = n > 0 ? calloc((size_t)n, sizeof *out) : NULL;
    int k = 0;
    for (int i = 0; i < n; i++) {
        const LobbyRoom *r = &rs[i];
        if (r->net_version && r->net_version != NET_VERSION) continue;      /* we could not join it */
        UiRoom *u = &out[k++];
        snprintf(u->name, sizeof u->name, "%s", r->name);
        snprintf(u->owner, sizeof u->owner, "%s", r->owner);
        snprintf(u->address, sizeof u->address, "%s", r->address);
        snprintf(u->game, sizeof u->game, "%s", r->game_name);
        u->port = r->port;
        u->players = r->nmembers;
        u->max_players = (int)r->max_players;
        u->password = r->has_password;
        u->this_game = r->game_id == NX_TITLE_ID;
        for (int m = 0; m < r->nmembers && !u->this_game; m++) u->this_game = r->members[m].game_id == NX_TITLE_ID;
    }
    if (n > 0) lobby_free(rs, n);
    AcquireSRWLockExclusive(&lk);
    free(lrooms);
    lrooms = out;
    nlrooms = k;
    snprintf(lerr, sizeof lerr, "%s", n < 0 ? (*err ? err : "The room list could not be fetched.") : "");
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&lloading, 0);
    return 0;
}

void netmenu_lobby_refresh(void) {
    if (InterlockedCompareExchange(&lloading, 1, 0)) return;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, lobby_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&lloading, 0);
}

/* copies up to max rooms; *loading while a fetch runs; err: why the last fetch failed ("" if not) */
int netmenu_lobby(UiRoom *out, int max, bool *loading, char *err, size_t errn) {
    AcquireSRWLockShared(&lk);
    int n = nlrooms < max ? nlrooms : max;
    if (n > 0) memcpy(out, lrooms, (size_t)n * sizeof *out);
    snprintf(err, errn, "%s", lerr);
    ReleaseSRWLockShared(&lk);
    *loading = lloading != 0;
    return n;
}
