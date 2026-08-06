# Top-down racer — raylib engine + game
#
#   make                  build for the desktop (OpenGL 3.3)
#   make PLATFORM=drm     build for a Raspberry Pi console (DRM/KMS + GLES2)
#   make run              build and run
#   make test             build and run the engine unit tests
#   make shaders          validate the GLSL for both backends
#   make level            regenerate the demo track through Blender
#   make clean            remove build output (raylib included)
#
# raylib is a git submodule pinned to its 5.5 tag and is built from source by
# this Makefile, through raylib's own Makefile — no CMake, no zig. First time:
#
#   git submodule update --init
#
# Windows: build from an MSYS2 MinGW64 shell (or Git Bash with a MinGW gcc).
# The rules below use sh, mkdir -p and rm, which cmd.exe does not provide.

PLATFORM ?= desktop

# ---------------------------------------------------------------------------
# Host detection. Windows sets OS in the environment even under MSYS2, where
# uname also works; everywhere else uname is the answer.
# ---------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
  HOST := Windows
else
  HOST := $(shell uname -s)
endif

CC       := gcc
BUILD    := build/$(PLATFORM)

RAYLIB_DIR := vendor/raylib
RAYLIB_SRC := $(RAYLIB_DIR)/src
RAYLIB_LIB := $(BUILD)/libraylib.a

# ---------------------------------------------------------------------------
# Per-platform link libraries, and how raylib itself should be configured.
# ---------------------------------------------------------------------------
ifeq ($(PLATFORM),drm)
  # Renders straight to /dev/dri with no window system. Linux only.
  ifneq ($(HOST),Linux)
    $(error PLATFORM=drm needs Linux with DRM/KMS; this host is $(HOST). Use the default \
            desktop build instead)
  endif
  RAYLIB_PLATFORM := PLATFORM_DRM
  RAYLIB_GRAPHICS := GRAPHICS_API_OPENGL_ES2
  PLATFORM_CFLAGS := -DENGINE_PLATFORM_DRM
  PLATFORM_LDLIBS := -lEGL -lGLESv2 -lgbm -ldrm -lm -lpthread -ldl -lrt
else
  RAYLIB_PLATFORM := PLATFORM_DESKTOP
  RAYLIB_GRAPHICS := GRAPHICS_API_OPENGL_33
  PLATFORM_CFLAGS :=
  ifeq ($(HOST),Windows)
    PLATFORM_LDLIBS := -lopengl32 -lgdi32 -lwinmm
    EXE := .exe
  else ifeq ($(HOST),Darwin)
    # Untested here; these are raylib's documented macOS frameworks.
    PLATFORM_LDLIBS := -framework OpenGL -framework Cocoa -framework IOKit \
                       -framework CoreVideo -framework CoreAudio
  else
    PLATFORM_LDLIBS := -lGL -lX11 -lXrandr -lXi -lXcursor -lXinerama \
                       -lm -lpthread -ldl -lrt
  endif
endif

# raylib picks cmd.exe for its shell whenever OS=Windows_NT, which is wrong in
# an MSYS2 shell; its `clean` would then call `del`.
ifeq ($(HOST),Windows)
  RAYLIB_SHELL := PLATFORM_SHELL=sh
endif

# RAYLIB_RELEASE_PATH must be absolute: raylib's Makefile runs from its own
# directory. Objects stay in raylib's tree, which its .gitignore already covers,
# so building leaves the submodule clean.
#
# Both this and $(MAKE) are quoted at every use below. On Windows they routinely
# contain spaces and brackets — "C:/Program Files (x86)/GnuWin32/bin/make" — and
# an unquoted expansion is a shell syntax error, not a missing-file error.
RAYLIB_MAKEFLAGS := PLATFORM=$(RAYLIB_PLATFORM) GRAPHICS=$(RAYLIB_GRAPHICS) \
                    RAYLIB_RELEASE_PATH="$(abspath $(BUILD))" $(RAYLIB_SHELL)

WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wmissing-prototypes
# -MMD -MP emit a .d file per object listing the headers it used, so editing a
# header rebuilds everything that includes it. Without this, changing a struct
# leaves stale objects reading fields at the wrong offsets.
CFLAGS   := -std=c11 -O2 -g $(WARNINGS) -MMD -MP $(PLATFORM_CFLAGS) \
            -Iengine/include -Igame/include -I$(RAYLIB_SRC)
LDLIBS   := $(PLATFORM_LDLIBS)

ENGINE_SRC := $(wildcard engine/src/*.c)
GAME_SRC   := $(wildcard game/src/*.c)
TEST_SRC   := $(wildcard tests/*.c)

ENGINE_OBJ := $(ENGINE_SRC:%.c=$(BUILD)/%.o)
GAME_OBJ   := $(GAME_SRC:%.c=$(BUILD)/%.o)
# Game code minus its entry point, so tests can drive a whole race in-process.
GAME_LIB   := $(filter-out $(BUILD)/game/src/main.o,$(GAME_OBJ))

TARGET     := $(BUILD)/racer$(EXE)
TEST_BIN   := $(BUILD)/tests$(EXE)

.PHONY: all run clean test level engine shaders raylib submodule

all: $(TARGET)

$(TARGET): $(ENGINE_OBJ) $(GAME_OBJ) $(RAYLIB_LIB)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(ENGINE_OBJ) $(GAME_OBJ) $(RAYLIB_LIB) $(LDLIBS)
	@echo "built $@"

# --- raylib ------------------------------------------------------------------

raylib: $(RAYLIB_LIB)

# raylib compiles its objects in its own source directory and does not tag them
# by platform, so a desktop build and a DRM build would otherwise mix. Cleaning
# first costs a full rebuild, but only when this library is actually missing.
$(RAYLIB_LIB): $(RAYLIB_SRC)/raylib.h
	@mkdir -p $(BUILD)
	@echo "building raylib for $(RAYLIB_PLATFORM) ($(RAYLIB_GRAPHICS))"
	"$(MAKE)" -C $(RAYLIB_SRC) clean $(RAYLIB_MAKEFLAGS)
	"$(MAKE)" -C $(RAYLIB_SRC) $(RAYLIB_MAKEFLAGS)

# Present only when the submodule has not been checked out.
$(RAYLIB_SRC)/raylib.h:
	@echo ""
	@echo "raylib sources are missing — vendor/raylib is a git submodule."
	@echo "Run:  git submodule update --init"
	@echo ""
	@exit 1

submodule:
	git submodule update --init

# --- project -----------------------------------------------------------------

# Engine objects only — handy while the game layer is in flux.
engine: $(ENGINE_OBJ)

$(BUILD)/%.o: %.c | $(RAYLIB_SRC)/raylib.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Tests run the engine and the race simulation with no window: nothing below the
# renderer touches OpenGL. raylib is still linked for TraceLog, file IO and maths.
$(TEST_BIN): $(TEST_SRC) $(ENGINE_OBJ) $(GAME_LIB) $(RAYLIB_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(ENGINE_OBJ) $(GAME_LIB) $(RAYLIB_LIB) $(LDLIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

# Validates both GLSL variants. The GLES2 one cannot be exercised by running the
# game on a machine with no display, so it is checked statically instead.
shaders:
	@tools/check_shaders.sh

run: $(TARGET)
	./$(TARGET)

level:
	blender --background --python tools/blender/build_demo_track.py

clean:
	rm -rf build
	@if [ -f $(RAYLIB_SRC)/Makefile ]; then \
	    "$(MAKE)" -C $(RAYLIB_SRC) clean $(RAYLIB_MAKEFLAGS) >/dev/null 2>&1 || true; \
	fi

-include $(ENGINE_OBJ:.o=.d) $(GAME_OBJ:.o=.d)
