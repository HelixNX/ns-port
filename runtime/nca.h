/* The game read straight from the user's own NSPs (base, update, DLC) and firmware NCAs, decrypted with their
 * keys (nca.c): nothing is extracted. */
#ifndef NCA_H
#define NCA_H
#include "a64rt.h"
#include <stdbool.h>

typedef struct NxStorage NxStorage;
u64 nx_storage_size(NxStorage *s);
bool nx_storage_read(NxStorage *s, u64 off, void *dst, u64 size);    /* false past the end */

/* scans <dir> (and <dir>\firmware) for NSPs, NCAs and prod.keys / title.keys (also %USERPROFILE%\.switch);
 * false (with a message box's text in err) if the game is not there */
bool nx_game_open(const char *dir, char *err, size_t errn);
bool nx_game_present(const char *dir);              /* NSPs in dir: the game is read from them */
NxStorage *nx_game_romfs(void);                     /* the program's RomFS image, the update applied */
void *nx_game_exefs_file(const char *name, u64 *size);    /* malloc'd; NULL if absent */
NxStorage *nx_data_romfs(u64 id);                   /* add-on content or firmware system data by data id */
void *nx_game_control_file(const char *name, u64 *size);  /* control.nacp, icon_AmericanEnglish.dat; malloc'd */

/* image.c: <dir>\<module>.img from the ExeFS's NSOs, unless they are there already */
bool image_build(const char *dir, char *err, size_t errn);

/* a file inside a RomFS image: its offset in the image and size; false if not there */
bool nx_romfs_find(NxStorage *romfs, const char *path, u64 *off, u64 *size);
#endif
