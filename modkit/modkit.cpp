/* modkit: Dear ImGui for mods (see modkit.h). */
#include "modkit.h"
#include <imgui_impl_vulkan.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <windows.h>

namespace modkit {

static const NxModHost *g_host;
static char g_ini[MAX_PATH + 64];

/* one queued window event, as ImGui wants it (window thread -> render thread) */
struct Ev {
    enum Kind { MOVE, BUTTON, WHEEL, KEY, TEXT, FOCUS } kind;
    float x, y;                 /* MOVE: pixels; WHEEL: steps */
    int button;
    bool down;
    ImGuiKey key;
    SDL_Keymod mods;
    char text[32];
};
static std::mutex g_lock;
static std::vector<Ev> g_events;
static volatile bool g_open;

static struct {
    bool ready;
    VkFormat format;
    LARGE_INTEGER last, freq;
} g_r;

void init(const NxModHost *host, const char *name) {
    g_host = host;
    snprintf(g_ini, sizeof g_ini, "%s\\mods", host->cache_dir);
    CreateDirectoryA(g_ini, NULL);
    snprintf(g_ini, sizeof g_ini, "%s\\mods\\%s.ini", host->cache_dir, name);
}

const NxModHost *host() { return g_host; }

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

static void push(const Ev &e) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_events.size() < 4096) g_events.push_back(e);
}

void set_open(bool open) {
    if (open == g_open) return;
    g_open = open;
    SDL_Window *w = g_host ? g_host->window : NULL;
    if (open) {
        if (g_host && g_host->release_mouse) g_host->release_mouse();
        if (w) SDL_StartTextInput(w);
    } else if (w)
        SDL_StopTextInput(w);
    Ev e = {};
    e.kind = Ev::FOCUS;
    e.down = open;
    push(e);
}

bool event(const SDL_Event *e, bool open) {
    if (!open) return false;
    Ev q = {};
    float d = 1.0f;
    if (g_host && g_host->window) d = SDL_GetWindowPixelDensity(g_host->window);
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        q.kind = Ev::MOVE, q.x = e->motion.x * d, q.y = e->motion.y * d;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        q.kind = Ev::BUTTON, q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 :
                   e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        q.x = e->button.x * d, q.y = e->button.y * d;
        if (q.button < 0) return true;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        q.kind = Ev::WHEEL, q.x = e->wheel.x, q.y = e->wheel.y;
        break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        q.kind = Ev::KEY, q.down = e->type == SDL_EVENT_KEY_DOWN;
        q.key = key_of(e->key.key, e->key.scancode), q.mods = e->key.mod;
        break;
    case SDL_EVENT_TEXT_INPUT:
        q.kind = Ev::TEXT;
        snprintf(q.text, sizeof q.text, "%s", e->text.text ? e->text.text : "");
        break;
    default:
        return false;                       /* window events etc. stay the runtime's */
    }
    push(q);
    return true;
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

static void check_vk(VkResult r) {
    if (r < 0 && g_host) g_host->log(0, "modkit: Vulkan error %d", (int)r);
}

bool begin(const NxModFrame *f) {
    if (!f->view || !f->width || !f->height) return false;
    if (!g_r.ready) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = g_ini;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        ImGuiStyle &st = ImGui::GetStyle();
        float scale = f->height >= 1400 ? 1.75f : f->height >= 1000 ? 1.35f : 1.0f;
        st.ScaleAllSizes(scale);
        st.FontScaleMain = scale;
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
        g_r.format = f->format;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_r.format;
        ii.CheckVkResultFn = check_vk;
        if (!ImGui_ImplVulkan_Init(&ii)) {
            g_host->log(0, "modkit: ImGui's Vulkan backend did not start");
            return false;
        }
        QueryPerformanceFrequency(&g_r.freq);
        QueryPerformanceCounter(&g_r.last);
        g_r.ready = true;
        g_host->log(1, "modkit: ImGui %s on the swapchain (%ux%u, format %d)", IMGUI_VERSION, f->width, f->height,
                    (int)f->format);
    }
    ImGuiIO &io = ImGui::GetIO();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_r.last.QuadPart) / (float)g_r.freq.QuadPart;
    g_r.last = now;
    io.DeltaTime = dt > 0 ? (dt < 0.25f ? dt : 0.25f) : 1.0f / 60;
    io.DisplaySize = ImVec2((float)f->width, (float)f->height);
    drain(io);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    return true;
}

void end(const NxModFrame *f) {
    ImGui::Render();
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

}  // namespace modkit
