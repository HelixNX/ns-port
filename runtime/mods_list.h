/* What the menu (ui.cpp, C++) sees of mods (mods.c): kept free of the runtime's C headers */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nxmod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a DLL found in mods\ (Mods > Manage lists them all) and a window a mod registered (the Mods menu) */
typedef struct ModFile {
    char file[128];
    char status[96];                        /* "loaded", "off ...", "declined ...", "does not load ..." */
    bool enabled;                           /* settings.ini mods.off: applies at the next start */
    const NxMod *mod;                       /* NULL unless loaded */
} ModFile;
typedef struct ModWindow {
    char title[96];
    bool (*is_open)(void);
    void (*toggle)(void);
    int file;                               /* index into mods_files() */
    int hotkey;                             /* options.h hotkey index */
} ModWindow;

uint32_t mods_files(const ModFile **out);
uint32_t mods_windows(const ModWindow **out);
void mods_window_toggle(uint32_t i);        /* the menu (render thread) or a hotkey (window thread) */
void mods_set_enabled(uint32_t i, bool on); /* for the next start */
bool mods_any_window_open(void);
void mods_ui(void);                         /* ui.cpp, render thread, inside its ImGui frame */

#ifdef __cplusplus
}
#endif
