/* modkit: Dear ImGui for mods (runtime/nxmod.h, version 4). The runtime owns the ImGui context and draws its menu;
 * a mod draws its windows into the same frame, from its ui() callback (render thread).
 *
 *   nx_mod_init:  if (!modkit::init(host, "my_mod")) return NULL;     // the runtime's ImGui, or decline
 *                 modkit::window("My window", "F6", my_is_open, my_toggle);   // Mods menu + a rebindable hotkey
 *   ui():         if (my_is_open()) { ImGui::Begin("My window", &open) ...; ImGui::End(); }
 * Opening a window frees the mouse; the game keeps its keyboard unless the window has keyboard focus. */
#pragma once
#include "nxmod.h"
#include <imgui.h>

namespace modkit {

bool init(const NxModHost *host, const char *name);  /* false: an older runtime or a different ImGui: decline */
const NxModHost *host();
const char *name();
bool window(const char *title, const char *default_key, bool (*is_open)(void), void (*toggle)(void));

}  // namespace modkit
