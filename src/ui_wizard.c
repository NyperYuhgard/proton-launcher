/* ui_wizard.c - First-run setup.
 *
 * Every path is a question rather than an assumption, so a user with
 * Proton on an external drive or a shared installation lands in the same
 * working setup as someone using the default locations.
 */

#include "ui_private.h"

#include <gtk/gtk.h>
#include <unistd.h>
#include <string.h>

typedef struct {
    PlWindow *window;
    PlConfig *config;
    char     *config_file;

    GtkWidget *dialog;
    GtkWidget *proton_entry;
    GtkWidget *prefix_entry;
    GtkWidget *log_entry;
    GtkWidget *status_label;
    GtkWidget *next_button;
    GtkWidget *back_button;
    GtkWidget *finish_button;

    GtkWidget *pages[3];
    int        current_page;
} Wizard;

static void refresh_validation (Wizard *self);

/* ------------------------------------------------------------------ */

static void
on_pick_dir (GtkButton *button, gpointer user_data)
{
    GtkWidget *entry   = GTK_WIDGET (g_object_get_data (G_OBJECT (button), "entry"));
    GtkWidget *chooser;
    char      *current = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));

    if (g_file_test (current, G_FILE_TEST_IS_DIR)) {
        char *parent = g_path_get_dirname (current);

        chooser = gtk_file_chooser_dialog_new (
            "Select the Proton folder", GTK_WINDOW (entry),
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser), parent);
        g_free (parent);
    } else {
        chooser = gtk_file_chooser_dialog_new (
            "Select the Proton folder", GTK_WINDOW (entry),
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
            "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (chooser),
                                             g_get_home_dir ());
    }

    if (gtk_dialog_run (GTK_DIALOG (chooser)) == GTK_RESPONSE_ACCEPT) {
        char *folder = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));

        gtk_entry_set_text (GTK_ENTRY (entry), folder);
        g_free (folder);
    }

    gtk_widget_destroy (chooser);
    g_free (current);
}

/* Count usable Proton builds in 'dir'. Returns -1 when unreadable. */
static int
count_proton_builds (const char *dir)
{
    GDir       *d;
    const char *name;
    int         count = 0;

    d = g_dir_open (dir, 0, NULL);
    if (d == NULL)
        return -1;

    while ((name = g_dir_read_name (d)) != NULL) {
        char    *script;
        gboolean usable;

        if (!g_str_has_prefix (name, "GE-Proton") &&
            !g_str_has_prefix (name, "Proton-")  &&
            !g_str_has_prefix (name, "Proton "))
            continue;

        script = g_build_filename (dir, name, "proton", NULL);
        usable = g_file_test (script, G_FILE_TEST_EXISTS) &&
                 access (script, X_OK) == 0;
        g_free (script);

        if (usable)
            count++;
    }
    g_dir_close (d);

    return count;
}

static void
refresh_validation (Wizard *self)
{
    const char *proton_dir  = gtk_entry_get_text (GTK_ENTRY (self->proton_entry));
    const char *prefix_root = gtk_entry_get_text (GTK_ENTRY (self->prefix_entry));
    int         count;
    char       *message;

    switch (self->current_page) {
    case 0:
        count = count_proton_builds (proton_dir);

        if (count < 0) {   /* unreadable directory */
            message = g_strdup_printf (
                "That folder cannot be read:\n%s\n\n"
                "Select the directory that contains your GE-Proton folders, "
                "for example ~/.steam/steam/compatibilitytools.d",
                proton_dir);
            gtk_label_set_text (GTK_LABEL (self->status_label), message);
            g_free (message);
            gtk_widget_set_sensitive (self->next_button, FALSE);
            return;
        }

        if (count == 0) {
            message = g_strdup_printf (
                "No Proton build found in:\n%s\n\nExpected a folder such as "
                "GE-Proton10-4 containing an executable 'proton' script.\n\n"
                "Download a release from "
                "github.com/GloriousEggroll/proton-ge-custom first.",
                proton_dir);
            gtk_label_set_text (GTK_LABEL (self->status_label), message);
            g_free (message);
            gtk_widget_set_sensitive (self->next_button, FALSE);
            return;
        }

        message = g_strdup_printf ("%d Proton build%s found.",
                                   count, count == 1 ? "" : "s");
        gtk_label_set_text (GTK_LABEL (self->status_label), message);
        g_free (message);
        gtk_widget_set_sensitive (self->next_button, TRUE);
        return;

    case 1:
        if (prefix_root == NULL || *prefix_root == '\0') {
            gtk_label_set_text (GTK_LABEL (self->status_label),
                                "Choose where prefixes should be stored.");
            gtk_widget_set_sensitive (self->next_button, FALSE);
            return;
        }

        message = g_strdup_printf ("Prefixes will be created in:\n%s", prefix_root);
        gtk_label_set_text (GTK_LABEL (self->status_label), message);
        g_free (message);
        gtk_widget_set_sensitive (self->next_button, TRUE);
        return;

    default:
        gtk_widget_set_sensitive (self->finish_button, TRUE);
        return;
    }
}

