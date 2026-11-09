/* nxui: the menu over the game (runtime/ui_host.h says why it is a DLL of its own).
 *
 * Esc frees the mouse (and never reaches the game); while the mouse is free the menu shows: a bar across the top
 * (it fades in on mouse movement and stays while used; F1 pins it) with Options (Video, Controls) and Mods (each
 * mod's windows, Manage), and whatever windows are open. A click on the game captures the mouse again and hides it
 * all. The game keeps the keyboard and gamepad meanwhile, except while the menu has keyboard focus (a window was
 * clicked, or a text box is active).
 *
 * Threads: nxui_event / nxui_tick on the window thread queue what ImGui needs; nxui_draw on the render thread feeds
 * it in and renders into the swapchain image (dynamic rendering; texture uploads under the runtime's queue lock).
 * One ImGui context for the runtime and every mod (NxModHost.imgui_context). */
#include "ui_host.h"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <atomic>
#include <mutex>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <windows.h>

#define EXPORT extern "C" __declspec(dllexport)

static const UiHost *H;
static char g_ini[MAX_PATH + 32];
static ImGuiContext *g_ctx;

/* ---------------------------------------------------------------------------------------------- shared state */
static std::atomic<bool> want_mouse{false}, want_kb{false}, want_text{false}, menu_open{false};
static std::atomic<bool> pinned{false}, req_release{false}, opt_open{false}, mods_open{false}, was_visible{false};
static std::atomic<bool> rooms_open{false}, modal_wanted{false};
static std::atomic<uint64_t> last_move{0};          /* SDL_GetTicks of the last mouse movement over the game */
enum { CAP_NONE, CAP_KEY, CAP_PAD, CAP_HOTKEY };
static std::atomic<int> cap_kind{CAP_NONE}, cap_index{-1};     /* rebinding: waiting for a key or a button */
static std::atomic<int> opt_tab{-1};                /* a tab to bring forward (0 video, 1 controls) */

static const uint64_t BAR_LINGER_MS = 2500;

static bool visible() { return !H->mouse_captured(); }
static bool bar_visible(uint64_t now) {
    return visible() && (pinned || now - last_move < BAR_LINGER_MS || menu_open || want_mouse);
}

/* ---------------------------------------------------------------------------------------------- events */
struct Ev {
    enum Kind { MOVE, BUTTON, WHEEL, KEY, TEXT, FOCUS } kind;
    float x, y;
    int button;
    bool down;
    ImGuiKey key;
    SDL_Keymod mods;
    char text[32];
};
static std::mutex g_lock;
static std::vector<Ev> g_events;

static void push(const Ev &e) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_events.size() < 4096) g_events.push_back(e);
}

static ImGuiKey key_of(SDL_Keycode k, SDL_Scancode sc) {
    if (k >= SDLK_A && k <= SDLK_Z) return (ImGuiKey)(ImGuiKey_A + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9) return (ImGuiKey)(ImGuiKey_0 + (k - SDLK_0));
    switch (sc) {
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

static void push_key(const SDL_Event *e) {
    Ev q = {};
    q.kind = Ev::KEY;
    q.down = e->type == SDL_EVENT_KEY_DOWN;
    q.key = key_of(e->key.key, e->key.scancode);
    q.mods = e->key.mod;
    push(q);
}

static float density() { return H->window ? SDL_GetWindowPixelDensity(H->window) : 1.0f; }

/* a key or button for the binding being set (window thread) */
static bool capture_event(const SDL_Event *e) {
    int kind = cap_kind, i = cap_index;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        SDL_Scancode sc = e->key.scancode;
        if (sc != SDL_SCANCODE_ESCAPE) {
            if (kind == CAP_KEY) H->set_key(i, (int)sc);
            else if (kind == CAP_HOTKEY) H->set_hotkey_key(i, (int)sc);
            else return true;                           /* a gamepad button is wanted: keys other than Esc wait */
        }
        cap_kind = CAP_NONE;
        return true;
    }
    if (e->type == SDL_EVENT_KEY_UP) return true;
    if (kind == CAP_PAD) {
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            H->set_pad(i, (int)e->gbutton.button);
            cap_kind = CAP_NONE;
            return true;
        }
        if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->gaxis.value > 16000 &&
            (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            H->set_pad(i, SDL_GAMEPAD_BUTTON_COUNT + (e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            cap_kind = CAP_NONE;
            return true;
        }
    }
    return false;
}

EXPORT bool nxui_event(const SDL_Event *e) {
    uint64_t now = SDL_GetTicks();
    if (cap_kind != CAP_NONE && capture_event(e)) return true;
    bool key = e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP;
    if (key && e->key.scancode == SDL_SCANCODE_ESCAPE) {    /* Esc frees the mouse; never the game's */
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && H->mouse_captured()) {
            H->capture_mouse(false);
            last_move = now;
        } else if (visible())
            push_key(e);                                    /* closes ImGui's popups */
        return true;
    }
    bool vis = visible();
    if (key) {
        if (vis && want_text) { push_key(e); return true; }  /* typing: no hotkeys, nothing for the game */
        if (e->type == SDL_EVENT_KEY_DOWN && H->hotkey(e)) return true;
        if (!vis) return false;
        push_key(e);
        return want_kb;
    }
    if (!vis) return false;
    float d = density();
    Ev q = {};
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        last_move = now;
        q.kind = Ev::MOVE, q.x = e->motion.x * d, q.y = e->motion.y * d;
        push(q);
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (down && !want_mouse && cap_kind == CAP_NONE) {
            pinned = false;                                 /* a click on the game: back to it (input.c captures) */
            return false;
        }
        q.kind = Ev::BUTTON, q.down = down;
        q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 :
                   e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        q.x = e->button.x * d, q.y = e->button.y * d;
        if (q.button >= 0) push(q);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!want_mouse) return false;
        q.kind = Ev::WHEEL, q.x = e->wheel.x, q.y = e->wheel.y;
        push(q);
        return true;
    case SDL_EVENT_TEXT_INPUT:
        q.kind = Ev::TEXT;
        snprintf(q.text, sizeof q.text, "%s", e->text.text ? e->text.text : "");
        push(q);
        return true;
    default:
        return false;
    }
}

