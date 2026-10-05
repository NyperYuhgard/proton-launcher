/* pl_config.c - Persistent settings backed by an INI file. */

#include "pl.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

/* ------------------------------------------------------------------ */

PlConfig *
pl_config_new (void)
{
    PlConfig *self = g_new0 (PlConfig, 1);

    self->extra_env = g_ptr_array_new_with_free_func (g_free);
    pl_config_set_defaults (self);

    return self;
}

static void
free_string (char **s)
{
    g_free (*s);
    *s = NULL;
}

void
pl_config_free (PlConfig *self)
{
    if (self == NULL)
        return;

    free_string (&self->proton_dir);
    free_string (&self->proton_version);
    free_string (&self->prefix_root);
    free_string (&self->log_dir);
    free_string (&self->game_id);

    if (self->extra_env != NULL) {
        g_ptr_array_unref (self->extra_env);
        self->extra_env = NULL;
    }

    g_free (self);
}

void
pl_config_set_defaults (PlConfig *self)
{
    g_free (self->proton_dir);
    g_free (self->prefix_root);
    g_free (self->log_dir);
    g_free (self->game_id);
    g_free (self->proton_version);

    /* Default to the well-known Steam compatibility tools location, which
     * is where Proton-GE installs itself when Steam is present. */
    self->proton_dir =
        g_build_filename (g_get_home_dir (), ".steam", "steam",
                          "compatibilitytools.d", NULL);
    self->prefix_root = g_build_filename (g_get_user_data_dir (),
                                         "proton-launcher", "prefixes", NULL);
    self->log_dir     = g_build_filename (g_get_user_cache_dir (),
                                         "proton-launcher", "logs", NULL);
    self->game_id     = g_strdup ("umu-default");

    self->proton_version   = NULL;   /* NULL means "newest available" */
    self->wined3d_default  = FALSE;
    self->proton_log       = FALSE;
    self->first_run_done   = FALSE;
    self->win_width        = 900;
    self->win_height       = 720;
    self->win_maximized    = FALSE;
}

char *
pl_config_default_path (void)
{
    return g_build_filename (g_get_user_config_dir (),
                             "proton-launcher", "config.ini", NULL);
}

/* ------------------------------------------------------------------ */

static char *
get_path_key (GKeyFile *kf, const char *group, const char *key,
              const char *fallback)
{
    g_autofree char *raw = g_key_file_get_string (kf, group, key, NULL);
    char   *expanded;
    char   *result;

    if (raw == NULL)
        return g_strdup (fallback);

    /* Allow "~" and "$VAR" so the config stays portable between machines. */
    expanded = g_strdup (raw);

    if (expanded[0] == '~' && (expanded[1] == '/' || expanded[1] == '\0')) {
        char *joined = g_build_filename (g_get_home_dir (), expanded + 1, NULL);
        g_free (expanded);
        return joined;
    }

    {
        /* Environment expansion: honour $VAR and ${VAR}. */
        GString *out = g_string_new (NULL);
        const char *p = expanded;

        while (*p != '\0') {
            if (*p == '$' && p[1] != '\0') {
                const char *start;
                GString     *name;
                gboolean     braced = (p[1] == '{');

                start = braced ? p + 2 : p + 1;
                name = g_string_new (NULL);
                while (*start != '\0' &&
                       (g_ascii_isalnum (*start) || *start == '_'))
                    g_string_append_c (name, *start++);

                if (name->len > 0 && (braced || *start != '$')) {
                    const char *val = g_getenv (name->str);

                    if (val != NULL)
                        g_string_append (out, val);
                    g_string_free (name, TRUE);
                    p = (braced && *start == '}') ? start + 1 : start;
                    continue;
                }
                g_string_free (name, TRUE);
            }
            g_string_append_c (out, *p++);
        }
        g_free (expanded);
        result = g_string_free (out, FALSE);
    }

    return result;
}

static gboolean
get_bool_key (GKeyFile *kf, const char *group, const char *key, gboolean fallback)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean (kf, group, key, &error);

    if (error != NULL) {
        g_error_free (error);
        return fallback;
    }
    return value;
}

static int
get_int_key (GKeyFile *kf, const char *group, const char *key, int fallback)
{
    GError *error = NULL;
    int value = g_key_file_get_integer (kf, group, key, &error);

    if (error != NULL) {
        g_error_free (error);
        return fallback;
    }
    return value;
}

