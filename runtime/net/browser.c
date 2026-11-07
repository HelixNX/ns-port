/* The room browser, shown before the game starts (Win32, like Eden's "Public Room Browser"): the public rooms with
 * their preferred game, name, players and host, filtered by default to rooms for this game by title id (the room's
 * preferred game, or a member playing it; never by name), plus hide-empty / hide-full and a text search. Join (or a
 * double click) asks for the password if the room has one and joins it; Direct Connect joins host[:port] (and asks
 * for the password when the room turns it down for one); Play Offline starts without a room. Closing the window
 * quits.
 * Settings (nickname, filters, last direct address) persist in <save dir>/netplay.ini. */
#include "net.h"
#include "game_config.h"
#include "../nca.h"
#include <stdlib.h>
#include <process.h>
#include <windows.h>
#include <commctrl.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

enum {
    IDC_NICK = 100, IDC_SEARCH, IDC_GAME_ONLY, IDC_HIDE_EMPTY, IDC_HIDE_FULL, IDC_REFRESH, IDC_LIST, IDC_MEMBERS,
    IDC_DESC, IDC_STATUS, IDC_DIRECT, IDC_DIRECT_JOIN, IDC_JOIN, IDC_OFFLINE, IDC_PW_EDIT, IDC_PW_OK, IDC_PW_CANCEL,
};
enum { WM_LOBBY_DONE = WM_APP + 1, WM_JOIN_DONE };
enum { COL_GAME, COL_NAME, COL_PLAYERS, COL_HOST };

static HWND wnd, h_nick, h_search, h_game_only, h_hide_empty, h_hide_full, h_refresh, h_list, h_members, h_desc,
    h_status, h_direct, h_direct_join, h_join, h_offline, h_lbl_nick, h_lbl_search, h_lbl_direct;
static HFONT font;
static LobbyRoom *rooms;
static int nrooms, *vis, nvis;
static int sort_col = COL_PLAYERS;
static bool sort_desc = true;
static bool busy;                           /* fetching or joining */
static enum { R_NONE, R_JOINED, R_OFFLINE } result;
static char ini_path[1100];
static char join_err[512];

/* ---------------------------------------------------------------------------------------------- helpers */
static void to_w(const char *s, wchar_t *out, int n) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n)) out[0] = 0;
    out[n - 1] = 0;
}
static void get_text(HWND h, char *out, int n) {
    wchar_t w[512];
    GetWindowTextW(h, w, 512);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL)) out[0] = 0;
}
static void set_text(HWND h, const char *s) {
    wchar_t w[1024];
    to_w(s, w, 1024);
    SetWindowTextW(h, w);
}
static bool checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

/* ListView in UTF-16 (the ListView_* macros pick the ANSI messages without UNICODE) */
static void lv_insert(HWND h, int row, wchar_t *text) {
    LVITEMW it = {LVIF_TEXT, row};
    it.pszText = text;
    SendMessageW(h, LVM_INSERTITEMW, 0, (LPARAM)&it);
}
static void lv_set(HWND h, int row, int col, wchar_t *text) {
    LVITEMW it = {0};
    it.iSubItem = col;
    it.pszText = text;
    SendMessageW(h, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}
static void lv_column(HWND h, int col, int width, const wchar_t *text) {
    LVCOLUMNW c = {LVCF_TEXT | LVCF_WIDTH, 0, width, (wchar_t *)text};
    SendMessageW(h, LVM_INSERTCOLUMNW, (WPARAM)col, (LPARAM)&c);
}

/* case-insensitive substring over UTF-8 (ASCII letters folded) */
static bool contains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && (*a | (*a >= 'A' && *a <= 'Z' ? 32 : 0)) == (*b | (*b >= 'A' && *b <= 'Z' ? 32 : 0))) a++, b++;
        if (!*b) return true;
    }
    return false;
}

/* nicknames rooms accept: 4-20 of letters, digits, space, '.', '_', '-' */
static bool valid_nick(const char *s) {
    size_t n = strlen(s);
    if (n < 4 || n > 20) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(" ._-", *s))) return false;
    return true;
}

