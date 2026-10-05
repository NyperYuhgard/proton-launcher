/* ui_prefs.c - Preferences dialog: every path is user-selectable.
 *
 * Nothing here is hardcoded to $HOME: the Proton directory, the prefix
 * root and the log directory can each point anywhere writable, which is
 * what makes the launcher usable with external drives or shared
 * installations.
 */

#include "ui_private.h"

#include <gtk/gtk.h>
#include <unistd.h>
#include <string.h>

typedef struct {
    PlWindow    *window;
    PlConfig    *config;
    char        *config_file;

    GtkWidget   *dialog;
    GtkWidget   *proton_entry;
    GtkWidget   *prefix_entry;
    GtkWidget   *log_entry;
    GtkWidget   *game_id_entry;
    GtkWidget   *wined3d_check;
    GtkWidget   *proton_log_check;
    GtkWidget   *env_view;
    GtkTextBuffer *env_buffer;
} PrefsDialog;

/* ------------------------------------------------------------------ */

/* A folder chooser that starts in the current value's parent. */
static GtkWidget *
make_dir_chooser (GtkWidget *parent, const char *title, const char *current)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new (
        title, GTK_WINDOW (parent), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);

    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser),
                                         current != NULL && *current != '\0'
                                         ? current : g_get_home_dir ());
    return chooser;
}

static void
on_pick_dir (GtkButton *button, gpointer user_data)
{
    GtkWidget *entry = GTK_WIDGET (g_object_get_data (G_OBJECT (button), "entry"));
    GtkWidget *chooser;
    char      *current;

    current = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));

    /* Start from the parent so the current folder stays selectable. */
    if (g_file_test (current, G_FILE_TEST_IS_DIR)) {
        char *parent_dir = g_path_get_dirname (current);

        chooser = make_dir_chooser (gtk_widget_get_toplevel (entry),
                                   "Select folder", parent_dir);
        g_free (parent_dir);
    } else {
        chooser = make_dir_chooser (gtk_widget_get_toplevel (entry),
                                   "Select folder", current);
    }

    if (gtk_dialog_run (GTK_DIALOG (chooser)) == GTK_RESPONSE_ACCEPT) {
        char *folder = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));

        gtk_entry_set_text (GTK_ENTRY (entry), folder);
        g_free (folder);
    }

    gtk_widget_destroy (chooser);
    g_free (current);
}

/* Build a labelled row: caption on the left, entry, then Browse. */
static GtkWidget *
path_row (const char *caption, const char *initial, GtkWidget **entry_out)
{
    GtkWidget *row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new (caption);
    GtkWidget *entry = gtk_entry_new ();
    GtkWidget *button = gtk_button_new_with_label ("Browse...");

    gtk_widget_set_size_request (label, 130, -1);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_valign (label, GTK_ALIGN_CENTER);

    gtk_entry_set_text (GTK_ENTRY (entry),
                        initial != NULL ? initial : "");
    gtk_widget_set_hexpand (entry, TRUE);

    g_object_set_data (G_OBJECT (button), "entry", entry);
    g_signal_connect (button, "clicked", G_CALLBACK (on_pick_dir), NULL);

    gtk_box_pack_start (GTK_BOX (row), label, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (row), button, FALSE, FALSE, 0);

    if (entry_out != NULL)
        *entry_out = entry;

    return row;
}

static void
show_error (GtkWidget *parent, const char *format, ...) G_GNUC_PRINTF (2, 3);