static void
show_page (Wizard *self, int page)
{
    for (int i = 0; i < 3; i++) {
        if (i == page)
            gtk_widget_show (self->pages[i]);
        else
            gtk_widget_hide (self->pages[i]);
    }

    self->current_page = page;

    gtk_widget_set_sensitive (self->back_button, page > 0);
    gtk_widget_set_sensitive (self->finish_button, page == 2);
    gtk_widget_set_sensitive (self->next_button, page < 2);

    refresh_validation (self);
}

static void
on_back (GtkButton *button, gpointer user_data)
{
    Wizard *self = user_data;

    if (self->current_page > 0)
        show_page (self, self->current_page - 1);
}

static void
on_next (GtkButton *button, gpointer user_data)
{
    Wizard *self = user_data;

    if (self->current_page < 2)
        show_page (self, self->current_page + 1);
}

/* Commit the wizard's answers and continue into the main window. */
static void
on_finish (GtkButton *button, gpointer user_data)
{
    Wizard  *self = user_data;
    PlConfig *config = self->config;
    const char *proton_dir;
    const char *prefix_root;
    const char *log_dir;
    GError    *error = NULL;

    proton_dir  = gtk_entry_get_text (GTK_ENTRY (self->proton_entry));
    prefix_root = gtk_entry_get_text (GTK_ENTRY (self->prefix_entry));
    log_dir     = gtk_entry_get_text (GTK_ENTRY (self->log_entry));

    g_mkdir_with_parents (prefix_root, 0755);
    if (log_dir != NULL && *log_dir != '\0')
        g_mkdir_with_parents (log_dir, 0755);

    g_free (config->proton_dir);
    config->proton_dir = g_strdup (proton_dir);

    g_free (config->prefix_root);
    config->prefix_root = g_strdup (prefix_root);

    g_free (config->log_dir);
    config->log_dir = (*log_dir != '\0') ? g_strdup (log_dir) : NULL;

    config->first_run_done = TRUE;

    if (!pl_config_save (config, self->config_file, &error)) {
        GtkWidget *dialog = gtk_message_dialog_new (
            GTK_WINDOW (self->dialog), GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not save settings:\n%s",
            error != NULL ? error->message : "unknown error");
        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);
        g_clear_error (&error);
        return;
    }

    pl_window_reload (self->window);
    gtk_widget_destroy (self->dialog);
}

static void
on_entry_changed (GtkEditable *editable, gpointer user_data)
{
    refresh_validation (user_data);
}

/* Fill the Proton entry with a well-known location. */
static void
on_preset_chosen (GtkComboBox *combo, gpointer user_data)
{
    Wizard *self = user_data;
    char   *preset = NULL;

    switch (gtk_combo_box_get_active (combo)) {
    case 0:
        preset = g_build_filename (g_get_home_dir (), ".steam", "steam",
                                   "compatibilitytools.d", NULL);
        break;
    case 1:
        preset = g_strdup (g_get_home_dir ());
        break;
    case 2:
        preset = g_build_filename (g_get_home_dir (), ".var", "app",
                                   "com.valvesoftware.Steam", "data", "Steam",
                                   "compatibilitytools.d", NULL);
        break;
    default:
        return;
    }

    gtk_entry_set_text (GTK_ENTRY (self->proton_entry), preset);
    g_free (preset);
    gtk_combo_box_set_active (combo, -1);
}

static void
on_response (GtkDialog *dialog, gint response, gpointer user_data)
{
    Wizard *self = user_data;

    /* on_finish destroys the dialog itself, so only handle the paths that
     * leave it alive: Cancel and the window-manager close button. Leaving
     * it alive would hand GTK a destroyed user_data pointer. */
    if (response == GTK_RESPONSE_CANCEL ||
        response == GTK_RESPONSE_DELETE_EVENT)
        gtk_widget_destroy (GTK_WIDGET (dialog));
    else
        on_finish (NULL, self);
}

