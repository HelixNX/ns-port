/* items.cpp: the item giver window (F7), drawn by the raids mod */
#pragma once

void items_init(const char *cache_dir);     /* after modkit::init */
void items_ui();                            /* inside the mod's ImGui frame */
bool items_open();
void items_toggle();
