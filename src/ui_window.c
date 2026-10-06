/* ui_window.c - Main window: pick a Proton build, a prefix and an
 * executable, then launch it or open a Wine utility.
 *
 * Layout:
 *
 *   ┌─ Proton build ─────────────┬─ Prefix ──────────────────┐
 *   │ [combo            ] Reload │ [combo        ] New / Del │
 *   └────────────────────────────┴──────────────────────────┘
 *   Executable [ entry                          ] Browse
 *   Renderer   (•) DXVK/vkd3d  ( ) WineD3D (OpenGL)
 *   [ Run ] [ Wine tools ▾ ] [ Stop ]      status label
 *   ┌─ Output ──────────────────────────────────────────────┐
 *   │ monospace log view                                   │
 *   └──────────────────────────────────────────────────────┘
 *   preflight banner
 */

#include "ui.h"
#include "ui_private.h"

#include <gtk/gtk.h>
#include <string.h>

struct _PlWindow {
    GtkWidget  *window;

    /* Configuration */
    PlConfig   *config;
    char       *config_file;

    /* Discovery */
    PlProtonList *protons;
    PlPrefixList *prefixes;
    PlRunner     *runner;
    PlReport     *report;

    /* Widgets */
    GtkWidget *proton_combo;
    GtkWidget *prefix_combo;
    GtkWidget *exe_entry;
    GtkWidget *wined3d_radio;
    GtkWidget *run_button;
    GtkWidget *stop_button;
    GtkWidget     *status_label;
    GtkWidget     *log_view;
    GtkTextBuffer *log_buffer;
    GtkWidget *banner;
    GtkWidget *banner_label;
    GtkWidget *tools_button;
    GtkWidget *tools_menu;
    GtkWidget *prefix_menu;

    /* Guards against re-entrant reloads while we rebuild combos. */
    gboolean   loading;
};

static void refresh_protons   (PlWindow *self);
static void refresh_prefixes  (PlWindow *self);
static void refresh_banner    (PlWindow *self);
static void refresh_actions   (PlWindow *self);
static void append_log        (PlWindow *self, const char *text);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const PlProton *
current_proton (PlWindow *self)
{
    int index;

    if (self->protons == NULL || self->protons->items->len == 0)
        return NULL;

    index = gtk_combo_box_get_active (GTK_COMBO_BOX (self->proton_combo));
    if (index < 0)
        return pl_proton_latest (self->protons);

    return g_ptr_array_index (self->protons->items, (guint) index);
}

static const PlPrefix *
current_prefix (PlWindow *self)
{
    int index = gtk_combo_box_get_active (GTK_COMBO_BOX (self->prefix_combo));

    if (self->prefixes == NULL || index < 0)
        return NULL;

    return g_ptr_array_index (self->prefixes->items, (guint) index);
}

/* Resolve which Proton build to use, honouring the stored preference
 * while falling back to the newest one available. */
static const PlProton *
resolve_proton (PlWindow *self)
{
    const PlProton *proton;

    if (self->protons == NULL || self->protons->items->len == 0)
        return NULL;

    if (self->config->proton_version != NULL) {
        proton = pl_proton_find (self->protons, self->config->proton_version);
        if (proton != NULL)
            return proton;
    }

    return pl_proton_latest (self->protons);
}

static char *
steam_stub_dir (PlWindow *self)
{
    /* Proton requires STEAM_COMPAT_CLIENT_INSTALL_PATH to name an existing
     * directory (it becomes the "t:" drive), so give it a real one inside
     * our own data directory rather than a fabricated path. */
    return g_build_filename (g_get_user_data_dir (), "proton-launcher",
                             "steam-stub", NULL);
}

static void
set_status (PlWindow *self, const char *format, ...) G_GNUC_PRINTF (2, 3);

static void
set_status (PlWindow *self, const char *format, ...)
{
    va_list args;
    char   *text;

    va_start (args, format);
    text = g_strdup_vprintf (format, args);
    va_end (args);

    gtk_label_set_text (GTK_LABEL (self->status_label), text);
    g_free (text);
}

/* Append to the log view, keeping it scrolled to the bottom. */
static void
append_log (PlWindow *self, const char *text)
{
    GtkTextIter  end;
    GtkTextMark *mark;

    if (text == NULL)
        return;

    gtk_text_buffer_get_end_iter (self->log_buffer, &end);
    gtk_text_buffer_insert (self->log_buffer, &end, text, -1);

    mark = gtk_text_buffer_create_mark (self->log_buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (self->log_view), mark,
                                  0.0, TRUE, 0.0, 0.0);
    gtk_text_buffer_delete_mark (self->log_buffer, mark);
}

/* ------------------------------------------------------------------ */
/* Runner callbacks                                                    */
/* ------------------------------------------------------------------ */

