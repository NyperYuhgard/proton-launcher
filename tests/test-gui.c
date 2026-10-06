/* test-gui.c - Smoke test for Proton Launcher's GTK3 interface.
 *
 * There is no display-independent way to unit test GTK, so this drives the
 * real widgets inside a real main loop. It exists because the dialogs cache
 * state across open/close cycles, which is exactly where a use-after-free
 * hides: the Preferences dialog is kept alive between clicks, so if it is
 * destroyed by the window manager and the cached pointer is not cleared, the
 * second click reuses freed widgets.
 *
 * The steps mirror what a user does:
 *   1. open Preferences
 *   2. let it take down (as a window manager would) and open it again
 *   3. change the paths and press Apply
 *   4. close the main window and check the config was persisted
 *
 * Run with: make check-gui   (skips itself when there is no display)
 */

#include "pl.h"
#include "ui.h"
#include "ui_private.h"

#include <gtk/gtk.h>

#include <glib/gstdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tiny assertion harness                                              */
/* ------------------------------------------------------------------ */

static guint n_checks;
static guint n_failed;

#define CHECK(expr, ...)                                                \
    G_STMT_START {                                                      \
        n_checks++;                                                     \
        if (!(expr)) {                                                  \
            n_failed++;                                                 \
            g_printerr ("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_printerr ("       ");                                    \
            g_printerr (__VA_ARGS__);                                   \
            g_printerr ("\n");                                          \
        }                                                               \
    } G_STMT_END

/* ------------------------------------------------------------------ */
/* Widget lookup helpers                                               */
/* ------------------------------------------------------------------ */

/* Depth-first search for the first widget matching 'predicate'. GTK keeps
 * its children in a plain list, so this stays cheap for our small trees. */
static GtkWidget *
find_widget (GtkWidget *root, gboolean (*predicate) (GtkWidget *))
{
    GList     *children;
    GList     *it;
    GtkWidget *found = NULL;

    if (root == NULL)
        return NULL;

    if (predicate (root))
        return root;

    if (!GTK_IS_CONTAINER (root))
        return NULL;

    children = gtk_container_get_children (GTK_CONTAINER (root));

    for (it = children; it != NULL; it = it->next) {
        found = find_widget (it->data, predicate);

        if (found != NULL)
            break;
    }

    g_list_free (children);
    return found;
}

static gboolean
is_button_named (GtkWidget *widget)
{
    const char *label;

    if (!GTK_IS_BUTTON (widget))
        return FALSE;

    label = gtk_button_get_label (GTK_BUTTON (widget));

    return label != NULL && g_strcmp0 (label, "Preferences") == 0;
}

static gboolean
is_button_named_apply (GtkWidget *widget)
{
    if (!GTK_IS_BUTTON (widget))
        return FALSE;

    return g_strcmp0 (gtk_button_get_label (GTK_BUTTON (widget)),
                      "Apply") == 0 ||
           g_strcmp0 (gtk_button_get_label (GTK_BUTTON (widget)),
                      "_Apply") == 0;
}

/* Collect every GtkEntry in the tree, in creation order. The path rows are
 * built in a fixed order, so index 1 is the prefix root. */
static void
collect_entries (GtkWidget *widget, GList **out)
{
    GList *children;
    GList *it;

    if (GTK_IS_ENTRY (widget)) {
        *out = g_list_prepend (*out, widget);
        return;
    }

    if (!GTK_IS_CONTAINER (widget))
        return;

    children = gtk_container_get_children (GTK_CONTAINER (widget));

    for (it = children; it != NULL; it = it->next)
        collect_entries (it->data, out);

    g_list_free (children);
}

static GList *
find_all_entries (GtkWidget *root)
{
    GList *out = NULL;

    collect_entries (root, &out);
    return g_list_reverse (out);
}

/* First toplevel window carrying 'title', visible or not. */
static GtkWidget *
find_toplevel (const char *title)
{
    GList     *toplevels = gtk_window_list_toplevels ();
    GList     *it;
    GtkWidget *found = NULL;

    for (it = toplevels; it != NULL; it = it->next) {
        GtkWidget  *widget = it->data;
        const char *t;

        if (!GTK_IS_WINDOW (widget))
            continue;

        t = gtk_window_get_title (GTK_WINDOW (widget));

        if (t != NULL && g_strcmp0 (t, title) == 0) {
            found = widget;
            break;
        }
    }

    g_list_free (toplevels);
    return found;
}

/* How many visible toplevel windows carry 'title'. */
static guint
count_dialogs (const char *title)
{
    GList *toplevels = gtk_window_list_toplevels ();
    GList *it;
    guint  n = 0;

    for (it = toplevels; it != NULL; it = it->next) {
        GtkWidget  *widget = it->data;
        const char *t;

        if (!GTK_IS_WINDOW (widget))
            continue;

        t = gtk_window_get_title (GTK_WINDOW (widget));

        if (t != NULL && g_strcmp0 (t, title) == 0 &&
            gtk_widget_get_visible (widget))
            n++;
    }

    g_list_free (toplevels);
    return n;
}

/* The "Wine tools" menu button, or NULL. */
static GtkWidget *
find_tools_button (GtkWidget *widget)
{
    if (GTK_IS_MENU_BUTTON (widget)) {
        if (g_strcmp0 (gtk_button_get_label (GTK_BUTTON (widget)),
                       "Wine tools") == 0)
            return widget;
    }

    if (GTK_IS_CONTAINER (widget)) {
        GList     *children = gtk_container_get_children (GTK_CONTAINER (widget));
        GList     *it;
        GtkWidget *found = NULL;

        for (it = children; it != NULL && found == NULL; it = it->next)
            found = find_tools_button (it->data);

        g_list_free (children);
        return found;
    }

    return NULL;
}

/* The GtkMenu attached to the "Wine tools" menu button, or NULL. */
static GtkMenu *
find_tools_menu (GtkWidget *widget)
{
    GtkWidget *button = find_tools_button (widget);

    return button != NULL ? gtk_menu_button_get_popup (GTK_MENU_BUTTON (button))
                          : NULL;
}

/* Assert that the Wine tools menu is populated and that every expected tool
 * is reachable by id. The menu is built from a table, so a bad entry would
 * otherwise only show up as a menu item that does nothing. */
static void
check_tools_menu (GtkWidget *root)
{
    static const char *expected[] = {
        "winecfg", "regedit", "control",
        "taskmgr", "msinfo32",
        "explorer", "winefile", "notepad", "cmd",
        "msiexec", "uninstaller",
        "wineboot-init", "wineboot-update",
        NULL
    };
    GtkMenu   *menu = find_tools_menu (root);
    GList     *children;
    GList     *it;
    guint      n_items = 0;
    guint      n_seps  = 0;

    CHECK (menu != NULL, "the Wine tools menu exists");

    if (menu == NULL)
        return;

    children = gtk_container_get_children (GTK_CONTAINER (menu));

    for (it = children; it != NULL; it = it->next) {
        GtkWidget *child = it->data;

        if (GTK_IS_SEPARATOR_MENU_ITEM (child)) {
            n_seps++;
            continue;
        }

        if (!GTK_IS_MENU_ITEM (child))
            continue;

        n_items++;
        CHECK (g_object_get_data (G_OBJECT (child), "tool-id") != NULL,
               "every Wine tool entry carries a tool id");
        CHECK (gtk_menu_item_get_label (GTK_MENU_ITEM (child)) != NULL,
               "every Wine tool entry carries a label");
    }

    CHECK (n_items == G_N_ELEMENTS (expected) - 1,
           "the menu has %u entries, expected %u", n_items,
           (guint) G_N_ELEMENTS (expected) - 1);
    CHECK (n_seps > 0, "the menu groups its entries with separators");

    for (gsize i = 0; expected[i] != NULL; i++) {
        gboolean found = FALSE;

        for (it = children; it != NULL; it = it->next) {
            GtkWidget *child = it->data;

            if (GTK_IS_MENU_ITEM (child) &&
                g_strcmp0 (g_object_get_data (G_OBJECT (child), "tool-id"),
                           expected[i]) == 0) {
                found = TRUE;
                break;
            }
        }

        CHECK (found, "the menu offers '%s'", expected[i]);
    }

    g_list_free (children);

    /* The attached menu is not one of the button's container children, so
     * gtk_widget_show_all() on the window never reaches it. If the items are
     * left hidden the menu reports a natural size of 0x0 and clicking the
     * button pops up nothing at all -- which looks exactly like a button that
     * has no options. So require every entry to actually be visible. */
    children = gtk_container_get_children (GTK_CONTAINER (menu));

    for (it = children; it != NULL; it = it->next) {
        if (!GTK_IS_MENU_ITEM (it->data))
            continue;

        CHECK (gtk_widget_get_visible (it->data),
               "entry '%s' is visible, not just present",
               g_object_get_data (G_OBJECT (it->data), "tool-id"));
    }

    g_list_free (children);

    /* End to end: pop the menu up and require it to occupy a real on-screen
     * area. A 0x0 popup is the reported bug. */
    {
        GtkWidget *button = find_tools_button (root);
        gint       w = 0, h = 0;

        gtk_menu_popup_at_widget (menu, button, GDK_GRAVITY_SOUTH_WEST,
                                  GDK_GRAVITY_NORTH_WEST, NULL);

        while (gtk_events_pending ())
            gtk_main_iteration ();

        w = gtk_widget_get_allocated_width (GTK_WIDGET (menu));
        h = gtk_widget_get_allocated_height (GTK_WIDGET (menu));

        CHECK (gtk_widget_get_mapped (GTK_WIDGET (menu)),
               "the Wine tools menu maps when popped up");
        CHECK (w > 0 && h > 0,
               "the popped-up menu has a real size (%dx%d)", w, h);

        gtk_menu_popdown (menu);

        while (gtk_events_pending ())
            gtk_main_iteration ();
    }
}

/* ------------------------------------------------------------------ */
/* The scenario                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    PlWindow *window;
    char     *root;
    char     *config_file;
    int       step;
    int       timeout_hit;
} Scenario;

static void
step_failure (Scenario *sc, const char *what)
{
    n_failed++;
    g_printerr ("  FAIL step %d: %s\n", sc->step, what);
    gtk_main_quit ();
}

static gboolean
advance (gpointer user_data)
{
    Scenario *sc = user_data;
    GtkWidget *root = pl_window_root (sc->window);

    if (++sc->timeout_hit > 200) {
        step_failure (sc, "timed out waiting for the UI to settle");
        return G_SOURCE_REMOVE;
    }

    switch (sc->step) {
    case 0: {
        GtkWidget *button = find_widget (root, is_button_named);

        check_tools_menu (root);

        n_checks++;
        if (button == NULL) {
            n_failed++;
            g_printerr ("  FAIL: no Preferences button in the main window\n");
            break;
        }

        g_signal_emit_by_name (button, "clicked");
        sc->step = 1;
        return G_SOURCE_CONTINUE;
    }

    case 1: {
        guint n = count_dialogs ("Preferences");

        if (n != 1)
            break;

        n_checks++;
        CHECK (count_dialogs ("Preferences") == 1,
               "the Preferences dialog opened");

        /* Take the dialog down the way a window manager would: destroy it
         * outright rather than sending a response. */
        {
            GtkWidget *dialog = find_toplevel ("Preferences");

            CHECK (dialog != NULL, "found the Preferences dialog to destroy");

            if (dialog != NULL)
                gtk_widget_destroy (dialog);
        }

        sc->step = 2;
        return G_SOURCE_CONTINUE;
    }

    case 2: {
        if (count_dialogs ("Preferences") != 0)
            break;

        CHECK (count_dialogs ("Preferences") == 0,
               "the dialog was destroyed");

        /* Second click: the dialog must be rebuilt from scratch, not reuse
         * the widgets that were just freed. */
        {
            GtkWidget *button = find_widget (root, is_button_named);

            if (button == NULL) {
                step_failure (sc, "Preferences button disappeared");
                break;
            }

            g_signal_emit_by_name (button, "clicked");
        }

        sc->step = 3;
        return G_SOURCE_CONTINUE;
    }

    case 3: {
        if (count_dialogs ("Preferences") != 1)
            break;

        n_checks++;
        CHECK (count_dialogs ("Preferences") == 1,
               "reopening after a destroy rebuilt the dialog");

        /* Apply a new prefix root through the real widgets, so the parsing
         * and persistence path is exercised rather than stubbed. */
        {
            GtkWidget *dialog = find_toplevel ("Preferences");

            if (dialog == NULL) {
                step_failure (sc, "dialog vanished between steps");
                break;
            }

            /* Entries are created in order: Proton builds, Prefixes, Logs. */
            {
                GList *entries = find_all_entries (dialog);

                CHECK (g_list_length (entries) >= 3,
                       "the dialog has at least three path entries (got %u)",
                       g_list_length (entries));

                if (g_list_length (entries) >= 3) {
                    GtkWidget *prefixes = g_list_nth_data (entries, 1);

                    gtk_entry_set_text (GTK_ENTRY (prefixes), sc->root);
                }

                g_list_free (entries);

                {
                    GtkWidget *apply = find_widget (dialog, is_button_named_apply);

                    CHECK (apply != NULL, "the dialog has an Apply button");

                    if (apply != NULL)
                        g_signal_emit_by_name (apply, "clicked");
                }
            }
        }

        sc->step = 4;
        return G_SOURCE_CONTINUE;
    }

    case 4: {
        if (count_dialogs ("Preferences") != 0)
            break;

        n_checks++;
        CHECK (count_dialogs ("Preferences") == 0,
               "Apply closed the dialog");

        CHECK (g_strcmp0 (pl_window_config (sc->window)->prefix_root, sc->root) == 0,
               "Apply updated the prefix root in memory (got '%s')",
               pl_window_config (sc->window)->prefix_root);

        gtk_widget_destroy (root);
        sc->step = 5;
        return G_SOURCE_CONTINUE;
    }

    case 5: {
        PlConfig *reloaded = pl_config_new ();
        gboolean  ok;

        ok = pl_config_load (reloaded, sc->config_file, NULL);

        CHECK (ok, "the config file could be read back");

        if (ok) {
            CHECK (g_strcmp0 (reloaded->prefix_root, sc->root) == 0,
                   "the prefix root was persisted (got '%s')",
                   reloaded->prefix_root);
            CHECK (reloaded->first_run_done,
                   "first-run-done was persisted, so the wizard stays away");
        }

        pl_config_free (reloaded);
        gtk_main_quit ();
        return G_SOURCE_REMOVE;
    }

    default:
        gtk_main_quit ();
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

int
main (int argc, char *argv[])
{
    Scenario  sc = { 0 };
    char     *proton_dir;
    char     *prefix_root;
    char     *log_dir;
    char     *ini;
    GError   *error = NULL;
    guint     source;

    if (!gtk_init_check (&argc, &argv)) {
        g_print ("# no display available, skipping the GUI smoke test\n");
        return 0;
    }

    g_print ("# GUI smoke test\n");

    sc.root = g_dir_make_tmp ("proton-launcher-gui-XXXXXX", &error);

    if (sc.root == NULL) {
        g_printerr ("cannot create a temporary directory: %s\n",
                    error != NULL ? error->message : "?");
        g_clear_error (&error);
        return 1;
    }

    proton_dir = g_build_filename (sc.root, "protons", NULL);
    prefix_root = g_build_filename (sc.root, "prefixes", NULL);
    log_dir = g_build_filename (sc.root, "logs", NULL);
    sc.config_file = g_build_filename (sc.root, "config.ini", NULL);

    g_mkdir_with_parents (proton_dir, 0755);

    /* A Proton build the preflight will reject (it is not a real script), so
     * the banner shows and the preflight path gets exercised too. */
    {
        char *build = g_build_filename (proton_dir, "GE-ProtonTest-9", NULL);
        char *script = g_build_filename (build, "proton", NULL);
        char *version = g_build_filename (build, "version", NULL);

        g_mkdir_with_parents (build, 0755);
        g_file_set_contents (script, "#!/bin/sh\nexit 0\n", -1, NULL);
        g_file_set_contents (version, "GE-ProtonTest-9\n", -1, NULL);
        g_chmod (script, 0755);

        g_free (version);
        g_free (script);
        g_free (build);
    }

    ini = g_strdup_printf (
        "[paths]\n"
        "proton-dir=%s\n"
        "prefix-root=%s\n"
        "log-dir=%s\n"
        "\n"
        "[proton]\n"
        "version=GE-ProtonTest-9\n"
        "game-id=umu-default\n"
        "backend=proton\n"
        "wined3d-default=false\n"
        "\n"
        "[logging]\n"
        "proton-log=true\n"
        "\n"
        "[ui]\n"
        "first-run-done=true\n"
        "window-width=900\n"
        "window-height=720\n"
        "window-maximized=false\n",
        proton_dir, prefix_root, log_dir);
    g_file_set_contents (sc.config_file, ini, -1, NULL);
    g_free (ini);

    sc.window = pl_window_new (sc.config_file);
    sc.step = 0;

    source = g_timeout_add (20, advance, &sc);
    (void) source;

    gtk_main ();

    pl_window_free (sc.window);

    {
        /* Remove the whole tree, depth first. */
        GDir *dir = g_dir_open (sc.root, 0, NULL);

        if (dir != NULL) {
            const char *name;

            while ((name = g_dir_read_name (dir)) != NULL) {
                char *child = g_build_filename (sc.root, name, NULL);

                /* The fixture is shallow by construction. */
                g_remove (child);
                g_free (child);
            }

            g_dir_close (dir);
        }

        g_rmdir (sc.root);
    }

    g_free (log_dir);
    g_free (prefix_root);
    g_free (proton_dir);
    g_free (sc.config_file);
    g_free (sc.root);

    g_print ("# %u checks, %u failed\n", n_checks, n_failed);
    return n_failed == 0 ? 0 : 1;
}