EXPORT void nxui_tick(void) {
    if (req_release.exchange(false) && H->mouse_captured()) H->capture_mouse(false);
    bool vis = visible();
    static bool text_on;
    bool t = vis && want_text;
    if (t != text_on && H->window) {
        if (t) SDL_StartTextInput(H->window); else SDL_StopTextInput(H->window);
        text_on = t;
    }
    if (vis != was_visible.exchange(vis)) {
        Ev q = {};
        q.kind = Ev::FOCUS, q.down = vis;
        push(q);
        if (!vis) cap_kind = CAP_NONE;
    }
}

EXPORT bool nxui_wants_keyboard(void) { return visible() && (want_kb || want_text); }

EXPORT bool nxui_active(void) {
    if (!visible()) return false;
    return bar_visible(SDL_GetTicks()) || opt_open || mods_open || rooms_open || modal_wanted ||
           H->mods_any_window_open() || cap_kind != CAP_NONE;
}

/* the menu hotkey (window thread) */
EXPORT void nxui_toggle_menu(void) {
    if (H->mouse_captured()) { H->capture_mouse(false); pinned = true; }
    else pinned = !pinned;
    last_move = pinned ? SDL_GetTicks() : 0;
}

/* a mod's window opened (window thread from a hotkey, or the menu) */
EXPORT void nxui_show(void) {
    req_release = true;
    last_move = SDL_GetTicks();
}

/* ---------------------------------------------------------------------------------------------- the theme
 * A game's theme is a palette in its game.json ("theme": background, panel, accent, accent2, text as "#rrggbb",
 * rounding in pixels), baked into the exe (UiHost.theme); every ImGui colour is derived from it. The player picks
 * the game's theme (the default) or ImGui's Dark / Light / Classic, and a UI scale (auto: by the window's height),
 * in Options > Interface (settings.ini ui_theme, ui_scale). */
struct Palette { ImVec4 bg, panel, accent, accent2, text; float rounding; bool ok; };
static std::atomic<bool> style_dirty{true};

static bool hex_colour(const char *s, ImVec4 *c) {
    unsigned v;
    if (!s || *s != '#' || strlen(s) < 7 || sscanf(s + 1, "%6x", &v) != 1) return false;
    *c = ImVec4(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, 1.0f);
    return true;
}

static Palette game_palette() {
    Palette p = {};
    p.rounding = 4;
    int have = 0;
    const char *s = H->theme ? H->theme : "";
    while (*s) {
        const char *eq = strchr(s, '='), *end = strchr(s, ';');
        if (!end) end = s + strlen(s);
        if (eq && eq < end) {
            char k[32] = "", v[32] = "";
            snprintf(k, sizeof k, "%.*s", (int)(eq - s), s);
            snprintf(v, sizeof v, "%.*s", (int)(end - eq - 1), eq + 1);
            if (!strcmp(k, "background")) have += hex_colour(v, &p.bg);
            else if (!strcmp(k, "panel")) have += hex_colour(v, &p.panel);
            else if (!strcmp(k, "accent")) have += hex_colour(v, &p.accent);
            else if (!strcmp(k, "accent2")) have += hex_colour(v, &p.accent2);
            else if (!strcmp(k, "text")) have += hex_colour(v, &p.text);
            else if (!strcmp(k, "rounding")) p.rounding = (float)atof(v);
        }
        s = *end ? end + 1 : end;
    }
    p.ok = have == 5;
    return p;
}

static ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
static ImVec4 alpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

static void apply_palette(ImGuiStyle &st, const Palette &p) {
    ImGui::StyleColorsDark(&st);                    /* every colour has a value; the palette's replace them */
    ImVec4 *c = st.Colors;
    ImVec4 dim = mix(p.text, p.bg, 0.5f);
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = alpha(p.bg, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = alpha(p.panel, 0.98f);
    c[ImGuiCol_Border] = alpha(mix(p.panel, p.accent, 0.45f), 0.8f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.panel;
    c[ImGuiCol_FrameBgHovered] = mix(p.panel, p.accent, 0.30f);
    c[ImGuiCol_FrameBgActive] = mix(p.panel, p.accent, 0.50f);
    c[ImGuiCol_TitleBg] = mix(p.bg, p.panel, 0.5f);
    c[ImGuiCol_TitleBgActive] = mix(p.panel, p.accent, 0.55f);
    c[ImGuiCol_TitleBgCollapsed] = alpha(p.bg, 0.8f);
    c[ImGuiCol_MenuBarBg] = p.panel;
    c[ImGuiCol_ScrollbarBg] = alpha(p.bg, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = mix(p.panel, p.accent, 0.45f);
    c[ImGuiCol_ScrollbarGrabHovered] = mix(p.panel, p.accent, 0.70f);
    c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    c[ImGuiCol_CheckMark] = p.accent2;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = p.accent2;
    c[ImGuiCol_Button] = mix(p.panel, p.accent, 0.45f);
    c[ImGuiCol_ButtonHovered] = p.accent;
    c[ImGuiCol_ButtonActive] = mix(p.accent, p.accent2, 0.5f);
    c[ImGuiCol_Header] = mix(p.panel, p.accent, 0.40f);
    c[ImGuiCol_HeaderHovered] = mix(p.panel, p.accent, 0.75f);
    c[ImGuiCol_HeaderActive] = p.accent;
    c[ImGuiCol_Separator] = alpha(mix(p.panel, p.accent, 0.35f), 0.9f);
    c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accent2;
    c[ImGuiCol_ResizeGrip] = alpha(p.accent, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = alpha(p.accent, 0.65f);
    c[ImGuiCol_ResizeGripActive] = p.accent2;
    c[ImGuiCol_Tab] = mix(p.panel, p.accent, 0.25f);
    c[ImGuiCol_TabHovered] = p.accent;
    c[ImGuiCol_TabSelected] = mix(p.panel, p.accent, 0.65f);
    c[ImGuiCol_TabSelectedOverline] = p.accent2;
    c[ImGuiCol_TabDimmed] = p.panel;
    c[ImGuiCol_TabDimmedSelected] = mix(p.panel, p.accent, 0.40f);
    c[ImGuiCol_TableHeaderBg] = mix(p.panel, p.accent, 0.20f);
    c[ImGuiCol_TableBorderStrong] = mix(p.panel, p.accent, 0.35f);
    c[ImGuiCol_TableBorderLight] = mix(p.panel, p.accent, 0.20f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = alpha(p.panel, 0.45f);
    c[ImGuiCol_TextSelectedBg] = alpha(p.accent, 0.40f);
    c[ImGuiCol_PlotHistogram] = p.accent2;
    c[ImGuiCol_PlotLines] = p.accent;
    c[ImGuiCol_NavCursor] = p.accent2;
    c[ImGuiCol_DragDropTarget] = p.accent2;
    st.WindowRounding = p.rounding;
    st.ChildRounding = p.rounding;
    st.PopupRounding = p.rounding;
    st.FrameRounding = p.rounding * 0.6f;
    st.GrabRounding = p.rounding * 0.6f;
    st.ScrollbarRounding = p.rounding;
    st.TabRounding = p.rounding * 0.6f;
    st.WindowBorderSize = 1;
    st.FrameBorderSize = 0;
    st.WindowPadding = ImVec2(12, 10);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 6);
}

static const char *theme_choice() { return H->setting("ui_theme", "game"); }

/* render thread, before a frame: the style for the theme and scale chosen (from scratch: ScaleAllSizes adds up) */
static void build_style(uint32_t height) {
    static float last_scale;
    int pct = atoi(H->setting("ui_scale", "0"));
    float scale = pct > 0 ? pct / 100.0f : height >= 1400 ? 1.75f : height >= 1000 ? 1.35f : 1.0f;
    if (!style_dirty.exchange(false) && scale == last_scale) return;
    last_scale = scale;
    ImGuiStyle st;
    const char *t = theme_choice();
    Palette p = game_palette();
    if (!strcmp(t, "light")) ImGui::StyleColorsLight(&st);
    else if (!strcmp(t, "classic")) ImGui::StyleColorsClassic(&st);
    else if (!strcmp(t, "game") && p.ok) apply_palette(st, p);
    else ImGui::StyleColorsDark(&st);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
    ImGui::GetStyle() = st;
}

static void page_interface() {
    Palette p = game_palette();
    struct Choice { const char *id, *name; };
    char game_name[128];
    snprintf(game_name, sizeof game_name, "%s", H->title ? H->title : "Game");
    const Choice choices[] = {{"game", game_name}, {"dark", "Dark"}, {"light", "Light"}, {"classic", "Classic"}};
    const char *cur = theme_choice();
    ImGui::SetNextItemWidth(300);
    const char *shown = "Dark";
    for (const Choice &c : choices) if (!strcmp(c.id, cur)) shown = c.name;
    if (ImGui::BeginCombo("Theme", shown)) {
        for (const Choice &c : choices) {
            if (!strcmp(c.id, "game") && !p.ok) continue;       /* a game without a theme: ImGui's dark */
            if (ImGui::Selectable(c.name, !strcmp(c.id, cur))) { H->set_setting("ui_theme", c.id); style_dirty = true; }
        }
        ImGui::EndCombo();
    }
    int pct = atoi(H->setting("ui_scale", "0"));
    static const int scales[] = {0, 75, 100, 125, 150, 175, 200, 250};
    char cs[32];
    snprintf(cs, sizeof cs, pct ? "%d%%" : "Auto", pct);
    ImGui::SetNextItemWidth(300);
    if (ImGui::BeginCombo("Menu size", cs)) {
        for (int s : scales) {
            snprintf(cs, sizeof cs, s ? "%d%%" : "Auto", s);
            if (ImGui::Selectable(cs, s == pct)) {
                snprintf(cs, sizeof cs, "%d", s);
                H->set_setting("ui_scale", cs);
                style_dirty = true;
            }
        }
        ImGui::EndCombo();
    }
    if (p.ok) {                                         /* the game's palette, for reference */
        ImGui::TextDisabled("Colours:");
        ImVec4 sw[] = {p.bg, p.panel, p.accent, p.accent2, p.text};
        for (int i = 0; i < 5; i++) {
            ImGui::SameLine();
            ImGui::ColorButton(("##sw" + std::to_string(i)).c_str(), sw[i], ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
        }
    }
}

/* ---------------------------------------------------------------------------------------------- the pages */
static const char *key_name(int sc) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "(none)";
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    return n && *n ? n : "?";
}

static void help(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/* the "Set" / "Clear" pair of one binding; the row shows "press ..." while it waits */
static void bind_buttons(int kind, int i, const char *what, void (*clear)(int)) {
    ImGui::PushID(kind * 1000 + i);
    if (cap_kind == kind && cap_index == i) {
        ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "%s...", what);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) cap_kind = CAP_NONE;
    } else {
        if (ImGui::SmallButton("Set")) { cap_index = i; cap_kind = kind; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) clear(i);
    }
    ImGui::PopID();
}
static void clear_key(int i) { H->set_key(i, SDL_SCANCODE_UNKNOWN); }
static void clear_pad(int i) { H->set_pad(i, -1); }
static void clear_hotkey(int i) { H->set_hotkey_key(i, SDL_SCANCODE_UNKNOWN); }

static void page_video() {
    struct Rate { int32_t v; const char *name; bool dev; };
    static const Rate rates[] = {{-1, "30", false}, {60, "60", false}, {120, "120", true},
                                 {0, "Uncapped", true}};
    int32_t r = H->framerate();
    const char *cur = "?";
    for (const Rate &x : rates) if (x.v == r) cur = x.name;
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Frame rate", cur)) {
        for (const Rate &x : rates)
            if ((!x.dev || H->development) && ImGui::Selectable(x.name, x.v == r)) H->set_framerate(x.v);
        ImGui::EndCombo();
    }
    help("Above 30, the game runs on our timing hooks.");
    uint32_t s = H->render_scale();
    char cs[32];
    snprintf(cs, sizeof cs, "%ux  (%up)", s, 1080 * s);
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Render scale", cs)) {
        for (uint32_t k = 1; k <= 4; k++) {
            snprintf(cs, sizeof cs, "%ux  (%up)", k, 1080 * k);
            if (ImGui::Selectable(cs, k == s)) H->set_render_scale(k);
        }
        ImGui::EndCombo();
    }
    help("Draws above 1080p, then scales to the window.");
    bool fps = H->fps_counter();
    if (ImGui::Checkbox("FPS counter", &fps)) H->set_fps_counter(fps);
    bool full = H->fullscreen();
    if (ImGui::Checkbox("Fullscreen", &full)) H->set_fullscreen(full);
}

static void page_keyboard() {
    if (ImGui::Button("Reset")) H->reset_keys();
    if (!ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Switch");
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int a = 0; a < H->action_count; a++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(H->action_label(a));
        ImGui::TableNextColumn();
        int k = H->key(a);
        ImGui::TextUnformatted(key_name(k));
        if (k != SDL_SCANCODE_UNKNOWN) {                /* the same key twice: both act */
            for (int b = 0; b < H->action_count; b++)
                if (b != a && H->key(b) == k) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "also %s", H->action_label(b)); break; }
            for (int h = 0; h < H->hotkey_count(); h++)
                if (H->hotkey_key(h) == k) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "hotkey too"); break; }
        }
        ImGui::TableNextColumn();
        bind_buttons(CAP_KEY, a, "Press a key", clear_key);
    }
    ImGui::EndTable();
}

