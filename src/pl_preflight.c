/* pl_preflight.c - Check that a Proton build can actually start.
 *
 * The most common failure for a non-Steam Proton setup is a missing
 * runtime dependency that Proton's shebang needs. Modern GE-Proton ships
 * `proton` as a Python 3 script that imports `filelock`, so a stock
 * distro python without that module makes every launch die immediately
 * with a traceback. Detecting that up front is far more useful than
 * handing the user a log full of Python errors.
 */

#include "pl.h"

#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static PlCheck *
check_new (const char *id, const char *title, PlCheckLevel level,
           const char *detail, const char *remedy)
{
    PlCheck *check = g_new0 (PlCheck, 1);

    check->id     = g_strdup (id);
    check->title  = g_strdup (title);
    check->level  = level;
    check->detail = g_strdup (detail);
    check->remedy = (remedy != NULL) ? g_strdup (remedy) : NULL;
    return check;
}

static void
check_free (PlCheck *check)
{
    if (check == NULL)
        return;
    g_free (check->id);
    g_free (check->title);
    g_free (check->detail);
    g_free (check->remedy);
    g_free (check);
}

void
pl_preflight_free (PlReport *self)
{
    if (self == NULL)
        return;
    if (self->checks != NULL)
        g_ptr_array_unref (self->checks);
    g_free (self);
}

gboolean
pl_preflight_can_run (PlReport *self)
{
    return self != NULL && self->can_run;
}

char *
pl_preflight_summary (PlReport *self)
{
    guint fails = 0;
    guint warns = 0;

    if (self == NULL)
        return g_strdup ("Not checked");

    for (guint i = 0; i < self->checks->len; i++) {
        const PlCheck *check = g_ptr_array_index (self->checks, i);

        if (check->level == PL_CHECK_FAIL)
            fails++;
        else if (check->level == PL_CHECK_WARN)
            warns++;
    }

    if (fails == 0 && warns == 0)
        return g_strdup ("All checks passed");
    if (fails == 0)
        return g_strdup_printf ("%u warning%s", warns, warns == 1 ? "" : "s");

    return g_strdup_printf ("%u problem%s blocking launch",
                            fails, fails == 1 ? "" : "s");
}

/* ------------------------------------------------------------------ */

gboolean
pl_has_umu_exe (const char *proton_path, const char *pfx_path)
{
    static const char *relatives[] = {
        "drive_c/windows/system32/umu.exe",
        "drive_c/windows/syswow64/umu.exe",
    };
    char *candidates[5];
    gsize n = 0;
    gboolean found = FALSE;

    /* The executable prefix must contain umu.exe; Proton's non-Steam path
     * invokes it and fails if it is absent. */
    if (pfx_path != NULL) {
        for (gsize i = 0; i < G_N_ELEMENTS (relatives) && n < 5; i++)
            candidates[n++] = g_build_filename (pfx_path, relatives[i], NULL);
    }

    /* Proton's bundled template prefix ships umu.exe and copies it into a
     * fresh prefix, so an empty prefix is not itself a problem. */
    if (proton_path != NULL && n < 5) {
        candidates[n++] = g_build_filename (proton_path, "files", "share",
                                            "default_pfx",
                                            "drive_c/windows/system32/umu.exe", NULL);
        candidates[n++] = g_build_filename (proton_path, "files", "share",
                                            "default_pfx",
                                            "drive_c/windows/syswow64/umu.exe", NULL);
    }

    /* Free every candidate, including any left over once a match is found,
     * otherwise a common prefix leaks the remaining path strings. */
    for (gsize i = 0; i < n; i++) {
        if (!found)
            found = g_file_test (candidates[i], G_FILE_TEST_EXISTS);
        g_free (candidates[i]);
    }

    return found;
}

/* Run a command synchronously and capture its stdout. Returns the exit
 * status, or -1 when the command could not be run at all. */
static int
try_run (const char *const *argv, char **output)
{
    char    *stdout_buf = NULL;
    char    *stderr_buf = NULL;
    GError  *error = NULL;
    int      status = -1;

    /* Capture stderr too: a failing probe like `import filelock` prints a
     * traceback, and leaking that into the launcher's console is noise the
     * user cannot act on (the check already reports it properly). */
    if (!g_spawn_sync (NULL, (char **) argv, NULL,
                       G_SPAWN_SEARCH_PATH,
                       NULL, NULL,
                       &stdout_buf,   /* standard_output */
                       &stderr_buf,   /* standard_error  */
                       &status, &error)) {
        g_clear_error (&error);
        return -1;
    }

    if (output != NULL)
        *output = stdout_buf;
    else
        g_free (stdout_buf);

    g_free (stderr_buf);

    return status;
}

