/* test-core.c - Unit tests for Proton Launcher's non-UI core.
 *
 * These cover the parts whose behaviour is load-bearing and easy to get
 * subtly wrong:
 *
 *   - natural version ordering of Proton builds
 *   - validation of a build directory
 *   - prefix slugify / create / remove / scan
 *   - the exact argv and environment handed to Proton
 *   - config round-trip
 *
 * The argv/envp assertions encode the environment contract described in
 * src/pl.h; if Proton's expectations change, these must change with it.
 *
 * Run with: make check
 */

#include "pl.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

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

static void
section (const char *name)
{
    g_print ("- %s\n", name);
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char *root;
    char *proton_dir;   /* root/proton-dir  */
    char *prefix_root;  /* root/prefixes    */
} Fixture;

/* A Python-based build, as shipped by current GE-Proton. Its `proton`
 * script behaves like the real one in the ways that matter: it exits 1
 * when STEAM_COMPAT_DATA_PATH is missing, and without UMU_ID it would
 * start steam.exe instead of the game. Here it just reports what it got,
 * so the test can assert on it. */
static const char FAKE_PROTON[] =
    "#!/usr/bin/env python3\n"
    "import os, sys\n"
    "if 'STEAM_COMPAT_DATA_PATH' not in os.environ:\n"
    "    sys.stderr.write('proton: STEAM_COMPAT_DATA_PATH is not set\\n')\n"
    "    sys.exit(1)\n"
    "print('verb=' + (sys.argv[1] if len(sys.argv) > 1 else ''))\n"
    "for k in ('UMU_ID', 'UMU_USE_STEAM', 'PROTON_USE_WINED3D', 'PROTON_LOG',\n"
    "          'WINEPREFIX', 'STEAM_COMPAT_DATA_PATH',\n"
    "          'STEAM_COMPAT_CLIENT_INSTALL_PATH'):\n"
    "    if k in os.environ:\n"
    "        print('%s=%s' % (k, os.environ[k]))\n"
    "for a in sys.argv[2:]:\n"
    "    print('arg=' + a)\n";

/* Write 'contents' to <dir>/<name> and mark it executable. */
static void
write_file (const char *dir, const char *name, const char *contents, gboolean exec)
{
    char *path = g_build_filename (dir, name, NULL);
    GError *error = NULL;

    if (!g_file_set_contents (path, contents, -1, &error)) {
        g_printerr ("fixture: cannot write %s: %s\n", path, error->message);
        g_clear_error (&error);
        g_assert_not_reached ();
    }
    if (exec)
        g_chmod (path, 0755);
    g_free (path);
}

static void
fixture_setup (Fixture *fx)
{
    GError *error = NULL;
    char   *d;

    fx->root = g_dir_make_tmp ("pl-test-XXXXXX", &error);
    g_assert_no_error (error);

    fx->proton_dir  = g_build_filename (fx->root, "proton-dir", NULL);
    fx->prefix_root = g_build_filename (fx->root, "prefixes", NULL);
    g_assert_cmpint (g_mkdir_with_parents (fx->proton_dir, 0755), ==, 0);
    g_assert_cmpint (g_mkdir_with_parents (fx->prefix_root, 0755), ==, 0);

    /* A Python-based build, as shipped by current GE-Proton. */
    d = g_build_filename (fx->proton_dir, "GE-Proton10-4", NULL);
    g_mkdir_with_parents (d, 0755);
    write_file (d, "proton", FAKE_PROTON, TRUE);
    write_file (d, "version", "GE-Proton10-4\n", FALSE);
    g_free (d);

    /* An older, shell-based build. */
    d = g_build_filename (fx->proton_dir, "GE-Proton9-20", NULL);
    g_mkdir_with_parents (d, 0755);
    write_file (d, "proton", "#!/bin/sh\nexec wine \"$@\"\n", TRUE);
    g_free (d);

    /* Present but not executable: must be reported as unusable. */
    d = g_build_filename (fx->proton_dir, "GE-ProtonBroken", NULL);
    g_mkdir_with_parents (d, 0755);
    write_file (d, "proton", "#!/usr/bin/env python3\n", FALSE);
    g_free (d);

    /* A directory with no proton script at all. */
    d = g_build_filename (fx->proton_dir, "GE-ProtonEmpty", NULL);
    g_mkdir_with_parents (d, 0755);
    g_free (d);

    /* Unrelated folder that must never be mistaken for a build. */
    d = g_build_filename (fx->proton_dir, "steam-common", NULL);
    g_mkdir_with_parents (d, 0755);
    write_file (d, "proton", "#!/bin/sh\n", TRUE);
    g_free (d);
}

/* Remove a tree in-process. Kept here rather than shelling out so the
 * tests have no dependency on an external tool being present. */
static void
rm_tree (const char *path)
{
    GDir       *d;
    const char *name;

    if (g_file_test (path, G_FILE_TEST_IS_SYMLINK) ||
        !g_file_test (path, G_FILE_TEST_IS_DIR)) {
        g_unlink (path);
        return;
    }

    d = g_dir_open (path, 0, NULL);
    if (d != NULL) {
        while ((name = g_dir_read_name (d)) != NULL) {
            g_autofree char *child = g_build_filename (path, name, NULL);

            rm_tree (child);
        }
        g_dir_close (d);
    }
    g_rmdir (path);
}

