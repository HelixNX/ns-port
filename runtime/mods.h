/* The runtime's side of mods (mods.c; the interface itself is nxmod.h) */
#pragma once
#include "a64rt.h"
#include "vk.h"                             /* first: VK_NO_PROTOTYPES before nxmod.h's vulkan.h */
#include "nxmod.h"

void mods_load(SDL_Window *window);         /* window thread, before its first event */
bool mods_event(const SDL_Event *e);        /* true: a mod used it */
bool mods_want_input(void);                 /* input.c: the game gets no input while true */
bool mods_draw_any(void);                   /* vk.c: a mod draws (the image goes to COLOR_ATTACHMENT for it) */
/* vk.c, inside the present: the image in COLOR_ATTACHMENT_OPTIMAL, left so */
void mods_draw(VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat format, u32 w, u32 h, u32 count);
