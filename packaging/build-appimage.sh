#!/bin/bash
# build-appimage.sh - package Proton Launcher as an AppImage.
#
# The AppImage bundles the application only. Proton builds, prefixes and logs
# stay on the user's disk and are pointed at from the first-run wizard, so
# nothing large or machine-specific is ever baked into the image.
#
# Usage:  ./packaging/build-appimage.sh
# Result: dist/proton-launcher-2.0.0-x86_64.AppImage
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="$(sed -n 's/^VERSION *:= *//p' Makefile | head -1)"
ARCH="$(uname -m)"

# Multiarch triplet, so i386 loaders do not leak into the x86_64 image.
LIBDIR="$(gcc -print-multiarch 2>/dev/null || echo x86_64-linux-gnu)"
TOOLS="packaging/.tools"
APP_NAME="proton-launcher"
APP_DIR="dist/AppDir"
OUT_DIR="dist"

# Only x86_64 is built here. AppImageKit 13 dropped the current asset name in
# favour of "obsolete-", so pin 12 for a stable tag; linuxdeploy's newest
# published tag is 1-alpha-20251107-1.
APPIMAGETOOL_VERSION="12"
APPIMAGETOOL_URL="https://github.com/AppImage/AppImageKit/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"
LINUXDEPLOY_VERSION="1-alpha-20251107-1"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage"

[ "$ARCH" = "x86_64" ] || die "only x86_64 is supported by this script"

say() { printf '\033[1;33m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ #
# Sanity checks                                                       #
# ------------------------------------------------------------------ #

command -v pkg-config >/dev/null || die "pkg-config is required"
pkg-config --exists gtk+-3.0 glib-2.0 gio-2.0 ||
    die "GTK3 development files are missing (install libgtk-3-dev)"
command -v curl >/dev/null || command -v wget >/dev/null ||
    die "either curl or wget is required to fetch the tooling"

# ------------------------------------------------------------------ #
# Fetch the AppImage tooling                                          #
# ------------------------------------------------------------------ #

fetch() {
    local url="$1" dest="$2"

    if [ -x "$dest" ]; then
        say "using cached $(basename "$dest")"
        return
    fi

    say "downloading $(basename "$dest")"
    if ! curl -fL --retry 3 -o "$dest.part" "$url"; then
        wget -O "$dest.part" "$url" || die "could not download $url"
    fi
    chmod +x "$dest.part"
    mv "$dest.part" "$dest"
}

mkdir -p "$TOOLS"
fetch "$APPIMAGETOOL_URL" "$TOOLS/appimagetool"
fetch "$LINUXDEPLOY_URL" "$TOOLS/linuxdeploy"

# AppImageKit refuses to run as root unless it is told otherwise.
export APPIMAGE_EXTRACT_AND_RUN=1
if [ "$(id -u)" -eq 0 ]; then
    say "running as root: AppImageKit will refuse unless you allow it"
    die "build as a normal user instead"
fi

# ------------------------------------------------------------------ #
# Build the application                                               #
# ------------------------------------------------------------------ #

say "compiling $APP_NAME $VERSION"
make clean >/dev/null
make CFLAGS="-O2 -g" >/dev/null

# ------------------------------------------------------------------ #
# Assemble AppDir                                                     #
# ------------------------------------------------------------------ #

say "assembling AppDir"
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/usr/bin" "$APP_DIR/usr/share/applications" \
         "$APP_DIR/usr/share/icons/hicolor/scalable/apps"

install -m 0755 "build/$APP_NAME" "$APP_DIR/usr/bin/$APP_NAME"
install -m 0644 data/"$APP_NAME".desktop \
    "$APP_DIR/usr/share/applications/$APP_NAME.desktop"
install -m 0644 data/"$APP_NAME".svg \
    "$APP_DIR/usr/share/icons/hicolor/scalable/apps/$APP_NAME.svg"

# Icons at the AppImage root are what desktops look for when the AppImage is
# mounted at an arbitrary point.
install -m 0644 data/"$APP_NAME".svg "$APP_DIR/$APP_NAME.svg"
install -m 0644 data/"$APP_NAME".svg "$APP_DIR/.DirIcon"

install -m 0755 packaging/AppRun "$APP_DIR/AppRun"

# linuxdeploy copies the GTK3 stack and rewrites rpaths to $ORIGIN. The
# gtk plugin (gdk-pixbuf loaders, hicolor icons) has no published releases
# anymore, so those two pieces are wired up by hand below.
say "bundling libraries with linuxdeploy (this takes a minute)"
"$TOOLS/linuxdeploy" \
    --appdir "$APP_DIR" \
    --executable="$APP_DIR/usr/bin/$APP_NAME" \
    --desktop-file="$APP_DIR/usr/share/applications/$APP_NAME.desktop" \
    --icon-file="$APP_DIR/usr/share/icons/hicolor/scalable/apps/$APP_NAME.svg" \
    --custom-apprun=packaging/AppRun \
    --output appimage \
    >/dev/null

# gdk-pixbuf resolves its loader modules at runtime. Without them the AppImage
# starts but draws no icons, and GTK logs "Could not load a pixbuf loader".
# The cache format only accepts absolute paths, so it is written with a
# placeholder that AppRun substitutes for the real mount point.
BUNDLE_DIR="$APP_DIR/usr/lib/$APP_NAME/gdk-pixbuf"
LOADER_CACHE="$BUNDLE_DIR/loaders.cache"
mkdir -p "$BUNDLE_DIR"

LOADERS=()
while IFS= read -r -d '' module; do
    LOADERS+=("$module")
    cp "$module" "$BUNDLE_DIR/"
done < <(find "/usr/lib/$LIBDIR" -path '*/gdk-pixbuf-2.0/*/loaders/*.so' -print0 2>/dev/null)

if [ ${#LOADERS[@]} -eq 0 ]; then
    say "WARNING: no gdk-pixbuf loaders found; icons may not render"
elif ! command -v gdk-pixbuf-query-loaders >/dev/null; then
    # Without the query tool there is no way to build a loaders.cache, and a
    # hand-written one is fragile: gdk-pixbuf falls back to the host's
    # loaders, which is fine on a normal desktop system.
    say "WARNING: gdk-pixbuf-query-loaders is not installed, skipping the cache"
    say "         (install libgdk-pixbuf2.0-bin to bundle the loaders)"
else
    say "bundling ${#LOADERS[@]} gdk-pixbuf loaders"

    # Query from the copies, then rewrite their absolute path to a
    # placeholder: loaders.cache only understands absolute paths, and the
    # AppImage's mount point is not known until run time.
    gdk-pixbuf-query-loaders "$BUNDLE_DIR"/*.so \
        | sed "s|\"$BUNDLE_DIR|\"@@APPDIR@@/usr/lib/$APP_NAME/gdk-pixbuf|g" \
        > "$LOADER_CACHE"

    chmod 0644 "$LOADER_CACHE"
fi

# ------------------------------------------------------------------ #
# Turn it into an AppImage                                            #
# ------------------------------------------------------------------ #

mkdir -p "$OUT_DIR"
say "building the AppImage"
ARCH="$ARCH" "$TOOLS/appimagetool" "$APP_DIR" "$OUT_DIR/$APP_NAME-$VERSION-$ARCH.AppImage"

say "done: $OUT_DIR/$APP_NAME-$VERSION-$ARCH.AppImage"
ls -lh "$OUT_DIR/$APP_NAME-$VERSION-$ARCH.AppImage"