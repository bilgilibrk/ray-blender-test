#!/usr/bin/env bash
# One-time setup: check out the raylib submodule, report any missing system
# packages, and fetch the Kenney kit if the models are not already present.
#
#   tools/setup.sh
#
# raylib itself is built by the top-level Makefile, from raylib's own Makefile.
# Nothing here compiles anything.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KIT_URL="https://kenney.nl/media/pages/assets/racing-kit/933b8fd9fd-1677580949/kenney_racing-kit.zip"

say() { printf '\n== %s\n' "$*"; }

# --- raylib submodule ---------------------------------------------------------
say "checking out submodules"
if [[ -d "$REPO/.git" ]]; then
    git -C "$REPO" submodule update --init
else
    echo "not a git checkout — skipping (expected vendor/raylib to be present)"
fi

if [[ ! -f "$REPO/vendor/raylib/src/raylib.h" ]]; then
    echo "error: vendor/raylib is still empty. Clone with --recurse-submodules, or run"
    echo "       git submodule update --init"
    exit 1
fi
echo "raylib $(git -C "$REPO/vendor/raylib" describe --tags 2>/dev/null || echo '(detached)') ready"

# --- system packages ------------------------------------------------------------
# raylib links against the platform's GL and windowing libraries; the package
# names differ per distribution, the -l flags do not.
say "checking build dependencies"

case "$(uname -s)" in
    Linux)
        if command -v pacman >/dev/null 2>&1; then
            PKGS="base-devel mesa libx11 libxrandr libxi libxcursor libxinerama alsa-lib"
            [[ -e /dev/dri ]] && PKGS="$PKGS libdrm mesa"
            echo "Arch: sudo pacman -S --needed $PKGS"
        elif command -v apt-get >/dev/null 2>&1; then
            MISSING=()
            for pkg in build-essential libgl1-mesa-dev libx11-dev libxrandr-dev libxi-dev \
                       libxcursor-dev libxinerama-dev libxkbcommon-dev libasound2-dev \
                       libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev; do
                dpkg -s "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
            done
            if (( ${#MISSING[@]} )); then
                echo "Debian/Ubuntu/Raspberry Pi OS:"
                echo "  sudo apt-get install -y --no-install-recommends ${MISSING[*]}"
            else
                echo "all Debian packages present"
            fi
        elif command -v dnf >/dev/null 2>&1; then
            echo "Fedora: sudo dnf install gcc make mesa-libGL-devel libX11-devel \\"
            echo "        libXrandr-devel libXi-devel libXcursor-devel libXinerama-devel alsa-lib-devel"
        else
            echo "unrecognised distribution — install a C toolchain plus the GL, X11 and ALSA headers"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*)
        echo "MSYS2: pacman -S --needed mingw-w64-x86_64-gcc make git"
        echo "Build from the MinGW64 shell; nothing else is required on Windows."
        ;;
    Darwin)
        echo "macOS: Xcode command line tools provide everything raylib needs."
        ;;
    *)
        echo "unrecognised system '$(uname -s)' — install a C toolchain and GL headers"
        ;;
esac

# --- Kenney Racing Kit ------------------------------------------------------------
# The models are committed (they are CC0), so this only runs on a stripped checkout.
if [[ ! -f "$REPO/assets/models/roadStraight.glb" ]]; then
    say "downloading the Kenney Racing Kit"
    mkdir -p "$REPO/vendor" "$REPO/assets/models" "$REPO/assets/licenses"
    curl -sSL -o "$REPO/vendor/kenney_racing-kit.zip" "$KIT_URL"
    unzip -q -j -o "$REPO/vendor/kenney_racing-kit.zip" 'Models/GLTF format/*.glb' \
        -d "$REPO/assets/models/"
    unzip -q -j -o "$REPO/vendor/kenney_racing-kit.zip" 'License.txt' -d "$REPO/assets/licenses/"
    mv "$REPO/assets/licenses/License.txt" "$REPO/assets/licenses/kenney-racing-kit-LICENSE.txt"
fi

say "done — now run:  make && ./build/desktop/racer"