static void
on_runner_line (const char *line, void *user_data)
{
    PlWindow  *self = user_data;
    GtkTextIter end;

    /* Output is forwarded verbatim; expand tabs so columnar Wine and
     * Proton diagnostics stay legible. */
    gtk_text_buffer_get_end_iter (self->log_buffer, &end);
    gtk_text_buffer_insert_with_tags_by_name (self->log_buffer, &end, line, -1,
                                              "output", NULL);

    {
        GtkTextMark *mark = gtk_text_buffer_create_mark (self->log_buffer,
                                                         NULL, &end, FALSE);
        gtk_text_view_scroll_to_mark (GTK_TEXT_VIEW (self->log_view), mark,
                                      0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark (self->log_buffer, mark);
    }
}

static void
on_runner_exit (gboolean normal_exit, int status, void *user_data)
{
    PlWindow *self = user_data;

    refresh_actions (self);

    if (normal_exit && status == 0) {
        set_status (self, "Session ended normally");
        gtk_window_set_title (GTK_WINDOW (self->window), PL_APP_NAME);
    } else if (normal_exit) {
        set_status (self, "Session ended with exit code %d", status);
    } else {
        set_status (self, "Session ended by signal");
    }

    /* When PROTON_LOG is on, point the user at the file Proton wrote. */
    if (self->config->proton_log && self->config->log_dir != NULL) {
        append_log (self, "\nProton debug log: ");
        append_log (self, self->config->log_dir);
        append_log (self, "\n");
    }
}

/* ------------------------------------------------------------------ */
/* Launching                                                           */
/* ------------------------------------------------------------------ */

static void
start_request (PlWindow *self, PlRunRequest *req)
{
    char  *stub;
    GError *error = NULL;

    if (pl_runner_is_running (self->runner)) {
        GtkWidget *dialog = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
            "A session is already running.\n\nStop it before starting a new one.");

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        return;
    }

    stub = steam_stub_dir (self);
    g_mkdir_with_parents (stub, 0755);

    gtk_text_buffer_set_text (self->log_buffer, "", -1);

    /* Persist the renderer choice and Proton selection. */
    if (self->config->proton_version != NULL && req->proton != NULL) {
        g_free (self->config->proton_version);
        self->config->proton_version = g_strdup (req->proton->id);
    }

    if (!pl_run_start (self->runner, req, stub, self->config->proton_log,
                       on_runner_line, on_runner_exit, self, &error)) {
        GtkWidget *dialog;

        append_log (self, error != NULL ? error->message : "Failed to launch\n");
        set_status (self, "Failed to launch");

        dialog = gtk_message_dialog_new (GTK_WINDOW (self->window),
                                         GTK_DIALOG_MODAL,
                                         GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                         "Could not start Proton.\n\n%s",
                                         error != NULL ? error->message : "");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);

        g_clear_error (&error);
        g_free (stub);
        refresh_actions (self);
        return;
    }

    g_free (stub);

    set_status (self, "Running: %s",
                req->exe != NULL ? req->exe : "Wine tool");
    refresh_actions (self);
}

static void
on_run_clicked (GtkButton *button, gpointer user_data)
{
    PlWindow     *self = user_data;
    const PlProton *proton = current_proton (self);
    const PlPrefix *prefix = current_prefix (self);
    PlRunRequest  req;
    const char   *exe;
    char         *exe_abs = NULL;

    if (proton == NULL || prefix == NULL) {
        GtkWidget *dialog = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "Select a Proton build and a prefix before running.");

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        return;
    }

    exe = gtk_entry_get_text (GTK_ENTRY (self->exe_entry));
    if (exe == NULL || *exe == '\0') {
        GtkWidget *dialog = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "Choose an executable to run.");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        return;
    }

    if (!g_path_is_absolute (exe)) {
        GFile *file = g_file_new_for_commandline_arg (exe);

        exe_abs = g_file_get_path (file);
        g_object_unref (file);
    } else {
        exe_abs = g_strdup (exe);
    }

    if (!g_file_test (exe_abs, G_FILE_TEST_EXISTS)) {
        char      *message = g_strdup_printf ("File not found:\n%s", exe_abs);
        GtkWidget *dialog  = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE, "%s", message);

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        g_free (message);
        g_free (exe_abs);
        return;
    }

    memset (&req, 0, sizeof (req));
    req.verb       = PL_VERB_RUN;
    req.proton     = proton;
    req.prefix     = prefix;
    req.exe        = exe_abs;
    req.working_dir = g_path_get_dirname (exe_abs);
    req.wined3d    = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->wined3d_radio));
    req.game_id    = self->config->game_id;

    start_request (self, &req);

    g_free ((char *) req.working_dir);
    g_free (exe_abs);
}

/* ------------------------------------------------------------------ */
/* Wine tools                                                          */
/* ------------------------------------------------------------------ */

/* One entry in the Wine tools menu.
 *
 * `argv` is what `proton runinprefix` receives, so every program name here is
 * the one Windows resolves inside the prefix. These are Wine builtins shipped
 * in <prefix>/pfx/drive_c/windows/system32, except regedit.exe, which lives in
 * windows/ rather than system32/. Names were resolved with
 * `proton runinprefix cmd /c where ...` against a real GE-Proton prefix, and
 * each one was then launched to confirm it actually opens a window.
 *
 * An entry with a NULL id is a separator.
 */
typedef struct {
    const char *id;
    const char *label;
    const char *argv[4];      /* NULL-terminated */
} WineTool;