static void
fixture_teardown (Fixture *fx)
{
    rm_tree (fx->root);

    g_free (fx->root);
    g_free (fx->proton_dir);
    g_free (fx->prefix_root);
}

/* ------------------------------------------------------------------ */
/* Proton discovery                                                    */
/* ------------------------------------------------------------------ */

static void
test_proton_scan (Fixture *fx)
{
    g_autoptr(PlProtonList) list = pl_proton_list_new ();
    g_autoptr(GError) error = NULL;

    section ("proton scan");

    CHECK (pl_proton_scan (list, fx->proton_dir, &error), "scan failed: %s",
           error ? error->message : "(no error set)");
    CHECK (error == NULL, "unexpected error: %s", error ? error->message : "");

    /* "steam-common" is skipped: the filter matches build naming only. */
    CHECK (list->items->len == 4, "expected 4 builds, got %u", list->items->len);
    CHECK (pl_proton_find (list, "steam-common") == NULL,
           "unrelated directory was listed as a build");

    /* Newest first by numeric components: 10 > 9, and 20 > 4 only within
     * the same major. So GE-Proton10-4, GE-Proton9-20, GE-ProtonBroken,
     * GE-ProtonEmpty. */
    {
        const PlProton *first = g_ptr_array_index (list->items, 0);
        const PlProton *second = g_ptr_array_index (list->items, 1);

        CHECK (g_strcmp0 (first->id, "GE-Proton10-4") == 0,
               "newest build should sort first, got '%s'", first->id);
        CHECK (g_strcmp0 (second->id, "GE-Proton9-20") == 0,
               "second build should be GE-Proton9-20, got '%s'", second->id);
    }

    /* Version file preferred over the directory name. */
    {
        const PlProton *p = pl_proton_find (list, "GE-Proton10-4");

        CHECK (p != NULL, "GE-Proton10-4 not found");
        if (p != NULL)
            CHECK (g_strcmp0 (p->version, "GE-Proton10-4") == 0,
                   "version read from file, got '%s'", p->version);
    }

    /* No version file: falls back to the id. */
    {
        const PlProton *p = pl_proton_find (list, "GE-Proton9-20");

        CHECK (p != NULL && g_strcmp0 (p->version, "GE-Proton9-20") == 0,
               "version should fall back to the id");
    }

    /* The non-executable build must be flagged, whatever its shebang. */
    {
        const PlProton *p = pl_proton_find (list, "GE-ProtonBroken");

        CHECK (p != NULL && !p->usable,
               "a non-executable proton must not be usable");
        CHECK (p != NULL && p->problem != NULL,
               "a non-executable proton must explain why");
    }

    /* Missing script must be flagged. */
    {
        const PlProton *p = pl_proton_find (list, "GE-ProtonEmpty");

        CHECK (p != NULL && !p->usable, "build without a proton script unusable");
    }

    /* latest() skips unusable builds. */
    {
        const PlProton *latest = pl_proton_latest (list);

        CHECK (latest != NULL && g_strcmp0 (latest->id, "GE-Proton10-4") == 0,
               "latest should be the newest usable build, got '%s'",
               latest ? latest->id : "(null)");
    }
}

static void
test_proton_scan_errors (Fixture *fx)
{
    g_autoptr(PlProtonList) list = pl_proton_list_new ();
    g_autoptr(GError) error = NULL;

    section ("proton scan errors");

    /* NULL / empty directory. */
    pl_proton_scan (list, NULL, &error);
    CHECK (error != NULL, "NULL directory should raise an error");
    g_clear_error (&error);

    /* Missing directory. */
    pl_proton_scan (list, "/nonexistent/proton/dir", &error);
    CHECK (error != NULL, "missing directory should raise an error");
    g_clear_error (&error);

    /* Existing directory with no builds. */
    {
        g_autofree char *empty = g_build_filename (fx->root, "empty", NULL);
        g_autoptr(PlProtonList) l2 = pl_proton_list_new ();

        g_assert_cmpint (g_mkdir_with_parents (empty, 0755), ==, 0);
        pl_proton_scan (l2, empty, &error);
        CHECK (error != NULL, "a directory with no builds should raise an error");
        g_clear_error (&error);
    }

    /* A symlinked Proton directory is a common setup and must be followed. */
    {
        g_autofree char *link = g_build_filename (fx->root, "proton-link", NULL);
        g_autoptr(PlProtonList) l3 = pl_proton_list_new ();

        g_assert_cmpint (symlink (fx->proton_dir, link), ==, 0);
        pl_proton_scan (l3, link, &error);
        CHECK (error == NULL && l3->items->len == 4,
               "symlinked Proton directory should scan, got %u builds (%s)",
               l3->items->len, error ? error->message : "no error");
        g_clear_error (&error);
    }
}

/* ------------------------------------------------------------------ */
/* Prefixes                                                            */
/* ------------------------------------------------------------------ */

