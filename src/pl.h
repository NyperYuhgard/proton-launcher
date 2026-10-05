/* pl.h - Core types and API for Proton Launcher.
 *
 * Proton Launcher runs Proton-GE builds directly, without Steam.
 * See README.md for the environment contract expected by Proton.
 */
/* Request POSIX interfaces (lstat, access, kill, ...) explicitly: the
 * sources compile as strict C11, which otherwise hides these symbols. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define PL_APP_ID          "io.github.nyper.protonlauncher"
#define PL_APP_NAME        "Proton Launcher"
#define PL_VERSION         "2.0.0"

/* ------------------------------------------------------------------ */
/* Proton builds                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char    *id;        /* directory name, e.g. "GE-Proton10-4"   */
    char    *path;      /* absolute path to the Proton build       */
    char    *version;   /* contents of <path>/version, or NULL     */
    gboolean usable;    /* passed validation                       */
    char    *problem;   /* why it is unusable, or NULL             */
} PlProton;

typedef struct {
    GPtrArray *items;   /* PlProton*, newest version first         */
} PlProtonList;

PlProtonList *pl_proton_list_new   (void);
void          pl_proton_list_free  (PlProtonList *self);
void          pl_proton_list_clear (PlProtonList *self);

/* Scan 'dir' for Proton builds. Never fails: an empty list with a
 * populated error is a valid result when the directory is unusable.
 * Returns TRUE when at least one build was found. */
gboolean      pl_proton_scan       (PlProtonList *self,
                                    const char   *dir,
                                    GError      **error);
const PlProton *pl_proton_find     (PlProtonList *self, const char *id);
/* Newest build by natural version ordering, or NULL when empty. */
const PlProton *pl_proton_latest   (PlProtonList *self);

/* ------------------------------------------------------------------ */
/* Prefixes                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char    *id;        /* directory name under the prefix root      */
    char    *path;      /* STEAM_COMPAT_DATA_PATH for this prefix     */
    char    *pfx;       /* WINEPREFIX: <path>/pfx                     */
    char    *label;     /* display label                              */
    gboolean seeded;    /* Proton has initialised it (version file)   */
} PlPrefix;

typedef struct {
    GPtrArray *items;   /* PlPrefix*                                  */
} PlPrefixList;

PlPrefixList *pl_prefix_list_new   (void);
void          pl_prefix_list_free  (PlPrefixList *self);
void          pl_prefix_list_scan (PlPrefixList *self, const char *root);
const PlPrefix *pl_prefix_find_by_id (PlPrefixList *self, const char *id);
/* Newest entry, i.e. the one with the highest mtime. */
const PlPrefix *pl_prefix_latest    (PlPrefixList *self);

/* Create an empty prefix directory ready for Proton to populate. */
gboolean pl_prefix_create (const char *root, const char *id, GError **error);
/* Recursive delete, symlinks are unlinked rather than followed. */
gboolean pl_prefix_remove (const char *root, const char *id, GError **error);
/* Turn a display label into a filesystem-safe id. */
char    *pl_prefix_slugify (const char *label);

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    PL_VERB_RUN,           /* proton run <exe>            - launch a game  */
    PL_VERB_RUNINPREFIX,   /* proton runinprefix <argv..> - Wine tools     */
    PL_VERB_DESTROYPREFIX  /* proton destroyprefix       - clean tracked  */
} PlVerb;

typedef struct {
    PlVerb        verb;
    const PlProton *proton;
    const PlPrefix *prefix;
    const char   *exe;          /* host path to the .exe, or NULL      */
    const char  *working_dir;   /* defaults to the .exe directory      */
    GPtrArray    *args;         /* extra argv entries (char*, NULL-t.) */
    gboolean      wined3d;      /* PROTON_USE_WINED3D=1                 */
    const char   *game_id;      /* UMU_ID, e.g. "umu-default"           */
} PlRunRequest;

typedef struct _PlRunner PlRunner;

typedef void (*PlRunnerLineFunc)   (const char *line, void *user_data);
typedef void (*PlRunnerExitFunc)   (gboolean normal_exit, int status, void *user_data);

PlRunner *pl_runner_new         (void);
void      pl_runner_free        (PlRunner *self);
gboolean  pl_runner_is_running  (PlRunner *self);

/* Build the argv and environment Proton expects. Exposed so the log view
 * can show exactly what will be executed. */
char    **pl_run_build_argv     (const PlRunRequest *req, GError **error);
char    **pl_run_build_envp     (const PlRunRequest *req,
                                 const char          *steam_stub,
                                 gboolean             proton_log,
                                 GError             **error);

/* Start the request. 'on_line' receives merged stdout/stderr chunks
 * (NUL-terminated, may contain several lines); 'on_exit' fires once.
 * Returns FALSE and sets 'error' when the child could not be spawned. */
gboolean  pl_run_start          (PlRunner            *self,
                                 const PlRunRequest  *req,
                                 const char          *steam_stub,
                                 gboolean             proton_log,
                                 PlRunnerLineFunc     on_line,
                                 PlRunnerExitFunc     on_exit,
                                 void                *user_data,
                                 GError             **error);
void      pl_runner_cancel     (PlRunner *self);

/* ------------------------------------------------------------------ */
/* Preflight dependency checks                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    PL_CHECK_OK,
    PL_CHECK_FAIL,
    PL_CHECK_WARN
} PlCheckLevel;

typedef struct _PlCheck PlCheck;

struct _PlCheck {
    char          *id;
    char          *title;
    PlCheckLevel   level;
    char          *detail;    /* what we found                        */
    char          *remedy;    /* what the user should do, or NULL     */
};

typedef struct {
    GPtrArray *checks;         /* PlCheck*                              */
    gboolean    can_run;       /* every hard requirement passed         */
} PlReport;

PlReport *pl_preflight_run    (const char *proton_path, const char *pfx_path);
void      pl_preflight_free   (PlReport *self);
gboolean  pl_preflight_can_run(PlReport *self);
/* One-line summary suitable for a status bar. */
char     *pl_preflight_summary(PlReport *self);

/* True when 'umu.exe' exists either in the prefix or in Proton's
 * template prefix. Proton needs it to start non-Steam executables. */
gboolean  pl_has_umu_exe     (const char *proton_path, const char *pfx_path);

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char     *proton_dir;       /* directory holding the Proton builds */
    char     *proton_version;   /* selected build id, or NULL = latest */
    char     *prefix_root;      /* where prefixes live                 */
    char     *log_dir;          /* Proton debug logs                   */
    char     *game_id;          /* UMU_ID                              */
    gboolean  wined3d_default;
    gboolean  proton_log;       /* set PROTON_LOG=1                   */
    gboolean  first_run_done;
    GPtrArray *extra_env;       /* char* "KEY=VALUE"                   */
    int       win_width;
    int       win_height;
    gboolean  win_maximized;
} PlConfig;

PlConfig   *pl_config_new      (void);
void        pl_config_free     (PlConfig *self);
void        pl_config_set_defaults (PlConfig *self);
gboolean    pl_config_load     (PlConfig *self, const char *file, GError **error);
gboolean    pl_config_save     (PlConfig *self, const char *file, GError **error);

/* Default location of the config file, honouring XDG_CONFIG_HOME. */
char       *pl_config_default_path (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PlProtonList, pl_proton_list_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PlPrefixList, pl_prefix_list_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PlRunner,     pl_runner_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PlReport,     pl_preflight_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PlConfig,     pl_config_free)

G_END_DECLS