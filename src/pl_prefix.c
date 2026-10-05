/* pl_prefix.c - Wine prefix management.
 *
 * A prefix is a directory <root>/<id> that Proton fills with pfx/ on first
 * use. The directory itself is what STEAM_COMPAT_DATA_PATH must point at.
 */

#include "pl.h"

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */

static void
prefix_free (PlPrefix *prefix)
{
    if (prefix == NULL)
        return;
    g_free (prefix->id);
    g_free (prefix->path);
    g_free (prefix->pfx);
    g_free (prefix->label);
    g_free (prefix);
}

PlPrefixList *
pl_prefix_list_new (void)
{
    PlPrefixList *self = g_new0 (PlPrefixList, 1);

    self->items = g_ptr_array_new_with_free_func ((GDestroyNotify) prefix_free);
    return self;
}

void
pl_prefix_list_free (PlPrefixList *self)
{
    if (self == NULL)
        return;
    if (self->items != NULL)
        g_ptr_array_unref (self->items);
    g_free (self);
}

char *
pl_prefix_slugify (const char *label)
{
    GString *out;
    const char *p;
    char    *result;

    if (label == NULL)
        return g_strdup ("default");

    /* Trim surrounding whitespace before slugifying. */
    while (*label != '\0' && g_ascii_isspace (*label))
        label++;
    {
        const char *end = label + strlen (label);

        while (end > label && g_ascii_isspace (end[-1]))
            end--;
        out = g_string_new_len (label, (gssize) (end - label));
    }

    /* Append into a second buffer: iterating over out->str while appending
     * to it would re-read the characters just written. */
    {
        GString *slug = g_string_new (NULL);

        for (p = out->str; *p != '\0'; p++) {
            char c = *p;

            if (g_ascii_isalnum (c) || c == '.' || c == '-' || c == '_')
                g_string_append_c (slug, c);
            else
                g_string_append_c (slug, '_');
        }

        if (slug->len == 0) {
            g_string_free (slug, TRUE);
            g_string_free (out, TRUE);
            return g_strdup ("default");
        }

        result = g_string_free (slug, FALSE);
    }

    g_string_free (out, TRUE);
    return result;
}

void
pl_prefix_list_scan (PlPrefixList *self, const char *root)
{
    GDir       *d;
    const char *name;
    GError     *local = NULL;

    g_return_if_fail (self != NULL);

    g_ptr_array_set_size (self->items, 0);

    if (root == NULL || *root == '\0')
        return;

    if (!g_file_test (root, G_FILE_TEST_IS_DIR))
        return;

    d = g_dir_open (root, 0, &local);
    if (d == NULL) {
        g_clear_error (&local);
        return;
    }

    while ((name = g_dir_read_name (d)) != NULL) {
        char      *path;
        char      *pfx;
        char      *version_file;
        PlPrefix  *prefix;
        GStatBuf   st;

        if (g_str_has_prefix (name, "."))
            continue;

        path = g_build_filename (root, name, NULL);
        if (g_stat (path, &st) != 0 || !S_ISDIR (st.st_mode)) {
            g_free (path);
            continue;
        }

        pfx = g_build_filename (path, "pfx", NULL);

        prefix          = g_new0 (PlPrefix, 1);
        prefix->id      = g_strdup (name);
        prefix->path    = path;             /* owns */
        prefix->pfx     = pfx;
        prefix->label   = g_strdup (name);

        version_file = g_build_filename (path, "version", NULL);
        prefix->seeded = g_file_test (version_file, G_FILE_TEST_EXISTS);
        g_free (version_file);

        g_ptr_array_add (self->items, prefix);
    }
    g_dir_close (d);
}

const PlPrefix *
pl_prefix_find_by_id (PlPrefixList *self, const char *id)
{
    guint i;

    if (self == NULL || id == NULL)
        return NULL;

    for (i = 0; i < self->items->len; i++) {
        const PlPrefix *prefix = g_ptr_array_index (self->items, i);

        if (g_strcmp0 (prefix->id, id) == 0)
            return prefix;
    }
    return NULL;
}

const PlPrefix *
pl_prefix_latest (PlPrefixList *self)
{
    const PlPrefix *best = NULL;
    GStatBuf        best_st = { 0 };
    gboolean        have_best = FALSE;

    if (self == NULL)
        return NULL;

    for (guint i = 0; i < self->items->len; i++) {
        const PlPrefix *prefix = g_ptr_array_index (self->items, i);
        GStatBuf        st;

        if (g_stat (prefix->path, &st) != 0)
            continue;

        if (!have_best || st.st_mtime > best_st.st_mtime) {
            best = prefix;
            best_st = st;
            have_best = TRUE;
        }
    }

    return best;
}

/* ------------------------------------------------------------------ */

gboolean
pl_prefix_create (const char *root, const char *id, GError **error)
{
    char *path;
    char *pfx;

    g_return_val_if_fail (root != NULL && *root != '\0', FALSE);
    g_return_val_if_fail (id != NULL && *id != '\0', FALSE);

    if (g_mkdir_with_parents (root, 0755) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "Cannot create %s: %s", root, g_strerror (errno));
        return FALSE;
    }

    path = g_build_filename (root, id, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS)) {
        g_free (path);
        return TRUE;   /* already present: creating is idempotent */
    }

    if (g_mkdir_with_parents (path, 0755) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "Cannot create prefix %s: %s", path, g_strerror (errno));
        g_free (path);
        return FALSE;
    }

    /* Proton creates pfx/ itself, but an empty placeholder keeps the
     * WINEPREFIX path valid for tools that probe it up front. */
    pfx = g_build_filename (path, "pfx", NULL);
    if (g_mkdir_with_parents (pfx, 0755) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "Cannot create %s: %s", pfx, g_strerror (errno));
        g_free (pfx);
        g_free (path);
        return FALSE;
    }

    g_free (pfx);
    g_free (path);
    return TRUE;
}

/* Recursive delete that never follows symlinks out of the tree. */
static void
remove_recursive (const char *path)
{
    GDir       *d;
    const char *name;
    GStatBuf    st;

    if (lstat (path, &st) != 0)
        return;

    if (S_ISLNK (st.st_mode) || !S_ISDIR (st.st_mode)) {
        g_unlink (path);
        return;
    }

    d = g_dir_open (path, 0, NULL);
    if (d == NULL) {
        /* Directory is likely non-readable; surface it rather than
         * silently leaving a half-deleted prefix. */
        g_chmod (path, 0700);
        d = g_dir_open (path, 0, NULL);
        if (d == NULL) {
            g_unlink (path);
            return;
        }
    }

    while ((name = g_dir_read_name (d)) != NULL) {
        char *child = g_build_filename (path, name, NULL);

        remove_recursive (child);
        g_free (child);
    }
    g_dir_close (d);

    g_rmdir (path);
}

gboolean
pl_prefix_remove (const char *root, const char *id, GError **error)
{
    char    *path;
    gboolean ok;

    g_return_val_if_fail (root != NULL, FALSE);
    g_return_val_if_fail (id != NULL, FALSE);

    /* Refuse to operate on anything that is not a child of root. */
    if (strchr (id, '/') != NULL || g_str_equal (id, "..") ||
        g_str_equal (id, ".")) {
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                             "Invalid prefix identifier");
        return FALSE;
    }

    path = g_build_filename (root, id, NULL);

    if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
        g_free (path);
        return TRUE;
    }

    remove_recursive (path);

    ok = !g_file_test (path, G_FILE_TEST_EXISTS);
    if (!ok) {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                     "Could not fully remove %s", path);
    }

    g_free (path);
    return ok;
}