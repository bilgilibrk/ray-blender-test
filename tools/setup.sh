#!/usr/bin/env bash
# Fetches and builds everything under vendor/ that the repository does not carry:
# raylib (as static libs for both targets) and, if the models are missing, the
# Kenney Racing Kit.
#
#   tools/setup.sh
#
# Re-running is safe; existing artifacts are left alone unless --force is given.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$REPO/vendor"
RAYLIB_VERSION="5.5"
KIT_URL="https://kenney.nl/media/pages/assets/racing-kit/933b8fd9fd-1677580949/kenney_racing-kit.zip"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

say() { printf '\n== %s\n' "$*"; }

# --- system packages ---------------------------------------------------------
say "checking build dependencies"
MISSING=()
for pkg in libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev \
           libxinerama-dev libxkbcommon-dev libasound2-dev libdrm-dev libgbm-dev \
           libegl1-mesa-dev libgles2-mesa-dev; do
    dpkg -s "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
done
if (( ${#MISSING[@]} )); then
    echo "missing: ${MISSING[*]}"
    echo "install with:  sudo apt-get install -y --no-install-recommends ${MISSING[*]}"
    echo "(continuing — the desktop build may still work if you only need X11)"
fi

# --- raylib -------------------------------------------------------------------
mkdir -p "$VENDOR"
if [[ ! -d "$VENDOR/raylib" ]]; then
    say "cloning raylib $RAYLIB_VERSION"
    git clone --depth 1 --branch "$RAYLIB_VERSION" https://github.com/raysan5/raylib.git \
        "$VENDOR/raylib"
fi

OUT="$VENDOR/raylib-build"
if [[ $FORCE -eq 1 || ! -f "$OUT/lib/libraylib_desktop.a" || ! -f "$OUT/lib/libraylib_drm.a" ]]; then
    mkdir -p "$OUT/lib" "$OUT/include"
    build_raylib() {
        local name=$1; shift
        say "building raylib for $name"
        make -C "$VENDOR/raylib/src" clean >/dev/null 2>&1 || true
        make -C "$VENDOR/raylib/src" -j"$(nproc)" "$@" >/dev/null
        cp "$VENDOR/raylib/src/libraylib.a" "$OUT/lib/libraylib_$name.a"
    }
    # Desktop uses GL 3.3 via GLFW/X11; DRM renders straight to /dev/dri with GLES2.
    build_raylib desktop PLATFORM=PLATFORM_DESKTOP GRAPHICS=GRAPHICS_API_OPENGL_33
    build_raylib drm     PLATFORM=PLATFORM_DRM     GRAPHICS=GRAPHICS_API_OPENGL_ES2
    cp "$VENDOR/raylib/src"/{raylib.h,raymath.h,rlgl.h} "$OUT/include/"
    make -C "$VENDOR/raylib/src" clean >/dev/null 2>&1 || true
else
    say "raylib already built (use --force to rebuild)"
fi

# --- Kenney Racing Kit ----------------------------------------------------------
# The models are committed (they are CC0), so this only runs on a stripped checkout.
if [[ ! -f "$REPO/assets/models/roadStraight.glb" ]]; then
    say "downloading the Kenney Racing Kit"
    curl -sSL -o "$VENDOR/kenney_racing-kit.zip" "$KIT_URL"
    mkdir -p "$REPO/assets/models" "$REPO/assets/licenses"
    unzip -q -j -o "$VENDOR/kenney_racing-kit.zip" 'Models/GLTF format/*.glb' \
        -d "$REPO/assets/models/"
    unzip -q -j -o "$VENDOR/kenney_racing-kit.zip" 'License.txt' -d "$REPO/assets/licenses/"
    mv "$REPO/assets/licenses/License.txt" "$REPO/assets/licenses/kenney-racing-kit-LICENSE.txt"
fi

say "done — now run:  make && ./build/desktop/racer"
