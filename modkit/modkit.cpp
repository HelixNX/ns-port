/* modkit: Dear ImGui for mods (see modkit.h). */
#include "modkit.h"
#include <stdio.h>
#include <string.h>

namespace modkit {

static const NxModHost *g_host;
static char g_name[64];

bool init(const NxModHost *host, const char *name) {
    g_host = host;
    snprintf(g_name, sizeof g_name, "%s", name);
    if (host->version < 4 || !host->imgui_context) {
        host->log(0, "%s: this game has no menu for mods (runtime too old, or nxui.dll missing)", name);
        return false;
    }
    if (host->imgui_version != IMGUI_VERSION_NUM) {
        host->log(0, "%s: built with Dear ImGui %d, the game has %d: rebuild the mod", name, IMGUI_VERSION_NUM,
                  host->imgui_version);
        return false;
    }
    ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)host->imgui_alloc, (ImGuiMemFreeFunc)host->imgui_free,
                                 host->imgui_alloc_ud);
    ImGui::SetCurrentContext((ImGuiContext *)host->imgui_context);
    return true;
}

const NxModHost *host() { return g_host; }
const char *name() { return g_name; }

bool window(const char *title, const char *default_key, bool (*is_open)(void), void (*toggle)(void)) {
    return g_host && g_host->add_window && g_host->add_window(title, default_key, is_open, toggle);
}

}  // namespace modkit