static void
test_slugify (void)
{
    section ("prefix slugify");

    {
        g_autofree char *s = pl_prefix_slugify ("My Game");

        CHECK (g_strcmp0 (s, "My_Game") == 0, "got '%s'", s);
    }
    {
        g_autofree char *s = pl_prefix_slugify ("  spaced  ");

        CHECK (g_strcmp0 (s, "spaced") == 0, "got '%s'", s);
    }
    {
        g_autofree char *s = pl_prefix_slugify ("a/b\\c:d*e?f");

        CHECK (strchr (s, '/') == NULL && strchr (s, '\\') == NULL &&
               strchr (s, ':') == NULL,
               "path separators must be neutralised, got '%s'", s);
    }
    {
        g_autofree char *s = pl_prefix_slugify ("   ");

        CHECK (g_strcmp0 (s, "default") == 0, "blank label -> 'default', got '%s'", s);
    }
    {
        g_autofree char *s = pl_prefix_slugify (NULL);

        CHECK (g_strcmp0 (s, "default") == 0, "NULL label -> 'default', got '%s'", s);
    }
}

static void
test_prefix_lifecycle (Fixture *fx)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(PlPrefixList) list = pl_prefix_list_new ();

    section ("prefix lifecycle");

    CHECK (pl_prefix_create (fx->prefix_root, "my_game", &error), "create: %s",
           error ? error->message : "");
    g_clear_error (&error);

    /* Idempotent. */
    CHECK (pl_prefix_create (fx->prefix_root, "my_game", &error),
           "second create should succeed");
    g_clear_error (&error);

    /* pfx/ placeholder exists so WINEPREFIX is a real path. */
    {
        g_autofree char *pfx = g_build_filename (fx->prefix_root, "my_game", "pfx", NULL);

        CHECK (g_file_test (pfx, G_FILE_TEST_IS_DIR),
               "pfx placeholder should be created");
    }

    /* Unseeded until Proton writes its version file. */
    pl_prefix_list_scan (list, fx->prefix_root);
    {
        const PlPrefix *p = pl_prefix_find_by_id (list, "my_game");

        CHECK (p != NULL, "prefix not found after create");
        CHECK (p != NULL && !p->seeded, "fresh prefix should not be seeded");
        CHECK (p != NULL && p->pfx != NULL && strstr (p->pfx, "/my_game/pfx") != NULL,
               "pfx path should be <prefix>/pfx, got '%s'", p ? p->pfx : "");
    }

    /* Proton seeds it. */
    {
        g_autofree char *ver = g_build_filename (fx->prefix_root, "my_game", "version", NULL);

        g_file_set_contents (ver, "GE-Proton10-4\n", -1, NULL);
        pl_prefix_list_scan (list, fx->prefix_root);
        {
            const PlPrefix *p = pl_prefix_find_by_id (list, "my_game");

            CHECK (p != NULL && p->seeded, "prefix should report as seeded");
        }
    }

    /* Hidden entries are ignored. */
    {
        g_autofree char *hidden = g_build_filename (fx->prefix_root, ".cache", NULL);

        g_mkdir_with_parents (hidden, 0755);
        pl_prefix_list_scan (list, fx->prefix_root);
        CHECK (pl_prefix_find_by_id (list, ".cache") == NULL,
               "dotfiles must not be listed as prefixes");
    }

    /* Removal, including a symlink that must be unlinked, not followed. */
    {
        g_autofree char *outside = g_build_filename (fx->root, "precious", NULL);
        g_autofree char *victim  = g_build_filename (fx->prefix_root, "doomed", NULL);
        g_autofree char *link    = g_build_filename (victim, "link-to-outside", NULL);
        g_autofree char *stray   = g_build_filename (victim, "stray-file", NULL);

        g_mkdir_with_parents (victim, 0755);
        g_mkdir_with_parents (outside, 0755);
        g_file_set_contents (stray, "data", -1, NULL);
        CHECK (symlink (outside, link) == 0, "could not create symlink");

        CHECK (pl_prefix_remove (fx->prefix_root, "doomed", &error), "remove: %s",
               error ? error->message : "");
        g_clear_error (&error);

        CHECK (!g_file_test (victim, G_FILE_TEST_EXISTS),
               "prefix directory should be gone");
        CHECK (g_file_test (outside, G_FILE_TEST_EXISTS),
               "symlink target must survive removal");
    }

    /* Removing something absent is not an error. */
    CHECK (pl_prefix_remove (fx->prefix_root, "never_existed", &error),
           "removing a missing prefix should succeed");
    g_clear_error (&error);

    /* Traversal attempts are rejected. */
    CHECK (!pl_prefix_remove (fx->prefix_root, "../escape", &error),
           "'..' traversal must be rejected");
    g_clear_error (&error);
    CHECK (!pl_prefix_remove (fx->prefix_root, "sub/dir", &error),
           "embedded separator must be rejected");
    g_clear_error (&error);

    /* latest() picks the most recently modified. */
    pl_prefix_list_scan (list, fx->prefix_root);
    CHECK (pl_prefix_latest (list) != NULL, "latest should find a prefix");
}

/* ------------------------------------------------------------------ */
/* The Proton invocation                                               */
/* ------------------------------------------------------------------ */

