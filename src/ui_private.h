/* ui_private.h - Interface between the window and its dialogs. */

#pragma once

#include "pl.h"
#include "ui.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Implemented by ui_window.c */
GtkWidget *pl_window_root        (PlWindow *self);
PlConfig  *pl_window_config      (PlWindow *self);
char      *pl_window_config_file (PlWindow *self);
void       pl_window_reload      (PlWindow *self);
void       pl_window_set_exe     (PlWindow *self, const char *path);

/* Implemented by ui_wizard.c: returns TRUE when setup is needed. */
gboolean   pl_wizard_run         (PlWindow *self);

/* Implemented by ui_prefs.c */
void pl_prefs_dialog_show (GtkButton *button, gpointer user_data);

G_END_DECLS