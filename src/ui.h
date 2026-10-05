/* ui.h - GTK3 user interface for Proton Launcher. */

#pragma once

#include "pl.h"

G_BEGIN_DECLS

typedef struct _PlWindow PlWindow;

/* Creates the main window. 'config_file' may be NULL for the default
 * location. The returned window owns the PlConfig. */
PlWindow *pl_window_new (const char *config_file);

void      pl_window_run (PlWindow *self);
void      pl_window_free (PlWindow *self);

G_END_DECLS