# Proton Launcher

Run Proton builds and Windows games on Linux without Steam.

Proton Launcher is a GTK3 desktop application written in C. It picks a Proton
build, a Wine prefix and an executable, then invokes Proton directly with the
environment Proton expects. It does not download Proton, does not install
umu-launcher, and does not modify anything outside the folders you choose.

## What it does

- Discovers GE-Proton builds in a folder you pick and orders them by natural
  version, so `GE-Proton10-4` sorts above `GE-Proton9-20`.
- Manages one prefix per game, so installed programs, settings and saves do
  not interfere with each other.
- Runs a `.exe` with `proton run`, or opens Wine utilities with
  `proton runinprefix` (winecfg, regedit, explorer, taskmgr, control, cmd,
  notepad, and `wineboot -u` to update a prefix).
- Switches between DXVK/vkd3d-Proton and OpenGL wined3d per session.
- Runs a preflight that reports the dependency problems which otherwise show
  up as a Python traceback in the log.
- Forwards arbitrary `KEY=VALUE` variables to Proton.

## Requirements

| Requirement | Why |
|---|---|
| A Proton build, already downloaded and extracted | Nothing is fetched for you. Download a release from [proton-ge-custom](https://github.com/GloriousEggroll/proton-ge-custom). |
| Python 3 with the `filelock` module | Current GE-Proton ships `proton` as a Python 3 script that imports `filelock` at startup. Without it, every launch dies immediately. On Debian/Ubuntu: `sudo apt install python3-filelock`. |
| A Vulkan-capable GPU and driver | DXVK and vkd3d-Proton are Vulkan-only. WineD3D works without it, but is much slower and has worse compatibility. |

umu-launcher is **not** required and is not installed. Proton's own script is
invoked directly, which is what GE-Proton documents as the supported path.

## The environment contract

This is the part that matters, and it is why Proton Launcher exists at all.
Proton is written assuming it is running under Steam and umu, and it fails in
confusing ways when it is not. These are the variables it reads, taken from
reading its source:

| Variable | Set to | Why |
|---|---|---|
| `STEAM_COMPAT_DATA_PATH` | the prefix directory | Proton exits with code 1 when this is missing. The prefix lives at `$STEAM_COMPAT_DATA_PATH/pfx`. |
| `STEAM_COMPAT_CLIENT_INSTALL_PATH` | a real stub directory | Read unconditionally during prefix setup, so a missing value raises `KeyError`. Proton mounts it as the `t:` drive. Proton Launcher creates an empty directory at `$XDG_DATA_HOME/proton-launcher/steam-stub` for this. |
| `UMU_ID` | your game id, default `umu-default` | Selects the non-Steam launch path. |
| `UMU_USE_STEAM` | `0` | Explicitly opts out of the Steam client handshake. |
| `PROTON_USE_WINED3D` | `1`, only for WineD3D | OpenGL wined3d instead of DXVK/vkd3d. Proton rewrites its own `DllOverrides`, so no registry edits are needed. |
| `PROTON_LOG`, `SteamGameId`, `SteamAppId` | `1`, `0`, `0` | Proton's logger keys its filename off `SteamGameId` and returns early without it, so a synthetic id is required to get any log at all. |
| `WINEPREFIX` | `<prefix>/pfx` | Proton derives this itself, but Wine tools invoked outside of `run` still read it. |

The most important of these is `UMU_ID`. Without it, `proton run <exe>`
launches `steam.exe` with your game as an argument instead of launching the
game, which cannot work here because there is no Steam client to complete the
IPC handshake.

`src/pl_run.c` builds this environment and is the reference implementation.
The tests assert the exact argv and envp, so if Proton's expectations change,
`make check` fails until they are updated.

## Building

```sh
make            # build/proton-launcher
make check      # core unit tests, no display or Proton needed
make check-gui  # GTK smoke test, skips itself when there is no display
make install    # to $(PREFIX)/bin, default /usr/local
make clean
```

Dependencies for compiling: `libgtk-3-dev`, `pkg-config`.

To build with sanitizers:

```sh
make clean
make CFLAGS="-O1 -g -fsanitize=address,undefined" \
     LDFLAGS="-fsanitize=address,undefined" check-asan
```

## Running

```sh
proton-launcher                    # start the GUI
proton-launcher --version
proton-launcher /path/to/game.exe  # prefill the executable field
```

On first run a setup wizard asks for the folder containing your Proton builds,
where prefixes should live, and where to write Proton logs. Every path is a
folder chooser with the well-known locations offered as presets. All of them
stay editable in Preferences afterwards.

Settings are stored in `$XDG_CONFIG_HOME/proton-launcher/config.ini`. Values
may use `~` and `$VAR`, which are expanded when the file is read.

## Building the AppImage

```sh
./packaging/build-appimage.sh
```

Produces `dist/proton-launcher-<version>-x86_64.AppImage`. The script downloads
`appimagetool` and `linuxdeploy` into `packaging/.tools/` on first run, so it
needs network access once and must not be run as root.

The AppImage bundles the application only. Proton builds, prefixes and logs
stay on your disk, so the image is small and nothing machine-specific is baked
in. GTK's own libraries are bundled, so the image runs on systems without
GTK3 installed.

Optional: install `libgdk-pixbuf2.0-bin` so the build can bundle the gdk-pixbuf
loader modules. Without it the AppImage falls back to the host's loaders, which
is fine on a normal desktop system.

## Layout

```
src/pl.h            core types and the environment contract
src/pl_run.c        argv/envp construction and child supervision
src/pl_proton.c     build discovery and validation
src/pl_prefix.c     prefix create/scan/remove
src/pl_preflight.c  dependency checks
src/pl_config.c     config.ini handling
src/ui_window.c     main window
src/ui_wizard.c     first-run setup
src/ui_prefs.c      preferences dialog
tests/test-core.c   headless unit tests
tests/test-gui.c    GTK smoke test
```

The core has no GTK dependency, which is what lets `make check` run headless.

## Scope

Deliberately not included: automatic Proton downloads, installing umu-launcher,
vkd3d/warp shim management, and diagnostics beyond the preflight checks. These
were either broken in the previous implementation or not needed for running
games.

## Licence

Proton is a separate project with its own licence; this launcher bundles none
of it.