static bool room_for_game(const LobbyRoom *r) {
    if (r->game_id == NX_TITLE_ID) return true;
    for (int i = 0; i < r->nmembers; i++) if (r->members[i].game_id == NX_TITLE_ID) return true;
    return false;
}

/* ---------------------------------------------------------------------------------------------- settings */
static void ini_get(const char *key, char *out, size_t n, const char *def) {
    snprintf(out, n, "%s", def);
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[600];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        snprintf(out, n, "%s", line + kl + 1);
        out[strcspn(out, "\r\n")] = 0;
    }
    fclose(f);
}

static void save_settings(void) {
    FILE *f = fopen(ini_path, "w");
    if (!f) return;
    char nick[64], direct[256];
    get_text(h_nick, nick, sizeof nick);
    get_text(h_direct, direct, sizeof direct);
    fprintf(f, "nickname=%s\ngame_only=%d\nhide_empty=%d\nhide_full=%d\ndirect=%s\n", nick, checked(h_game_only),
            checked(h_hide_empty), checked(h_hide_full), direct);
    fclose(f);
}

static void default_nick(char *out, size_t n) {
    char user[64] = "";
    DWORD len = sizeof user;
    GetUserNameA(user, &len);
    size_t k = 0;
    for (const char *s = user; *s && k < 20 && k + 1 < n; s++)
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr("._-", *s)) out[k++] = *s;
    out[k] = 0;
    if (k < 4) snprintf(out, n, "Player%04u", (unsigned)(GetTickCount() % 10000));
}

/* ---------------------------------------------------------------------------------------------- the list */
static int cmp_rooms(const void *pa, const void *pb) {
    const LobbyRoom *a = &rooms[*(const int *)pa], *b = &rooms[*(const int *)pb];
    int c = 0;
    switch (sort_col) {
    case COL_GAME: c = _stricmp(a->game_name, b->game_name); break;
    case COL_NAME: c = _stricmp(a->name, b->name); break;
    case COL_PLAYERS: c = a->nmembers != b->nmembers ? (a->nmembers < b->nmembers ? -1 : 1)
                                                     : (a->max_players < b->max_players ? -1 : a->max_players > b->max_players); break;
    case COL_HOST: c = _stricmp(a->owner, b->owner); break;
    }
    if (!c) c = _stricmp(a->name, b->name);
    return sort_desc ? -c : c;
}

static void show_details(void) {
    ListView_DeleteAllItems(h_members);
    int sel = ListView_GetNextItem(h_list, -1, LVNI_SELECTED);
    EnableWindow(h_join, sel >= 0 && !busy);
    if (sel < 0 || sel >= nvis) { set_text(h_desc, ""); return; }
    const LobbyRoom *r = &rooms[vis[sel]];
    char d[1600];
    snprintf(d, sizeof d, "%s\r\n%s:%u%s%s\r\n\r\n%s", r->name, r->address, r->port, r->has_password ? "  (password)" : "",
             r->net_version != NET_VERSION ? "  (different protocol version)" : "", r->description);
    set_text(h_desc, d);
    for (int i = 0; i < r->nmembers; i++) {
        wchar_t w[128];
        to_w(r->members[i].nickname, w, 128);
        lv_insert(h_members, i, w);
        to_w(r->members[i].game_name, w, 128);
        lv_set(h_members, i, 1, w);
    }
}