static const WineTool wine_tools[] = {
    { "winecfg",  "Configuration",          { "winecfg", NULL } },
    { "regedit",  "Registry Editor",        { "regedit", NULL } },
    { "control",  "Control Panel",          { "control", NULL } },

    { NULL, NULL, { NULL } },

    { "taskmgr",  "Task Manager",           { "taskmgr", NULL } },
    { "msinfo32", "System Information",     { "msinfo32", NULL } },

    { NULL, NULL, { NULL } },

    { "explorer", "File Explorer",          { "explorer", NULL } },
    { "winefile", "Wine File Manager",      { "winefile", NULL } },
    { "notepad",  "Notepad",                { "notepad", NULL } },
    /* A bare `cmd` is interactive and the launcher has no terminal to give
     * it, so it would read EOF and exit at once; wineconsole opens a window
     * instead. */
    { "cmd",      "Command Prompt",         { "wineconsole", "cmd", NULL } },

    { NULL, NULL, { NULL } },

    { "msiexec",    "Windows Installer",    { "msiexec", NULL } },
    { "uninstaller", "Add/Remove Programs",  { "uninstaller", NULL } },

    { NULL, NULL, { NULL } },

    { "wineboot-init",   "Initialize prefix", { "wineboot", "--init", NULL } },
    { "wineboot-update", "Update prefix",    { "wineboot", "-u", NULL } },
};

static const WineTool *
wine_tool_find (const char *id)
{
    if (id == NULL)
        return NULL;

    for (gsize i = 0; i < G_N_ELEMENTS (wine_tools); i++) {
        if (wine_tools[i].id != NULL && g_strcmp0 (wine_tools[i].id, id) == 0)
            return &wine_tools[i];
    }
    return NULL;
}

/* Wine utilities must go through `runinprefix`; `run` would try to start
 * them through the game launcher. */
static void
run_wine_tool (PlWindow *self, const char *const *tool_argv)
{
    const PlProton *proton = current_proton (self);
    const PlPrefix *prefix = current_prefix (self);
    PlRunRequest   req;
    GPtrArray      *args;

    if (proton == NULL || prefix == NULL) {
        GtkWidget *dialog = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
            "Select a Proton build and a prefix first.");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        return;
    }

    args = g_ptr_array_new ();
    for (gsize i = 0; tool_argv[i] != NULL; i++)
        g_ptr_array_add (args, g_strdup (tool_argv[i]));

    memset (&req, 0, sizeof (req));
    req.verb    = PL_VERB_RUNINPREFIX;
    req.proton  = proton;
    req.prefix  = prefix;
    req.args    = args;
    req.game_id = self->config->game_id;

    start_request (self, &req);

    g_ptr_array_unref (args);
}

static void
on_tool_clicked (GtkWidget *widget, gpointer user_data)
{
    PlWindow       *self = user_data;
    const WineTool *tool;
    const char     *id = g_object_get_data (G_OBJECT (widget), "tool-id");

    tool = wine_tool_find (id);
    if (tool == NULL)
        return;

    run_wine_tool (self, tool->argv);
}

static void
on_stop_clicked (GtkButton *button, gpointer user_data)
{
    PlWindow *self = user_data;

    pl_runner_cancel (self->runner);
    set_status (self, "Stopping session...");
    append_log (self, "\n-- stop requested --\n");
}

/* ------------------------------------------------------------------ */
/* Prefix management                                                   */
/* ------------------------------------------------------------------ */

static void
on_reset_prefix (GtkWidget *widget, gpointer user_data)
{
    PlWindow     *self = user_data;
    const PlPrefix *prefix = current_prefix (self);
    GtkWidget    *dialog;
    PlRunRequest  req;

    if (prefix == NULL || current_proton (self) == NULL)
        return;

    dialog = gtk_message_dialog_new (
        GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Reset prefix '%s'?\n\n"
        "Windows programs, settings and saved games inside the prefix are "
        "permanently deleted. The prefix is recreated on the next launch.",
        prefix->label);
    gtk_window_set_title (GTK_WINDOW (dialog), "Reset prefix");

    if (gtk_dialog_run (GTK_DIALOG (dialog)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy (dialog);
        return;
    }
    gtk_widget_destroy (dialog);

    memset (&req, 0, sizeof (req));
    req.verb    = PL_VERB_DESTROYPREFIX;
    req.proton  = current_proton (self);
    req.prefix  = prefix;
    req.game_id = self->config->game_id;

    start_request (self, &req);
}

static void
on_new_prefix (GtkWidget *widget, gpointer user_data)
{
    PlWindow  *self = user_data;
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *entry;
    GtkWidget *label;
    GError    *error = NULL;
    char      *name;

    dialog = gtk_dialog_new_with_buttons ("New prefix", GTK_WINDOW (self->window),
                                          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          "_Cancel", GTK_RESPONSE_CANCEL,
                                          "_Create", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 380, -1);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    label = gtk_label_new ("Name for the new prefix:");
    gtk_widget_set_margin_start (label, 12);
    gtk_widget_set_margin_end (label, 12);
    gtk_widget_set_margin_top (label, 12);
    gtk_container_add (GTK_CONTAINER (content), label);

    entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_entry_set_text (GTK_ENTRY (entry), "default");
    gtk_widget_set_margin_start (entry, 12);
    gtk_widget_set_margin_end (entry, 12);
    gtk_widget_set_margin_top (entry, 6);
    gtk_widget_set_margin_bottom (entry, 12);
    gtk_container_add (GTK_CONTAINER (content), entry);

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
        name = pl_prefix_slugify (gtk_entry_get_text (GTK_ENTRY (entry)));

        if (!pl_prefix_create (self->config->prefix_root, name, &error)) {
            GtkWidget *msg = gtk_message_dialog_new (
                GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
                error != NULL ? error->message : "Could not create prefix");
            gtk_dialog_run (GTK_DIALOG (msg));
            gtk_widget_destroy (msg);
            g_clear_error (&error);
        } else {
            /* Point the freshly created prefix at a real Proton build so it
             * gets initialised immediately rather than failing later. */
            const PlProton *proton = current_proton (self);

            if (proton != NULL) {
                /* Re-scan so the new prefix is present in the list, then
                 * run wineboot --init against it. Proton builds the rest of
                 * the prefix on first use; this just makes the initial
                 * creation happen now rather than on the first launch. */
                PlPrefixList *tmp = pl_prefix_list_new ();
                PlRunRequest  req;
                const PlPrefix *fresh;

                pl_prefix_list_scan (tmp, self->config->prefix_root);
                fresh = pl_prefix_find_by_id (tmp, name);

                if (fresh != NULL) {
                    memset (&req, 0, sizeof (req));
                    req.verb    = PL_VERB_RUNINPREFIX;
                    req.proton  = proton;
                    req.prefix  = fresh;
                    req.game_id = self->config->game_id;

                    req.args = g_ptr_array_new ();
                    g_ptr_array_add (req.args, g_strdup ("wineboot"));
                    g_ptr_array_add (req.args, g_strdup ("--init"));

                    start_request (self, &req);
                    g_ptr_array_unref (req.args);
                }

                pl_prefix_list_free (tmp);
            }

            set_status (self, "Created prefix '%s'", name);
        }
        g_free (name);
    }

    gtk_widget_destroy (dialog);
    refresh_prefixes (self);
}

