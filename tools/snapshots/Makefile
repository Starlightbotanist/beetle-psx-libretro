PYTHON ?= python3
HARNESS_CACHE ?=
ifeq ($(origin TOOLS),undefined)
TOOLS := $(shell "$(PYTHON)" fetch_harness.py $(if $(HARNESS_CACHE),--cache "$(HARNESS_CACHE)") --path)
harness:
	"$(PYTHON)" fetch_harness.py $(if $(HARNESS_CACHE),--cache "$(HARNESS_CACHE)") $(if $(filter 1,$(OFFLINE)),--offline)
else
harness:
	@test -f "$(TOOLS)/snapshot_smoke.c" -a -f "$(TOOLS)/run_profiles.py" || { echo 'TOOLS must name a snapshot harness directory'; exit 1; }
endif

.PHONY: harness

.DEFAULT_GOAL := all
CC ?= cc
CFLAGS ?= -O2 -g
CORE ?= ../../mednafen_psx_libretro.so
SCALE ?= 1

ifeq ($(OS),Windows_NT)
EXE = .exe
else
LDLIBS += -ldl
endif

all: build/snapshot_smoke$(EXE) build/gl_errors_test$(EXE) build/display_mode_test$(EXE)

build/snapshot_smoke$(EXE): harness
	$(MAKE) -B -C "$(TOOLS)" BUILD="$(CURDIR)/build" all

build/gl_errors_fragment.h: ../../rhi/rhi_lib_gl.c
	mkdir -p build
	sed -n '/^static bool gl_snapshot_check_errors(.*)$$/,/^}/p' $< > $@

build/gl_errors_test$(EXE): gl_errors_test.c build/gl_errors_fragment.h
	$(CC) $(CFLAGS) -std=c99 -Wall -Wextra -Werror=implicit-function-declaration $< -o $@ $(LDFLAGS)

build/display_mode_fragment.h: ../../rhi/rhi_intf.c
	mkdir -p build
	sed -n '/^void rhi_intf_set_display_mode(/,/^}/p' $< > $@

build/display_mode_test$(EXE): display_mode_test.c build/display_mode_fragment.h
	$(CC) $(CFLAGS) -std=c99 -Wall -Wextra -Werror=implicit-function-declaration $< -o $@ $(LDFLAGS)

check: all
	./build/gl_errors_test$(EXE)
	./build/display_mode_test$(EXE)
	./build/snapshot_smoke$(EXE) "$(abspath $(CORE))" "$(CURDIR)/build/fixture" $(SCALE)

.PHONY: all check