static void refill(void) {
    char search[256];
    get_text(h_search, search, sizeof search);
    bool game_only = checked(h_game_only), hide_empty = checked(h_hide_empty), hide_full = checked(h_hide_full);
    free(vis);
    vis = malloc((nrooms ? nrooms : 1) * sizeof *vis);
    nvis = 0;
    for (int i = 0; i < nrooms; i++) {
        const LobbyRoom *r = &rooms[i];
        if (game_only && !room_for_game(r)) continue;
        if (hide_empty && r->nmembers == 0) continue;
        if (hide_full && r->max_players && (u32)r->nmembers >= r->max_players) continue;
        if (*search && !contains(r->name, search) && !contains(r->game_name, search) && !contains(r->description, search) &&
            !contains(r->owner, search)) continue;
        vis[nvis++] = i;
    }
    qsort(vis, nvis, sizeof *vis, cmp_rooms);
    SendMessageW(h_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(h_list);
    for (int k = 0; k < nvis; k++) {
        const LobbyRoom *r = &rooms[vis[k]];
        wchar_t w[300];
        char s[300];
        const char *g = r->game_name;
        while (*g == ' ') g++;                          /* some rooms pad the name */
        to_w(g, w, 300);
        lv_insert(h_list, k, w);
        snprintf(s, sizeof s, "%s%s", r->has_password ? "\xF0\x9F\x94\x92 " : "", r->name);
        to_w(s, w, 300);
        lv_set(h_list, k, COL_NAME, w);
        swprintf(w, 300, L"%d / %u", r->nmembers, r->max_players);
        lv_set(h_list, k, COL_PLAYERS, w);
        to_w(r->owner, w, 300);
        lv_set(h_list, k, COL_HOST, w);
    }
    SendMessageW(h_list, WM_SETREDRAW, TRUE, 0);
    char st[128];
    snprintf(st, sizeof st, "%d of %d rooms shown", nvis, nrooms);
    if (!busy) set_text(h_status, st);
    show_details();
}

/* ---------------------------------------------------------------------------------------------- background work */
typedef struct LobbyResult { LobbyRoom *rooms; int n; char err[256]; } LobbyResult;

static unsigned __stdcall fetch_thread(void *arg) {
    LobbyResult *res = calloc(1, sizeof *res);
    res->n = lobby_fetch(&res->rooms, res->err, sizeof res->err);
    PostMessageW((HWND)arg, WM_LOBBY_DONE, 0, (LPARAM)res);
    return 0;
}

static void set_busy(bool b, const char *status) {
    busy = b;
    EnableWindow(h_refresh, !b);
    EnableWindow(h_direct_join, !b);
    EnableWindow(h_offline, !b);
    EnableWindow(h_join, !b && ListView_GetNextItem(h_list, -1, LVNI_SELECTED) >= 0);
    if (status) set_text(h_status, status);
}

static void refresh(void) {
    if (busy) return;
    set_busy(true, "Fetching the room list...");
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, fetch_thread, wnd, 0, NULL));
}

typedef struct JoinArgs { char host[256], nick[64], password[128]; u16 port; } JoinArgs;