static void
on_delete_prefix (GtkWidget *widget, gpointer user_data)
{
    PlWindow     *self = user_data;
    const PlPrefix *prefix = current_prefix (self);
    GtkWidget    *dialog;
    GError       *error = NULL;

    if (prefix == NULL)
        return;

    dialog = gtk_message_dialog_new (
        GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Delete prefix '%s'?\n\nAll contents are permanently removed.",
        prefix->label);
    gtk_window_set_title (GTK_WINDOW (dialog), "Delete prefix");

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
        if (!pl_prefix_remove (self->config->prefix_root, prefix->id, &error)) {
            GtkWidget *msg = gtk_message_dialog_new (
                GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
                error != NULL ? error->message : "Could not delete prefix");
            gtk_dialog_run (GTK_DIALOG (msg));
            gtk_widget_destroy (msg);
            g_clear_error (&error);
        }
    }

    gtk_widget_destroy (dialog);

    /* The selected prefix may be gone, so re-seed the list and re-run the
     * preflight against whatever is selected now. */
    refresh_prefixes (self);
    refresh_banner (self);
    refresh_actions (self);
}

static void
on_open_prefix_dir (GtkWidget *widget, gpointer user_data)
{
    PlWindow       *self = user_data;
    const PlPrefix *prefix = current_prefix (self);
    GError         *error = NULL;
    char           *uri = NULL;

    if (prefix == NULL)
        return;

    uri = g_filename_to_uri (prefix->path, NULL, &error);
    if (uri == NULL) {
        GtkWidget *msg = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
            error != NULL ? error->message : "Invalid path");
        gtk_dialog_run (GTK_DIALOG (msg));
        gtk_widget_destroy (msg);
        g_clear_error (&error);
        return;
    }

    if (!gtk_show_uri_on_window (GTK_WINDOW (self->window), uri,
                                 GDK_CURRENT_TIME, &error)) {
        GtkWidget *msg = gtk_message_dialog_new (
            GTK_WINDOW (self->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_CLOSE,
            "Could not open a file manager.\n\n%s",
            error != NULL ? error->message : "");
        gtk_dialog_run (GTK_DIALOG (msg));
        gtk_widget_destroy (msg);
        g_clear_error (&error);
    }

    }

/* ------------------------------------------------------------------ */
/* Selection changes                                                   */
/* ------------------------------------------------------------------ */

static void
on_proton_changed (GtkComboBox *combo, gpointer user_data)
{
    PlWindow *self = user_data;
    int       index;

    if (self->loading)
        return;

    index = gtk_combo_box_get_active (combo);
    if (index >= 0 && index < (int) self->protons->items->len) {
        const PlProton *proton = g_ptr_array_index (self->protons->items,
                                                    (guint) index);

        g_free (self->config->proton_version);
        self->config->proton_version = g_strdup (proton->id);
    }

    refresh_banner (self);
    refresh_actions (self);
}

static void
on_reload_clicked (GtkButton *button, gpointer user_data)
{
    PlWindow *self = user_data;

    refresh_protons (self);
    refresh_prefixes (self);
    refresh_banner (self);
    refresh_actions (self);
    set_status (self, "Reloaded");
}

static void
on_exe_browse (GtkButton *button, gpointer user_data)
{
    PlWindow *self = user_data;
    GtkWidget *chooser;

    chooser = gtk_file_chooser_dialog_new (
        "Select an executable", GTK_WINDOW (self->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT, NULL);

    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser),
                                         g_get_home_dir ());

    if (gtk_dialog_run (GTK_DIALOG (chooser)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));

        gtk_entry_set_text (GTK_ENTRY (self->exe_entry), filename);
        g_free (filename);
    }

    gtk_widget_destroy (chooser);
}