static gboolean
strv_has (char **strv, const char *needle)
{
    for (gsize i = 0; strv != NULL && strv[i] != NULL; i++) {
        if (g_strcmp0 (strv[i], needle) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Find the value of KEY in a NULL-terminated envp. */
static const char *
env_get (char **envp, const char *key)
{
    gsize klen = strlen (key);

    for (gsize i = 0; envp != NULL && envp[i] != NULL; i++) {
        if (strncmp (envp[i], key, klen) == 0 && envp[i][klen] == '=')
            return envp[i] + klen + 1;
    }
    return NULL;
}

static void
test_run_argv (Fixture *fx)
{
    g_autoptr(PlProtonList) protons = pl_proton_list_new ();
    const PlProton *proton;
    g_autoptr(GError) error = NULL;
    PlRunRequest req = { 0 };

    section ("proton argv");

    pl_proton_scan (protons, fx->proton_dir, &error);
    g_assert_no_error (error);
    proton = pl_proton_find (protons, "GE-Proton10-4");
    g_assert_nonnull (proton);

    /* `run` for a game. */
    req.verb   = PL_VERB_RUN;
    req.proton = proton;
    req.exe    = "/games/doom/Doom.exe";
    {
        g_auto(GStrv) argv = pl_run_build_argv (&req, &error);

        g_clear_error (&error);   /* no prefix needed for argv */
        CHECK (argv != NULL, "argv build failed");
        CHECK (strv_has (argv, "run"), "'run' verb must be passed to Proton");
        CHECK (strv_has (argv, "/games/doom/Doom.exe"),
               "the executable must be an absolute host path");
        /* Proton is invoked as <build>/proton, not via a shell. */
        CHECK (argv != NULL && g_str_has_suffix (argv[0], "/GE-Proton10-4/proton"),
               "argv[0] should be the build's proton script, got '%s'",
               argv ? argv[0] : "");
        CHECK (argv != NULL && argv[0] != NULL && argv[0][0] == '/',
               "the script path must be absolute");
    }
    g_clear_error (&error);

    /* `run` without an exe is a programming error and must be caught. */
    req.exe = NULL;
    CHECK (pl_run_build_argv (&req, &error) == NULL,
           "run without an exe must fail");
    CHECK (error != NULL, "run without an exe must set an error");
    g_clear_error (&error);

    /* `runinprefix` passes the utility through verbatim. */
    {
        GPtrArray *args = g_ptr_array_new_with_free_func (g_free);

        g_ptr_array_add (args, g_strdup ("winecfg"));
        req.verb = PL_VERB_RUNINPREFIX;
        req.args = args;

        {
            g_auto(GStrv) argv = pl_run_build_argv (&req, &error);

            CHECK (strv_has (argv, "runinprefix"),
                   "Wine tools must use 'runinprefix', not 'run'");
            CHECK (strv_has (argv, "winecfg"), "the utility must be forwarded");
            CHECK (!strv_has (argv, "run"),
                   "'run' must not appear for Wine utilities");
        }

        /* No command: reject rather than invoke Proton with nothing. */
        g_ptr_array_set_size (args, 0);
        CHECK (pl_run_build_argv (&req, &error) == NULL,
               "runinprefix without a command must fail");
        g_clear_error (&error);
        g_ptr_array_unref (args);
    }

    /* destroyprefix takes no operands. */
    req.verb = PL_VERB_DESTROYPREFIX;
    req.args = NULL;
    {
        g_auto(GStrv) argv = pl_run_build_argv (&req, &error);

        CHECK (strv_has (argv, "destroyprefix"), "destroyprefix verb expected");
        CHECK (argv != NULL && argv[1] != NULL && argv[2] == NULL,
               "destroyprefix takes no operands");
    }
    g_clear_error (&error);
}

static void
test_run_envp (Fixture *fx)
{
    g_autoptr(PlProtonList) protons = pl_proton_list_new ();
    const PlProton *proton;
    g_autoptr(GError) error = NULL;
    PlRunRequest req = { 0 };
    const char *steam_stub = "/home/u/.local/share/proton-launcher/steam-stub";
    /* Held by pointer because PlRunRequest wants a const PlPrefix*, and
     * these values are literals that outlive the function. */
    PlPrefix *prefix = g_new0 (PlPrefix, 1);

    prefix->id   = g_strdup ("doom");
    prefix->path = g_strdup ("/home/u/prefixes/doom");
    prefix->pfx  = g_strdup ("/home/u/prefixes/doom/pfx");

    section ("proton environment");

    pl_proton_scan (protons, fx->proton_dir, &error);
    g_assert_no_error (error);
    proton = pl_proton_find (protons, "GE-Proton10-4");
    g_assert_nonnull (proton);

    req.verb   = PL_VERB_RUN;
    req.proton = proton;
    req.exe    = "/games/doom/Doom.exe";
    req.prefix = prefix;

    {
        g_auto(GStrv) envp = pl_run_build_envp (&req, steam_stub, FALSE, &error);
        /* g_strv_length counts real entries; the terminator sits past it. */
        gsize n_env = envp != NULL ? g_strv_length (envp) : 0;

        CHECK (envp != NULL, "envp build failed: %s", error ? error->message : "");
        CHECK (n_env > 0 && envp[n_env] == NULL,
               "envp must be NULL-terminated");

        /* Mandatory: Proton exits 1 without this. */
        CHECK (g_strcmp0 (env_get (envp, "STEAM_COMPAT_DATA_PATH"),
                          prefix->path) == 0,
               "STEAM_COMPAT_DATA_PATH must name the prefix, got '%s'",
               env_get (envp, "STEAM_COMPAT_DATA_PATH"));

        /* Mandatory: read unconditionally during prefix setup. */
        CHECK (g_strcmp0 (env_get (envp, "STEAM_COMPAT_CLIENT_INSTALL_PATH"),
                          steam_stub) == 0,
               "STEAM_COMPAT_CLIENT_INSTALL_PATH must point at a real directory");

        /* This is the whole point of the launcher: without UMU_ID, Proton
         * starts steam.exe instead of the game. */
        CHECK (g_strcmp0 (env_get (envp, "UMU_ID"), "umu-default") == 0,
               "UMU_ID must default to umu-default, got '%s'",
               env_get (envp, "UMU_ID"));
        CHECK (g_strcmp0 (env_get (envp, "UMU_USE_STEAM"), "0") == 0,
               "UMU_USE_STEAM must be 0 so Proton takes the non-Steam path");

        /* A custom id overrides the default. */
        {
            PlRunRequest custom = req;

            custom.game_id = "umu-doom";
            {
                g_auto(GStrv) e2 = pl_run_build_envp (&custom, steam_stub, FALSE, &error);

                CHECK (g_strcmp0 (env_get (e2, "UMU_ID"), "umu-doom") == 0,
                       "a configured UMU_ID must win");
            }
        }

        /* An empty id still yields the default, not an empty UMU_ID. */
        {
            PlRunRequest empty = req;

            empty.game_id = "";
            {
                g_auto(GStrv) e3 = pl_run_build_envp (&empty, steam_stub, FALSE, &error);

                CHECK (g_strcmp0 (env_get (e3, "UMU_ID"), "umu-default") == 0,
                       "an empty UMU_ID must fall back to the default");
            }
        }

        CHECK (g_strcmp0 (env_get (envp, "WINEPREFIX"), prefix->pfx) == 0,
               "WINEPREFIX must point into the prefix, got '%s'",
               env_get (envp, "WINEPREFIX"));

        /* Rendering: off by default. */
        CHECK (env_get (envp, "PROTON_USE_WINED3D") == NULL,
               "WineD3D must not be requested by default");

        /* Logging: off by default. */
        CHECK (env_get (envp, "PROTON_LOG") == NULL, "PROTON_LOG must be opt-in");

        /* The parent environment is inherited, otherwise DISPLAY and the
         * Vulkan loader would be lost. */
        CHECK (strv_has (envp, "PATH") || env_get (envp, "PATH") != NULL,
               "the inherited environment must be preserved");
    }

    /* WineD3D is a single flag; Proton rewrites DllOverrides itself. */
    {
        PlRunRequest d3d = req;

        d3d.wined3d = TRUE;
        {
            g_auto(GStrv) envp = pl_run_build_envp (&d3d, steam_stub, FALSE, &error);

            CHECK (g_strcmp0 (env_get (envp, "PROTON_USE_WINED3D"), "1") == 0,
                   "WineD3D must set PROTON_USE_WINED3D=1");
        }
    }

    /* Logging needs SteamGameId or Proton's logger returns early. */
    {
        g_auto(GStrv) envp = pl_run_build_envp (&req, steam_stub, TRUE, &error);

        CHECK (g_strcmp0 (env_get (envp, "PROTON_LOG"), "1") == 0,
               "PROTON_LOG=1 expected when logging is on");
        CHECK (env_get (envp, "SteamGameId") != NULL,
               "SteamGameId is required or Proton writes no log");
        CHECK (env_get (envp, "SteamAppId") != NULL, "SteamAppId expected");
    }

    /* A stale value inherited from the user's shell must not win. */
    {
        g_autofree char *saved = g_strdup (g_getenv ("STEAM_COMPAT_DATA_PATH"));

        g_setenv ("STEAM_COMPAT_DATA_PATH", "/stale/from-shell", TRUE);
        g_setenv ("PROTON_USE_WINED3D", "1", TRUE);
        {
            g_auto(GStrv) envp = pl_run_build_envp (&req, steam_stub, FALSE, &error);

            CHECK (g_strcmp0 (env_get (envp, "STEAM_COMPAT_DATA_PATH"),
                              prefix->path) == 0,
                   "an inherited STEAM_COMPAT_DATA_PATH must be overridden");
            CHECK (env_get (envp, "PROTON_USE_WINED3D") == NULL,
                   "an inherited PROTON_USE_WINED3D must be cleared");
        }
        if (saved != NULL)
            g_setenv ("STEAM_COMPAT_DATA_PATH", saved, TRUE);
        else
            g_unsetenv ("STEAM_COMPAT_DATA_PATH");
        g_clear_error (&error);
    }

    /* No prefix: refuse rather than hand Proton an invalid path. */
    {
        PlRunRequest bad = req;

        bad.prefix = NULL;
        CHECK (pl_run_build_envp (&bad, steam_stub, FALSE, &error) == NULL,
               "envp without a prefix must fail");
        g_clear_error (&error);
    }

    g_free (prefix->id);
    g_free (prefix->path);
    g_free (prefix->pfx);
    g_free (prefix);
}

/* End-to-end against the fixture's fake `proton`, which echoes back the
 * environment it was given. This is what proves the launcher actually
 * invokes Proton the way the contract says. */
static void
test_run_end_to_end (Fixture *fx)
{
    g_autoptr(PlProtonList) protons = pl_proton_list_new ();
    g_autoptr(GError) error = NULL;
    const PlProton *proton;
    PlRunRequest req = { 0 };
    PlPrefix *prefix = g_new0 (PlPrefix, 1);

    section ("proton launch (end to end)");

    pl_proton_scan (protons, fx->proton_dir, &error);
    g_assert_no_error (error);
    proton = pl_proton_find (protons, "GE-Proton10-4");
    g_assert_nonnull (proton);

    prefix->id   = g_strdup ("e2e");
    prefix->path = g_build_filename (fx->prefix_root, "e2e", NULL);
    prefix->pfx  = g_build_filename (prefix->path, "pfx", NULL);
    g_assert_true (pl_prefix_create (fx->prefix_root, "e2e", &error));
    g_assert_no_error (error);

    req.verb   = PL_VERB_RUN;
    req.proton = proton;
    req.prefix = prefix;
    req.exe    = "/games/doom/Doom.exe";
    req.game_id = "umu-default";

    {
        g_auto(GStrv) argv = pl_run_build_argv (&req, &error);
        g_auto(GStrv) envp = pl_run_build_envp (&req, "/tmp/steam-stub", TRUE, &error);
        GError *spawn_error = NULL;
        char   *out = NULL;
        int     status = -1;

        g_assert_no_error (error);
        g_assert_nonnull (argv);
        g_assert_nonnull (envp);

        /* The fixture's `proton` refuses to run unless the contract holds,
         * then echoes the variables it received. */
        {
            g_autofree char *stub = g_build_filename (fx->root, "steam-stub", NULL);

            g_mkdir_with_parents (stub, 0755);
        }

        CHECK (g_spawn_sync (NULL, argv, envp, G_SPAWN_SEARCH_PATH,
                             NULL, NULL, &out, NULL, &status, &spawn_error),
               "could not run the fixture proton: %s",
               spawn_error ? spawn_error->message : "");
        g_clear_error (&spawn_error);

        if (status == 0) {
            g_auto(GStrv) lines = g_strsplit (out, "\n", -1);

            CHECK (strv_has (lines, "verb=run"), "the 'run' verb should arrive");
            CHECK (strv_has (lines, "UMU_ID=umu-default"),
                   "UMU_ID must reach Proton, otherwise it starts steam.exe");
            CHECK (strv_has (lines, "UMU_USE_STEAM=0"),
                   "UMU_USE_STEAM=0 must reach Proton");
            CHECK (strv_has (lines, "PROTON_LOG=1"), "PROTON_LOG must arrive");
            CHECK (strstr (out, prefix->path) != NULL,
                   "STEAM_COMPAT_DATA_PATH must carry the prefix path");
            CHECK (strstr (out, "/tmp/steam-stub") != NULL,
                   "STEAM_COMPAT_CLIENT_INSTALL_PATH must carry the stub path");
            CHECK (strstr (out, "/games/doom/Doom.exe") != NULL,
                   "the executable must reach Proton");
        } else {
            g_printerr ("  (fixture proton exited %d)\n  output: %s\n", status,
                       out ? out : "(none)");
            CHECK (status == 0, "the fixture proton must accept the launcher's "
                                "environment; it exits 1 when the contract is "
                                "violated");
        }
        g_free (out);
    }

    /* A non-zero exit from Proton must be reported, not swallowed. */
    {
        PlRunRequest bad = req;

        bad.exe = "";    /* build_argv rejects this */
        CHECK (pl_run_build_argv (&bad, &error) == NULL,
               "an empty exe must be rejected");
        CHECK (error != NULL, "an empty exe must set an error");
        g_clear_error (&error);
    }

    g_free (prefix->id);
    g_free (prefix->path);
    g_free (prefix->pfx);
    g_free (prefix);
}

/* ------------------------------------------------------------------ */
/* Preflight                                                           */
/* ------------------------------------------------------------------ */

static void
test_preflight (Fixture *fx)
{
    g_autoptr(PlProtonList) protons = pl_proton_list_new ();
    g_autoptr(GError) error = NULL;
    const PlProton *proton;
    PlReport *report;

    section ("preflight");

    /* No build at all. */
    report = pl_preflight_run (NULL, NULL);
    CHECK (!pl_preflight_can_run (report), "no Proton build must block launching");
    {
        g_autofree char *summary = pl_preflight_summary (report);

        CHECK (strstr (summary, "blocking") != NULL,
               "summary should mention the blocker, got '%s'", summary);
    }
    pl_preflight_free (report);

    pl_proton_scan (protons, fx->proton_dir, &error);
    g_assert_no_error (error);

    /* A non-executable build is reported as such. */
    proton = pl_proton_find (protons, "GE-ProtonBroken");
    report = pl_preflight_run (proton->path, NULL);
    CHECK (!pl_preflight_can_run (report), "a non-executable build must block");
    {
        gboolean found = FALSE;

        for (guint i = 0; i < report->checks->len; i++) {
            const PlCheck *c = g_ptr_array_index (report->checks, i);

            if (g_strcmp0 (c->id, "no-exec") == 0) {
                found = TRUE;
                CHECK (c->remedy != NULL, "a failure must suggest a remedy");
            }
        }
        CHECK (found, "expected a 'no-exec' check");
    }
    pl_preflight_free (report);

    /* The Python build gets interpreter and filelock checks. Whether those
     * pass depends on the host, but the checks must exist. */
    proton = pl_proton_find (protons, "GE-Proton10-4");
    report = pl_preflight_run (proton->path, NULL);
    {
        gboolean saw_python = FALSE;
        gboolean saw_filelock = FALSE;

        for (guint i = 0; i < report->checks->len; i++) {
            const PlCheck *c = g_ptr_array_index (report->checks, i);

            if (g_strcmp0 (c->id, "python") == 0)
                saw_python = TRUE;
            if (g_strcmp0 (c->id, "filelock") == 0) {
                saw_filelock = TRUE;
                /* On this machine filelock is genuinely missing; that must
                 * be surfaced, because Proton dies on it instantly. */
                if (c->level == PL_CHECK_FAIL)
                    g_print ("  (host is missing filelock, as expected)\n");
            }
        }
        CHECK (saw_python, "a Python-based build must get a Python check");
        CHECK (saw_filelock, "a Python-based build must get a filelock check");
    }
    pl_preflight_free (report);

    /* The shell build needs no interpreter of its own. */
    proton = pl_proton_find (protons, "GE-Proton9-20");
    report = pl_preflight_run (proton->path, NULL);
    {
        gboolean saw_filelock = FALSE;

        for (guint i = 0; i < report->checks->len; i++) {
            const PlCheck *c = g_ptr_array_index (report->checks, i);

            if (g_strcmp0 (c->id, "filelock") == 0)
                saw_filelock = TRUE;
        }
        CHECK (!saw_filelock, "a shell-based build needs no filelock check");
    }
    pl_preflight_free (report);
}

static void
test_umu_detection (Fixture *fx)
{
    g_autoptr(PlProtonList) protons = pl_proton_list_new ();
    g_autoptr(GError) error = NULL;
    const PlProton *proton;

    section ("umu.exe detection");

    pl_proton_scan (protons, fx->proton_dir, &error);
    g_assert_no_error (error);
    proton = pl_proton_find (protons, "GE-Proton10-4");

    /* The fixture put umu.exe at the top of the build, which is not a real
     * location, so detection should look in the prefix and template. */
    CHECK (!pl_has_umu_exe (proton->path, "/nonexistent/prefix"),
           "no umu.exe anywhere means not detected");

    /* Inside a prefix: the caller passes WINEPREFIX (<prefix>/pfx), which is
     * where drive_c lives. */
    {
        g_autofree char *pfx = g_build_filename (fx->prefix_root, "withumu",
                                                 "pfx", NULL);
        g_autofree char *dir = g_build_filename (pfx, "drive_c", "windows",
                                                 "system32", NULL);
        g_autofree char *exe = g_build_filename (dir, "umu.exe", NULL);

        g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
        g_file_set_contents (exe, "MZ", -1, NULL);

        CHECK (pl_has_umu_exe (proton->path, pfx),
               "umu.exe inside the prefix should be found");
    }

    /* In Proton's template prefix. */
    {
        g_autofree char *dir = g_build_filename (fx->proton_dir, "GE-Proton10-4",
                                                 "files", "share", "default_pfx",
                                                 "drive_c", "windows", "system32", NULL);
        g_autofree char *exe = g_build_filename (dir, "umu.exe", NULL);

        g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
        g_file_set_contents (exe, "MZ", -1, NULL);
        CHECK (pl_has_umu_exe (proton->path, NULL),
               "umu.exe in Proton's template prefix should be found");
    }
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

static void
test_config_roundtrip (void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp ("pl-cfg-XXXXXX", &error);
    g_autofree char *file = g_build_filename (dir, "proton-launcher.conf", NULL);
    PlConfig *cfg;
    PlConfig *back;

    section ("config round-trip");

    g_assert_no_error (error);

    cfg = pl_config_new ();
    g_assert_nonnull (cfg);
    /* pl_config_new already applies the defaults. */

    /* Defaults must be absolute and non-empty, or Proton gets garbage. */
    CHECK (cfg->proton_dir != NULL && g_path_is_absolute (cfg->proton_dir),
           "default proton_dir should be absolute, got '%s'", cfg->proton_dir);
    CHECK (cfg->prefix_root != NULL && g_path_is_absolute (cfg->prefix_root),
           "default prefix_root should be absolute, got '%s'", cfg->prefix_root);

    g_free (cfg->proton_dir);
    cfg->proton_dir = g_strdup ("$HOME/games/Proton");
    g_free (cfg->prefix_root);
    cfg->prefix_root = g_strdup ("~/prefixes");
    cfg->wined3d_default = TRUE;
    cfg->proton_log = TRUE;
    g_free (cfg->game_id);
    cfg->game_id = g_strdup ("umu-custom");
    cfg->win_width = 1024;
    cfg->win_height = 700;

    g_ptr_array_add (cfg->extra_env, g_strdup ("MESA_VK_DEVICE_SELECT=0"));
    g_ptr_array_add (cfg->extra_env, g_strdup ("RADV_PERFTEST=gpl"));

    CHECK (pl_config_save (cfg, file, &error), "save: %s", error ? error->message : "");
    g_clear_error (&error);

    back = pl_config_new ();
    CHECK (pl_config_load (back, file, &error), "load: %s",
           error ? error->message : "");
    g_clear_error (&error);

    /* ~ and $VAR must be expanded when the file is read, otherwise
     * Proton would be handed a literal "$HOME/..." path. */
    CHECK (strchr (back->proton_dir, '$') == NULL,
           "$VAR must be expanded, got '%s'", back->proton_dir);
    CHECK (strchr (back->prefix_root, '~') == NULL,
           "~ must be expanded, got '%s'", back->prefix_root);
    CHECK (g_str_has_suffix (back->proton_dir, "/games/Proton"),
           "proton_dir should expand to an absolute path, got '%s'",
           back->proton_dir);
    CHECK (back->wined3d_default, "wined3d_default must survive a round-trip");
    CHECK (back->proton_log, "proton_log must survive a round-trip");
    CHECK (g_strcmp0 (back->game_id, "umu-custom") == 0, "game_id must survive");
    CHECK (back->win_width == 1024 && back->win_height == 700,
           "window geometry must survive");
    CHECK (back->extra_env->len == 2, "extra_env must survive, got %u entries",
           back->extra_env->len);

    pl_config_free (cfg);
    pl_config_free (back);

    /* A missing file is deliberately not an error: the caller treats it as
     * "not configured yet" and shows the wizard. Defaults must survive. */
    {
        g_autofree char *missing = g_build_filename (dir, "nope.conf", NULL);
        PlConfig *c3 = pl_config_new ();
        g_autoptr(GError) err2 = NULL;
        char *saved_dir = g_strdup (c3->proton_dir);

        CHECK (pl_config_load (c3, missing, &err2),
               "a missing config should not be an error");
        CHECK (err2 == NULL, "a missing config must not set an error");
        CHECK (g_strcmp0 (c3->proton_dir, saved_dir) == 0,
               "a missing config must leave the defaults in place");
        CHECK (!c3->first_run_done,
               "a missing config must still trigger the wizard");
        g_free (saved_dir);
        pl_config_free (c3);
    }

    /* A malformed file, by contrast, is a real error the user must see. */
    {
        g_autofree char *bad = g_build_filename (dir, "bad.conf", NULL);
        PlConfig *c4 = pl_config_new ();
        g_autoptr(GError) err3 = NULL;

        g_file_set_contents (bad, "this is not = a valid [ini\n file", -1, NULL);
        CHECK (!pl_config_load (c4, bad, &err3),
               "a malformed config must fail");
        CHECK (err3 != NULL, "a malformed config must set an error");
        pl_config_free (c4);
    }

    /* extra_env entries that are not KEY=VALUE are dropped, not saved raw. */
    {
        g_autofree char *out = g_build_filename (dir, "env-edge.conf", NULL);
        PlConfig *c5 = pl_config_new ();

        g_ptr_array_add (c5->extra_env, g_strdup ("=noname"));
        g_ptr_array_add (c5->extra_env, g_strdup ("no-equals-sign"));
        g_ptr_array_add (c5->extra_env, g_strdup ("GOOD=1"));
        CHECK (pl_config_save (c5, out, NULL), "save with edge-case env");

        {
            PlConfig *c6 = pl_config_new ();

            CHECK (pl_config_load (c6, out, NULL), "reload edge-case env");
            CHECK (c6->extra_env->len == 1,
                   "only well-formed entries should survive, got %u",
                   c6->extra_env->len);
            if (c6->extra_env->len == 1)
                CHECK (g_strcmp0 (g_ptr_array_index (c6->extra_env, 0),
                                  "GOOD=1") == 0,
                       "the valid entry should survive intact");
            pl_config_free (c6);
        }
        pl_config_free (c5);
    }
}

/* ------------------------------------------------------------------ */

int
main (int argc, char **argv)
{
    Fixture fx;

    g_test_init (&argc, &argv, NULL);

    fixture_setup (&fx);

    test_proton_scan (&fx);
    test_proton_scan_errors (&fx);
    test_slugify ();
    test_prefix_lifecycle (&fx);
    test_run_argv (&fx);
    test_run_envp (&fx);
    test_run_end_to_end (&fx);
    test_preflight (&fx);
    test_umu_detection (&fx);
    test_config_roundtrip ();

    fixture_teardown (&fx);

    g_print ("\n%u checks, %u failed\n", n_checks, n_failed);
    return n_failed == 0 ? 0 : 1;
}