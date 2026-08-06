#!/usr/bin/env bash
# Compiles the scene shader for both graphics backends and validates the GLSL.
#
# The DRM/GLES2 target needs a display to run, so on a headless machine this is
# the only thing standing between a typo in the GLSL 100 variant and a build
# that fails at startup on real hardware.
#
#   tools/check_shaders.sh
#
# Needs glslang-tools; skips with a warning if it is not installed.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if ! command -v glslangValidator >/dev/null 2>&1; then
    echo "glslangValidator not found — skipping (sudo apt-get install glslang-tools)"
    exit 0
fi

cat > "$WORK/dump.c" <<'EOF'
#include <stdio.h>
#include "engine/light.h"
#if defined(ENGINE_PLATFORM_DRM)
    #define ENGINE_GLSL_VERSION 100
#else
    #define ENGINE_GLSL_VERSION 330
#endif
#include "scene_shader.inc"

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *v = fopen(argv[1], "w");
    FILE *f = fopen(argv[2], "w");
    if (!v || !f) return 1;
    fputs(kVertexShader, v);
    fputs(kFragmentShader, f);
    fclose(v);
    fclose(f);
    return 0;
}
EOF

status=0
for target in desktop drm; do
    define=""
    [[ $target == drm ]] && define="-DENGINE_PLATFORM_DRM"

    gcc -std=c11 $define \
        -I"$REPO/engine/include" -I"$REPO/engine/src" -I"$REPO/vendor/raylib/src" \
        -o "$WORK/dump_$target" "$WORK/dump.c"
    "$WORK/dump_$target" "$WORK/$target.vert" "$WORK/$target.frag"

    version=$(head -1 "$WORK/$target.vert")
    printf '%-8s %-12s ' "$target" "$version"
    if glslangValidator "$WORK/$target.vert" "$WORK/$target.frag" > "$WORK/$target.log" 2>&1; then
        echo "ok"
    else
        echo "FAILED"
        sed 's/^/    /' "$WORK/$target.log"
        status=1
    fi
done

exit $status
