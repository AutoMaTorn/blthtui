#ifndef BLTTUI_UI_H
#define BLTTUI_UI_H

#include "bt.h"

/* Run the interactive newt interface until the user quits.
 * Owns newtInit()/newtFinished(). Returns 0 on normal exit. */
int ui_run(bt_ctx *ctx);

#endif /* BLTTUI_UI_H */