/* ------------------------------------------------------------------ */
/* Refresh                                                             */
/* ------------------------------------------------------------------ */

static void
refresh_protons (PlWindow *self)
{
    g_autofree char *preferred = NULL;
    GError         *error = NULL;
    int             active = 0;

    self->loading = TRUE;

    gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (self->proton_combo));

    /* Keep the id as a string: pl_proton_scan() frees every PlProton it
     * replaces, so the pointer resolve_proton() hands back would dangle. */
    {
        const PlProton *chosen = resolve_proton (self);

        if (chosen != NULL)
            preferred = g_strdup (chosen->id);
    }

    pl_proton_scan (self->protons, self->config->proton_dir, &error);

    for (guint i = 0; i < self->protons->items->len; i++) {
        const PlProton *proton = g_ptr_array_index (self->protons->items, i);
        char           *label;

        if (proton->usable)
            label = g_strdup (proton->id);
        else
            label = g_strdup_printf ("%s  (unusable: %s)",
                                     proton->id, proton->problem);

        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->proton_combo),
                                        label);
        g_free (label);

        if (preferred != NULL && g_strcmp0 (proton->id, preferred) == 0)
            active = (int) i;
    }

    if (self->protons->items->len > 0)
        gtk_combo_box_set_active (GTK_COMBO_BOX (self->proton_combo), active);

    self->loading = FALSE;

    if (error != NULL) {
        append_log (self, error->message);
        g_clear_error (&error);
    }
}

static void
refresh_prefixes (PlWindow *self)
{
    char *active_id = NULL;
    int   index = gtk_combo_box_get_active (GTK_COMBO_BOX (self->prefix_combo));

    self->loading = TRUE;

    if (index >= 0 && self->prefixes != NULL &&
        (guint) index < self->prefixes->items->len) {
        const PlPrefix *prefix = g_ptr_array_index (self->prefixes->items,
                                                    (guint) index);
        active_id = g_strdup (prefix->id);
    }

    gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (self->prefix_combo));

    pl_prefix_list_scan (self->prefixes, self->config->prefix_root);

    /* Guarantee at least one entry so the UI is never empty. */
    if (self->prefixes->items->len == 0) {
        if (pl_prefix_create (self->config->prefix_root, "default", NULL))
            pl_prefix_list_scan (self->prefixes, self->config->prefix_root);
    }

    {
        int restore = -1;

        for (guint i = 0; i < self->prefixes->items->len; i++) {
            const PlPrefix *prefix = g_ptr_array_index (self->prefixes->items, i);
            char           *label;

            /* Mark prefixes Proton has not initialised yet, so a fresh one is
             * distinguishable from one that already holds installed games. */
            if (prefix->seeded)
                label = g_strdup (prefix->label);
            else
                label = g_strdup_printf ("%s  (not initialised yet)",
                                         prefix->label);

            gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (self->prefix_combo),
                                            label);
            g_free (label);

            if (active_id != NULL && g_strcmp0 (prefix->id, active_id) == 0)
                restore = (int) i;
        }

        if (restore < 0)
            restore = 0;
        if (self->prefixes->items->len > 0)
            gtk_combo_box_set_active (GTK_COMBO_BOX (self->prefix_combo), restore);
    }

    self->loading = FALSE;
    g_free (active_id);
}

static void
refresh_banner (PlWindow *self)
{
    const PlProton *proton = current_proton (self);
    const PlPrefix *prefix = current_prefix (self);
    char           *summary;

    pl_preflight_free (self->report);
    self->report = pl_preflight_run (proton != NULL ? proton->path : NULL,
                                     prefix != NULL ? prefix->pfx : NULL);

    summary = pl_preflight_summary (self->report);

    if (pl_preflight_can_run (self->report)) {
        gtk_widget_hide (self->banner);
    } else {
        char *text = g_strdup_printf ("%s - open Preflight for details", summary);

        gtk_label_set_text (GTK_LABEL (self->banner_label), text);
        gtk_widget_show (self->banner);
        g_free (text);
    }

    g_free (summary);
}

static void
refresh_actions (PlWindow *self)
{
    gboolean running = pl_runner_is_running (self->runner);

    gtk_widget_set_sensitive (self->run_button, !running);
    gtk_widget_set_sensitive (self->stop_button, running);
    gtk_widget_set_sensitive (GTK_WIDGET (self->tools_menu), !running);
    gtk_widget_set_sensitive (GTK_WIDGET (self->tools_button), !running);

    /* Changing the build or prefix mid-session would not affect the child
     * process, so freeze those controls while it runs. */
    gtk_widget_set_sensitive (self->proton_combo, !running);
    gtk_widget_set_sensitive (self->prefix_combo, !running);
    gtk_widget_set_sensitive (self->wined3d_radio, !running);
}