gboolean
pl_config_load (PlConfig *self, const char *file, GError **error)
{
    GKeyFile  *kf;
    gboolean   ok = TRUE;
    GError    *local = NULL;
    char     **keys;
    gsize      i;

    g_return_val_if_fail (self != NULL, FALSE);

    kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, file, G_KEY_FILE_NONE, &local)) {
        /* A missing config is not an error: the caller runs the wizard. */
        if (g_error_matches (local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free (local);
            g_key_file_free (kf);
            return TRUE;
        }
        g_propagate_error (error, local);
        g_key_file_free (kf);
        return FALSE;
    }

    g_free (self->proton_dir);
    self->proton_dir = get_path_key (kf, "paths", "proton-dir",
                                     g_get_home_dir ());

    g_free (self->prefix_root);
    self->prefix_root = get_path_key (kf, "paths", "prefix-root", NULL);

    g_free (self->log_dir);
    self->log_dir = get_path_key (kf, "paths", "log-dir", NULL);

    free_string (&self->proton_version);
    self->proton_version = g_key_file_get_string (kf, "proton", "version", NULL);
    if (self->proton_version != NULL && *self->proton_version == '\0') {
        g_clear_pointer (&self->proton_version, g_free);
    }

    {
        g_autofree char *game_id = g_key_file_get_string (kf, "proton",
                                                         "game-id", NULL);

        g_free (self->game_id);
        self->game_id = ((game_id != NULL && *game_id != '\0')
                         ? g_strdup (game_id) : g_strdup ("umu-default"));
    }

    self->wined3d_default = get_bool_key (kf, "proton", "wined3d-default", FALSE);
    self->proton_log       = get_bool_key (kf, "logging", "proton-log", FALSE);
    self->first_run_done   = get_bool_key (kf, "ui", "first-run-done", FALSE);

    self->win_width     = get_int_key (kf, "ui", "window-width", 900);
    self->win_height    = get_int_key (kf, "ui", "window-height", 720);
    self->win_maximized = get_bool_key (kf, "ui", "window-maximized", FALSE);

    if (self->win_width < 400 || self->win_height < 300) {
        self->win_width = 900;
        self->win_height = 720;
    }

    /* Optional "KEY=value" overrides forwarded verbatim to Proton. */
    g_ptr_array_set_size (self->extra_env, 0);
    {
        gsize n_keys = 0;
        keys = g_key_file_get_keys (kf, "environment", &n_keys, NULL);
        for (i = 0; keys != NULL && i < n_keys; i++) {
            char *value = g_key_file_get_string (kf, "environment", keys[i], NULL);

            if (value != NULL) {
                g_ptr_array_add (self->extra_env,
                                 g_strdup_printf ("%s=%s", keys[i], value));
                g_free (value);
            }
        }
        if (keys != NULL)
            g_strfreev (keys);
    }

    g_key_file_free (kf);
    return ok;
}

gboolean
pl_config_save (PlConfig *self, const char *file, GError **error)
{
    GKeyFile  *kf;
    gboolean   ok;
    GError    *local = NULL;
    char      *dir;
    gsize      i;

    g_return_val_if_fail (self != NULL, FALSE);

    dir = g_path_get_dirname (file);
    if (g_mkdir_with_parents (dir, 0755) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "Cannot create %s: %s", dir, g_strerror (errno));
        g_free (dir);
        return FALSE;
    }
    g_free (dir);

    kf = g_key_file_new ();

    g_key_file_set_string (kf, "paths", "proton-dir",
                           self->proton_dir ? self->proton_dir : "");
    g_key_file_set_string (kf, "paths", "prefix-root",
                           self->prefix_root ? self->prefix_root : "");
    g_key_file_set_string (kf, "paths", "log-dir",
                           self->log_dir ? self->log_dir : "");

    if (self->proton_version != NULL)
        g_key_file_set_string (kf, "proton", "version", self->proton_version);
    g_key_file_set_string (kf, "proton", "game-id",
                           self->game_id ? self->game_id : "umu-default");
    g_key_file_set_boolean (kf, "proton", "wined3d-default", self->wined3d_default);

    g_key_file_set_boolean (kf, "logging", "proton-log", self->proton_log);

    g_key_file_set_boolean (kf, "ui", "first-run-done", self->first_run_done);
    g_key_file_set_integer (kf, "ui", "window-width", self->win_width);
    g_key_file_set_integer (kf, "ui", "window-height", self->win_height);
    g_key_file_set_boolean (kf, "ui", "window-maximized", self->win_maximized);

    /* Store overrides as individual keys so GKeyFile escapes them for us. */
    for (i = 0; i < self->extra_env->len; i++) {
        const char *entry = g_ptr_array_index (self->extra_env, i);
        const char *eq = strchr (entry, '=');
        char       *key;

        if (eq == NULL || eq == entry)
            continue;
        key = g_strndup (entry, (gsize) (eq - entry));
        g_key_file_set_string (kf, "environment", key, eq + 1);
        g_free (key);
    }

    ok = g_key_file_save_to_file (kf, file, &local);
    if (!ok)
        g_propagate_error (error, local);

    g_key_file_free (kf);
    return ok;
}