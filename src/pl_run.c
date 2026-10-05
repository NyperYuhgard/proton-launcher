/* pl_run.c - Build the Proton invocation and supervise the child process.
 *
 * Environment contract derived from reading Proton's own source:
 *
 *   STEAM_COMPAT_DATA_PATH            mandatory; Proton exits 1 without it.
 *                                    Its prefix lives at $path/pfx.
 *   STEAM_COMPAT_CLIENT_INSTALL_PATH  mandatory; read unconditionally
 *                                    during prefix setup, and used as the
 *                                    mount point for the "t:" drive.
 *   UMU_ID + UMU_USE_STEAM!=1         selects the non-Steam launch path.
 *                                    Without UMU_ID, `proton run` starts
 *                                    steam.exe instead of the game.
 *   PROTON_USE_WINED3D=1              OpenGL wined3d instead of DXVK/vkd3d.
 *
 * Wine utilities (winecfg, regedit, explorer, taskmgr) must use the
 * `runinprefix` verb: `run` would route them through the game launcher.
 */

#include "pl.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

struct _PlRunner {
    guint   watch_stdout;
    guint   watch_stderr;
    guint   watch_child;
    GPid    pid;
    gboolean running;
    gboolean cancelled;

    GIOChannel *out_ch;
    GIOChannel *err_ch;
    GString    *pending;

    PlRunnerLineFunc on_line;
    PlRunnerExitFunc on_exit;
    void           *user_data;
};

/* ------------------------------------------------------------------ */
/* argv / env construction                                             */
/* ------------------------------------------------------------------ */

static char *
proton_script (const PlRunRequest *req)
{
    return g_build_filename (req->proton->path, "proton", NULL);
}

char **
pl_run_build_argv (const PlRunRequest *req, GError **error)
{
    GPtrArray *argv;
    char      *script;
    const char *verb;

    g_return_val_if_fail (req != NULL, NULL);
    g_return_val_if_fail (req->proton != NULL, NULL);

    /* Note: argv does not depend on the prefix, only the environment does,
     * so a missing prefix is reported by pl_run_build_envp instead. That
     * keeps this function usable for showing a command preview before a
     * prefix has been chosen. */
    script = proton_script (req);

    argv = g_ptr_array_new_with_free_func (g_free);
    g_ptr_array_add (argv, script);

    switch (req->verb) {
    case PL_VERB_RUN:          verb = "run";           break;
    case PL_VERB_RUNINPREFIX:  verb = "runinprefix";   break;
    case PL_VERB_DESTROYPREFIX: verb = "destroyprefix"; break;
    default:
        g_ptr_array_unref (argv);
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                             "Unsupported verb");
        return NULL;
    }
    g_ptr_array_add (argv, g_strdup (verb));

    if (req->verb == PL_VERB_RUN) {
        if (req->exe == NULL || *req->exe == '\0') {
            g_ptr_array_unref (argv);
            g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                                 "No executable selected");
            return NULL;
        }
        /* Absolute host path: Proton's launcher expects one and converts
         * it to a Windows path itself. */
        g_ptr_array_add (argv, g_strdup (req->exe));
    } else if (req->verb == PL_VERB_RUNINPREFIX) {
        if (req->args == NULL || req->args->len == 0) {
            g_ptr_array_unref (argv);
            g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                                 "No command given");
            return NULL;
        }
        for (guint i = 0; i < req->args->len; i++)
            g_ptr_array_add (argv, g_strdup (g_ptr_array_index (req->args, i)));
    }

    g_ptr_array_add (argv, NULL);
    return (char **) g_ptr_array_free (argv, FALSE);
}

static void
env_add (GPtrArray *env, const char *key, const char *value)
{
    if (value != NULL)
        g_ptr_array_add (env, g_strdup_printf ("%s=%s", key, value));
}