/* ------------------------------------------------------------------ */

static GtkWidget *
make_page (void)
{
    GtkWidget *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);

    gtk_widget_set_margin_start (page, 12);
    gtk_widget_set_margin_end (page, 12);
    gtk_widget_set_margin_top (page, 12);
    return page;
}

static GtkWidget *
make_intro_page (void)
{
    GtkWidget *page  = make_page ();
    GtkWidget *title = gtk_label_new ("Welcome to Proton Launcher");
    GtkWidget *body;

    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (title), "title-4");
    gtk_box_pack_start (GTK_BOX (page), title, FALSE, FALSE, 0);

    body = gtk_label_new (
        "This launcher runs Proton builds directly on Linux, without Steam.\n\n"
        "You will need:\n"
        "  1. A Proton build already downloaded and extracted, such as\n"
        "     GE-Proton from github.com/GloriousEggroll/proton-ge-custom\n"
        "  2. Python 3 with the 'filelock' module, which current Proton\n"
        "     builds require at startup\n\n"
        "The next steps let you point the launcher at both locations. "
        "Nothing is installed or modified outside them.");

    gtk_label_set_xalign (GTK_LABEL (body), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (body), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (body), 62);
    gtk_box_pack_start (GTK_BOX (page), body, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *
make_proton_page (Wizard *self)
{
    GtkWidget *page  = make_page ();
    GtkWidget *title = gtk_label_new ("Where are your Proton builds?");
    GtkWidget *body;
    GtkWidget *row;
    GtkWidget *button;
    GtkWidget *combo;

    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (title), "heading");
    gtk_box_pack_start (GTK_BOX (page), title, FALSE, FALSE, 0);

    body = gtk_label_new (
        "Select the folder that CONTAINS your Proton folders, not the Proton "
        "folder itself.\nFor example, if you extracted a release you should "
        "pick the directory holding GE-Proton10-4.");
    gtk_label_set_xalign (GTK_LABEL (body), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (body), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (body), 62);
    gtk_box_pack_start (GTK_BOX (page), body, FALSE, FALSE, 0);

    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    self->proton_entry = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (self->proton_entry), self->config->proton_dir);
    gtk_widget_set_hexpand (self->proton_entry, TRUE);
    g_signal_connect (self->proton_entry, "changed",
                      G_CALLBACK (on_entry_changed), self);
    gtk_box_pack_start (GTK_BOX (row), self->proton_entry, TRUE, TRUE, 0);

    button = gtk_button_new_with_label ("Browse...");
    g_object_set_data (G_OBJECT (button), "entry", self->proton_entry);
    g_signal_connect (button, "clicked", G_CALLBACK (on_pick_dir), NULL);
    gtk_box_pack_start (GTK_BOX (row), button, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (page), row, FALSE, FALSE, 0);

    /* Offer the common locations so most people do not need the browser. */
    combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "steam",
                               "Steam compatibility tools");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "home",
                               "Home folder");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), "flatpak",
                               "Steam Flatpak");
    g_signal_connect (combo, "changed", G_CALLBACK (on_preset_chosen), self);
    gtk_box_pack_start (GTK_BOX (page), combo, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *
make_folders_page (Wizard *self)
{
    GtkWidget *page  = make_page ();
    GtkWidget *title = gtk_label_new ("Where should your data live?");
    GtkWidget *body;
    struct { const char *caption; GtkWidget **entry; const char *value; } rows[] = {
        { "Prefixes", &self->prefix_entry, NULL },
        { "Proton logs", &self->log_entry, NULL },
    };

    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_style_context_add_class (gtk_widget_get_style_context (title), "heading");
    gtk_box_pack_start (GTK_BOX (page), title, FALSE, FALSE, 0);

    body = gtk_label_new (
        "A prefix is a self-contained Windows installation holding a game's "
        "files, settings and saves. Keep one prefix per game so they do not "
        "interfere with each other.\n\nBoth folders are created if they do not "
        "exist, and you can change them later in Preferences.");
    gtk_label_set_xalign (GTK_LABEL (body), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (body), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (body), 62);
    gtk_box_pack_start (GTK_BOX (page), body, FALSE, FALSE, 0);

    for (gsize i = 0; i < G_N_ELEMENTS (rows); i++) {
        GtkWidget *row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new (rows[i].caption);
        GtkWidget *button = gtk_button_new_with_label ("Browse...");
        const char *value = rows[i].value != NULL ? rows[i].value
                          : (i == 0 ? self->config->prefix_root
                                    : self->config->log_dir);

        gtk_widget_set_size_request (label, 110, -1);
        gtk_label_set_xalign (GTK_LABEL (label), 0.0);
        gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
        gtk_box_pack_start (GTK_BOX (row), label, FALSE, FALSE, 0);

        *rows[i].entry = gtk_entry_new ();
        gtk_entry_set_text (GTK_ENTRY (*rows[i].entry), value);
        gtk_widget_set_hexpand (*rows[i].entry, TRUE);
        g_signal_connect (*rows[i].entry, "changed",
                          G_CALLBACK (on_entry_changed), self);
        gtk_box_pack_start (GTK_BOX (row), *rows[i].entry, TRUE, TRUE, 0);

        g_object_set_data (G_OBJECT (button), "entry", *rows[i].entry);
        g_signal_connect (button, "clicked", G_CALLBACK (on_pick_dir), NULL);
        gtk_box_pack_start (GTK_BOX (row), button, FALSE, FALSE, 0);

        gtk_box_pack_start (GTK_BOX (page), row, FALSE, FALSE, 0);
    }

    return page;
}

/* ------------------------------------------------------------------ */

gboolean
pl_wizard_run (PlWindow *self)
{
    Wizard *wiz;
    GtkWidget *content;
    GtkWidget *nav;
    GtkWidget *frame;

    if (self == NULL)
        return FALSE;

    wiz = g_new0 (Wizard, 1);
    wiz->window        = self;
    wiz->config        = pl_window_config (self);
    wiz->config_file   = pl_window_config_file (self);
    wiz->current_page  = 0;

    wiz->dialog = gtk_dialog_new_with_buttons ("Proton Launcher setup",
                                               GTK_WINDOW (pl_window_root (self)),
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "_Cancel", GTK_RESPONSE_CANCEL,
                                               NULL, NULL);
    gtk_window_set_default_size (GTK_WINDOW (wiz->dialog), 640, 460);

    content = gtk_dialog_get_content_area (GTK_DIALOG (wiz->dialog));

    frame = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (frame, 12);
    gtk_widget_set_margin_end (frame, 12);
    gtk_widget_set_margin_top (frame, 12);
    gtk_widget_set_margin_bottom (frame, 12);
    gtk_container_add (GTK_CONTAINER (content), frame);

    wiz->pages[0] = make_intro_page ();
    wiz->pages[1] = make_proton_page (wiz);
    wiz->pages[2] = make_folders_page (wiz);

    for (int i = 0; i < 3; i++)
        gtk_box_pack_start (GTK_BOX (frame), wiz->pages[i], TRUE, TRUE, 0);

    wiz->status_label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (wiz->status_label), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (wiz->status_label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (wiz->status_label), 66);
    gtk_style_context_add_class (gtk_widget_get_style_context (wiz->status_label),
                                 "dim-label");
    gtk_box_pack_start (GTK_BOX (frame), wiz->status_label, FALSE, FALSE, 0);

    nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top (nav, 6);
    wiz->back_button = gtk_button_new_with_label ("Back");
    g_signal_connect (wiz->back_button, "clicked", G_CALLBACK (on_back), wiz);
    gtk_box_pack_start (GTK_BOX (nav), wiz->back_button, FALSE, FALSE, 0);

    gtk_widget_set_hexpand (nav, TRUE);

    wiz->next_button = gtk_button_new_with_label ("Next");
    gtk_widget_set_can_default (wiz->next_button, TRUE);
    g_signal_connect (wiz->next_button, "clicked", G_CALLBACK (on_next), wiz);
    gtk_box_pack_start (GTK_BOX (nav), wiz->next_button, FALSE, FALSE, 0);

    wiz->finish_button = gtk_button_new_with_label ("Finish");
    gtk_widget_set_can_default (wiz->finish_button, TRUE);
    g_signal_connect (wiz->finish_button, "clicked", G_CALLBACK (on_finish), wiz);
    gtk_box_pack_start (GTK_BOX (nav), wiz->finish_button, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (frame), nav, FALSE, FALSE, 0);

    g_signal_connect (wiz->dialog, "response", G_CALLBACK (on_response), wiz);

    gtk_widget_show_all (wiz->dialog);
    show_page (wiz, 0);

    /* Modal: the main window stays inert until the paths are settled. */
    gtk_dialog_run (GTK_DIALOG (wiz->dialog));

    g_free (wiz);
    return TRUE;
}