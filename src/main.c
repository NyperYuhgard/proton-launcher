/* main.c - Entry point.
 *
 * Usage:
 *   proton-launcher            start the GUI
 *   proton-launcher --version  print the version
 *   proton-launcher FILE.exe   prefill the executable field
 *
 * Note: --help is handled by GOptionContext, so it stays consistent with
 * the GTK options registered in the same context.
 */

#include "pl.h"
#include "ui.h"
#include "ui_private.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

static gboolean opt_version = FALSE;
static char   **opt_files   = NULL;

static const GOptionEntry options[] = {
    { "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
      "Print the version and exit", NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &opt_files,
      NULL, "[FILE]" },
    { NULL, 0, 0, 0, NULL, NULL, NULL }
};

static const char *usage_summary =
    "Runs Proton builds on Linux without Steam.\n"
    "\n"
    "Proton builds must already be downloaded and extracted; the setup "
    "wizard on first run asks where they are. Nothing is installed or "
    "modified outside the folders you choose.";

static const char *usage_description =
    "FILE is an optional Windows executable to prefill the executable field.\n"
    "\n"
    "Settings live in $XDG_CONFIG_HOME/proton-launcher/config.ini and every "
    "path in it can be changed from the Preferences dialog.";

int
main (int argc, char *argv[])
{
    GOptionContext *context;
    GError         *error = NULL;
    PlWindow       *window;

    context = g_option_context_new ("[FILE] - run Proton without Steam");
    g_option_context_set_summary (context, usage_summary);
    g_option_context_set_description (context, usage_description);
    g_option_context_add_main_entries (context, options, NULL);
    g_option_context_add_group (context, gtk_get_option_group (FALSE));

    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("%s: %s\n", PL_APP_NAME,
                    error != NULL ? error->message : "invalid arguments");
        g_clear_error (&error);
        g_option_context_free (context);
        return 1;
    }
    g_option_context_free (context);

    if (opt_version) {
        g_print ("%s %s\n", PL_APP_NAME, PL_VERSION);
        return 0;
    }

    if (!gtk_init_check (&argc, &argv)) {
        g_printerr ("%s: cannot open a display.\n"
                    "Set DISPLAY or WAYLAND_DISPLAY and try again.\n",
                    PL_APP_NAME);
        return 1;
    }

    window = pl_window_new (NULL);

    if (opt_files != NULL && opt_files[0] != NULL) {
        GFile *file = g_file_new_for_commandline_arg (opt_files[0]);
        char  *path = g_file_get_path (file);

        if (path != NULL) {
            pl_window_set_exe (window, path);
            g_free (path);
        }
        g_object_unref (file);
    }

    pl_window_run (window);
    pl_window_free (window);

    g_strfreev (opt_files);
    return 0;
}