char **
pl_run_build_envp (const PlRunRequest *req,
                   const char          *steam_stub,
                   gboolean             proton_log,
                   GError             **error)
{
    GPtrArray *env;
    g_auto(GStrv) inherited = NULL;
    const char *game_id;

    g_return_val_if_fail (req != NULL, NULL);
    g_return_val_if_fail (req->proton != NULL, NULL);

    /* Not a g_return_if_fail: a missing prefix is a normal user-facing
     * state (nothing created yet), so report it as an error the caller
     * can show rather than a programming fault. */
    if (req->prefix == NULL) {
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                             "No prefix selected");
        return NULL;
    }

    env = g_ptr_array_new_with_free_func (g_free);

    /* Start from the parent environment so DISPLAY, XAUTHORITY, DBUS and
     * the Vulkan loader are all still reachable. */
    inherited = g_get_environ ();
    for (gsize i = 0; inherited != NULL && inherited[i] != NULL; i++) {
        const char *entry = inherited[i];

        /* Skip variables we are about to define ourselves, so a stale
         * value inherited from a shell cannot win. */
        if (g_str_has_prefix (entry, "STEAM_COMPAT_") ||
            g_str_has_prefix (entry, "PROTON_") ||
            g_str_has_prefix (entry, "UMU_") ||
            g_str_has_prefix (entry, "WINEPREFIX="))
            continue;

        g_ptr_array_add (env, g_strdup (entry));
    }

    /* Proton reads this first and exits 1 when it is missing. */
    env_add (env, "STEAM_COMPAT_DATA_PATH", req->prefix->path);

    /* Proton dereferences this unconditionally while setting up the
     * prefix, so it must name a directory that actually exists. */
    if (steam_stub != NULL)
        env_add (env, "STEAM_COMPAT_CLIENT_INSTALL_PATH", steam_stub);

    /* Selects the non-Steam path. Without UMU_ID, `proton run` launches
     * steam.exe with the game as an argument, which fails here because
     * there is no Steam client to satisfy the IPC handshake. */
    game_id = (req->game_id != NULL && *req->game_id != '\0')
            ? req->game_id : "umu-default";
    env_add (env, "UMU_ID", game_id);
    env_add (env, "UMU_USE_STEAM", "0");

    /* Proton rewrites WINEPREFIX itself from STEAM_COMPAT_DATA_PATH, but
     * tools invoked outside of `run` still read it. */
    env_add (env, "WINEPREFIX", req->prefix->pfx);

    if (req->wined3d) {
        /* OpenGL-based wined3d rather than Vulkan DXVK/vkd3d. Proton
         * rewrites the DllOverrides itself, so no registry edits are
         * needed here. */
        env_add (env, "PROTON_USE_WINED3D", "1");
    }

    if (proton_log) {
        /* Proton's logger keys the filename off SteamGameId and returns
         * early without it, so supply a synthetic one. */
        env_add (env, "PROTON_LOG", "1");
        env_add (env, "SteamGameId", "0");
        env_add (env, "SteamAppId", "0");
    }

    g_ptr_array_add (env, NULL);
    return (char **) g_ptr_array_free (env, FALSE);
}

/* g_clear_pointer needs a single-argument destructor. */
static void
free_pending (GString *string)
{
    if (string != NULL)
        g_string_free (string, TRUE);
}

/* ------------------------------------------------------------------ */
/* Process supervision                                                 */
/* ------------------------------------------------------------------ */

static void
emit_pending (PlRunner *self)
{
    char *chunk;

    if (self->pending == NULL || self->on_line == NULL)
        return;

    chunk = g_string_free (self->pending, FALSE);
    self->pending = g_string_new (NULL);

    if (*chunk != '\0')
        self->on_line (chunk, self->user_data);

    g_free (chunk);
}

/* Upper bound on reads per callback. Keeps a noisy Proton from holding the
 * main loop while it drains a large backlog; the watch is level-triggered, so
 * whatever is left is picked up on the next dispatch. */
#define PL_READ_CHUNKS_PER_CALLBACK 16

/* Read a pipe to exhaustion. Safe because the fd is non-blocking: this
 * terminates on G_IO_STATUS_AGAIN or EOF instead of waiting for the writer.
 *
 * Needed because on_channel_ready() caps how much it reads per dispatch, so
 * data can still be sitting in the pipe when it stops. */
