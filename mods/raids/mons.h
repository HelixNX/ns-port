/* mons.cpp: the mon spawner window (F8), drawn by the raids mod */
#pragma once

void mons_init(const char *cache_dir);      /* after modkit::init */
void mons_ui();                             /* inside the mod's ImGui frame */
bool mons_open();
void mons_toggle();
