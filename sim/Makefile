# ---------------------------------------------------------------------------
# sim/Makefile - headless LVGL 8.4 host renderer for the Claude usage widget.
#
# Builds LVGL + ../ui/ui.c + the harness into ./sim, which renders an 800x480
# screen into memory and writes a PNG. Plain C (gcc, NOT g++ - LVGL 8 is C99
# and does not compile as C++ without warnings turning into grief).
#
# Intended to run inside WSL Ubuntu, from PowerShell:
#   wsl -d Ubuntu -- bash -lc "cd /mnt/c/path/to/claude_desk_panel/sim && make shots"
# ---------------------------------------------------------------------------

# --- paths ------------------------------------------------------------------
# Where the Arduino libraries live: lvgl/ and lv_conf.h. Under WSL that is a
# Windows path, and $(HOME) is the Linux home, so it cannot be guessed - run.ps1
# passes it, or set it yourself:
#
#   make ARDUINO_LIBS=/mnt/c/Users/<you>/Documents/Arduino/libraries
#   make ARDUINO_LIBS=~/Arduino/libraries          # native Linux
ARDUINO_LIBS ?= $(HOME)/Arduino/libraries
LVGL_DIR     ?= $(ARDUINO_LIBS)/lvgl
LV_CONF_DIR  ?= $(ARDUINO_LIBS)

# Fail with the reason rather than a thousand missing-header errors.
ifeq ($(wildcard $(LVGL_DIR)/lvgl.h),)
  $(error LVGL not found at $(LVGL_DIR). Set ARDUINO_LIBS to the Arduino \
libraries folder holding lvgl/ and lv_conf.h, e.g. \
make ARDUINO_LIBS=/mnt/c/Users/you/Documents/Arduino/libraries)
endif

BUILD := build
BIN   := sim
SHOTS := shots

# ---------------------------------------------------------------------------
# UI source selection.
#
# The real UI is ../ui/ui.c. Until it exists we fall back to ui_stub.c, a
# throwaway implementation of ui.h that draws a rectangle and a few labels so
# the render + PNG pipeline can be proven end to end. The moment ../ui/ui.c
# appears, this picks it up automatically and the stub is ignored - it is NOT
# linked alongside, which would give duplicate symbols.
# ---------------------------------------------------------------------------
UI_SRCS := $(wildcard ../ui/*.c) $(wildcard ../assets/claude_logo.c)
ifeq ($(strip $(UI_SRCS)),)
  UI_SRCS   := ui_stub.c
  UI_ORIGIN := ui_stub.c (../ui/ui.c not present yet)
else
  UI_ORIGIN := $(UI_SRCS)
endif

# --- sources ---------------------------------------------------------------
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')
SIM_SRCS  := main.c png_write.c

# --- objects (mirror the source tree under build/) -------------------------
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(BUILD)/lvgl/%.o,$(LVGL_SRCS))
SIM_OBJS  := $(patsubst %.c,$(BUILD)/sim/%.o,$(SIM_SRCS))
UI_OBJS   := $(patsubst %.c,$(BUILD)/ui/%.o,$(notdir $(UI_SRCS)))
OBJS      := $(LVGL_OBJS) $(SIM_OBJS) $(UI_OBJS)

# --- flags -----------------------------------------------------------------
# LV_CONF_INCLUDE_SIMPLE makes lv_conf_internal.h do #include "lv_conf.h",
# which the -I on LV_CONF_DIR then resolves. -I on LVGL_DIR is what lets
# ui.h say #include <lvgl.h>.
CC       ?= gcc
INCLUDES := -I. -I.. -I../ui -I../assets -I$(LVGL_DIR) -I$(LV_CONF_DIR)
# UI_HAVE_LOGO_IMG selects the real Claude mark (assets/claude_logo.c) over
# ui.c's twelve-line fallback. Must match what build.ps1 passes to the firmware,
# or the simulator renders a different header than the board does.
DEFINES  := -DLV_CONF_INCLUDE_SIMPLE -DUI_HAVE_LOGO_IMG
BASEFLAGS := -std=gnu99 -O2 -g $(INCLUDES) $(DEFINES) -MMD -MP
# Our code gets warnings; vendored LVGL does not (it is noisy under -Wextra and
# we are not allowed to patch it).
CFLAGS_OURS := $(BASEFLAGS) -Wall -Wextra -Wno-unused-parameter
CFLAGS_LVGL := $(BASEFLAGS) -w
LDFLAGS  :=
LDLIBS   := -lm

# ---------------------------------------------------------------------------
.PHONY: all shots clean distclean info
.DEFAULT_GOAL := all

all: $(BIN)

info:
	@echo "LVGL_DIR    = $(LVGL_DIR)"
	@echo "LV_CONF_DIR = $(LV_CONF_DIR)"
	@echo "UI sources  = $(UI_ORIGIN)"
	@echo "LVGL .c     = $(words $(LVGL_SRCS)) files"

$(BIN): $(OBJS)
	@echo "  LD    $@   [ui: $(UI_ORIGIN)]"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    lvgl/$*.c"
	@$(CC) $(CFLAGS_LVGL) -c $< -o $@

$(BUILD)/sim/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS_OURS) -c $< -o $@

# Two patterns for the UI object so it works whether the source came from
# ../ui/ or from the local stub.
$(BUILD)/ui/%.o: ../assets/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS_OURS) -c $< -o $@

$(BUILD)/ui/%.o: ../ui/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS_OURS) -c $< -o $@

$(BUILD)/ui/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS_OURS) -c $< -o $@

# ---------------------------------------------------------------------------
# Every scenario, regenerated into shots/.
# ---------------------------------------------------------------------------
shots: $(BIN)
	@mkdir -p $(SHOTS)
	./$(BIN) --page usage    --theme dark               --out $(SHOTS)/usage-dark.png
	./$(BIN) --page usage    --theme light              --out $(SHOTS)/usage-light.png
	./$(BIN) --page home     --theme dark               --out $(SHOTS)/home-dark.png
	./$(BIN) --page home     --theme light              --out $(SHOTS)/home-light.png
	./$(BIN) --page settings --theme light              --out $(SHOTS)/settings-light.png
	./$(BIN) --page settings --theme dark               --out $(SHOTS)/settings-dark.png
	./$(BIN) --page usage    --theme dark --state stale --out $(SHOTS)/stale.png
	./$(BIN) --page usage    --theme dark --state nowifi --out $(SHOTS)/nowifi.png
	./$(BIN) --page usage    --theme dark --state bridge --out $(SHOTS)/bridge.png
	./$(BIN) --page usage    --theme dark --state limit  --out $(SHOTS)/limit.png
	./$(BIN) --wifi          --theme dark               --out $(SHOTS)/wifi.png
	./$(BIN) --page usage    --theme dark --dim 40      --out $(SHOTS)/usage-dim40.png
	@echo "--- $(SHOTS)/ ---"
	@ls -l $(SHOTS)

clean:
	rm -rf $(BUILD) $(BIN)

distclean: clean
	rm -rf $(SHOTS)

-include $(OBJS:.o=.d)
