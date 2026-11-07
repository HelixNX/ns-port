/* The menu (ui.c loads nxui.dll; ui_host.h) */
#pragma once
#include "a64rt.h"
#include "vk.h"
#include <SDL3/SDL.h>

bool ui_init(SDL_Window *window);           /* window thread, after options_init / input_init, before mods_load */
bool ui_event(const SDL_Event *e);          /* window thread, first for every event; true: used */
void ui_tick(void);                         /* window thread, once a loop */
bool ui_wants_keyboard(void);               /* input.c: the game gets no keyboard while true */
bool ui_active(void);                       /* vk.c: something to draw this frame */
void ui_toggle_menu(void);                  /* the menu hotkey */
void ui_show(void);                         /* a mod's window opened: free the mouse, show the menu */
/* vk.c, inside the present: the image in COLOR_ATTACHMENT_OPTIMAL, left so */
void ui_draw(VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat format, u32 w, u32 h, u32 count);