static void
pl_channel_drain (GIOChannel *channel, PlRunner *self)
{
    char      buf[4096];
    gsize     bytes_read = 0;
    GIOStatus status;

    if (channel == NULL)
        return;

    do {
        status = g_io_channel_read_chars (channel, buf, sizeof (buf),
                                          &bytes_read, NULL);
        if (bytes_read > 0 && self->pending != NULL)
            g_string_append_len (self->pending, buf, (gssize) bytes_read);
    } while (status == G_IO_STATUS_NORMAL && bytes_read > 0);
}

static gboolean
on_channel_ready (GIOChannel   *source,
                  GIOCondition  condition,
                  gpointer      user_data)
{
    PlRunner *self = user_data;
    char      buf[4096];
    gsize     bytes_read = 0;
    GIOStatus status;
    int       chunks = 0;

    if (self->pending == NULL)
        self->pending = g_string_new (NULL);

    /* The fd is non-blocking (see pl_channel_watch), so this loop always
     * terminates on G_IO_STATUS_AGAIN once the pipe runs dry instead of
     * parking the main loop inside read() until the child writes again. */
    do {
        status = g_io_channel_read_chars (source, buf, sizeof (buf),
                                          &bytes_read, NULL);
        if (bytes_read > 0)
            g_string_append_len (self->pending, buf, (gssize) bytes_read);
        chunks++;
    } while (status == G_IO_STATUS_NORMAL && bytes_read > 0 &&
             chunks < PL_READ_CHUNKS_PER_CALLBACK);

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        /* All writers are gone, so the tail of the output is still readable
         * but will never trigger another G_IO_IN. Take it now. */
        pl_channel_drain (source, self);
    }

    emit_pending (self);

    if (condition & (G_IO_HUP | G_IO_ERR)) {
        /* Hand the channel back to whoever owns it and clear the stored id:
         * returning G_SOURCE_REMOVE destroys the source, so on_child_exit()
         * must not try to remove it again. Match on the channel, since this
         * callback serves both pipes. */
        if (source == self->out_ch) {
            self->watch_stdout = 0;
            self->out_ch = NULL;
        } else if (source == self->err_ch) {
            self->watch_stderr = 0;
            self->err_ch = NULL;
        }

        g_io_channel_shutdown (source, FALSE, NULL);
        g_io_channel_unref (source);   /* close_on_unref closes the fd */
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Wrap one pipe end in a GIOChannel and start watching it.
 *
 * g_spawn_async_with_pipes() hands back *blocking* descriptors, and
 * g_io_channel_unix_new() does not change that. With a blocking fd the read
 * loop above would sit inside read() for as long as Proton has nothing to say
 * -- which is the whole game session -- holding the GTK main loop and
 * freezing the window until the child exits. Setting O_NONBLOCK makes the read
 * return G_IO_STATUS_AGAIN instead, so the watch releases the main loop
 * immediately and the UI stays responsive while the game runs.
 */
static guint
pl_channel_watch (gint          fd,
                  GIOChannel  **channel_out,
                  PlRunner     *self)
{
    GIOChannel *channel;
    int         flags;

    *channel_out = NULL;

    if (fd < 0)
        return 0;

    flags = fcntl (fd, F_GETFL, 0);
    if (flags != -1)
        (void) fcntl (fd, F_SETFL, flags | O_NONBLOCK);

    channel = g_io_channel_unix_new (fd);
    g_io_channel_set_encoding (channel, NULL, NULL);
    g_io_channel_set_buffered (channel, FALSE);
    g_io_channel_set_close_on_unref (channel, TRUE);

    *channel_out = channel;

    return g_io_add_watch (channel, G_IO_IN | G_IO_HUP | G_IO_ERR,
                           on_channel_ready, self);
}

static void
on_child_exit (GPid  pid,
               gint  status,
               gpointer user_data)
{
    PlRunner *self = user_data;

    g_spawn_close_pid (pid);

    /* Take whatever the pipes still hold before tearing the watches down,
     * then flush it, so no output is lost on the last burst. */
    pl_channel_drain (self->out_ch, self);
    pl_channel_drain (self->err_ch, self);
    emit_pending (self);

    if (self->watch_stdout != 0) {
        g_source_remove (self->watch_stdout);
        self->watch_stdout = 0;
    }
    if (self->watch_stderr != 0) {
        g_source_remove (self->watch_stderr);
        self->watch_stderr = 0;
    }

    g_clear_pointer (&self->out_ch, g_io_channel_unref);
    g_clear_pointer (&self->err_ch, g_io_channel_unref);
    g_clear_pointer (&self->pending, free_pending);

    self->running   = FALSE;
    self->watch_child = 0;
    self->pid = 0;

    if (self->on_exit != NULL) {
        if (self->cancelled)
            self->on_exit (TRUE, 0, self->user_data);
        else
            self->on_exit (WIFEXITED (status), WIFEXITED (status) ? WEXITSTATUS (status) : -1,
                           self->user_data);
    }
}

gboolean
pl_run_start (PlRunner           *self,
              const PlRunRequest *req,
              const char         *steam_stub,
              gboolean            proton_log,
              PlRunnerLineFunc    on_line,
              PlRunnerExitFunc    on_exit,
              void               *user_data,
              GError            **error)
{
    g_auto(GStrv) argv = NULL;
    g_auto(GStrv) envp = NULL;
    gint          out_fd = -1;
    gint          err_fd = -1;
    GPid          pid = 0;
    char         *cwd = NULL;

    g_return_val_if_fail (self != NULL, FALSE);

    if (self->running) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                             "A session is already running");
        return FALSE;
    }

    argv = pl_run_build_argv (req, error);
    if (argv == NULL)
        return FALSE;

    envp = pl_run_build_envp (req, steam_stub, proton_log, error);
    if (envp == NULL)
        return FALSE;

    /* Proton chdirs around, but many games resolve data files relative to
     * the executable, so start from that directory when we know it. */
    if (req->working_dir != NULL && *req->working_dir != '\0') {
        if (g_file_test (req->working_dir, G_FILE_TEST_IS_DIR))
            cwd = g_strdup (req->working_dir);
    }

    self->on_line     = on_line;
    self->on_exit     = on_exit;
    self->user_data   = user_data;
    self->cancelled   = FALSE;
    self->pending     = g_string_new (NULL);

    if (!g_spawn_async_with_pipes (cwd, argv, envp,
                                   G_SPAWN_DO_NOT_REAP_CHILD,
                                   NULL, NULL, &pid,
                                   NULL, &out_fd, &err_fd, error)) {
        g_clear_pointer (&self->pending, free_pending);
        return FALSE;
    }
    g_free (cwd);

    self->pid     = pid;
    self->running = TRUE;

    self->watch_stdout = pl_channel_watch (out_fd, &self->out_ch, self);
    self->watch_stderr = pl_channel_watch (err_fd, &self->err_ch, self);

    self->watch_child = g_child_watch_add (pid, on_child_exit, self);

    return TRUE;
}

void
pl_runner_cancel (PlRunner *self)
{
    g_return_if_fail (self != NULL);

    if (!self->running || self->pid == 0)
        return;

    self->cancelled = TRUE;

    /* Proton spawns wine under its own process group; signal the group so
     * wineserver and any child windows come down too. */
    kill (-self->pid, SIGTERM);
    kill (self->pid, SIGTERM);
}

PlRunner *
pl_runner_new (void)
{
    return g_new0 (PlRunner, 1);
}

void
pl_runner_free (PlRunner *self)
{
    if (self == NULL)
        return;

    if (self->running)
        pl_runner_cancel (self);

    g_clear_pointer (&self->out_ch, g_io_channel_unref);
    g_clear_pointer (&self->err_ch, g_io_channel_unref);
    g_clear_pointer (&self->pending, free_pending);
    g_free (self);
}

gboolean
pl_runner_is_running (PlRunner *self)
{
    return self != NULL && self->running;
}