/* Does 'python3 -c "import filelock"' succeed? */
static gboolean
python_has_filelock (const char *python, char **version_out)
{
    const char *argv[] = { python, "-c",
                           "import sys, filelock; "
                           "print(sys.version.split()[0])",
                           NULL };
    char *output = NULL;
    int   status = try_run (argv, &output);

    if (status == 0 && output != NULL) {
        if (version_out != NULL)
            *version_out = g_strstrip (g_strdup (output));
        g_free (output);
        return TRUE;
    }

    g_free (output);
    return FALSE;
}

static char *
python_version (const char *python)
{
    const char *argv[] = { python, "-c",
                           "import sys; print(sys.version.split()[0])", NULL };
    char *output = NULL;

    if (try_run (argv, &output) == 0) {
        char *stripped = g_strstrip (g_strdup (output));

        g_free (output);
        return stripped;
    }

    g_free (output);
    return NULL;
}

/* Read a script's shebang line. Returns NULL when absent or unreadable. */
static char *
read_shebang (const char *script)
{
    char    *contents = NULL;
    char    *result = NULL;
    gsize    length = 0;
    char    *newline;

    if (!g_file_get_contents (script, &contents, &length, NULL))
        return NULL;

    if (length < 3 || contents[0] != '#' || contents[1] != '!')
        goto out;

    newline = strchr (contents, '\n');
    if (newline != NULL)
        *newline = '\0';
    result = g_strstrip (g_strdup (contents + 2));

out:
    g_free (contents);
    return result;
}