static void
show_error (GtkWidget *parent, const char *format, ...)
{
    va_list args;
    char   *text;
    GtkWidget *dialog;

    va_start (args, format);
    text = g_strdup_vprintf (format, args);
    va_end (args);

    dialog = gtk_message_dialog_new (GTK_WINDOW (parent), GTK_DIALOG_MODAL,
                                     GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", text);
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
    g_free (text);
}

/* Verify the chosen Proton directory before accepting it, so the user
 * gets an explanation rather than an empty combo box later. Sets
 * '*explanation' to a newly allocated message whenever it returns FALSE. */
static gboolean
validate_proton_dir (const char *dir, char **explanation)
{
    GDir       *d;
    const char *name;
    gboolean    found = FALSE;

    if (dir == NULL || *dir == '\0') {
        *explanation = g_strdup ("No folder selected.");
        return FALSE;
    }

    if (!g_file_test (dir, G_FILE_TEST_IS_DIR)) {
        *explanation = g_strdup_printf ("Not a directory:\n%s", dir);
        return FALSE;
    }

    d = g_dir_open (dir, 0, NULL);
    if (d == NULL) {
        *explanation = g_strdup_printf ("Cannot read directory:\n%s", dir);
        return FALSE;
    }

    while ((name = g_dir_read_name (d)) != NULL) {
        char *child;
        char *script;
        gboolean usable;

        if (!g_str_has_prefix (name, "GE-Proton") &&
            !g_str_has_prefix (name, "Proton-")  &&
            !g_str_has_prefix (name, "Proton "))
            continue;

        child  = g_build_filename (dir, name, NULL);
        script = g_build_filename (child, "proton", NULL);
        usable = g_file_test (script, G_FILE_TEST_EXISTS) &&
                 access (script, X_OK) == 0;

        g_free (script);
        g_free (child);

        if (usable) {
            found = TRUE;
            break;
        }
    }
    g_dir_close (d);

    if (!found) {
        *explanation = g_strdup_printf (
            "No usable Proton build in:\n%s\n\n"
            "Expected a folder such as GE-Proton10-4 containing an "
            "executable 'proton' script.", dir);
        return FALSE;
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */

static void
apply_prefs (PrefsDialog *self, GtkWidget *dialog)
{
    PlConfig   *config = self->config;
    const char *proton_dir;
    const char *prefix_root;
    const char *log_dir;
    const char *game_id;
    g_autoptr(GError) error = NULL;
    char       *explanation = NULL;
    g_autofree char *env_text = NULL;
    const char *cursor;

    proton_dir  = gtk_entry_get_text (GTK_ENTRY (self->proton_entry));
    prefix_root = gtk_entry_get_text (GTK_ENTRY (self->prefix_entry));
    log_dir     = gtk_entry_get_text (GTK_ENTRY (self->log_entry));
    game_id     = gtk_entry_get_text (GTK_ENTRY (self->game_id_entry));

    if (proton_dir == NULL || *proton_dir == '\0') {
        show_error (dialog, "Choose the folder that contains your Proton builds.");
        return;
    }
    if (prefix_root == NULL || *prefix_root == '\0') {
        show_error (dialog, "Choose where prefixes should be stored.");
        return;
    }

    if (!validate_proton_dir (proton_dir, &explanation)) {
        show_error (dialog, "%s", explanation);
        return;
    }

    /* The prefix root must be creatable before we commit to it. */
    if (g_mkdir_with_parents (prefix_root, 0755) != 0) {
        show_error (dialog, "Cannot create prefix folder:\n%s", prefix_root);
        return;
    }

    if (log_dir != NULL && *log_dir != '\0')
        g_mkdir_with_parents (log_dir, 0755);

    g_free (config->proton_dir);
    config->proton_dir = g_strdup (proton_dir);

    g_free (config->prefix_root);
    config->prefix_root = g_strdup (prefix_root);

    g_free (config->log_dir);
    config->log_dir = (log_dir != NULL && *log_dir != '\0')
                      ? g_strdup (log_dir) : NULL;

    g_free (config->game_id);
    config->game_id = (*game_id != '\0') ? g_strdup (game_id)
                                         : g_strdup ("umu-default");

    config->wined3d_default =
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->wined3d_check));

    config->proton_log =
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->proton_log_check));

    /* Parse "KEY=VALUE" lines from the environment editor. */
    g_ptr_array_set_size (config->extra_env, 0);
    {
        GtkTextIter start, end;

        gtk_text_buffer_get_bounds (self->env_buffer, &start, &end);
        env_text = gtk_text_buffer_get_text (self->env_buffer, &start, &end, FALSE);
    }

    cursor = env_text;
    while (cursor != NULL && *cursor != '\0') {
        g_autofree char *line = NULL;
        const char     *newline = strchr (cursor, '\n');
        char           *mutable_newline = (char *) newline;

        if (newline != NULL)
            *mutable_newline = '\0';

        line = g_strstrip (g_strdup (cursor));

        /* Skip blanks and comments; ignore malformed entries. */
        if (*line != '\0' && *line != '#' && strchr (line, '=') != NULL)
            g_ptr_array_add (config->extra_env, g_strdup (line));

        if (newline == NULL)
            break;
        cursor = newline + 1;
    }

    if (!pl_config_save (config, self->config_file, &error)) {
        show_error (dialog, "Could not save settings:\n%s", error->message);
        return;
    }

    gtk_widget_hide (dialog);
    pl_window_reload (self->window);
}

