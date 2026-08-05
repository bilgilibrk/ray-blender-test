# Top-down racer — raylib engine + game
#
#   make                  build for the desktop (X11/OpenGL 3.3)
#   make PLATFORM=drm     build for a Raspberry Pi console (DRM/KMS + GLES2)
#   make run              build and run
#   make test             build and run the engine unit tests
#   make shaders          validate the GLSL for both backends
#   make level            regenerate the demo track through Blender
#   make clean

PLATFORM ?= desktop

CC       := gcc
RAYLIB   := vendor/raylib-build
BUILD    := build/$(PLATFORM)

WARNINGS := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wpointer-arith -Wcast-align \
            -Wstrict-prototypes -Wmissing-prototypes
# -MMD -MP emit a .d file per object listing the headers it used, so editing a
# header rebuilds everything that includes it. Without this, changing a struct
# leaves stale objects reading fields at the wrong offsets.
CFLAGS   := -std=c11 -O2 -g $(WARNINGS) -MMD -MP \
            -Iengine/include -Igame/include -I$(RAYLIB)/include
LDLIBS   := -lm -lpthread -ldl -lrt

ifeq ($(PLATFORM),drm)
  CFLAGS    += -DENGINE_PLATFORM_DRM
  RAYLIB_LIB := $(RAYLIB)/lib/libraylib_drm.a
  LDLIBS    += -lEGL -lGLESv2 -lgbm -ldrm
else
  RAYLIB_LIB := $(RAYLIB)/lib/libraylib_desktop.a
  LDLIBS    += -lGL -lX11 -lXrandr -lXi -lXcursor -lXinerama
endif

ENGINE_SRC := $(wildcard engine/src/*.c)
GAME_SRC   := $(wildcard game/src/*.c)
TEST_SRC   := $(wildcard tests/*.c)

ENGINE_OBJ := $(ENGINE_SRC:%.c=$(BUILD)/%.o)
GAME_OBJ   := $(GAME_SRC:%.c=$(BUILD)/%.o)
# Game code minus its entry point, so tests can drive a whole race in-process.
GAME_LIB   := $(filter-out $(BUILD)/game/src/main.o,$(GAME_OBJ))

TARGET     := $(BUILD)/racer
TEST_BIN   := $(BUILD)/tests

.PHONY: all run clean test level engine shaders dirs

all: $(TARGET)

$(TARGET): $(ENGINE_OBJ) $(GAME_OBJ) $(RAYLIB_LIB)
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(ENGINE_OBJ) $(GAME_OBJ) $(RAYLIB_LIB) $(LDLIBS)
	@echo "built $@"

# Engine objects only — handy while the game layer is in flux.
engine: $(ENGINE_OBJ)

$(BUILD)/%.o: %.c
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

-include $(ENGINE_OBJ:.o=.d) $(GAME_OBJ:.o=.d)
