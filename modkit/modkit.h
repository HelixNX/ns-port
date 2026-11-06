/* modkit: Dear ImGui for mods (runtime/nxmod.h), drawn over the game.
 *
 * The window's events arrive on the window thread and drawing happens on the render thread, so ImGui's own SDL3
 * backend (which calls SDL from NewFrame) is not used: event() queues what ImGui needs and begin() feeds it in on
 * the render thread. Rendering is ImGui's Vulkan backend with dynamic rendering into the swapchain image; its
 * uploads go to the runtime's queue under the runtime's queue lock.
 *
 *   nx_mod_init:  modkit::init(host, "my_mod");
 *   event():      return modkit::event(e, open);      // open: the mod's window shows (it then takes every key
 *                                                       // and mouse event, and the game gets no input)
 *   draw():       if (modkit::begin(f)) { ImGui::Begin(...) ...; modkit::end(f); }
 * modkit::set_open() (window thread) starts and stops SDL's text input and frees the mouse. */
#pragma once
#include "nxmod.h"
#include <imgui.h>

namespace modkit {

void init(const NxModHost *host, const char *name);  /* ImGui's settings go to <cache>\mods\<name>.ini */
const NxModHost *host();
bool event(const SDL_Event *e, bool open);   /* window thread: true when it was used */
void set_open(bool open);                     /* window thread: the UI shows (text input on, mouse freed) */
bool begin(const NxModFrame *f);              /* render thread: a new ImGui frame (false: not ready) */
void end(const NxModFrame *f);                /* render thread: render it into f->view */

}  // namespace modkit