static unsigned __stdcall join_thread(void *arg) {
    JoinArgs *a = arg;
    bool ok = room_join(a->host, a->port, a->nick, a->password, join_err, sizeof join_err);
    PostMessageW(wnd, WM_JOIN_DONE, ok, 0);
    free(a);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- password prompt */
static bool pw_done, pw_ok;
static HWND pw_edit;
static char *pw_out;
static size_t pw_n;

static LRESULT CALLBACK pw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && (LOWORD(w) == IDC_PW_OK || LOWORD(w) == IDOK)) {
        get_text(pw_edit, pw_out, (int)pw_n);
        pw_ok = pw_done = true;
        DestroyWindow(h);
        return 0;
    }
    if ((m == WM_COMMAND && (LOWORD(w) == IDC_PW_CANCEL || LOWORD(w) == IDCANCEL)) || m == WM_CLOSE) {
        pw_done = true;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool ask_password(const char *room, char *out, size_t n) {
    static bool registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc = {0, pw_proc, 0, 0, inst, NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_BTNFACE + 1), NULL, L"NxPassword"};
        RegisterClassW(&wc);
        registered = true;
    }
    RECT pr;
    GetWindowRect(wnd, &pr);
    int W = 380, H = 150;
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NxPassword", L"Room password", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (pr.left + pr.right - W) / 2, (pr.top + pr.bottom - H) / 2, W, H, wnd, NULL, inst, NULL);
    wchar_t label[300], wroom[256];
    to_w(room, wroom, 256);
    swprintf(label, 300, L"Password for \"%ls\":", wroom);
    HWND c[4];
    c[0] = CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 12, 12, W - 40, 20, d, NULL, inst, NULL);
    c[1] = pw_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                                     12, 36, W - 40, 24, d, (HMENU)IDC_PW_EDIT, inst, NULL);
    c[2] = CreateWindowW(L"BUTTON", L"Join", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - 210, 72, 85, 28, d, (HMENU)IDC_PW_OK, inst, NULL);
    c[3] = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - 115, 72, 85, 28, d, (HMENU)IDC_PW_CANCEL, inst, NULL);
    for (int i = 0; i < 4; i++) SendMessageW(c[i], WM_SETFONT, (WPARAM)font, TRUE);
    pw_done = pw_ok = false;
    pw_out = out;
    pw_n = n;
    EnableWindow(wnd, FALSE);
    ShowWindow(d, SW_SHOW);
    SetFocus(pw_edit);
    MSG msg;
    while (!pw_done && GetMessageW(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(d, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    EnableWindow(wnd, TRUE);
    SetForegroundWindow(wnd);
    return pw_ok;
}

/* ---------------------------------------------------------------------------------------------- joining */
static void start_join(const char *host, u16 port, const char *password) {
    char nick[64];
    get_text(h_nick, nick, sizeof nick);
    if (!valid_nick(nick)) {
        MessageBoxW(wnd, L"The nickname must be 4 to 20 characters: letters, digits, spaces, '.', '_' or '-'.",
                    L"Nickname", MB_ICONWARNING);
        SetFocus(h_nick);
        return;
    }
    save_settings();
    JoinArgs *a = calloc(1, sizeof *a);
    snprintf(a->host, sizeof a->host, "%s", host);
    snprintf(a->nick, sizeof a->nick, "%s", nick);
    snprintf(a->password, sizeof a->password, "%s", password ? password : "");
    a->port = port;
    char st[400];
    snprintf(st, sizeof st, "Joining %s:%u...", host, port);
    set_busy(true, st);
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, join_thread, a, 0, NULL));
}

static char direct_host[256];          /* the last Direct Connect address ("": the last join was from the list) */
static u16 direct_port;

static void join_selected(void) {
    int sel = ListView_GetNextItem(h_list, -1, LVNI_SELECTED);
    if (busy || sel < 0 || sel >= nvis) return;
    direct_host[0] = 0;
    const LobbyRoom *r = &rooms[vis[sel]];
    char pw[128] = "";
    if (r->has_password && !ask_password(r->name, pw, sizeof pw)) return;
    start_join(r->address, r->port, pw);
}

/* host, host:port or [v6]:port */
static void join_direct(void) {
    char text[256], host[256];
    get_text(h_direct, text, sizeof text);
    char *s = text;
    while (*s == ' ') s++;
    u16 port = NET_DEFAULT_PORT;
    char *colon = strrchr(s, ':');
    snprintf(host, sizeof host, "%s", s);
    if (colon && strchr(s, ':') == colon) {
        host[colon - s] = 0;
        port = (u16)atoi(colon + 1);
    }
    host[strcspn(host, " ")] = 0;
    if (!*host || !port) { MessageBoxW(wnd, L"Enter an address as host or host:port.", L"Direct connect", MB_ICONWARNING); return; }
    char pw[128] = "";
    snprintf(direct_host, sizeof direct_host, "%s", host);  /* a room with a password: asked for (WM_JOIN_DONE) */
    direct_port = port;
    start_join(host, port, pw);
}

