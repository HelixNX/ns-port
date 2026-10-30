/* The public room list: GET https://<NET_LOBBY_HOST>/lobby (WinHTTP) answers {"rooms": [room, ...]} with
 *   room:   address, port, name, description, owner, preferredGameName, preferredGameId, maxPlayers, netVersion,
 *           hasPassword, externalGuid, id, players: [member, ...]
 *   member: nickname, username, avatarUrl, gameName, gameId
 * Title ids are integers above 2^53, so numbers are parsed as integers, never as doubles. */
#include "net.h"
#include <stdlib.h>
#include <windows.h>
#include <winhttp.h>

/* ---------------------------------------------------------------------------------------------- JSON */
typedef struct Js { const char *p, *end; bool ok; } Js;

static void ws(Js *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static bool eat(Js *j, char c) { ws(j); if (j->p < j->end && *j->p == c) { j->p++; return true; } return false; }

static void put_utf8(char *out, size_t n, size_t *len, u32 cp) {
    char b[4];
    int k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); k = 4; }
    if (*len + k < n) { memcpy(out + *len, b, k); *len += k; }
}

static u32 hex4(Js *j) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        char c = j->p < j->end ? *j->p++ : 0;
        v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
    }
    return v;
}

/* a string into out (truncated; out may be NULL to skip it) */
static void jstr(Js *j, char *out, size_t n) {
    size_t len = 0;
    if (!eat(j, '"')) { j->ok = false; return; }
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { if (out && len + 1 < n) out[len++] = c; continue; }
        if (j->p >= j->end) break;
        c = *j->p++;
        u32 cp = c;
        switch (c) {
        case 'n': cp = '\n'; break;
        case 't': cp = '\t'; break;
        case 'r': cp = '\r'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'u':
            cp = hex4(j);
            if (cp >= 0xd800 && cp < 0xdc00 && j->p + 6 <= j->end && j->p[0] == '\\' && j->p[1] == 'u') {
                j->p += 2;
                u32 lo = hex4(j);
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            }
            break;
        }
        if (out) put_utf8(out, n, &len, cp);
    }
    if (j->p < j->end) j->p++; else j->ok = false;
    if (out && n) out[len] = 0;
}

static void jskip(Js *j);

static u64 jnum(Js *j) {
    ws(j);
    u64 v = 0;
    bool neg = j->p < j->end && *j->p == '-';
    if (neg) j->p++;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') v = v * 10 + (u64)(*j->p++ - '0');
    while (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E' || *j->p == '+' || *j->p == '-' ||
                             (*j->p >= '0' && *j->p <= '9'))) j->p++;
    return neg ? 0 : v;
}

static bool jbool(Js *j) {
    ws(j);
    if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) { j->p += 4; return true; }
    jskip(j);
    return false;
}

static void jskip(Js *j) {
    ws(j);
    if (j->p >= j->end) { j->ok = false; return; }
    char c = *j->p;
    if (c == '"') jstr(j, NULL, 0);
    else if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (eat(j, close)) return;
        do {
            if (c == '{') { jstr(j, NULL, 0); if (!eat(j, ':')) { j->ok = false; return; } }
            jskip(j);
        } while (j->ok && eat(j, ','));
        if (!eat(j, close)) j->ok = false;
    } else if (c == '-' || (c >= '0' && c <= '9')) jnum(j);
    else if (j->end - j->p >= 4 && (!memcmp(j->p, "true", 4) || !memcmp(j->p, "null", 4))) j->p += 4;
    else if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) j->p += 5;
    else j->ok = false;
}

/* iterate an object: while (jkey(j, key, &first)) { ...read the value... } */
static bool jkey(Js *j, char *key, size_t n, bool *first) {
    if (*first) { *first = false; if (!eat(j, '{')) { j->ok = false; return false; } if (eat(j, '}')) return false; }
    else if (!eat(j, ',')) { if (!eat(j, '}')) j->ok = false; return false; }
    jstr(j, key, n);
    if (!eat(j, ':')) j->ok = false;
    return j->ok;
}

static void parse_member(Js *j, LobbyMember *m) {
    char key[64];
    bool first = true;
    while (jkey(j, key, sizeof key, &first)) {
        if (!strcmp(key, "nickname")) jstr(j, m->nickname, sizeof m->nickname);
        else if (!strcmp(key, "username")) jstr(j, m->username, sizeof m->username);
        else if (!strcmp(key, "gameName")) jstr(j, m->game_name, sizeof m->game_name);
        else if (!strcmp(key, "gameId")) m->game_id = jnum(j);
        else jskip(j);
    }
}

