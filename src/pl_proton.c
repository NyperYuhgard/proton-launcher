/* pl_proton.c - Discovery and validation of Proton builds. */

#include "pl.h"

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */

static void
proton_free (PlProton *proton)
{
    if (proton == NULL)
        return;
    g_free (proton->id);
    g_free (proton->path);
    g_free (proton->version);
    g_free (proton->problem);
    g_free (proton);
}

PlProtonList *
pl_proton_list_new (void)
{
    PlProtonList *self = g_new0 (PlProtonList, 1);

    self->items = g_ptr_array_new_with_free_func ((GDestroyNotify) proton_free);
    return self;
}

void
pl_proton_list_clear (PlProtonList *self)
{
    g_return_if_fail (self != NULL);
    if (self->items != NULL)
        g_ptr_array_set_size (self->items, 0);
}

void
pl_proton_list_free (PlProtonList *self)
{
    if (self == NULL)
        return;
    if (self->items != NULL)
        g_ptr_array_unref (self->items);
    g_free (self);
}

const PlProton *
pl_proton_find (PlProtonList *self, const char *id)
{
    guint i;

    if (self == NULL || id == NULL)
        return NULL;

    for (i = 0; i < self->items->len; i++) {
        const PlProton *proton = g_ptr_array_index (self->items, i);

        if (g_strcmp0 (proton->id, id) == 0)
            return proton;
    }
    return NULL;
}

const PlProton *
pl_proton_latest (PlProtonList *self)
{
    guint i;

    if (self == NULL || self->items->len == 0)
        return NULL;

    for (i = 0; i < self->items->len; i++) {
        const PlProton *proton = g_ptr_array_index (self->items, i);

        if (proton->usable)
            return proton;
    }
    /* Nothing validated: fall back to the first entry. */
    return g_ptr_array_index (self->items, 0);
}

/* ------------------------------------------------------------------ */

/* Read the first line of 'file', trimmed. Returns NULL when unreadable. */
static char *
read_first_line (const char *file)
{
    char *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents (file, &contents, &length, NULL))
        return NULL;

    if (length == 0) {
        g_free (contents);
        return NULL;
    }

    /* Tolerate CRLF so builds packaged on Windows still parse. */
    contents[strcspn (contents, "\r\n")] = '\0';
    if (*contents == '\0') {
        g_free (contents);
        return NULL;
    }
    return contents;
}

/* Split a version-ish string into its numeric components, so that
 * "GE-Proton9-20" yields {"9","20"} and compares above "GE-Proton9-4". */
static char **
split_digits (const char *text)
{
    GPtrArray  *out = g_ptr_array_new ();
    const char *p = text;

    while (*p != '\0') {
        const char *start;

        if (!g_ascii_isdigit (*p)) {
            p++;
            continue;
        }

        start = p;
        while (g_ascii_isdigit (*p))
            p++;

        g_ptr_array_add (out, g_strndup (start, (gsize) (p - start)));
    }

    g_ptr_array_add (out, NULL);
    return (char **) g_ptr_array_free (out, FALSE);
}

/* Compare "GE-Proton10-4" against "GE-Proton9-4" by numeric components.
 * Element by element, then by length, then lexically for stability. */
static int
compare_versions (const char *a, const char *b)
{
    g_auto(GStrv) pa = split_digits (a);
    g_auto(GStrv) pb = split_digits (b);
    int i;

    for (i = 0; pa[i] != NULL && pb[i] != NULL; i++) {
        gint64 va = g_ascii_strtoll (pa[i], NULL, 10);
        gint64 vb = g_ascii_strtoll (pb[i], NULL, 10);

        if (va != vb)
            return (va < vb) ? -1 : 1;
    }

    if (pa[i] != NULL)
        return 1;   /* more components: newer */
    if (pb[i] != NULL)
        return -1;

    return g_strcmp0 (a, b);
}

static int
compare_protons (gconstpointer a, gconstpointer b)
{
    const PlProton *pa = *(const PlProton *const *) a;
    const PlProton *pb = *(const PlProton *const *) b;

    return compare_versions (pb->id, pa->id);
}

gboolean
pl_proton_scan (PlProtonList *self, const char *dir, GError **error)
{
    GDir           *d;
    const char     *name;
    GError         *local = NULL;
    char           *real_dir = NULL;
    guint           found = 0;

    g_return_val_if_fail (self != NULL, FALSE);

    pl_proton_list_clear (self);

    if (dir == NULL || *dir == '\0') {
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                             "No Proton directory configured");
        return FALSE;
    }

    /* Follow a symlinked Proton directory (a common setup). */
    {
        char *resolved = realpath (dir, NULL);

        real_dir = (resolved != NULL) ? resolved : g_strdup (dir);
    }

    if (!g_file_test (real_dir, G_FILE_TEST_IS_DIR)) {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                     "Proton directory does not exist: %s", real_dir);
        g_free (real_dir);
        return FALSE;
    }

    d = g_dir_open (real_dir, 0, &local);
    if (d == NULL) {
        g_propagate_error (error, local);
        g_free (real_dir);
        return FALSE;
    }

    while ((name = g_dir_read_name (d)) != NULL) {
        char       *child;
        PlProton   *proton;
        char       *script;
        char       *version_file;

        /* Match the conventional naming so unrelated folders in the same
         * directory are never mistaken for a runtime. */
        if (!g_str_has_prefix (name, "GE-Proton") &&
            !g_str_has_prefix (name, "Proton-")  &&
            !g_str_has_prefix (name, "Proton "))
            continue;

        child = g_build_filename (real_dir, name, NULL);
        if (!g_file_test (child, G_FILE_TEST_IS_DIR)) {
            g_free (child);
            continue;
        }

        proton = g_new0 (PlProton, 1);
        proton->id   = g_strdup (name);
        proton->path = child;

        script = g_build_filename (child, "proton", NULL);
        if (!g_file_test (script, G_FILE_TEST_EXISTS)) {
            proton->usable  = FALSE;
            proton->problem = g_strdup ("no 'proton' executable in this directory");
        } else if (g_file_test (script, G_FILE_TEST_IS_DIR)) {
            proton->usable  = FALSE;
            proton->problem = g_strdup ("'proton' is a directory, not a script");
        } else if (g_access (script, X_OK) != 0) {
            /* Proton is always launched as an executable, so a script that
             * is not +x can never work regardless of its interpreter. */
            proton->usable  = FALSE;
            proton->problem = g_strdup ("'proton' is not executable "
                                        "(chmod +x required)");
        } else {
            proton->usable = TRUE;
        }
        g_free (script);

        /* Prefer the version file the build ships; fall back to the id. */
        version_file = g_build_filename (child, "version", NULL);
        proton->version = read_first_line (version_file);
        g_free (version_file);
        if (proton->version == NULL)
            proton->version = g_strdup (proton->id);

        g_ptr_array_add (self->items, proton);
        found++;
    }
    g_dir_close (d);
    g_free (real_dir);

    if (found == 0) {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                     "No Proton builds found in %s. Expected a folder such as "
                     "GE-Proton10-4.", dir);
        return FALSE;
    }

    g_ptr_array_sort (self->items, compare_protons);
    return TRUE;
}