static void
on_prefs_response (GtkDialog *dialog, gint response, gpointer user_data)
{
    PrefsDialog *self = user_data;

    if (response == GTK_RESPONSE_APPLY)
        apply_prefs (self, GTK_WIDGET (dialog));
    else
        gtk_widget_hide (GTK_WIDGET (dialog));
}

static void
on_prefs_destroy (GtkWidget *widget, gpointer user_data)
{
    PrefsDialog *self = user_data;

    /* The dialog is transient, so the window manager or a parent destroy can
     * take it down at any time. Clear the cached pointer, otherwise the next
     * Preferences click would reuse a widget that no longer exists. */
    self->dialog = NULL;
}

/* ------------------------------------------------------------------ */

/* Create the dialog on first use and keep it, so reopening Preferences
 * restores what the user typed last time instead of discarding it. */
static GtkWidget *
build_prefs_dialog (PrefsDialog *self)
{
    PlWindow *window = self->window;
    GtkWidget *content;
    GtkWidget *box;
    GtkWidget *section;
    GtkWidget *note;
    GtkWidget *scroll;

    self->dialog = gtk_dialog_new_with_buttons (
        "Preferences", GTK_WINDOW (pl_window_root (window)),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply", GTK_RESPONSE_APPLY, NULL);
    gtk_window_set_default_size (GTK_WINDOW (self->dialog), 720, 620);

    content = gtk_dialog_get_content_area (GTK_DIALOG (self->dialog));
    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (box, 12);
    gtk_widget_set_margin_end (box, 12);
    gtk_widget_set_margin_top (box, 12);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_container_add (GTK_CONTAINER (content), box);

    /* ---- Folders ---- */
    section = gtk_label_new ("Folders");
    gtk_label_set_xalign (GTK_LABEL (section), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (section),
                                 "heading");
    gtk_box_pack_start (GTK_BOX (box), section, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (box),
        path_row ("Proton builds", self->config->proton_dir,
                  &self->proton_entry), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box),
        path_row ("Prefixes", self->config->prefix_root,
                  &self->prefix_entry), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box),
        path_row ("Proton logs", self->config->log_dir,
                  &self->log_entry), FALSE, FALSE, 0);

    /* ---- Runtime ---- */
    section = gtk_label_new ("Runtime");
    gtk_label_set_xalign (GTK_LABEL (section), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (section),
                                 "heading");
    gtk_box_pack_start (GTK_BOX (box), section, FALSE, FALSE, 0);

    {
        GtkWidget *row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new ("Game ID");

        gtk_widget_set_size_request (label, 130, -1);
        gtk_label_set_xalign (GTK_LABEL (label), 0.0);
        gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
        gtk_box_pack_start (GTK_BOX (row), label, FALSE, FALSE, 0);

        self->game_id_entry = gtk_entry_new ();
        gtk_entry_set_text (GTK_ENTRY (self->game_id_entry),
                            self->config->game_id);
        gtk_box_pack_start (GTK_BOX (row), self->game_id_entry, TRUE, TRUE, 0);

        gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);
    }

    note = gtk_label_new (
        "Proton is always invoked directly, as 'proton run <exe>' with "
        "UMU_ID set. That is the only supported way to use GE-Proton "
        "outside Steam, so there is nothing to configure here.");
    gtk_label_set_xalign (GTK_LABEL (note), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (note), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (note), 64);
    gtk_style_context_add_class (gtk_widget_get_style_context (note),
                                 "dim-label");
    gtk_box_pack_start (GTK_BOX (box), note, FALSE, FALSE, 0);

    /* ---- Options ---- */
    section = gtk_label_new ("Options");
    gtk_label_set_xalign (GTK_LABEL (section), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (section),
                                 "heading");
    gtk_box_pack_start (GTK_BOX (box), section, FALSE, FALSE, 0);

    self->wined3d_check =
        gtk_check_button_new_with_mnemonic ("Use _WineD3D (OpenGL) by default");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->wined3d_check),
                                  self->config->wined3d_default);
    gtk_box_pack_start (GTK_BOX (box), self->wined3d_check, FALSE, FALSE, 0);

    self->proton_log_check =
        gtk_check_button_new_with_mnemonic ("Enable Proton debug _logging");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->proton_log_check),
                                  self->config->proton_log);
    gtk_box_pack_start (GTK_BOX (box), self->proton_log_check, FALSE, FALSE, 0);

    {
        GtkWidget *hint = gtk_label_new (
            "Logging writes a steam-0.log into the log folder. Verbose, but "
            "required when reporting a launch problem.");

        gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (hint), 64);
        gtk_style_context_add_class (gtk_widget_get_style_context (hint),
                                     "dim-label");
        gtk_box_pack_start (GTK_BOX (box), hint, FALSE, FALSE, 0);
    }

    /* ---- Environment overrides ---- */
    section = gtk_label_new ("Environment overrides");
    gtk_label_set_xalign (GTK_LABEL (section), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (section),
                                 "heading");
    gtk_box_pack_start (GTK_BOX (box), section, FALSE, FALSE, 0);

    {
        GtkWidget *hint = gtk_label_new (
            "One KEY=VALUE per line, passed through to Proton. For example:\n"
            "  PROTON_USE_NTSYNC=0        disable NTSync\n"
            "  MANGOHUD=1                 enable MangoHud\n"
            "  DXVK_HUD=1                 show the DXVK overlay");

        gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
        gtk_label_set_max_width_chars (GTK_LABEL (hint), 64);
        gtk_style_context_add_class (gtk_widget_get_style_context (hint),
                                     "dim-label");
        gtk_box_pack_start (GTK_BOX (box), hint, FALSE, FALSE, 0);
    }

    self->env_view = gtk_text_view_new ();
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (self->env_view), TRUE);
    self->env_buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (self->env_view));

    {
        GString *initial = g_string_new (NULL);

        for (guint i = 0; i < self->config->extra_env->len; i++) {
            g_string_append (initial,
                             g_ptr_array_index (self->config->extra_env, i));
            g_string_append_c (initial, '\n');
        }
        gtk_text_buffer_set_text (self->env_buffer, initial->str, -1);
        g_string_free (initial, TRUE);
    }

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll),
                                         GTK_SHADOW_IN);
    gtk_widget_set_size_request (scroll, -1, 130);
    gtk_container_add (GTK_CONTAINER (scroll), self->env_view);
    gtk_box_pack_start (GTK_BOX (box), scroll, TRUE, TRUE, 0);

    g_signal_connect (self->dialog, "response",
                      G_CALLBACK (on_prefs_response), self);
    g_signal_connect (self->dialog, "destroy",
                      G_CALLBACK (on_prefs_destroy), self);

    return self->dialog;
}

void
pl_prefs_dialog_show (GtkButton *button, gpointer user_data)
{
    /* Static, so the dialog and its widget state survive being closed. */
    static PrefsDialog prefs;
    PlWindow *self = user_data;

    prefs.window      = self;
    prefs.config      = pl_window_config (self);
    prefs.config_file = pl_window_config_file (self);

    if (prefs.dialog == NULL) {
        build_prefs_dialog (&prefs);
    } else {
        /* Re-showing after Cancel must not silently apply stale edits, so
         * reset the fields to what is actually saved. */
        gtk_entry_set_text (GTK_ENTRY (prefs.proton_entry),
                            prefs.config->proton_dir);
        gtk_entry_set_text (GTK_ENTRY (prefs.prefix_entry),
                            prefs.config->prefix_root);
        gtk_entry_set_text (GTK_ENTRY (prefs.log_entry),
                            prefs.config->log_dir);
        gtk_entry_set_text (GTK_ENTRY (prefs.game_id_entry),
                            prefs.config->game_id);
    }

    gtk_widget_show_all (prefs.dialog);
}