/* ---------------------------------------------------------------------------------------------- the window */
static void layout(int W, int H) {
    int m = 12, y = 12, rowh = 26;
    MoveWindow(h_lbl_nick, m, y + 4, 70, 20, TRUE);
    MoveWindow(h_nick, m + 72, y, 160, rowh, TRUE);
    MoveWindow(h_lbl_search, m + 250, y + 4, 50, 20, TRUE);
    MoveWindow(h_search, m + 302, y, 170, rowh, TRUE);
    MoveWindow(h_game_only, m + 486, y + 2, 175, 22, TRUE);
    MoveWindow(h_hide_empty, m + 666, y + 2, 115, 22, TRUE);
    MoveWindow(h_hide_full, m + 784, y + 2, 105, 22, TRUE);
    MoveWindow(h_refresh, W - m - 110, y - 1, 110, rowh + 2, TRUE);
    int list_top = y + rowh + 10, bottom_h = 190, foot = 44;
    int list_h = H - list_top - bottom_h - foot;
    if (list_h < 100) list_h = 100;
    MoveWindow(h_list, m, list_top, W - 2 * m, list_h, TRUE);
    int dy = list_top + list_h + 8, dh = bottom_h - 16;
    MoveWindow(h_members, m, dy, 330, dh, TRUE);
    MoveWindow(h_desc, m + 340, dy, W - 2 * m - 340, dh, TRUE);
    int fy = H - foot + 8;
    MoveWindow(h_status, m, fy + 5, W - 760, 20, TRUE);
    MoveWindow(h_lbl_direct, W - 735, fy + 5, 95, 20, TRUE);
    MoveWindow(h_direct, W - 638, fy, 200, rowh + 2, TRUE);
    MoveWindow(h_direct_join, W - 430, fy, 120, rowh + 2, TRUE);
    MoveWindow(h_join, W - 300, fy, 120, rowh + 2, TRUE);
    MoveWindow(h_offline, W - m - 160 + 40, fy, 120, rowh + 2, TRUE);
    /* the list's columns share its width */
    int lw = W - 2 * m - 24;
    ListView_SetColumnWidth(h_list, COL_GAME, lw * 30 / 100);
    ListView_SetColumnWidth(h_list, COL_NAME, lw * 45 / 100);
    ListView_SetColumnWidth(h_list, COL_PLAYERS, lw * 10 / 100);
    ListView_SetColumnWidth(h_list, COL_HOST, lw * 15 / 100);
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static void create_controls(void) {
    char s[256];
    h_lbl_nick = child(L"STATIC", L"Nickname", 0, 0, -1);
    ini_get("nickname", s, sizeof s, "");
    if (!*s) default_nick(s, sizeof s);
    h_nick = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_NICK);
    SendMessageW(h_nick, EM_LIMITTEXT, 20, 0);
    set_text(h_nick, s);
    h_lbl_search = child(L"STATIC", L"Search", 0, 0, -1);
    h_search = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_SEARCH);
    wchar_t only[128], wt[96];
    to_w(NX_TITLE, wt, 96);
    swprintf(only, 128, L"%ls only", wt);
    h_game_only = child(L"BUTTON", only, WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_GAME_ONLY);
    h_hide_empty = child(L"BUTTON", L"Hide Empty", WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_HIDE_EMPTY);
    h_hide_full = child(L"BUTTON", L"Hide Full", WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_HIDE_FULL);
    ini_get("game_only", s, sizeof s, "1");
    SendMessageW(h_game_only, BM_SETCHECK, atoi(s) ? BST_CHECKED : BST_UNCHECKED, 0);
    ini_get("hide_empty", s, sizeof s, "0");
    SendMessageW(h_hide_empty, BM_SETCHECK, atoi(s) ? BST_CHECKED : BST_UNCHECKED, 0);
    ini_get("hide_full", s, sizeof s, "0");
    SendMessageW(h_hide_full, BM_SETCHECK, atoi(s) ? BST_CHECKED : BST_UNCHECKED, 0);
    h_refresh = child(L"BUTTON", L"Refresh List", WS_TABSTOP, 0, IDC_REFRESH);

    h_list = child(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, IDC_LIST);
    ListView_SetExtendedListViewStyle(h_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    static const wchar_t *cols[] = {L"Preferred Game", L"Room Name", L"Players", L"Host"};
    for (int i = 0; i < 4; i++) lv_column(h_list, i, 100, cols[i]);
    h_members = child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOSORTHEADER, WS_EX_CLIENTEDGE, IDC_MEMBERS);
    ListView_SetExtendedListViewStyle(h_members, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    lv_column(h_members, 0, 130, L"Member");
    lv_column(h_members, 1, 175, L"Playing");
    h_desc = child(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, IDC_DESC);

    h_status = child(L"STATIC", L"", SS_ENDELLIPSIS, 0, IDC_STATUS);
    h_lbl_direct = child(L"STATIC", L"Direct connect", SS_RIGHT, 0, -1);
    h_direct = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_DIRECT);
    ini_get("direct", s, sizeof s, "");
    set_text(h_direct, s);
    h_direct_join = child(L"BUTTON", L"Connect", WS_TABSTOP, 0, IDC_DIRECT_JOIN);
    h_join = child(L"BUTTON", L"Join Room", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_JOIN);
    h_offline = child(L"BUTTON", L"Play Offline", WS_TABSTOP, 0, IDC_OFFLINE);
    EnableWindow(h_join, FALSE);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: layout(LOWORD(l), HIWORD(l)); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize = (POINT){1060, 520}; return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_REFRESH: refresh(); return 0;
        case IDC_GAME_ONLY: case IDC_HIDE_EMPTY: case IDC_HIDE_FULL: refill(); save_settings(); return 0;
        case IDC_SEARCH: if (HIWORD(w) == EN_CHANGE) refill(); return 0;
        case IDC_JOIN: join_selected(); return 0;
        case IDC_DIRECT_JOIN: join_direct(); return 0;
        case IDC_OFFLINE: save_settings(); result = R_OFFLINE; DestroyWindow(h); return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)l;
        if (n->idFrom != IDC_LIST) break;
        if (n->code == LVN_ITEMCHANGED) show_details();
        else if (n->code == NM_DBLCLK) join_selected();
        else if (n->code == LVN_COLUMNCLICK) {
            int col = ((NMLISTVIEW *)l)->iSubItem;
            if (col == sort_col) sort_desc = !sort_desc;
            else { sort_col = col; sort_desc = col == COL_PLAYERS; }
            refill();
        } else if (n->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                int k = (int)cd->nmcd.dwItemSpec;
                bool green = cd->iSubItem == COL_PLAYERS && k < nvis && rooms[vis[k]].nmembers > 0;
                cd->clrText = green ? RGB(0, 140, 40) : GetSysColor(COLOR_WINDOWTEXT);
                return CDRF_DODEFAULT;
            }
        }
        break;
    }
    case WM_LOBBY_DONE: {
        LobbyResult *res = (LobbyResult *)l;
        set_busy(false, NULL);
        if (res->n >= 0) {
            lobby_free(rooms, nrooms);
            rooms = res->rooms;
            nrooms = res->n;
            refill();
        } else set_text(h_status, res->err);
        free(res);
        return 0;
    }
    case WM_JOIN_DONE:
        set_busy(false, "");
        if (w) { result = R_JOINED; DestroyWindow(h); return 0; }
        if (direct_host[0] && strstr(join_err, "password")) {   /* Direct Connect to a room with a password */
            char pw[128] = "", title[300];
            snprintf(title, sizeof title, "%s:%u", direct_host, direct_port);
            if (ask_password(title, pw, sizeof pw)) start_join(direct_host, direct_port, pw);
            return 0;
        }
        {
            wchar_t e[512];
            to_w(join_err, e, 512);
            MessageBoxW(h, e, L"Could not join the room", MB_ICONERROR);
            refill();
        }
        return 0;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void run_browser(void) {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    font = CreateFontIndirectW(&ncm.lfMessageFont);
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0, wnd_proc, 0, 0, inst, LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION), LoadCursor(NULL, IDC_ARROW),
                    (HBRUSH)(COLOR_BTNFACE + 1), NULL, L"NxRoomBrowser"};
    RegisterClassW(&wc);
    wchar_t title[160], wt[96];
    to_w(NX_TITLE, wt, 96);
    swprintf(title, 160, L"%ls - Public Room Browser", wt);
    int W = 1240, H = 760;
    wnd = CreateWindowExW(0, L"NxRoomBrowser", title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          (GetSystemMetrics(SM_CXSCREEN) - W) / 2, (GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H,
                          NULL, NULL, inst, NULL);
    create_controls();
    RECT rc;
    GetClientRect(wnd, &rc);
    layout(rc.right, rc.bottom);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    refresh();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Enter in the direct-connect box connects; elsewhere it joins the selected room */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && !busy) {
            if (msg.hwnd == h_direct) { join_direct(); continue; }
            if (msg.hwnd == h_list) { join_selected(); continue; }
        }
        if (!IsDialogMessageW(wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    lobby_free(rooms, nrooms);
    rooms = NULL;
    nrooms = 0;
    free(vis);
    vis = NULL;
    DeleteObject(font);
}

/* ---------------------------------------------------------------------------------------------- start-up */
/* the display version from the NACP (offset 0x3060), e.g. "3.0.5" */
static void nacp_version(const char *game_dir, char *out, size_t n) {
    char path[1100];
    snprintf(path, sizeof path, "%s/control/control.nacp", game_dir);
    out[0] = 0;
    if (nx_from_nsp()) {                            /* from the user's NSPs (nca.c) */
        u64 sz;
        char *nacp = nx_game_control_file("control.nacp", &sz);
        if (nacp && sz >= 0x3070) snprintf(out, n, "%.15s", nacp + 0x3060);
        free(nacp);
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char v[0x10] = {0};
    if (!fseek(f, 0x3060, SEEK_SET) && fread(v, 1, sizeof v, f) == sizeof v) snprintf(out, n, "%.15s", v);
    fclose(f);
}

void netplay_startup(const char *save_dir, const char *game_dir) {
    snprintf(ini_path, sizeof ini_path, "%s\\netplay.ini", save_dir);
    CreateDirectoryA(save_dir, NULL);
    char version[32];
    nacp_version(game_dir, version, sizeof version);
    room_set_game(NX_TITLE, NX_TITLE_ID, version);
    const char *e = getenv("NX_NETPLAY");
    if (e && !strcmp(e, "0")) return;
    if ((e = getenv("NX_ROOM")) && *e) {
        char spec[512], err[512], nick[64];
        snprintf(spec, sizeof spec, "%s", e);
        char *port = strchr(spec, ':'), *pw = port ? strchr(port + 1, ':') : NULL;
        if (port) *port++ = 0;
        if (pw) *pw++ = 0;
        const char *n = getenv("NX_NICK");
        if (n && *n) snprintf(nick, sizeof nick, "%s", n);
        else { ini_get("nickname", nick, sizeof nick, ""); if (!*nick) default_nick(nick, sizeof nick); }
        if (!room_join(spec, port ? (u16)atoi(port) : NET_DEFAULT_PORT, nick, pw ? pw : "", err, sizeof err))
            fatal("NX_ROOM: %s", err);
        return;
    }
    run_browser();
    if (result == R_NONE) exit(0);
    if (result == R_OFFLINE) hlog(1, "netplay: playing offline");
}

/* the in-game menu (netmenu.c) shares the nickname with the browser: netplay.ini's nickname= line */
void netplay_nickname(char *out, size_t n) {
    ini_get("nickname", out, n, "");
    if (!*out) default_nick(out, n);
}

bool netplay_set_nickname(const char *nick) {
    if (!valid_nick(nick)) return false;
    char lines[32][600];
    int nl = 0;
    bool found = false;
    FILE *f = fopen(ini_path, "r");
    while (f && nl < 32 && fgets(lines[nl], sizeof lines[nl], f)) {
        if (!strncmp(lines[nl], "nickname=", 9)) { snprintf(lines[nl], sizeof lines[nl], "nickname=%s\n", nick); found = true; }
        nl++;
    }
    if (f) fclose(f);
    if (!found && nl < 32) snprintf(lines[nl++], sizeof lines[0], "nickname=%s\n", nick);
    if (!(f = fopen(ini_path, "w"))) return false;
    for (int i = 0; i < nl; i++) fputs(lines[i], f);
    fclose(f);
    return true;
}