static void
on_preflight_clicked (GtkButton *button, gpointer user_data)
{
    PlWindow       *self = user_data;
    GtkWidget      *dialog;
    GtkWidget      *content;
    GtkWidget      *box;
    GtkWidget      *scroll;

    if (self->report == NULL)
        refresh_banner (self);

    dialog = gtk_dialog_new_with_buttons ("Preflight", GTK_WINDOW (self->window),
                                          GTK_DIALOG_DESTROY_WITH_PARENT,
                                          "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 620, 420);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    box     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_container_add (GTK_CONTAINER (content), box);

    for (guint i = 0; i < self->report->checks->len; i++) {
        const PlCheck *check = g_ptr_array_index (self->report->checks, i);
        const char     *icon;
        GtkWidget      *row;
        GtkWidget      *text_box;

        switch (check->level) {
        case PL_CHECK_OK:   icon = "emblem-ok-symbolic";        break;
        case PL_CHECK_WARN: icon = "dialog-warning-symbolic";   break;
        default:            icon = "dialog-error-symbolic";     break;
        }

        row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

        {
            GtkWidget *image = gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_BUTTON);

            gtk_widget_set_valign (image, GTK_ALIGN_START);
            gtk_box_pack_start (GTK_BOX (row), image, FALSE, FALSE, 0);
        }

        text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand (text_box, TRUE);

        {
            g_autofree char *text = g_strdup_printf ("%s - %s",
                                                      check->title, check->detail);
            GtkWidget *label = gtk_label_new (text);

            gtk_label_set_xalign (GTK_LABEL (label), 0.0);
            gtk_box_pack_start (GTK_BOX (text_box), label, FALSE, FALSE, 0);
        }

        if (check->remedy != NULL) {
            GtkWidget *label = gtk_label_new (check->remedy);

            gtk_label_set_xalign (GTK_LABEL (label), 0.0);
            gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
            gtk_widget_set_margin_start (label, 2);
            gtk_style_context_add_class (gtk_widget_get_style_context (label),
                                         "dim-label");
            gtk_box_pack_start (GTK_BOX (text_box), label, FALSE, FALSE, 0);
        }

        gtk_box_pack_start (GTK_BOX (row), text_box, TRUE, TRUE, 0);
        gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);
    }

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_container_add (GTK_CONTAINER (scroll), box);
    gtk_widget_show_all (scroll);
    gtk_container_add (GTK_CONTAINER (content), scroll);

    g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);
    gtk_widget_show_all (dialog);
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *
make_button (const char *label, GCallback callback, PlWindow *self)
{
    GtkWidget *button = gtk_button_new_with_label (label);

    g_signal_connect (button, "clicked", callback, self);
    return button;
}

