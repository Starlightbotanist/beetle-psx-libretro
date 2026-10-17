ROOT := ../..
CFLAGS ?= -O2 -Wall -Wextra -std=gnu89 -fwrapv
CPPFLAGS := -I$(ROOT) -I$(ROOT)/libretro-common/include
ifeq ($(OS),Windows_NT)
EXEEXT := .exe
endif

SRC := geometry.c $(ROOT)/mednafen/psx/gpu_polygon_sub.c

all: geometry$(EXEEXT)

geometry$(EXEEXT): $(SRC) $(ROOT)/mednafen/psx/gpu.h $(ROOT)/rhi/rhi_intf.h $(ROOT)/beetle_psx_globals.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC)

check: geometry$(EXEEXT)
	./geometry$(EXEEXT)

clean:
	rm -f geometry geometry.exe

.PHONY: all check clean