PlReport *
pl_preflight_run (const char *proton_path, const char *pfx_path)
{
    PlReport *self = g_new0 (PlReport, 1);
    char     *script = NULL;
    char     *shebang = NULL;

    self->checks  = g_ptr_array_new_with_free_func ((GDestroyNotify) check_free);
    self->can_run = TRUE;

    if (proton_path == NULL || !*proton_path) {
        g_ptr_array_add (self->checks,
                         check_new ("no-proton", "Proton build", PL_CHECK_FAIL,
                                    "No Proton build selected",
                                    "Choose a Proton build in Preferences"));
        self->can_run = FALSE;
        return self;
    }

    /* 1. The build directory and its launcher script. */
    script = g_build_filename (proton_path, "proton", NULL);
    if (!g_file_test (script, G_FILE_TEST_EXISTS)) {
        char *detail = g_strdup_printf ("No 'proton' script in %s", proton_path);

        g_ptr_array_add (self->checks,
                         check_new ("no-proton", "Proton build", PL_CHECK_FAIL,
                                    detail,
                                    "Pick the directory that contains the 'proton' script"));
        self->can_run = FALSE;
        g_free (detail);
        g_free (script);
        return self;
    }

    /* g_access returns 0 on success, so a non-zero result means the script
     * cannot be executed. */
    if (g_access (script, X_OK) != 0) {
        char *detail = g_strdup_printf ("%s is not executable", script);

        g_ptr_array_add (self->checks,
                         check_new ("no-exec", "Proton launcher", PL_CHECK_FAIL,
                                    detail,
                                    "Run: chmod +x \"proton\" in that directory"));
        self->can_run = FALSE;
        g_free (detail);
        g_free (script);
        return self;
    }

    {
        char *base = g_path_get_basename (proton_path);

        g_ptr_array_add (self->checks,
                         check_new ("proton", "Proton build", PL_CHECK_OK,
                                    base, NULL));
        g_free (base);
    }

    /* 2. Interpreter and its Python module dependencies.
     *    Current GE-Proton is a Python 3 script importing filelock. */
    shebang = read_shebang (script);

    if (shebang != NULL && strstr (shebang, "python") != NULL) {
        /* The interpreter is either "/usr/bin/python3" or, via env,
         * "/usr/bin/env python3". Parse to find the program token. */
        g_auto(GStrv) tokens = NULL;
        const char   *python = "python3";
        char         *version;
        GError       *parse_error = NULL;

        /* "/usr/bin/python3 foo" -> "/usr/bin/python3"
         * "/usr/bin/env python3" -> "python3" (the second token names it). */
        if (g_shell_parse_argv (shebang, NULL, &tokens, &parse_error) &&
            tokens != NULL && tokens[0] != NULL) {
            if (tokens[1] != NULL && g_str_has_suffix (tokens[0], "/env"))
                python = tokens[1];
            else
                python = tokens[0];
        } else {
            g_clear_error (&parse_error);
        }

        g_autofree char *python_path = g_find_program_in_path (python);

        if (python_path == NULL) {
            char *detail = g_strdup_printf ("'%s' is not installed", python);

            g_ptr_array_add (self->checks,
                             check_new ("python", "Python 3", PL_CHECK_FAIL,
                                        detail,
                                        "Install Python 3 (python3 package)"));
            self->can_run = FALSE;
            g_free (detail);
        } else {
            char *detail;

            version = python_version (python);
            detail  = g_strdup_printf ("%s (%s)", python,
                                       version != NULL ? version : "?");
            g_ptr_array_add (self->checks,
                             check_new ("python", "Python 3", PL_CHECK_OK,
                                        detail, NULL));
            g_free (detail);
            g_free (version);

            /* filelock: required by Proton's own script. */
            if (python_has_filelock (python, NULL)) {
                g_ptr_array_add (self->checks,
                                 check_new ("filelock", "filelock module",
                                            PL_CHECK_OK, "Available", NULL));
            } else {
                g_ptr_array_add (self->checks,
                                 check_new ("filelock", "filelock module",
                                            PL_CHECK_FAIL,
                                            "Not importable by Proton's script",
                                            "Install it: pip install filelock "
                                            "(or python3-filelock / python3-pip)"));
                self->can_run = FALSE;
            }
        }
    } else {
        /* Older shell-based Proton needs no interpreter of its own. */
        g_ptr_array_add (self->checks,
                         check_new ("python", "Interpreter", PL_CHECK_OK,
                                    "Shell script launcher", NULL));
    }
    g_free (shebang);

    /* 3. umu.exe, which Proton's non-Steam launch path invokes. */
    if (pl_has_umu_exe (proton_path, pfx_path)) {
        g_ptr_array_add (self->checks,
                         check_new ("umu", "umu.exe", PL_CHECK_OK,
                                    "Present in prefix or Proton template", NULL));
    } else {
        g_ptr_array_add (self->checks,
                         check_new ("umu", "umu.exe", PL_CHECK_WARN,
                                    "Not found yet; Proton copies it from its "
                                    "template prefix on first run",
                                    "If launching fails, reset the prefix or "
                                    "check that the build is complete"));
    }
    g_free (script);

    /* 4. A Vulkan driver, required by DXVK/vkd3d and therefore by the
     *    default renderer. WineD3D does not need it. */
    {
        g_autofree char *vulkaninfo = g_find_program_in_path ("vulkaninfo");
        gboolean        have_icd =
            g_file_test ("/usr/share/vulkan/icd.d", G_FILE_TEST_IS_DIR) ||
            g_file_test ("/usr/local/share/vulkan/icd.d", G_FILE_TEST_IS_DIR) ||
            g_file_test ("/etc/vulkan/icd.d", G_FILE_TEST_IS_DIR);

        if (vulkaninfo != NULL) {
            g_ptr_array_add (self->checks,
                             check_new ("vulkan", "Vulkan", PL_CHECK_OK,
                                        "vulkaninfo available", NULL));
        } else if (have_icd) {
            g_ptr_array_add (self->checks,
                             check_new ("vulkan", "Vulkan", PL_CHECK_WARN,
                                        "ICD manifests found, but vulkaninfo is "
                                        "not installed",
                                        "Install vulkan-tools to verify driver "
                                        "availability"));
        } else {
            g_ptr_array_add (self->checks,
                             check_new ("vulkan", "Vulkan", PL_CHECK_WARN,
                                        "No Vulkan ICD found; DXVK and vkd3d "
                                        "need a Vulkan driver",
                                        "Install your GPU's Vulkan driver, or "
                                        "use the WineD3D renderer"));
        }
    }

    return self;
}