static GtkWidget *
make_menu_item (GtkWidget *menu, const char *label, const char *id,
                GCallback callback, PlWindow *self)
{
    GtkWidget *item = gtk_menu_item_new_with_label (label);

    if (id != NULL)
        g_object_set_data (G_OBJECT (item), "tool-id", (char *) id);
    g_signal_connect (item, "activate", callback, self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    return item;
}

static void
build_ui (PlWindow *self)
{
    GtkWidget   *outer;
    GtkWidget   *grid;
    GtkWidget   *prefix_bar;
    GtkWidget   *run_bar;
    GtkWidget   *renderer_box;
    GtkWidget   *status_box;
    GtkWidget   *sep;
    GtkWidget   *log_label;
    GtkTextTag  *tag;
    GtkStyleContext *ctx;

    self->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title (GTK_WINDOW (self->window), PL_APP_NAME);
    gtk_window_set_default_size (GTK_WINDOW (self->window),
                                 self->config->win_width,
                                 self->config->win_height);
    if (self->config->win_maximized)
        gtk_window_maximize (GTK_WINDOW (self->window));

    g_signal_connect (self->window, "destroy", G_CALLBACK (gtk_main_quit), NULL);

    outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self->window), outer);

    /* ---- Proton / prefix selection bar ---- */
    prefix_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (prefix_bar, 12);
    gtk_widget_set_margin_end (prefix_bar, 12);
    gtk_widget_set_margin_top (prefix_bar, 12);
    gtk_container_add (GTK_CONTAINER (outer), prefix_bar);

    {
        GtkWidget *label = gtk_label_new ("Proton build");

        gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
        gtk_box_pack_start (GTK_BOX (prefix_bar), label, FALSE, FALSE, 0);
    }

    self->proton_combo = gtk_combo_box_text_new ();
    gtk_widget_set_hexpand (self->proton_combo, TRUE);
    g_signal_connect (self->proton_combo, "changed",
                      G_CALLBACK (on_proton_changed), self);
    gtk_box_pack_start (GTK_BOX (prefix_bar), self->proton_combo, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (prefix_bar),
                        make_button ("Reload", G_CALLBACK (on_reload_clicked), self),
                        FALSE, FALSE, 0);

    {
        GtkWidget *label = gtk_label_new ("Prefix");

        gtk_widget_set_margin_start (label, 12);
        gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
        gtk_box_pack_start (GTK_BOX (prefix_bar), label, FALSE, FALSE, 0);
    }

    self->prefix_combo = gtk_combo_box_text_new ();
    gtk_widget_set_hexpand (self->prefix_combo, TRUE);
    gtk_box_pack_start (GTK_BOX (prefix_bar), self->prefix_combo, TRUE, TRUE, 0);

    self->prefix_menu = gtk_menu_new ();
    {
        GtkWidget *button = gtk_menu_button_new ();

        gtk_button_set_label (GTK_BUTTON (button), "Prefix");
        gtk_menu_button_set_popup (GTK_MENU_BUTTON (button),
                                   GTK_WIDGET (self->prefix_menu));

        make_menu_item (self->prefix_menu, "New prefix...", NULL,
                        G_CALLBACK (on_new_prefix), self);
        make_menu_item (self->prefix_menu, "Reset prefix", NULL,
                        G_CALLBACK (on_reset_prefix), self);
        make_menu_item (self->prefix_menu, "Delete prefix", NULL,
                        G_CALLBACK (on_delete_prefix), self);
        gtk_menu_shell_append (GTK_MENU_SHELL (self->prefix_menu),
                               gtk_separator_menu_item_new ());
        make_menu_item (self->prefix_menu, "Open prefix folder", NULL,
                        G_CALLBACK (on_open_prefix_dir), self);

        gtk_box_pack_start (GTK_BOX (prefix_bar), button, FALSE, FALSE, 0);
    }

    /* ---- Executable + renderer ---- */
    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
    gtk_widget_set_margin_start (grid, 12);
    gtk_widget_set_margin_end (grid, 12);
    gtk_widget_set_margin_top (grid, 12);
    gtk_container_add (GTK_CONTAINER (outer), grid);

    {
        GtkWidget *label = gtk_label_new ("Executable");

        gtk_widget_set_halign (label, GTK_ALIGN_START);
        gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
    }

    self->exe_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->exe_entry),
                                    "/path/to/game.exe");
    gtk_widget_set_hexpand (self->exe_entry, TRUE);
    gtk_grid_attach (GTK_GRID (grid), self->exe_entry, 1, 0, 1, 1);

    gtk_grid_attach (GTK_GRID (grid),
                     make_button ("Browse...", G_CALLBACK (on_exe_browse), self),
                     2, 0, 1, 1);

    {
        GtkWidget *label = gtk_label_new ("Renderer");

        gtk_widget_set_halign (label, GTK_ALIGN_START);
        gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
    }

    renderer_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);

    self->wined3d_radio = gtk_check_button_new_with_mnemonic ("_WineD3D (OpenGL)");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->wined3d_radio),
                                  self->config->wined3d_default);
    gtk_box_pack_start (GTK_BOX (renderer_box), self->wined3d_radio, FALSE, FALSE, 0);

    {
        GtkWidget *hint = gtk_label_new ("Off = DXVK / vkd3d-Proton (Vulkan)");

        gtk_style_context_add_class (gtk_widget_get_style_context (hint),
                                     "dim-label");
        gtk_widget_set_valign (hint, GTK_ALIGN_CENTER);
        gtk_box_pack_start (GTK_BOX (renderer_box), hint, FALSE, FALSE, 0);
    }

    gtk_widget_set_hexpand (renderer_box, TRUE);
    gtk_grid_attach (GTK_GRID (grid), renderer_box, 1, 1, 2, 1);

    /* ---- Action bar ---- */
    run_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start (run_bar, 12);
    gtk_widget_set_margin_end (run_bar, 12);
    gtk_widget_set_margin_top (run_bar, 12);
    gtk_box_pack_start (GTK_BOX (outer), run_bar, FALSE, FALSE, 0);

    self->run_button = gtk_button_new_with_mnemonic ("_Run");
    gtk_widget_set_can_default (self->run_button, TRUE);
    g_signal_connect (self->run_button, "clicked",
                      G_CALLBACK (on_run_clicked), self);
    gtk_box_pack_start (GTK_BOX (run_bar), self->run_button, FALSE, FALSE, 0);

    self->stop_button = gtk_button_new_with_mnemonic ("_Stop");
    gtk_widget_set_sensitive (self->stop_button, FALSE);
    g_signal_connect (self->stop_button, "clicked",
                      G_CALLBACK (on_stop_clicked), self);
    gtk_box_pack_start (GTK_BOX (run_bar), self->stop_button, FALSE, FALSE, 0);

    self->tools_menu = gtk_menu_new ();
    {
        GtkWidget *button = gtk_menu_button_new ();

        gtk_button_set_label (GTK_BUTTON (button), "Wine tools");
        gtk_menu_button_set_popup (GTK_MENU_BUTTON (button),
                                   GTK_WIDGET (self->tools_menu));

        for (gsize i = 0; i < G_N_ELEMENTS (wine_tools); i++) {
            const WineTool *tool = &wine_tools[i];

            if (tool->id == NULL)
                gtk_menu_shell_append (GTK_MENU_SHELL (self->tools_menu),
                                       gtk_separator_menu_item_new ());
            else
                make_menu_item (self->tools_menu, tool->label, tool->id,
                                G_CALLBACK (on_tool_clicked), self);
        }

        /* The attached menu is not one of the button's container children, so
         * the gtk_widget_show_all() at the end of build_ui() never reaches it.
         * Left unshown, every item stays hidden, the menu reports a natural
         * size of 0x0, and clicking the button pops up an invisible nothing.
         * Show it here instead. */
        gtk_widget_show_all (GTK_WIDGET (self->tools_menu));

        self->tools_button = button;
        gtk_box_pack_start (GTK_BOX (run_bar), button, FALSE, FALSE, 0);
    }

    gtk_widget_set_hexpand (run_bar, TRUE);

    status_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    self->status_label = gtk_label_new ("Ready");
    gtk_label_set_ellipsize (GTK_LABEL (self->status_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign (self->status_label, GTK_ALIGN_END);
    gtk_box_pack_start (GTK_BOX (status_box), self->status_label, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (run_bar),
                        make_button ("Preflight", G_CALLBACK (on_preflight_clicked), self),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (run_bar),
                        make_button ("Preferences", G_CALLBACK (pl_prefs_dialog_show), self),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer), status_box, FALSE, FALSE, 0);

    /* ---- Output log ---- */
    sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (outer), sep, FALSE, FALSE, 0);

    log_label = gtk_label_new ("Output");
    gtk_widget_set_halign (log_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start (log_label, 12);
    gtk_widget_set_margin_top (log_label, 6);
    gtk_box_pack_start (GTK_BOX (outer), log_label, FALSE, FALSE, 0);

    self->log_view = gtk_text_view_new ();
    gtk_text_view_set_editable (GTK_TEXT_VIEW (self->log_view), FALSE);
    gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (self->log_view), FALSE);
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->log_view), TRUE);
    gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (self->log_view),
                                 GTK_WRAP_WORD_CHAR);
    ctx = gtk_widget_get_style_context (self->log_view);
    gtk_style_context_add_class (ctx, "pl-log");

    self->log_buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->log_view));
    tag = gtk_text_buffer_create_tag (self->log_buffer, "output",
                                      "family", "monospace", NULL);
    (void) tag;

    {
        GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);

        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add (GTK_CONTAINER (scroll), self->log_view);
        gtk_widget_set_vexpand (scroll, TRUE);
        gtk_widget_set_margin_top (scroll, 6);
        gtk_box_pack_start (GTK_BOX (outer), scroll, TRUE, TRUE, 0);
    }

    /* ---- Preflight banner ---- */
    self->banner = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    ctx = gtk_widget_get_style_context (self->banner);
    gtk_style_context_add_class (ctx, "pl-banner");
    self->banner_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (self->banner_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (self->banner_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start (GTK_BOX (self->banner), self->banner_label, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (outer), self->banner, FALSE, FALSE, 0);

    gtk_widget_show_all (self->window);
    gtk_widget_hide (self->banner);
}

/* ------------------------------------------------------------------ */

static void
on_window_state (GtkWidget *widget, GdkEventWindowState *event, gpointer data)
{
    PlWindow *self = data;

    if ((event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED) != 0)
        self->config->win_maximized =
            (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
}

static void
on_window_destroy (GtkWidget *widget, gpointer data)
{
    PlWindow *self = data;
    GError   *error = NULL;

    if (pl_runner_is_running (self->runner))
        pl_runner_cancel (self->runner);

    pl_config_save (self->config, self->config_file, &error);
    g_clear_error (&error);
}

GtkWidget *
pl_window_root (PlWindow *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->window;
}

PlConfig *
pl_window_config (PlWindow *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->config;
}

char *
pl_window_config_file (PlWindow *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->config_file;
}

/* Re-run discovery after the user changes paths or the prefix set. */
void
pl_window_reload (PlWindow *self)
{
    g_return_if_fail (self != NULL);

    refresh_protons (self);
    refresh_prefixes (self);
    refresh_banner (self);
    refresh_actions (self);
}

void
pl_window_set_exe (PlWindow *self, const char *path)
{
    g_return_if_fail (self != NULL);

    if (path != NULL)
        gtk_entry_set_text (GTK_ENTRY (self->exe_entry), path);
}

void
pl_window_free (PlWindow *self)
{
    if (self == NULL)
        return;

    pl_runner_free (self->runner);
    pl_proton_list_free (self->protons);
    pl_prefix_list_free (self->prefixes);
    pl_preflight_free (self->report);
    pl_config_free (self->config);
    g_free (self->config_file);
    g_free (self);
}

PlWindow *
pl_window_new (const char *config_file)
{
    PlWindow *self = g_new0 (PlWindow, 1);

    self->config      = pl_config_new ();
    self->config_file = (config_file != NULL) ? g_strdup (config_file)
                                              : pl_config_default_path ();
    self->protons = pl_proton_list_new ();
    self->prefixes = pl_prefix_list_new ();
    self->runner  = pl_runner_new ();
    self->report  = NULL;

    {
        GError *error = NULL;

        if (!pl_config_load (self->config, self->config_file, &error)) {
            g_printerr ("proton-launcher: %s\n",
                        error != NULL ? error->message : "config load failed");
            g_clear_error (&error);
        }
    }

    build_ui (self);

    g_signal_connect (self->window, "window-state-event",
                      G_CALLBACK (on_window_state), self);
    g_signal_connect (self->window, "destroy",
                      G_CALLBACK (on_window_destroy), self);

    refresh_protons (self);
    refresh_prefixes (self);
    refresh_banner (self);
    refresh_actions (self);

    if (self->protons->items->len == 0) {
        append_log (self,
            "No Proton builds found.\n\nOpen Preferences to set the directory "
            "that contains your GE-Proton folders, for example\n"
            "~/.steam/steam/compatibilitytools.d\n");
    }

    /* First run: ask for the paths instead of assuming them. */
    if (!self->config->first_run_done) {
        pl_wizard_run (self);

        /* The wizard may have written new paths. */
        refresh_protons (self);
        refresh_prefixes (self);
        refresh_banner (self);
        refresh_actions (self);
    }

    return self;
}

void
pl_window_run (PlWindow *self)
{
    g_return_if_fail (self != NULL);
    gtk_main ();
}