/* The software keyboard's stand-in (am.c, library applet 0x11): a small Win32 input box with the game's prompt.
 * Blocking: am.c runs it on a thread of its own, so the game keeps running while it is open. Enter is OK, Esc
 * cancels; the number pad (keyboard type 1, e.g. link codes) takes digits only. */
#include "swkbd.h"
#include <windows.h>

enum { ID_EDIT = 100, ID_LABEL };

typedef struct Box { HWND edit; bool ok; wchar_t *out; u32 out_len; } Box;

static LRESULT CALLBACK box_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    Box *b = (Box *)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            b->ok = LOWORD(wp) == IDOK;
            if (b->ok) GetWindowTextW(b->edit, b->out, (int)b->out_len);
            DestroyWindow(w);
        }
        return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

bool swkbd_prompt(const wchar_t *title, const wchar_t *prompt, u32 max_len, bool numeric, wchar_t *out, u32 out_len) {
    static volatile LONG registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!InterlockedExchange(&registered, 1)) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = box_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"nx_swkbd";
        RegisterClassW(&wc);
    }
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    UINT dpi = GetDpiForSystem();
#define S(v) MulDiv((v), (int)dpi, 96)
    int cw = S(420), ch = S(150);
    RECT rc = {0, 0, cw, ch};
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP, ex = WS_EX_TOPMOST | WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    Box b = {0, false, out, out_len};
    HWND w = CreateWindowExW(ex, L"nx_swkbd", title && *title ? title : L"Keyboard", style,
                             (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                             NULL, NULL, inst, NULL);
    if (!w) { DeleteObject(font); return false; }
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)&b);
    HWND label = CreateWindowExW(0, L"STATIC", prompt ? prompt : L"", WS_CHILD | WS_VISIBLE, S(12), S(12), cw - S(24),
                                 S(44), w, (HMENU)ID_LABEL, inst, NULL);
    b.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                             (numeric ? ES_NUMBER : 0), S(12), S(62), cw - S(24), S(26), w, (HMENU)ID_EDIT, inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cw - S(196), ch - S(44), S(88), S(30), w, (HMENU)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, cw - S(100), ch - S(44),
                                  S(88), S(30), w, (HMENU)IDCANCEL, inst, NULL);
#undef S
    HWND ctl[] = {label, b.edit, ok, cancel};
    for (int i = 0; i < 4; i++) SendMessageW(ctl[i], WM_SETFONT, (WPARAM)font, TRUE);
    if (max_len) SendMessageW(b.edit, EM_SETLIMITTEXT, max_len < out_len ? max_len : out_len - 1, 0);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(b.edit);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DeleteObject(font);
    return b.ok;
}
