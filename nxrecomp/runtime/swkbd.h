/* swkbd.c: the software keyboard's input box */
#pragma once
#include "hos.h"
#include <stddef.h>

/* shows the box and waits for it; false when cancelled. out gets the text (NUL-terminated); max_len 0 = no limit */
bool swkbd_prompt(const wchar_t *title, const wchar_t *prompt, u32 max_len, bool numeric, wchar_t *out, u32 out_len);