static void page_mouse() {
    float s = H->mouse_sens();
    ImGui::SetNextItemWidth(300);
    if (ImGui::SliderFloat("Sensitivity", &s, 0.25f, 4.0f, "%.2fx")) H->set_mouse_sens(s);
    bool inv = H->mouse_invert_y();
    if (ImGui::Checkbox("Invert up / down", &inv)) H->set_mouse_invert_y(inv);
    float rest = H->mouse_rest();
    ImGui::SetNextItemWidth(300);
    if (ImGui::SliderFloat("Resting speed", &rest, 0.0f, 500.0f, "%.0f px/s")) H->set_mouse_rest(rest);
    help("Slower mouse motion than this is ignored.");
}

static void page_gamepad() {
    const char *pad = H->pad_connected();
    if (pad) ImGui::Text("Connected: %s", pad);
    else ImGui::TextDisabled("No gamepad connected");
    float dz = H->pad_deadzone() * 100.0f;
    ImGui::SetNextItemWidth(300);
    if (ImGui::SliderFloat("Stick dead zone", &dz, 0.0f, 50.0f, "%.0f%%")) H->set_pad_deadzone(dz / 100.0f);
    if (ImGui::Button("Reset: Xbox layout")) H->reset_pad(false);
    ImGui::SameLine();
    if (ImGui::Button("Reset: Nintendo layout")) H->reset_pad(true);
    if (!ImGui::BeginTable("##pad", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Switch");
    ImGui::TableSetupColumn("Gamepad");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int a = 0; a < H->pad_action_count; a++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(H->action_label(a));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(H->pad_name(H->pad(a)));
        ImGui::TableNextColumn();
        bind_buttons(CAP_PAD, a, "Press a button", clear_pad);
    }
    ImGui::EndTable();
}

static void page_hotkeys() {
    if (ImGui::Button("Reset")) H->reset_hotkeys();
    ImGui::SameLine();
    ImGui::TextDisabled("Fixed: Esc, Alt+Enter");
    if (!ImGui::BeginTable("##hotkeys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Action");
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (int i = 0; i < H->hotkey_count(); i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(H->hotkey_label(i));
        ImGui::TableNextColumn();
        int k = H->hotkey_key(i);
        ImGui::TextUnformatted(key_name(k));
        if (k != SDL_SCANCODE_UNKNOWN)
            for (int a = 0; a < H->action_count; a++)
                if (H->key(a) == k) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "also %s", H->action_label(a)); break; }
        ImGui::TableNextColumn();
        bind_buttons(CAP_HOTKEY, i, "Press a key", clear_hotkey);
    }
    ImGui::EndTable();
}