static void parse_room(Js *j, LobbyRoom *r) {
    char key[64];
    bool first = true;
    while (jkey(j, key, sizeof key, &first)) {
        if (!strcmp(key, "name")) jstr(j, r->name, sizeof r->name);
        else if (!strcmp(key, "description")) jstr(j, r->description, sizeof r->description);
        else if (!strcmp(key, "owner")) jstr(j, r->owner, sizeof r->owner);
        else if (!strcmp(key, "address")) jstr(j, r->address, sizeof r->address);
        else if (!strcmp(key, "preferredGameName")) jstr(j, r->game_name, sizeof r->game_name);
        else if (!strcmp(key, "preferredGameId")) r->game_id = jnum(j);
        else if (!strcmp(key, "port")) r->port = (u16)jnum(j);
        else if (!strcmp(key, "maxPlayers")) r->max_players = (u32)jnum(j);
        else if (!strcmp(key, "netVersion")) r->net_version = (u32)jnum(j);
        else if (!strcmp(key, "hasPassword")) r->has_password = jbool(j);
        else if (!strcmp(key, "players")) {
            if (!eat(j, '[')) { jskip(j); continue; }
            int cap = 0;
            if (eat(j, ']')) continue;
            do {
                if (r->nmembers == cap) {
                    cap = cap ? cap * 2 : 8;
                    r->members = realloc(r->members, cap * sizeof *r->members);
                }
                LobbyMember *m = &r->members[r->nmembers++];
                memset(m, 0, sizeof *m);
                parse_member(j, m);
            } while (j->ok && eat(j, ','));
            if (!eat(j, ']')) j->ok = false;
        } else jskip(j);
    }
}

static int parse_lobby(const char *text, size_t n, LobbyRoom **out) {
    Js j = {text, text + n, true};
    char key[64];
    bool first = true;
    LobbyRoom *rooms = NULL;
    int count = 0, cap = 0;
    while (jkey(&j, key, sizeof key, &first)) {
        if (strcmp(key, "rooms") || !eat(&j, '[')) { jskip(&j); continue; }
        if (eat(&j, ']')) continue;
        do {
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                rooms = realloc(rooms, cap * sizeof *rooms);
            }
            LobbyRoom *r = &rooms[count++];
            memset(r, 0, sizeof *r);
            parse_room(&j, r);
        } while (j.ok && eat(&j, ','));
        if (!eat(&j, ']')) j.ok = false;
    }
    if (!j.ok) { lobby_free(rooms, count); return -1; }
    *out = rooms;
    return count;
}

void lobby_free(LobbyRoom *rooms, int n) {
    for (int i = 0; i < n; i++) free(rooms[i].members);
    free(rooms);
}

/* ---------------------------------------------------------------------------------------------- HTTPS */
int lobby_fetch(LobbyRoom **out, char *err, size_t errn) {
    *out = NULL;
    HINTERNET s = WinHttpOpen(L"nxrecomp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, NET_LOBBY_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", L"/lobby", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    char *body = NULL;
    size_t len = 0, cap = 0;
    int n = -1;
    DWORD status = 0, sl = sizeof status;
    if (q) WinHttpSetTimeouts(q, 5000, 5000, 10000, 10000);
    if (!q || !WinHttpSendRequest(q, L"Accept: application/json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(q, NULL)) {
        snprintf(err, errn, "Could not reach the room list (error %lu).", GetLastError());
        goto done;
    }
    WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sl, NULL);
    if (status != 200) { snprintf(err, errn, "The room list answered HTTP %lu.", status); goto done; }
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(q, &avail) || !avail) break;
        if (len + avail + 1 > cap) { cap = (len + avail + 1) * 2; body = realloc(body, cap); }
        if (!WinHttpReadData(q, body + len, avail, &got) || !got) break;
        len += got;
    }
    if (!body) { snprintf(err, errn, "The room list was empty."); goto done; }
    n = parse_lobby(body, len, out);
    if (n < 0) snprintf(err, errn, "Could not read the room list.");
done:
    free(body);
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return n;
}