static void window_options() {
    bool open = opt_open;
    ImGui::SetNextWindowSize(ImVec2(680, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Options", &open)) {
        int want = opt_tab.exchange(-1);
        if (ImGui::BeginTabBar("##options")) {
            if (ImGui::BeginTabItem("Video", nullptr, want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                page_video();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Interface", nullptr, want == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                page_interface();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls", nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ImGui::BeginTabBar("##controls")) {
                    if (ImGui::BeginTabItem("Keyboard")) { page_keyboard(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Mouse")) { page_mouse(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Gamepad")) { page_gamepad(); ImGui::EndTabItem(); }
                    if (ImGui::BeginTabItem("Hotkeys")) { page_hotkeys(); ImGui::EndTabItem(); }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) { opt_open = false; cap_kind = CAP_NONE; }
}

static void window_mods() {
    bool open = mods_open;
    ImGui::SetNextWindowSize(ImVec2(720, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Mods", &open)) {
        const ModFile *f;
        uint32_t n = H->mods_files(&f);
        ImGui::TextDisabled("DLLs in the mods folder. Changes apply at the next start.");
        if (!n) ImGui::TextDisabled("No mods installed.");
        else if (ImGui::BeginTable("##mods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Mod");
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();
            for (uint32_t i = 0; i < n; i++) {
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool on = f[i].enabled;
                if (ImGui::Checkbox("##on", &on)) H->mods_set_enabled(i, on);
                ImGui::TableNextColumn();
                if (f[i].mod) {
                    ImGui::Text("%s %s", f[i].mod->name ? f[i].mod->name : "?", f[i].mod->version ? f[i].mod->version : "");
                    if (f[i].mod->description && ImGui::BeginItemTooltip()) {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                        ImGui::TextUnformatted(f[i].mod->description);
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                } else
                    ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f[i].file);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f[i].status);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    if (!open) mods_open = false;
}

/* ---------------------------------------------------------------------------------------------- rooms
 * Multiplayer (games built with NETPLAY): the room we are in, the public list, direct connect, leaving; joins run
 * in the background (net/netmenu.c). Switching or leaving during a local session ends that session, so it asks. */
static UiRoom g_rooms[256];
static struct {
    bool active, need_pw, leave;            /* a join (or a leave) waiting for the modal's answer */
    char host[128], name[128];
    uint16_t port;
} pend;
static char pw_buf[128], nick_buf[64], direct_buf[160], direct_pw[128], search_buf[64], nick_msg[96];
static bool game_only = true;

static void want_join(const char *host, uint16_t port, bool need_pw, const char *name) {
    if (!need_pw && !H->in_session()) { H->room_join(host, port, ""); return; }
    pend.active = true;
    pend.leave = false;
    pend.need_pw = need_pw;
    snprintf(pend.host, sizeof pend.host, "%s", host);
    snprintf(pend.name, sizeof pend.name, "%s", name ? name : host);
    pend.port = port;
    pw_buf[0] = 0;
    modal_wanted = true;
}

static void want_leave() {
    if (!H->in_session()) { H->room_leave(); return; }
    pend.active = true;
    pend.leave = true;
    modal_wanted = true;
}

/* at the root of the frame: the password / "you are in a session" question */
static void room_modal() {
    if (!pend.active) { modal_wanted = false; return; }
    if (!ImGui::IsPopupOpen("Room##modal")) ImGui::OpenPopup("Room##modal");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Room##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (pend.leave) ImGui::TextUnformatted("Leave the room?");
    else ImGui::Text("Join \"%s\"?", pend.name);
    if (H->in_session())
        ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "This ends your current session.");
    if (!pend.leave && pend.need_pw) {
        ImGui::SetNextItemWidth(260);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Password", pw_buf, sizeof pw_buf, ImGuiInputTextFlags_Password);
    }
    bool go = ImGui::Button(pend.leave ? "Leave" : "Join");
    ImGui::SameLine();
    bool cancel = ImGui::Button("Cancel");
    if (go) {
        if (pend.leave) H->room_leave();
        else H->room_join(pend.host, pend.port, pw_buf);
    }
    if (go || cancel) { pend.active = false; modal_wanted = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

static void room_status_line() {
    int s = H->room_state();
    const char *m = H->room_message();
    if (s == UI_ROOM_JOINED) ImGui::Text("In room: %s", m);
    else if (s == UI_ROOM_JOINING) ImGui::Text("Joining %s ...", m);
    else if (s == UI_ROOM_FAILED) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", m);
    else ImGui::TextDisabled("Not in a room");
}

static bool host_port(const char *in, char *host, size_t n, uint16_t *port) {
    while (*in == ' ') in++;
    if (!*in) return false;
    snprintf(host, n, "%s", in);
    char *c = strrchr(host, ':');
    *port = 0;
    if (c && c[1] && strspn(c + 1, "0123456789") == strlen(c + 1)) { *port = (uint16_t)atoi(c + 1); *c = 0; }
    size_t l = strlen(host);
    while (l && host[l - 1] == ' ') host[--l] = 0;
    return *host != 0;
}

static bool contains_nocase(const char *s, const char *sub) {
    for (; *s; s++) {
        size_t k = 0;
        while (sub[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)sub[k])) k++;
        if (!sub[k]) return true;
    }
    return !*sub;
}

static void window_rooms() {
    bool open = rooms_open;
    ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Rooms", &open)) {
        if (ImGui::IsWindowAppearing()) {
            H->nickname(nick_buf, sizeof nick_buf);
            nick_msg[0] = 0;
            H->lobby_refresh();
        }
        room_status_line();
        int s = H->room_state();
        if (s == UI_ROOM_JOINED) { ImGui::SameLine(); if (ImGui::SmallButton("Leave")) want_leave(); }
        if (H->in_session())
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Switching ends your session.");
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Nickname", nick_buf, sizeof nick_buf);
        ImGui::SameLine();
        if (ImGui::Button("Save")) snprintf(nick_msg, sizeof nick_msg, "%s", H->set_nickname(nick_buf) ?
            "Saved" : "4-20 letters or digits");
        if (*nick_msg) { ImGui::SameLine(); ImGui::TextDisabled("%s", nick_msg); }

        ImGui::SeparatorText("Public rooms");
        bool loading;
        char err[256];
        int n = H->lobby(g_rooms, 256, &loading, err, sizeof err);
        if (ImGui::Button(loading ? "Loading..." : "Refresh") && !loading) H->lobby_refresh();
        ImGui::SameLine();
        ImGui::Checkbox("This game only", &game_only);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##search", "search", search_buf, sizeof search_buf);
        if (*err) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", err);
        float list_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.2f;
        if (ImGui::BeginTable("##rooms", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                            ImGuiTableFlags_BordersInnerV, ImVec2(0, list_h > 80 ? list_h : 80))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Room", 0, 3.0f);
            ImGui::TableSetupColumn("Game", 0, 2.0f);
            ImGui::TableSetupColumn("Players", 0, 0.8f);
            ImGui::TableSetupColumn("Host", 0, 1.4f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            for (int i = 0; i < n; i++) {
                const UiRoom &r = g_rooms[i];
                if (game_only && !r.this_game) continue;
                if (*search_buf && !contains_nocase(r.name, search_buf) && !contains_nocase(r.owner, search_buf) &&
                    !contains_nocase(r.game, search_buf)) continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool pick = ImGui::Selectable(r.name, false, ImGuiSelectableFlags_SpanAllColumns |
                                                             ImGuiSelectableFlags_AllowDoubleClick |
                                                             ImGuiSelectableFlags_AllowOverlap) &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                if (r.password) { ImGui::SameLine(); ImGui::TextDisabled("(password)"); }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.game);
                ImGui::TableNextColumn();
                ImGui::Text("%d / %d", r.players, r.max_players);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.owner);
                ImGui::TableNextColumn();
                bool full = r.max_players && r.players >= r.max_players;
                ImGui::BeginDisabled(full || s == UI_ROOM_JOINING);
                if (ImGui::SmallButton(full ? "Full" : "Join")) pick = true;
                ImGui::EndDisabled();
                if (pick && !full) want_join(r.address, r.port, r.password, r.name);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Direct connect");
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##direct", "host[:port]", direct_buf, sizeof direct_buf);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##dpw", "password", direct_pw, sizeof direct_pw, ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        char host[128];
        uint16_t port;
        ImGui::BeginDisabled(s == UI_ROOM_JOINING || !host_port(direct_buf, host, sizeof host, &port));
        if (ImGui::Button("Join##direct")) {
            if (H->in_session()) {
                want_join(host, port, true, host);
                snprintf(pw_buf, sizeof pw_buf, "%s", direct_pw);
            } else
                H->room_join(host, port, direct_pw);
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
    if (!open) rooms_open = false;
}

static void menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("Options")) {
        if (ImGui::MenuItem("Video...")) { opt_open = true; opt_tab = 0; }
        if (ImGui::MenuItem("Controls...")) { opt_open = true; opt_tab = 1; }
        if (ImGui::MenuItem("Interface...")) { opt_open = true; opt_tab = 2; }
        ImGui::Separator();
        bool full = H->fullscreen(), fps = H->fps_counter();
        if (ImGui::MenuItem("Fullscreen", "Alt+Enter", full)) H->set_fullscreen(!full);
        if (ImGui::MenuItem("FPS counter", nullptr, fps)) H->set_fps_counter(!fps);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mods")) {
        const ModWindow *w;
        uint32_t n = H->mods_windows(&w);
        if (!n) ImGui::MenuItem("(none)", nullptr, false, false);
        for (uint32_t i = 0; i < n; i++) {
            int k = H->hotkey_key(w[i].hotkey);
            if (ImGui::MenuItem(w[i].title, k ? key_name(k) : nullptr, w[i].is_open())) H->mods_window_toggle(i);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Manage mods...")) mods_open = true;
        ImGui::EndMenu();
    }
    if (H->netplay && ImGui::BeginMenu("Multiplayer")) {
        int s = H->room_state();
        const char *m = H->room_message();
        if (s == UI_ROOM_JOINED) ImGui::MenuItem(m, nullptr, false, false);
        else if (s == UI_ROOM_JOINING) ImGui::MenuItem("Joining...", nullptr, false, false);
        else ImGui::MenuItem("Not in a room", nullptr, false, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Rooms...")) rooms_open = true;
        if (ImGui::MenuItem("Leave room", nullptr, false, s == UI_ROOM_JOINED)) want_leave();
        ImGui::EndMenu();
    }
    const char *hint = "Esc: free the mouse";
    float wdt = ImGui::CalcTextSize(hint).x + ImGui::GetStyle().ItemSpacing.x * 2;
    if (ImGui::GetContentRegionAvail().x > wdt) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - wdt);
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::EndMainMenuBar();
}

/* ---------------------------------------------------------------------------------------------- rendering */
static struct {
    bool ready, failed;
    VkFormat format;
    LARGE_INTEGER last, freq;
} R;

static void check_vk(VkResult r) {
    if (r < 0 && H) H->log(0, "ui: Vulkan error %d", (int)r);
}

static void drain(ImGuiIO &io) {
    std::vector<Ev> evs;
    {
        std::lock_guard<std::mutex> l(g_lock);
        evs.swap(g_events);
    }
    for (const Ev &e : evs) {
        switch (e.kind) {
        case Ev::MOVE: io.AddMousePosEvent(e.x, e.y); break;
        case Ev::BUTTON: io.AddMousePosEvent(e.x, e.y); io.AddMouseButtonEvent(e.button, e.down); break;
        case Ev::WHEEL: io.AddMouseWheelEvent(e.x, e.y); break;
        case Ev::KEY:
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.mods & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.mods & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.mods & SDL_KMOD_ALT) != 0);
            if (e.key != ImGuiKey_None) io.AddKeyEvent(e.key, e.down);
            break;
        case Ev::TEXT: io.AddInputCharactersUTF8(e.text); break;
        case Ev::FOCUS:
            io.AddFocusEvent(e.down);
            if (!e.down) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        }
    }
}

static bool backend(const NxModFrame *f) {
    if (R.ready) return true;
    if (R.failed) return false;
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = f->instance;
    ii.PhysicalDevice = f->phys;
    ii.Device = f->dev;
    ii.QueueFamily = f->family;
    ii.Queue = f->queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = f->image_count < 4 ? 4 : f->image_count;    /* at least the runtime's frames in flight */
    ii.UseDynamicRendering = true;
    R.format = f->format;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &R.format;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        H->log(0, "ui: ImGui's Vulkan backend did not start: no menu");
        R.failed = true;
        return false;
    }
    QueryPerformanceFrequency(&R.freq);
    QueryPerformanceCounter(&R.last);
    R.ready = true;
    H->log(1, "ui: ImGui %s on the swapchain (%ux%u, format %d)", IMGUI_VERSION, f->width, f->height, (int)f->format);
    return true;
}

EXPORT void nxui_draw(const NxModFrame *f) {
    if (!f->view || !f->width || !f->height || !backend(f)) return;
    ImGuiIO &io = ImGui::GetIO();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - R.last.QuadPart) / (float)R.freq.QuadPart;
    R.last = now;
    io.DeltaTime = dt > 0 ? (dt < 0.25f ? dt : 0.25f) : 1.0f / 60;
    io.DisplaySize = ImVec2((float)f->width, (float)f->height);
    drain(io);
    build_style(f->height);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (visible()) {
        if (bar_visible(SDL_GetTicks())) menu_bar();
        if (opt_open) window_options();
        if (mods_open) window_mods();
        if (H->netplay && rooms_open) window_rooms();
        H->mods_ui();
        if (H->netplay) room_modal();
    }
    menu_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGui::Render();
    want_mouse = io.WantCaptureMouse;
    want_kb = io.WantCaptureKeyboard;
    want_text = io.WantTextInput;
    ImDrawData *dd = ImGui::GetDrawData();
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = f->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {f->width, f->height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(f->cmd, &ri);
    f->queue_lock();                        /* texture uploads submit to the shared queue */
    ImGui_ImplVulkan_RenderDrawData(dd, f->cmd);
    f->queue_unlock();
    vkCmdEndRendering(f->cmd);
}

/* ---------------------------------------------------------------------------------------------- start */
EXPORT bool nxui_init(const UiHost *host) {
    if (!host || host->version != NXUI_VERSION) return false;
    H = host;
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    snprintf(g_ini, sizeof g_ini, "%s\\ui.ini", host->cache_dir);
    io.IniFilename = g_ini;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    was_visible = !host->mouse_captured();
    return true;
}

EXPORT void nxui_imgui(int *version, void **ctx, void *(**alloc)(size_t, void *), void (**free_)(void *, void *),
                       void **ud) {
    ImGuiMemAllocFunc a;
    ImGuiMemFreeFunc f;
    ImGui::GetAllocatorFunctions(&a, &f, ud);
    *version = IMGUI_VERSION_NUM;
    *ctx = g_ctx;
    *alloc = a;
    *free_ = f;
}
